/*
 * Copyright (C) Igor Sysoev
 * Copyright (C) Nginx, Inc.
 * Copyright (C) Google Inc.
 */

#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>

#include "../common/ngx_http_brotli_headers.h"

#define NGX_HTTP_BROTLI_STATIC_OFF 0
#define NGX_HTTP_BROTLI_STATIC_ON 1
#define NGX_HTTP_BROTLI_STATIC_ALWAYS 2

typedef struct {
    ngx_uint_t enable;
} ngx_http_brotli_static_conf_t;

static ngx_conf_enum_t ngx_http_brotli_static[] = {
    {ngx_string("off"), NGX_HTTP_BROTLI_STATIC_OFF},
    {ngx_string("on"), NGX_HTTP_BROTLI_STATIC_ON},
    {ngx_string("always"), NGX_HTTP_BROTLI_STATIC_ALWAYS},
    {ngx_null_string, 0}};

static ngx_int_t ngx_http_brotli_static_handler(ngx_http_request_t *r);
static void *ngx_http_brotli_static_create_conf(ngx_conf_t *conf_ctx);
static char *ngx_http_brotli_static_merge_conf(ngx_conf_t *conf_ctx,
    void *parent, void *child);
static ngx_int_t ngx_http_brotli_static_init(ngx_conf_t *conf_ctx);

static ngx_command_t ngx_http_brotli_static_commands[] = {
    {ngx_string("brotli_static"),
        NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_LOC_CONF |
            NGX_CONF_TAKE1,
        ngx_conf_set_enum_slot, NGX_HTTP_LOC_CONF_OFFSET,
        offsetof(ngx_http_brotli_static_conf_t, enable),
        &ngx_http_brotli_static},
    ngx_null_command};

static ngx_http_module_t ngx_http_brotli_static_module_ctx = {
    NULL,                        /* preconfiguration */
    ngx_http_brotli_static_init, /* postconfiguration */

    NULL, /* create main configuration */
    NULL, /* init main configuration */

    NULL, /* create server configuration */
    NULL, /* merge server configuration */

    ngx_http_brotli_static_create_conf, /* create location configuration */
    ngx_http_brotli_static_merge_conf   /* merge location configuration */
};

ngx_module_t ngx_http_brotli_static_module = {NGX_MODULE_V1,
    &ngx_http_brotli_static_module_ctx, /* module context */
    ngx_http_brotli_static_commands,    /* module directives */
    NGX_HTTP_MODULE,                    /* module type */
    NULL,                               /* init master */
    NULL,                               /* init module */
    NULL,                               /* init process */
    NULL,                               /* init thread */
    NULL,                               /* exit thread */
    NULL,                               /* exit process */
    NULL,                               /* exit master */
    NGX_MODULE_V1_PADDING};

static /* const */ u_char ngx_http_brotli_static_suffix[] = ".br";
static const size_t       ngx_http_brotli_static_suffix_len = 3;

static ngx_int_t
ngx_http_brotli_static_handler(ngx_http_request_t *r)
{
    ngx_http_brotli_static_conf_t *brotli_cfg;
    ngx_int_t                      rc;
    u_char                        *last;
    ngx_str_t                      path;
    size_t                         root;
    ngx_log_t                     *log;
    ngx_http_core_loc_conf_t      *core_loc_cfg;
    ngx_open_file_info_t           file_info;
    ngx_buf_t                     *buf;
    ngx_chain_t                    out;

    /* Only GET and HEAD requests are supported. */
    if (!(r->method & (NGX_HTTP_GET | NGX_HTTP_HEAD))) {
        return NGX_DECLINED;
    }

    /* Only files are supported. */
    if (r->uri.data[r->uri.len - 1] == '/') {
        return NGX_DECLINED;
    }

    /* Get configuration and check if module is disabled. */
    brotli_cfg = ngx_http_get_module_loc_conf(r, ngx_http_brotli_static_module);
    if (brotli_cfg->enable == NGX_HTTP_BROTLI_STATIC_OFF) {
        return NGX_DECLINED;
    }

    if (brotli_cfg->enable == NGX_HTTP_BROTLI_STATIC_ALWAYS) {
        /* Ignore request properties (e.g. Accept-Encoding). */
    } else {
        /* NGX_HTTP_BROTLI_STATIC_ON */
        r->gzip_vary = 1;
        rc = ngx_http_brotli_claim_request(r);
        if (rc != NGX_OK) {
            return NGX_DECLINED;
        }
    }

    /* Get path and append the suffix. ngx_http_map_uri_to_path leaves path.len
       holding the size of the buffer it allocated - the path, the room asked
       for here, and a terminating zero - not the length of the string in it.
       So the length has to be computed from the write pointer, as nginx's own
       gzip_static does; adding the suffix length to what it returned would
       overshoot the string by four and the allocation itself by three. */
    last = ngx_http_map_uri_to_path(r, &path, &root,
        ngx_http_brotli_static_suffix_len);
    if (last == NULL) {
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }
    ngx_cpystrn(last, ngx_http_brotli_static_suffix,
        ngx_http_brotli_static_suffix_len + 1);
    path.len = last + ngx_http_brotli_static_suffix_len - path.data;

    log = r->connection->log;
    ngx_log_debug1(NGX_LOG_DEBUG_HTTP, log, 0, "http filename: \"%s\"",
        path.data);

    /* Prepare to read the file. */
    core_loc_cfg = ngx_http_get_module_loc_conf(r, ngx_http_core_module);
    ngx_memzero(&file_info, sizeof(ngx_open_file_info_t));
    file_info.read_ahead = core_loc_cfg->read_ahead;
    file_info.directio = core_loc_cfg->directio;
    file_info.valid = core_loc_cfg->open_file_cache_valid;
    file_info.min_uses = core_loc_cfg->open_file_cache_min_uses;
    file_info.errors = core_loc_cfg->open_file_cache_errors;
    file_info.events = core_loc_cfg->open_file_cache_events;
    rc = ngx_http_set_disable_symlinks(r, core_loc_cfg, &path, &file_info);
    if (rc != NGX_OK) {
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }

    /* Try to fetch file and process errors. */
    rc = ngx_open_cached_file(core_loc_cfg->open_file_cache, &path, &file_info,
        r->pool);
    if (rc != NGX_OK) {
        ngx_uint_t level;
        switch (file_info.err) {
            case 0:
                return NGX_HTTP_INTERNAL_SERVER_ERROR;

            case NGX_ENOENT:
            case NGX_ENOTDIR:
            case NGX_ENAMETOOLONG:
                return NGX_DECLINED;

#if (NGX_HAVE_OPENAT)
            case NGX_EMLINK:
            case NGX_ELOOP:
#endif
            case NGX_EACCES:
                level = NGX_LOG_ERR;
                break;

            default:
                level = NGX_LOG_CRIT;
                break;
        }
        ngx_log_error(level, log, file_info.err, "%s \"%s\" failed",
            file_info.failed, path.data);
        return NGX_DECLINED;
    }

    /* So far so good. */
    ngx_log_debug1(NGX_LOG_DEBUG_HTTP, log, 0, "http static fd: %d",
        file_info.fd);

    /* Only files are supported. */
    if (file_info.is_dir) {
        ngx_log_debug0(NGX_LOG_DEBUG_HTTP, log, 0, "http dir");
        return NGX_DECLINED;
    }
#if !(NGX_WIN32)
    if (!file_info.is_file) {
        ngx_log_error(NGX_LOG_CRIT, log, 0, "\"%s\" is not a regular file",
            path.data);
        return NGX_HTTP_NOT_FOUND;
    }
#endif

    /* Prepare request push the body. */
    r->root_tested = !r->error_page;
    rc = ngx_http_discard_request_body(r);
    if (rc != NGX_OK) {
        return rc;
    }
    log->action = "sending response to client";
    r->headers_out.status = NGX_HTTP_OK;
    r->headers_out.content_length_n = file_info.size;
    r->headers_out.last_modified_time = file_info.mtime;
    rc = ngx_http_set_etag(r);
    if (rc != NGX_OK) {
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }
    rc = ngx_http_set_content_type(r);
    if (rc != NGX_OK) {
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }

    /* Set "Content-Encoding" header. */
    if (ngx_http_brotli_set_content_encoding(r) != NGX_OK) {
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }

    /* Setup response body. */
    buf = ngx_pcalloc(r->pool, sizeof(ngx_buf_t));
    if (buf == NULL) {
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }
    buf->file = ngx_pcalloc(r->pool, sizeof(ngx_file_t));
    if (buf->file == NULL) {
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }
    buf->file_pos = 0;
    buf->file_last = file_info.size;
    if (buf->file_last) {
        buf->in_file = 1;
    } else {
        buf->in_file = 0;
    }
    if (r == r->main) {
        buf->last_buf = 1;
    } else {
        buf->last_buf = 0;
    }
    buf->last_in_chain = 1;
    buf->file->fd = file_info.fd;
    buf->file->name = path;
    buf->file->log = log;
    buf->file->directio = file_info.is_directio;
    out.buf = buf;
    out.next = NULL;

    /* Push the response header. */
    rc = ngx_http_send_header(r);
    if (rc == NGX_ERROR || rc > NGX_OK || r->header_only) {
        return rc;
    }

    /* Push the response body. */
    return ngx_http_output_filter(r, &out);
}

static void *
ngx_http_brotli_static_create_conf(ngx_conf_t *conf_ctx)
{
    ngx_http_brotli_static_conf_t *brotli_cfg;

    brotli_cfg =
        ngx_palloc(conf_ctx->pool, sizeof(ngx_http_brotli_static_conf_t));
    if (brotli_cfg == NULL) {
        return NULL;
    }

    brotli_cfg->enable = NGX_CONF_UNSET_UINT;

    return brotli_cfg;
}

static char *
ngx_http_brotli_static_merge_conf(ngx_conf_t *conf_ctx, void *parent,
    void *child)
{
    ngx_http_brotli_static_conf_t *prev_cfg = parent;
    ngx_http_brotli_static_conf_t *brotli_cfg = child;

    ngx_conf_merge_uint_value(brotli_cfg->enable, prev_cfg->enable,
        NGX_HTTP_BROTLI_STATIC_OFF);

    return NGX_CONF_OK;
}

static ngx_int_t
ngx_http_brotli_static_init(ngx_conf_t *conf_ctx)
{
    ngx_http_core_main_conf_t *core_main_cfg;
    ngx_http_handler_pt       *handler_slot;

    core_main_cfg =
        ngx_http_conf_get_module_main_conf(conf_ctx, ngx_http_core_module);

    handler_slot =
        ngx_array_push(&core_main_cfg->phases[NGX_HTTP_CONTENT_PHASE].handlers);
    if (handler_slot == NULL) {
        return NGX_ERROR;
    }

    *handler_slot = ngx_http_brotli_static_handler;

    return NGX_OK;
}
