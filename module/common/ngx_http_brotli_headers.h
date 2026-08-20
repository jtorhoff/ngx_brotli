/*
 * Copyright (C) Google Inc.
 */

/* HTTP header handling shared by the filter and the static module: reading
 * Accept-Encoding to decide whether a client will take Brotli, and labelling
 * a response that carries it.
 *
 * Both modules have to answer the same questions, and previously each carried
 * its own copy of the answers. The copies drifted: the static module's
 * Accept-Encoding parser had lost the length guard, and a fix to one did not
 * reach the other.
 *
 * The functions are static and live in the header rather than in a source
 * file of their own, so that neither module's "config" has to grow a second
 * entry in ngx_module_srcs. Each module is a separate translation unit, and a
 * separate shared object in a dynamic build, so each simply gets its own
 * copy - with no duplicate symbols and no build changes.
 */

#ifndef NGX_HTTP_BROTLI_HEADERS_H_INCLUDED_
#define NGX_HTTP_BROTLI_HEADERS_H_INCLUDED_

#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>

/* Optional whitespace, as RFC 9110 defines it for list separators. */
#define ngx_http_brotli_is_optional_whitespace(c) ((c) == ' ' || (c) == '\t')

static u_char *
ngx_http_brotli_skip_optional_whitespace(u_char *cursor, u_char *end)
{
    while (cursor < end && ngx_http_brotli_is_optional_whitespace(*cursor)) {
        cursor++;
    }
    return cursor;
}

/* Given the text following an encoding token, reports whether its parameters
   set the quality to zero, i.e. ";q=0", ";q=0.0", ";q=0.00" or ";q=0.000".
   Anything else - a different weight, an unrecognised parameter, no
   parameters at all - counts as acceptable. */
static ngx_uint_t
ngx_http_brotli_is_zero_weighted(u_char *cursor, u_char *end)
{
    ngx_uint_t digits;

    cursor = ngx_http_brotli_skip_optional_whitespace(cursor, end);
    if (cursor == end || *cursor++ != ';') {
        return 0;
    }
    cursor = ngx_http_brotli_skip_optional_whitespace(cursor, end);
    if (cursor == end || (*cursor != 'q' && *cursor != 'Q')) {
        return 0;
    }
    cursor++;
    cursor = ngx_http_brotli_skip_optional_whitespace(cursor, end);
    if (cursor == end || *cursor++ != '=') {
        return 0;
    }
    cursor = ngx_http_brotli_skip_optional_whitespace(cursor, end);
    /* Any weight not starting with "0" is non-zero. */
    if (cursor == end || *cursor++ != '0') {
        return 0;
    }
    /* "q=0" with nothing after it, or with no fraction. */
    if (cursor == end || *cursor != '.') {
        return 1;
    }
    cursor++;
    /* RFC 9110 permits at most three digits after the point. */
    for (digits = 0; digits < 3; digits++) {
        if (cursor == end) {
            return 1; /* "q=0." */
        }
        if (*cursor < '0' || *cursor > '9') {
            return 1;
        }
        if (*cursor > '0') {
            return 0; /* a non-zero digit */
        }
        cursor++;
    }
    return 1;
}

/* Decides whether the client will accept Brotli.

   "br" is taken whenever it appears as a token in Accept-Encoding, whatever
   weight it carries, with the single exception of an explicit zero - which
   RFC 9110 defines as "not acceptable". A wildcard ("*") is taken on the
   same terms, since RFC 9110 says it covers any coding not explicitly
   listed. Relative weights are deliberately ignored, so "gzip;q=1.0,
   br;q=0.1" still selects Brotli. Choosing Brotli then suppresses gzip for
   the request, in ngx_http_brotli_claim_request. */
static ngx_int_t
ngx_http_brotli_check_accept_encoding(ngx_http_request_t *r)
{
    ngx_table_elt_t *accept_encoding_entry;
    ngx_str_t       *accept_encoding;
    u_char          *start;
    u_char          *cursor;
    u_char          *end;
    u_char           before;
    u_char           after;
    u_char          *token;
    size_t           token_len;
    ngx_uint_t       pass;

    accept_encoding_entry = r->headers_in.accept_encoding;
    if (accept_encoding_entry == NULL) {
        return NGX_DECLINED;
    }
    accept_encoding = &accept_encoding_entry->value;

    start = accept_encoding->data;
    end = start + accept_encoding->len;

    /* Pass 0 looks for "br", pass 1 for the wildcard - either is enough to
       accept. */
    for (pass = 0; pass < 2; pass++) {
        if (pass == 0) {
            token = (u_char *) "br";
            token_len = 2;
        } else {
            token = (u_char *) "*";
            token_len = 1;
        }

        if (accept_encoding->len < token_len) {
            continue;
        }

        cursor = start;

        for (;;) {
            /* Bounded search, so a header without a terminating NUL can not be
               run off the end of. */
            cursor = ngx_strlcasestrn(cursor, end, token, token_len - 1);
            if (cursor == NULL) {
                break;
            }

            /* The token has to stand alone; reject a match inside a longer one
               such as "brotli" or "x-br". A match at either edge of the header
               is treated as if a separator sat beside it. */
            if (cursor == start) {
                before = ',';
            } else {
                before = cursor[-1];
            }
            cursor += token_len;
            if (cursor == end) {
                after = ',';
            } else {
                after = *cursor;
            }

            if (before != ',' &&
                !ngx_http_brotli_is_optional_whitespace(before)) {
                continue;
            }
            if (after != ',' && after != ';' &&
                !ngx_http_brotli_is_optional_whitespace(after)) {
                continue;
            }

            if (!ngx_http_brotli_is_zero_weighted(cursor, end)) {
                return NGX_OK;
            }
        }
    }

    return NGX_DECLINED;
}

/* Decides whether this request should be served Brotli at all, and claims it
   if so. Shared by both modules, which had identical copies of this.

   Returns NGX_OK only for a main request from a client that named "br" and
   speaks at least HTTP/1.1. The version test mirrors gzip_http_version, whose
   default is also 1.1: some HTTP/1.0 clients and intermediaries mishandle a
   compressed response, and gzip has always declined them.

   Claiming the request means suppressing gzip for it, so that a client
   advertising both gets Brotli. That happens only once every test has passed:
   when Brotli declines, gzip is left free to make its own decision, including
   applying its own version and proxy rules. */
static ngx_int_t
ngx_http_brotli_claim_request(ngx_http_request_t *r)
{
    if (r != r->main) {
        return NGX_DECLINED;
    }
    if (r->http_version < NGX_HTTP_VERSION_11) {
        return NGX_DECLINED;
    }
    if (ngx_http_brotli_check_accept_encoding(r) != NGX_OK) {
        return NGX_DECLINED;
    }
    r->gzip_tested = 1;
    r->gzip_ok = 0;
    return NGX_OK;
}

/* Labels the response as Brotli-encoded. Both modules set exactly this pair of
   headers and previously each built the list entry itself, so the version
   guard below had to be kept in step by hand in two places.

   Returns NGX_ERROR only if the header list could not be grown. The callers
   report that differently - the filter as NGX_ERROR, the static handler as a
   500 - so the mapping is left to them. */
static ngx_int_t
ngx_http_brotli_set_content_encoding(ngx_http_request_t *r)
{
    ngx_table_elt_t *content_encoding_entry;

    content_encoding_entry = ngx_list_push(&r->headers_out.headers);
    if (content_encoding_entry == NULL) {
        return NGX_ERROR;
    }

    content_encoding_entry->hash = 1;
#if nginx_version >= 1023000
    /* Since 1.23.0 the headers_out entries are linked, so a pushed entry has
       to terminate its own list. */
    content_encoding_entry->next = NULL;
#endif
    ngx_str_set(&content_encoding_entry->key, "Content-Encoding");
    ngx_str_set(&content_encoding_entry->value, "br");
    r->headers_out.content_encoding = content_encoding_entry;

    return NGX_OK;
}

#endif /* NGX_HTTP_BROTLI_HEADERS_H_INCLUDED_ */
