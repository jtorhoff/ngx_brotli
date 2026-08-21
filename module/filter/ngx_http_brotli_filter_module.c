/*
 * Copyright (C) Igor Sysoev
 * Copyright (C) Nginx, Inc.
 * Copyright (C) Google Inc.
 */

#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>

#include "../common/ngx_http_brotli_headers.h"

#if (NGX_HAVE_BROTLI_ENC_ENCODE_H)
#include <brotli/enc/encode.h>
#else
#include <brotli/encode.h>
#endif

/* Brotli and GZip modules never stack, i.e. when one of them sets
   "Content-Encoding" the other becomes a pass-through filter. Consequently,
   it is almost legal to reuse this "buffered" bit.
   IIUC, buffered == some data passed to filter has not been pushed further. */
#define NGX_HTTP_BROTLI_BUFFERED NGX_HTTP_GZIP_BUFFERED

/* How much input may be held back while waiting to learn the response size.
   Chosen to match Brotli's input block size at the qualities that use one:
   the encoder would have buffered that much internally without emitting
   anything, so holding it here is not observable. Note the block is only 64k
   for quality 4 and above - below that Brotli uses 16k (see ComputeLgBlock),
   and this cap is larger than the encoder's own. That costs nothing today
   because the deferral almost always ends earlier, when the caller asks for
   progress with a NULL chain, but it is why this is a ceiling rather than a
   target. */
#define NGX_HTTP_BROTLI_DEFER_INPUT (64 * 1024)

/* Module configuration. */
typedef struct {
    ngx_flag_t enable;

    /* Supported MIME types. */
    ngx_hash_t   types;
    ngx_array_t *types_keys;

    /* Minimal required length for compression (if known). */
    ssize_t min_length;

    /* Brotli encoder parameter: quality */
    ngx_int_t quality;

    /* Brotli encoder parameter: (max) lg_win */
    size_t lg_win;
} ngx_http_brotli_conf_t;

/* What, if anything, the single output buffer is currently holding. The three
   states are exclusive: output is taken from the encoder only while the buffer
   is idle, and committing it moves it straight from ready to busy. They were
   once two independent bits, which left a fourth combination that could not
   arise and had to be reasoned away on every read. */
typedef enum {
    /* Nothing held; the encoder may be asked for more. */
    NGX_HTTP_BROTLI_OUTPUT_IDLE = 0,
    /* Filled from the encoder, not yet handed to the next filter. */
    NGX_HTTP_BROTLI_OUTPUT_READY,
    /* Handed on, and not yet fully consumed. */
    NGX_HTTP_BROTLI_OUTPUT_BUSY
} ngx_http_brotli_output_e;

/* What one turn of the body filter's loop decided to do next. The loop owns
   the returns; a step only says which one, so that each phase can be read on
   its own without tracing control flow back out through the loop. */
typedef enum {
    /* Made progress; go round again. */
    NGX_HTTP_BROTLI_STEP_CONTINUE = 0,
    /* Nothing more to do this call; return NGX_OK. */
    NGX_HTTP_BROTLI_STEP_DONE,
    /* Blocked on the next filter; return NGX_AGAIN. */
    NGX_HTTP_BROTLI_STEP_AGAIN,
    /* Unrecoverable; the loop closes the stream and returns NGX_ERROR. */
    NGX_HTTP_BROTLI_STEP_FAILED
} ngx_http_brotli_step_e;

/* What the body filter should do once ngx_http_brotli_filter_prepare has
   settled the decisions that come before the encoder. This is deliberately
   not an ngx_int_t: the alternative was a sentinel return code, which would
   have had to be a value no filter below could ever hand back, and nothing
   but a comment would have kept that true.

   The caller treats DEFER and REJECT alike - both end the call and return
   "rc" - so they are separate for the reader rather than for the control
   flow. "Not yet" and "no" answer different questions, and the response may
   still be compressed after a DEFER. */
typedef enum {
    /* Accepted for encoding: nothing left to settle, so carry on into the
       encoder loop. The only outcome that does not write "rc", because it is
       the only one where the body filter does not return straight away. */
    NGX_HTTP_BROTLI_ACCEPT = 0,
    /* Not yet: too little of the body has arrived to answer the question
       brotli_min_length asks, or its size is still unknown and worth waiting
       a moment to learn before the encoder window is fixed. The input stays
       in ctx->in and a later call decides, so this may still end in
       compression. "rc" is NGX_OK - nothing has gone wrong. */
    NGX_HTTP_BROTLI_DEFER,
    /* Settled, and not encoded here: the response is passing through
       uncompressed, the header filter failed, or a special response was
       substituted below us. "rc" holds what the body filter should return,
       which is a different value in each of those cases. */
    NGX_HTTP_BROTLI_REJECT
} ngx_http_brotli_prepare_e;

/* Instance context. */
typedef struct {
    /* Brotli encoder instance. */
    BrotliEncoderState *encoder;

    /* Payload length; -1, if unknown. */
    off_t content_length;

    /* Input buffer chain. */
    ngx_chain_t *in;

    /* Output chain. */
    ngx_chain_t *out_chain;

    /* Output buffer. */
    ngx_buf_t *out_buf;

    /* 1 if the response headers are still ours to send. Set when the response
       length is unknown, so that brotli_min_length can be applied once enough
       of the body has been seen to judge it. */
    unsigned headers_postponed : 1;

    /* 1 if encoder is initialized, output chain and buffer are allocated. */
    unsigned initialized : 1;
    /* 1 if compression is finished / failed. */
    unsigned closed : 1;

    /* 1 once a buffer marked last_buf has been handed to the encoder. Never
       cleared: input cannot resume. Read to choose FINISH over FLUSH when
       draining, and to know the stream may be closed once the encoder says it
       is finished. */
    unsigned end_of_input : 1;
    /* 1 when a block has been closed and the next buffer taken from the
       encoder must carry the flush marker, so the filters below push it out
       instead of holding it for more. Despite sitting beside end_of_input
       this is an instruction rather than a state: take_output clears it as
       soon as it has acted on it. */
    unsigned flush_pending : 1;
    /* 1 if input has been handed to the encoder that it has not been asked
       to emit yet. BROTLI_OPERATION_PROCESS holds such data back until a
       block fills, so it has to be flushed explicitly when the caller wants
       output. */
    unsigned unflushed_input : 1;
    /* 1 if this call of the body filter arrived with no new data, i.e. nginx
       is asking for progress on what it has already handed over rather than
       adding to it. Set once per call, before the loop runs. */
    unsigned caller_wants_output : 1;

    /* State of out_buf. ngx_pcalloc starts it at IDLE. */
    ngx_http_brotli_output_e output;

    ngx_http_request_t *request;
} ngx_http_brotli_ctx_t;

/* Forward declarations. What each of these does is documented at its
   definition, not here, so that the explanation is where the code is. */

static ngx_int_t ngx_http_brotli_filter_ensure_stream_initialized(
    ngx_http_brotli_ctx_t *ctx);
static void ngx_http_brotli_filter_close(ngx_http_brotli_ctx_t *ctx);
static size_t ngx_http_brotli_filter_pending_input(ngx_chain_t *in,
    ngx_uint_t *complete, ngx_uint_t *urgent);

static void *ngx_http_brotli_filter_alloc(void *opaque, size_t size);
static void ngx_http_brotli_filter_free(void *opaque, void *address);
static void ngx_http_brotli_filter_cleanup(void *data);

static ngx_int_t ngx_http_brotli_filter_send_headers(ngx_http_request_t *r,
    ngx_http_brotli_ctx_t *ctx, ngx_uint_t compress);

static void *ngx_http_brotli_create_conf(ngx_conf_t *cf);
static char *ngx_http_brotli_merge_conf(ngx_conf_t *cf, void *parent,
    void *child);
static ngx_int_t ngx_http_brotli_filter_init(ngx_conf_t *cf);

static char *ngx_http_brotli_parse_wbits(ngx_conf_t *cf, void *post,
    void *data);

/* Configuration literals. */

static ngx_conf_num_bounds_t ngx_http_brotli_comp_level_bounds = {
    ngx_conf_check_num_bounds, BROTLI_MIN_QUALITY, BROTLI_MAX_QUALITY};

static ngx_conf_post_handler_pt ngx_http_brotli_parse_wbits_p =
    ngx_http_brotli_parse_wbits;

static ngx_command_t ngx_http_brotli_filter_commands[] = {
    {ngx_string("brotli"),
        NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_LOC_CONF |
            NGX_HTTP_LIF_CONF | NGX_CONF_FLAG,
        ngx_conf_set_flag_slot, NGX_HTTP_LOC_CONF_OFFSET,
        offsetof(ngx_http_brotli_conf_t, enable), NULL},

    {ngx_string("brotli_types"),
        NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_LOC_CONF |
            NGX_CONF_1MORE,
        ngx_http_types_slot, NGX_HTTP_LOC_CONF_OFFSET,
        offsetof(ngx_http_brotli_conf_t, types_keys),
        &ngx_http_html_default_types[0]},

    {ngx_string("brotli_comp_level"),
        NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_LOC_CONF |
            NGX_CONF_TAKE1,
        ngx_conf_set_num_slot, NGX_HTTP_LOC_CONF_OFFSET,
        offsetof(ngx_http_brotli_conf_t, quality),
        &ngx_http_brotli_comp_level_bounds},

    {ngx_string("brotli_window"),
        NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_LOC_CONF |
            NGX_CONF_TAKE1,
        ngx_conf_set_size_slot, NGX_HTTP_LOC_CONF_OFFSET,
        offsetof(ngx_http_brotli_conf_t, lg_win),
        &ngx_http_brotli_parse_wbits_p},

    {ngx_string("brotli_min_length"),
        NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_LOC_CONF |
            NGX_CONF_TAKE1,
        ngx_conf_set_size_slot, NGX_HTTP_LOC_CONF_OFFSET,
        offsetof(ngx_http_brotli_conf_t, min_length), NULL},

    ngx_null_command};

/* Module context hooks. */
static ngx_http_module_t ngx_http_brotli_filter_module_ctx = {
    NULL,                        /* pre-configuration */
    ngx_http_brotli_filter_init, /* post-configuration */

    NULL, /* create main configuration */
    NULL, /* init main configuration */

    NULL, /* create server configuration */
    NULL, /* merge server configuration */

    ngx_http_brotli_create_conf, /* create location configuration */
    ngx_http_brotli_merge_conf   /* merge location configuration */
};

/* Module descriptor. */
ngx_module_t ngx_http_brotli_filter_module = {NGX_MODULE_V1,
    &ngx_http_brotli_filter_module_ctx, /* module context */
    ngx_http_brotli_filter_commands,    /* module directives */
    NGX_HTTP_MODULE,                    /* module type */
    NULL,                               /* init master */
    NULL,                               /* init module */
    NULL,                               /* init process */
    NULL,                               /* init thread */
    NULL,                               /* exit thread */
    NULL,                               /* exit process */
    NULL,                               /* exit master */
    NGX_MODULE_V1_PADDING};

/* Next filter in the filter chain. */
static ngx_http_output_header_filter_pt ngx_http_next_header_filter;
static ngx_http_output_body_filter_pt   ngx_http_next_body_filter;

/* Process headers and decide if request is eligible for brotli compression. */
static ngx_int_t
ngx_http_brotli_header_filter(ngx_http_request_t *r)
{
    ngx_http_brotli_ctx_t  *ctx;
    ngx_http_brotli_conf_t *conf;

    conf = ngx_http_get_module_loc_conf(r, ngx_http_brotli_filter_module);

    /* Filter only if enabled. */
    if (!conf->enable) {
        return ngx_http_next_header_filter(r);
    }

    /* Bypass "header only" responses. */
    if (r->header_only) {
        return ngx_http_next_header_filter(r);
    }

    /* Bypass statuses that either carry no body, or carry one that must not be
       re-encoded. 1xx/204/304 have no body to compress, so all they would get
       is a "Content-Encoding" describing nothing - which a client is entitled
       to apply to the next body it associates with the response. A 206 body is
       a byte range, and the "Content-Range" beside it still describes the
       original entity, so compressing it corrupts the response.

       This is deliberately a deny list. The allow list it replaces (200/403/404
       only, dropped in eebeaf3) also excluded perfectly compressible responses
       such as 201, 422 and 500; those stay compressed. */
    if (r->headers_out.status < NGX_HTTP_OK ||
        r->headers_out.status == NGX_HTTP_NO_CONTENT ||
        r->headers_out.status == NGX_HTTP_PARTIAL_CONTENT ||
        r->headers_out.status == NGX_HTTP_NOT_MODIFIED) {
        return ngx_http_next_header_filter(r);
    }

    /* Bypass already compressed responses. */
    if (r->headers_out.content_encoding &&
        r->headers_out.content_encoding->value.len) {
        return ngx_http_next_header_filter(r);
    }

    /* If response size is known, do not compress tiny responses. */
    if (r->headers_out.content_length_n != -1 &&
        r->headers_out.content_length_n < conf->min_length) {
        return ngx_http_next_header_filter(r);
    }

    /* Compress only certain MIME-typed responses. */
    if (ngx_http_test_content_type(r, &conf->types) == NULL) {
        return ngx_http_next_header_filter(r);
    }

    r->gzip_vary = 1;

    /* Check if client support brotli encoding. */
    if (ngx_http_brotli_claim_request(r) != NGX_OK) {
        return ngx_http_next_header_filter(r);
    }

    /* Prepare instance context. */
    ctx = ngx_pcalloc(r->pool, sizeof(ngx_http_brotli_ctx_t));
    if (ctx == NULL) {
        return NGX_ERROR;
    }
    ctx->request = r;
    ctx->content_length = r->headers_out.content_length_n;
    ngx_http_set_ctx(r, ctx, ngx_http_brotli_filter_module);

    r->main_filter_need_in_memory = 1;

    /* When the length is unknown there is nothing yet to compare against
       brotli_min_length, and committing the headers here would settle the
       question for good. Hold them instead: the body filter sends them as soon
       as it has seen enough of the body to decide, which takes only
       brotli_min_length bytes rather than the whole response. */
    if (ctx->content_length < 0) {
        ctx->headers_postponed = 1;
        return NGX_OK;
    }

    return ngx_http_brotli_filter_send_headers(r, ctx, 1);
}

/* Commits headers that ngx_http_brotli_header_filter held back. With
   "compress" set the response is labelled and the encoder will run; without
   it the response passes through untouched, and gzip - suppressed earlier so
   that Brotli would win - is allowed to reconsider. */
static ngx_int_t
ngx_http_brotli_filter_send_headers(ngx_http_request_t *r,
    ngx_http_brotli_ctx_t *ctx, ngx_uint_t compress)
{
    ctx->headers_postponed = 0;

    if (!compress) {
        /* Nothing has been allocated yet on this path - headers are only
           postponed while the encoder does not exist - so this closes an
           empty instance. Going through close anyway keeps it the only thing
           that sets ctx->closed, so the flag cannot mean two things. */
        ngx_http_brotli_filter_close(ctx);
        r->gzip_tested = 0;
        r->gzip_ok = 0;
        return ngx_http_next_header_filter(r);
    }

    /* Tell the filters below that the body is compressed. */
    if (ngx_http_brotli_set_content_encoding(r) != NGX_OK) {
        return NGX_ERROR;
    }

    ngx_http_clear_content_length(r);
    ngx_http_clear_accept_ranges(r);
    ngx_http_weak_etag(r);

    return ngx_http_next_header_filter(r);
}


/* Hands the committed output buffer to the next filter, and reports whether
   the encoder may be touched again. The encoder must not be, while any of its
   output is still outstanding: out_buf points into memory Brotli owns.

   The loop calls this only while ctx->output is not IDLE, so the buffer is
   always in one of two states on entry, and "resend" says which. */
static ngx_http_brotli_step_e
ngx_http_brotli_filter_send_output(ngx_http_brotli_ctx_t *ctx)
{
    ngx_int_t           rc;
    ngx_uint_t          resend;
    off_t               outstanding;
    ngx_chain_t        *to_send;
    ngx_http_request_t *r;

    r = ctx->request;

    /* READY: freshly filled, so hand the chain over. BUSY: handed over once
       already and not yet fully consumed, so offer nothing new and see
       whether the filters below have moved any of it. */
    resend = (ctx->output == NGX_HTTP_BROTLI_OUTPUT_BUSY);
    to_send = resend ? NULL : ctx->out_chain;
    outstanding = ngx_buf_size(ctx->out_buf);

    rc = ngx_http_next_body_filter(r, to_send);

    if (!resend) {
        ctx->output = NGX_HTTP_BROTLI_OUTPUT_BUSY;
    }
    if (ngx_buf_size(ctx->out_buf) == 0) {
        ctx->output = NGX_HTTP_BROTLI_OUTPUT_IDLE;
    }

    if (rc == NGX_OK) {
        /* A resend that moved nothing means the filters below are holding the
           buffer and will not take more of it now, so going round again would
           only ask a second time. A first send is never a stall, however
           little of it was taken. */
        if (resend && ctx->output == NGX_HTTP_BROTLI_OUTPUT_BUSY &&
            ngx_buf_size(ctx->out_buf) == outstanding) {
            r->connection->buffered |= NGX_HTTP_BROTLI_BUFFERED;
            return NGX_HTTP_BROTLI_STEP_AGAIN;
        }
        return NGX_HTTP_BROTLI_STEP_CONTINUE;
    }

    if (rc == NGX_AGAIN) {
        if (ctx->output == NGX_HTTP_BROTLI_OUTPUT_BUSY) {
            /* Can't continue compression, let the outer filter decide. */
            if (ctx->in != NULL) {
                r->connection->buffered |= NGX_HTTP_BROTLI_BUFFERED;
            }
            return NGX_HTTP_BROTLI_STEP_AGAIN;
        }
        /* Inner filter has given up, but we can continue processing. */
        return NGX_HTTP_BROTLI_STEP_CONTINUE;
    }

    return NGX_HTTP_BROTLI_STEP_FAILED;
}

/* Wraps whatever the encoder has produced in out_buf, and marks it last or
   flush if this block ends the stream or a meta-block. */
static ngx_http_brotli_step_e
ngx_http_brotli_filter_take_output(ngx_http_brotli_ctx_t *ctx)
{
    size_t              available_output;
    u_char             *out;
    ngx_http_request_t *r;

    r = ctx->request;

    available_output = 0;
    out = (u_char *) BrotliEncoderTakeOutput(ctx->encoder, &available_output);
    if (out == NULL || available_output == 0) {
        return NGX_HTTP_BROTLI_STEP_FAILED;
    }

    ctx->out_buf->start = out;
    ctx->out_buf->pos = out;
    ctx->out_buf->last = out + available_output;
    ctx->out_buf->end = out + available_output;
    ctx->out_buf->last_buf = 0;
    ctx->out_buf->flush = 0;

    if (ctx->end_of_input && BrotliEncoderIsFinished(ctx->encoder)) {
        ctx->out_buf->last_buf = 1;
        r->connection->buffered &= ~NGX_HTTP_BROTLI_BUFFERED;
    } else if (ctx->flush_pending) {
        ctx->out_buf->flush = 1;
        r->connection->buffered &= ~NGX_HTTP_BROTLI_BUFFERED;
    }

    ctx->flush_pending = 0;
    ctx->output = NGX_HTTP_BROTLI_OUTPUT_READY;

    ngx_log_debug2(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
        "brotli out: %p, size:%uz", ctx->out_buf, ngx_buf_size(ctx->out_buf));

    return NGX_HTTP_BROTLI_STEP_CONTINUE;
}

/* Runs the encoder with no new input, which is how it is made to release what
   it is already holding: FINISH once the input has ended, FLUSH before then.
   Which one to use follows from ctx->end_of_input rather than from an
   argument, because those are the only terms on which either is correct.
   Both leave output for the next turn of the loop to collect, which is why
   both mark the connection buffered. */
static ngx_http_brotli_step_e
ngx_http_brotli_filter_drain_encoder(ngx_http_brotli_ctx_t *ctx)
{
    size_t                 available_input;
    size_t                 available_output;
    BROTLI_BOOL            ok;
    BrotliEncoderOperation operation;

    if (ctx->end_of_input) {
        operation = BROTLI_OPERATION_FINISH;
    } else {
        operation = BROTLI_OPERATION_FLUSH;
    }

    available_input = 0;
    available_output = 0;

    ok = BrotliEncoderCompressStream(ctx->encoder, operation, &available_input,
        NULL, &available_output, NULL, NULL);

    ctx->request->connection->buffered |= NGX_HTTP_BROTLI_BUFFERED;

    if (!ok) {
        return NGX_HTTP_BROTLI_STEP_FAILED;
    }

    return NGX_HTTP_BROTLI_STEP_CONTINUE;
}

/* Advances the encoder now that it holds no output: finishes the stream,
   drains what a flush would release, or pushes the next input buffer in. */
static ngx_http_brotli_step_e
ngx_http_brotli_filter_feed_encoder(ngx_http_brotli_ctx_t *ctx)
{
    ngx_http_request_t    *r;
    size_t                 input_size;
    size_t                 available_input;
    size_t                 available_output;
    size_t                 consumed_input;
    const uint8_t         *next_input_byte;
    BROTLI_BOOL            ok;
    ngx_chain_t           *link;
    BrotliEncoderOperation operation;

    r = ctx->request;

    if (BrotliEncoderIsFinished(ctx->encoder)) {
        r->connection->buffered &= ~NGX_HTTP_BROTLI_BUFFERED;
        ngx_http_brotli_filter_close(ctx);
        return NGX_HTTP_BROTLI_STEP_DONE;
    }

    if (ctx->end_of_input) {
        /* Ask the encoder to dump the leftover. */
        return ngx_http_brotli_filter_drain_encoder(ctx);
    }

    if (ctx->in == NULL) {
        /* Nothing left to compress. If the caller is asking for output and the
           encoder is holding input that PROCESS will not emit until a block
           fills, ask for it now: without this a slowly-produced response
           stalls until 64 KB has accumulated. Costs a few percent of ratio,
           because each flush closes a meta-block early. */
        if (ctx->caller_wants_output && ctx->unflushed_input) {
            ctx->unflushed_input = 0;
            /* Mark the resulting buffer so the filters below push it out
               rather than holding it for more. */
            ctx->flush_pending = 1;
            return ngx_http_brotli_filter_drain_encoder(ctx);
        }
        return NGX_HTTP_BROTLI_STEP_DONE;
    }

    /* TODO: coalesce tiny inputs, if they are not last/flush. */
    input_size = ngx_buf_size(ctx->in->buf);
    /* An empty buffer carries nothing to compress, but one marked last or
       flush still has to reach the encoder to close the stream or the block.
       Anything else is dropped. */
    if (input_size == 0 && !ctx->in->buf->last_buf && !ctx->in->buf->flush) {
        link = ctx->in;
        ctx->in = ctx->in->next;
        ngx_free_chain(r->pool, link);
        return NGX_HTTP_BROTLI_STEP_CONTINUE;
    }

    available_input = input_size;
    next_input_byte = (const uint8_t *) ctx->in->buf->pos;
    available_output = 0;
    if (ctx->in->buf->last_buf) {
        operation = BROTLI_OPERATION_FINISH;
    } else if (ctx->in->buf->flush) {
        operation = BROTLI_OPERATION_FLUSH;
    } else {
        operation = BROTLI_OPERATION_PROCESS;
    }

    ok = BrotliEncoderCompressStream(ctx->encoder, operation, &available_input,
        &next_input_byte, &available_output, NULL, NULL);
    r->connection->buffered |= NGX_HTTP_BROTLI_BUFFERED;
    if (!ok) {
        return NGX_HTTP_BROTLI_STEP_FAILED;
    }

    consumed_input = input_size - available_input;
    ctx->in->buf->pos += consumed_input;

    if (operation == BROTLI_OPERATION_PROCESS) {
        if (consumed_input > 0) {
            ctx->unflushed_input = 1;
        }
    } else {
        ctx->unflushed_input = 0;
    }

    if (consumed_input == input_size) {
        if (ctx->in->buf->last_buf) {
            ctx->end_of_input = 1;
        } else if (ctx->in->buf->flush) {
            ctx->flush_pending = 1;
        }
        link = ctx->in;
        ctx->in = ctx->in->next;
        ngx_free_chain(r->pool, link);
        return NGX_HTTP_BROTLI_STEP_CONTINUE;
    }

    /* Should never happen, just to make sure we don't enter infinite loop. */
    if (consumed_input == 0) {
        return NGX_HTTP_BROTLI_STEP_FAILED;
    }

    /* Partially consumed: the rest of this buffer goes in next time round. */
    return NGX_HTTP_BROTLI_STEP_CONTINUE;
}

/* Everything that has to be settled before the encoder can run: committing
   headers the header filter held back, and deciding whether to go on holding
   input while the response size is still unknown. Both questions only arise
   before the steady state, and both are answered from one walk of the input
   chain.

   Returns NGX_HTTP_BROTLI_ACCEPT when there is nothing left to settle and the
   caller should carry on into the encoder loop. DEFER and REJECT both end the
   call with "*rc" holding what the body filter should return, and differ in
   whether the response may still be compressed later - including, under
   REJECT, the pass-through case, where this has already handed the held input
   to the next filter. */
static ngx_http_brotli_prepare_e
ngx_http_brotli_filter_prepare(ngx_http_brotli_ctx_t *ctx, ngx_int_t *rc)
{
    ngx_int_t               header_rc;
    ngx_http_request_t     *r;
    ngx_http_brotli_conf_t *conf;
    size_t                  pending;
    ngx_uint_t              complete;
    ngx_uint_t              urgent;
    ngx_uint_t              compress;
    ngx_chain_t            *link;

    /* The steady state: the headers are away and the encoder exists, so there
       is nothing to settle and the input chain need not be walked at all. */
    if (!ctx->headers_postponed && ctx->initialized) {
        /* Continue with compression, rc is ignored downstream. */
        return NGX_HTTP_BROTLI_ACCEPT;
    }

    r = ctx->request;

    /* Both decisions below ask the chain the same three questions, and neither
       touches it, so they share one walk. */
    pending = ngx_http_brotli_filter_pending_input(ctx->in, &complete, &urgent);

    /* Headers held back because the length was unknown. Decide as soon as the
       body answers the only question brotli_min_length asks - is it at least
       that big - which needs min_length bytes, not the whole response, so this
       costs no meaningful latency. A flush marker means something downstream
       is waiting, so decide immediately and compress, since the total is still
       unknown. */
    if (ctx->headers_postponed) {
        conf = ngx_http_get_module_loc_conf(r, ngx_http_brotli_filter_module);

        if (complete) {
            /* Whole response in hand, so the comparison is exact. */
            compress = (pending >= (size_t) conf->min_length);
        } else if (urgent || pending >= (size_t) conf->min_length) {
            compress = 1;
        } else {
            /* Too little to judge, and nothing waiting on it. */
            *rc = NGX_OK;
            return NGX_HTTP_BROTLI_DEFER;
        }

        header_rc = ngx_http_brotli_filter_send_headers(r, ctx, compress);
        if (header_rc == NGX_ERROR) {
            ngx_http_brotli_filter_close(ctx);
            *rc = NGX_ERROR;
            return NGX_HTTP_BROTLI_REJECT;
        }
        /* A special response was substituted below us; the body we hold is no
           longer the one being sent. Must be checked before handing anything
           on. */
        if (header_rc > NGX_OK) {
            *rc = header_rc;
            return NGX_HTTP_BROTLI_REJECT;
        }

        if (!compress) {
            /* Pass the held input through untouched; ctx->closed makes every
               later call a straight hand-off. */
            link = ctx->in;
            ctx->in = NULL;
            r->connection->buffered &= ~NGX_HTTP_BROTLI_BUFFERED;
            *rc = ngx_http_next_body_filter(r, link);
            return NGX_HTTP_BROTLI_REJECT;
        }
    }

    /* Choosing the encoder window costs memory that scales with the window, so
       when the response size is unknown it is worth waiting a moment to see if
       the whole thing turns up. Brotli emits nothing for non-flush input below
       its block size anyway, so holding it here costs no latency. */
    if (!ctx->initialized) {
        if (complete) {
            /* Whole response in hand: size the window from it exactly. */
            if (ctx->content_length < 0) {
                ctx->content_length = (off_t) pending;
            }
        } else if (!ctx->caller_wants_output && !urgent &&
                   ctx->content_length < 0 &&
                   pending < NGX_HTTP_BROTLI_DEFER_INPUT) {
            /* Still might be a small response. Wait for the rest rather than
               commit to the configured window. Either the caller asking for
               output, or "urgent" meaning a filter downstream is waiting on
               these bytes, stops the holding and starts the encoding. */
            ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                "brotli deferring encoder: pending:%uz", pending);
            *rc = NGX_OK;
            return NGX_HTTP_BROTLI_DEFER;
        }
    }

    /* Continue with compression, rc is ignored downstream. */
    return NGX_HTTP_BROTLI_ACCEPT;
}

/* Response body filtration (compression). */
static ngx_int_t
ngx_http_brotli_body_filter(ngx_http_request_t *r, ngx_chain_t *in)
{
    ngx_int_t              rc;
    ngx_http_brotli_ctx_t *ctx;
    ngx_http_brotli_step_e step;

    ctx = ngx_http_get_module_ctx(r, ngx_http_brotli_filter_module);

    ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
        "http brotli filter");

    if (ctx == NULL || ctx->closed || r->header_only) {
        return ngx_http_next_body_filter(r, in);
    }

    /* Recorded before "in" is folded into ctx->in, which is what makes the two
       distinguishable further down: ctx->in running dry says the filter has
       nothing left to compress, this says the caller brought nothing new. */
    ctx->caller_wants_output = (in == NULL);

    /* If more input is provided - append it to our input chain. */
    if (in) {
        if (ngx_chain_add_copy(r->pool, &ctx->in, in) != NGX_OK) {
            ngx_http_brotli_filter_close(ctx);
            return NGX_ERROR;
        }
        r->connection->buffered |= NGX_HTTP_BROTLI_BUFFERED;
    }

    if (ngx_http_brotli_filter_prepare(ctx, &rc) != NGX_HTTP_BROTLI_ACCEPT) {
        return rc;
    }

    if (ngx_http_brotli_filter_ensure_stream_initialized(ctx) != NGX_OK) {
        ngx_http_brotli_filter_close(ctx);
        return NGX_ERROR;
    }

    /* Main loop, one phase per turn:
       - output still outstanding - push it down, and do not touch the encoder
         until it has all been consumed
       - encoder has output - wrap it
       - otherwise - advance the encoder: finish, flush, or feed it input

       Each phase returns what to do next rather than returning from here
       itself, so the four ways out of this filter are all written once, in the
       switch below. */
    for (;;) {
        if (ctx->output != NGX_HTTP_BROTLI_OUTPUT_IDLE) {
            step = ngx_http_brotli_filter_send_output(ctx);
        } else if (BrotliEncoderHasMoreOutput(ctx->encoder)) {
            step = ngx_http_brotli_filter_take_output(ctx);
        } else {
            step = ngx_http_brotli_filter_feed_encoder(ctx);
        }

        switch (step) {
            case NGX_HTTP_BROTLI_STEP_CONTINUE:
                break;

            case NGX_HTTP_BROTLI_STEP_DONE:
                return NGX_OK;

            case NGX_HTTP_BROTLI_STEP_AGAIN:
                return NGX_AGAIN;

            default:
                ngx_http_brotli_filter_close(ctx);
                return NGX_ERROR;
        }
    }

    /* Unreachable: the switch above either returns or goes round again. */
}

/* Totals the unconsumed input, reporting whether the chain closes the
   response ("complete") and whether anything in it demands to be pushed out
   now ("urgent"). */
static size_t
ngx_http_brotli_filter_pending_input(ngx_chain_t *in, ngx_uint_t *complete,
    ngx_uint_t *urgent)
{
    size_t total = 0;

    *complete = 0;
    *urgent = 0;

    for (; in; in = in->next) {
        total += ngx_buf_size(in->buf);
        if (in->buf->last_buf) {
            *complete = 1;
        }
        if (in->buf->flush) {
            *urgent = 1;
        }
    }

    return total;
}

/* Initializes encoder, output chain and buffer, if necessary. Returns NGX_OK
   if encoder is successfully initialized (have been already initialized),
   and requires objects are allocated. Returns NGX_ERROR otherwise. */
static ngx_int_t
ngx_http_brotli_filter_ensure_stream_initialized(ngx_http_brotli_ctx_t *ctx)
{
    ngx_http_request_t     *r;
    ngx_http_brotli_conf_t *conf;
    ngx_pool_cleanup_t     *cln;
    BROTLI_BOOL             ok;
    size_t                  wbits;

    if (ctx->initialized) {
        return NGX_OK;
    }

    r = ctx->request;
    conf = ngx_http_get_module_loc_conf(r, ngx_http_brotli_filter_module);

    /* Tune lg_win, if size is known. */
    if (ctx->content_length > 0) {
        wbits = BROTLI_MIN_WINDOW_BITS;
        while ((wbits < conf->lg_win) && (ctx->content_length > (1 << wbits))) {
            wbits++;
        }
    } else {
        wbits = conf->lg_win;
    }

    /* Encoder memory is not owned by the pool, so arrange for it to be released
       even if the request is aborted mid-stream. Registered before the encoder
       exists, so that a failure here can not strand an allocated instance. */
    cln = ngx_pool_cleanup_add(r->pool, 0);
    if (cln == NULL) {
        return NGX_ERROR;
    }
    cln->handler = ngx_http_brotli_filter_cleanup;
    cln->data = ctx;

    ctx->encoder = BrotliEncoderCreateInstance(ngx_http_brotli_filter_alloc,
        ngx_http_brotli_filter_free, r->pool);
    if (ctx->encoder == NULL) {
        ngx_log_error(NGX_LOG_ALERT, r->connection->log, 0,
            "OOM / BrotliEncoderCreateInstance");
        return NGX_ERROR;
    }

    ok = BrotliEncoderSetParameter(ctx->encoder, BROTLI_PARAM_QUALITY,
        (uint32_t) conf->quality);
    if (!ok) {
        ngx_log_error(NGX_LOG_ALERT, r->connection->log, 0,
            "BrotliEncoderSetParameter(QUALITY, %uD) failed",
            (uint32_t) conf->quality);
        return NGX_ERROR;
    }

    ok = BrotliEncoderSetParameter(ctx->encoder, BROTLI_PARAM_LGWIN,
        (uint32_t) wbits);
    if (!ok) {
        ngx_log_error(NGX_LOG_ALERT, r->connection->log, 0,
            "BrotliEncoderSetParameter(LGWIN, %uD) failed", (uint32_t) wbits);
        return NGX_ERROR;
    }

    ctx->out_buf = ngx_calloc_buf(r->pool);
    if (ctx->out_buf == NULL) {
        return NGX_ERROR;
    }
    ctx->out_buf->temporary = 1;

    ctx->out_chain = ngx_alloc_chain_link(r->pool);
    if (ctx->out_chain == NULL) {
        return NGX_ERROR;
    }
    ctx->out_chain->buf = ctx->out_buf;
    ctx->out_chain->next = NULL;

    /* Last, so that the flag means what it says: the encoder exists and both
       output objects are allocated. Every failure above returns NGX_ERROR
       with it still clear, and the caller closes the stream on that, so the
       half-built state is never seen again - ctx->closed makes every later
       call a straight hand-off. */
    ctx->initialized = 1;

    ngx_log_debug2(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
        "brotli encoder initialized: lvl:%i win:%d", conf->quality,
        (1 << wbits));

    return NGX_OK;
}

/* The encoder allocates from the heap, not from the request pool. Brotli makes
   frequent short-lived sub-page allocations (e.g. one per meta-block), and
   ngx_pfree only reclaims "large" blocks, so pool-backed ones would pile up
   until the request ends. "opaque" is still the pool, but only for logging. */
static void *
ngx_http_brotli_filter_alloc(void *opaque, size_t size)
{
    ngx_pool_t *pool = opaque;
    void       *p;

    p = ngx_alloc(size, pool->log);

    ngx_log_debug2(NGX_LOG_DEBUG_HTTP, pool->log, 0,
        "brotli alloc: %p, size:%uz", p, size);

    return p;
}

static void
ngx_http_brotli_filter_free(void *opaque, void *address)
{
#if (NGX_DEBUG)
    ngx_pool_t *pool = opaque;

    ngx_log_debug1(NGX_LOG_DEBUG_HTTP, pool->log, 0, "brotli free: %p",
        address);
#endif

    ngx_free(address);
}

/* Releases the encoder if the request is terminated before compression is
   finished, i.e. when ngx_http_brotli_filter_close is never reached. */
static void
ngx_http_brotli_filter_cleanup(void *data)
{
    ngx_http_brotli_ctx_t *ctx = data;

    /* Normally the encoder is already gone: ngx_http_brotli_filter_close resets
       the field. This is the abort path. */
    if (ctx->encoder != NULL) {
        BrotliEncoderDestroyInstance(ctx->encoder);
        ctx->encoder = NULL;
    }
}

/* Marks instance as closed and performs cleanup. */
static void
ngx_http_brotli_filter_close(ngx_http_brotli_ctx_t *ctx)
{
    ctx->closed = 1;
    if (ctx->encoder) {
        BrotliEncoderDestroyInstance(ctx->encoder);
        ctx->encoder = NULL;
    }
    if (ctx->out_chain) {
        /* Worth doing: ngx_free_chain returns the link to the pool's chain
           free list, where ngx_alloc_chain_link will pick it up again. */
        ngx_free_chain(ctx->request->pool, ctx->out_chain);
        ctx->out_chain = NULL;
    }
    /* out_buf is pool memory and small, so there is nothing to hand back -
       ngx_pfree only reclaims blocks large enough to have been malloc'd on
       their own, and would just walk pool->large to say so. Dropping the
       pointer is the whole of the cleanup; the buffer dies with the pool. */
    ctx->out_buf = NULL;
}

static void *
ngx_http_brotli_create_conf(ngx_conf_t *cf)
{
    ngx_http_brotli_conf_t *conf;

    conf = ngx_pcalloc(cf->pool, sizeof(ngx_http_brotli_conf_t));
    if (conf == NULL) {
        return NULL;
    }

    /* ngx_pcalloc fills result with zeros ->
         conf->types = { NULL };
         conf->types_keys = NULL; */

    conf->enable = NGX_CONF_UNSET;

    conf->quality = NGX_CONF_UNSET;
    conf->lg_win = NGX_CONF_UNSET_SIZE;
    conf->min_length = NGX_CONF_UNSET;

    return conf;
}

static char *
ngx_http_brotli_merge_conf(ngx_conf_t *cf, void *parent, void *child)
{
    ngx_http_brotli_conf_t *prev = parent;
    ngx_http_brotli_conf_t *conf = child;
    char                   *rc;

    ngx_conf_merge_value(conf->enable, prev->enable, 0);

    /* 4 rather than the 6 this module used to default to. Measured against
       script/corpus with a release build: the corpus compresses to 228,062
       bytes at quality 4 against 212,327 at 6 - 7.4% larger - for 10.0 ms of
       encoder time against 18.6, a little over half. Unlike the lg_win change
       below this buys nothing in memory: peak live encoder bytes are flat
       across the range, 1.94 MB at 4 against 1.93 MB at 6. It trades ratio
       for CPU, and only that.

       Worth knowing before moving it again: 4 is not the knee of the curve.
       Quality 5 costs 0.8% in ratio against 6 and saves 20% of the time,
       where the further step down to 4 costs another 6.6% in ratio to save
       another 33%. Most of the CPU is available at 5 for almost none of the
       bytes; going to 4 is a deliberate choice to spend bytes on latency. */
    ngx_conf_merge_value(conf->quality, prev->quality, 4);
    /* 16 bits (64k) rather than the 19 (512k) this module used to default to.
       Brotli picks a much cheaper hasher at lg_win <= 16, so 64k is the largest
       window before encoder memory jumps: measured against script/corpus, a
       streamed response peaks near 0.99 MB here against 4.0 MB at 512k. What
       that costs in ratio depends on the content, since it only bites where a
       match would have reached back beyond 64 KB - +0.04% on CSS, +0.4% on JS,
       +1.0% on minified JS, +2.2% on prose, +2.7% on HTML, +1.6% over the
       corpus as a whole. Those are at quality 6, which was the default when
       they were taken; at the current default of 4 the same change costs
       +1.1% over the corpus and +2.4% at its worst, a lower quality having
       fewer long-range matches to give up. 32k and 16k cost the same memory
       as 64k while compressing worse, so 64k is the useful floor rather than
       the smallest possible value. */
    ngx_conf_merge_size_value(conf->lg_win, prev->lg_win, 16);
    /* 20 is the gzip module's default, but Brotli is a far worse deal on tiny
       responses: an encoder instance costs ~560 KB regardless of how little it
       is asked to compress, and measured against realistic JSON the compressed
       body only starts beating the original around 96 bytes - closer to 128
       once the "Content-Encoding" header is paid for. 256 clears that with
       room to spare. */
    ngx_conf_merge_value(conf->min_length, prev->min_length, 256);

    rc = ngx_http_merge_types(cf, &conf->types_keys, &conf->types,
        &prev->types_keys, &prev->types, ngx_http_html_default_types);
    if (rc != NGX_CONF_OK) {
        return NGX_CONF_ERROR;
    }

    return NGX_CONF_OK;
}

/* Prepend to filter chain. */
static ngx_int_t
ngx_http_brotli_filter_init(ngx_conf_t *cf)
{
    ngx_http_next_header_filter = ngx_http_top_header_filter;
    ngx_http_top_header_filter = ngx_http_brotli_header_filter;

    ngx_http_next_body_filter = ngx_http_top_body_filter;
    ngx_http_top_body_filter = ngx_http_brotli_body_filter;

    return NGX_OK;
}

/* Translate "window size" to window bits (log2), and check bounds. */
static char *
ngx_http_brotli_parse_wbits(ngx_conf_t *cf, void *post, void *data)
{
    size_t *parameter = data;
    size_t  bits;
    size_t  wsize;

    for (bits = BROTLI_MIN_WINDOW_BITS; bits <= BROTLI_MAX_WINDOW_BITS;
        bits++) {
        wsize = 1u << bits;
        if (*parameter == wsize) {
            *parameter = bits;
            return NGX_CONF_OK;
        }
    }

    return "must be 1k, 2k, 4k, 8k, 16k, 32k, 64k, 128k, 256k, 512k, 1m, 2m, "
           "4m, 8m or 16m";
}
