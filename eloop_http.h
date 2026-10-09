#ifndef ELOOP_HTTP_H_
#define ELOOP_HTTP_H_

#include "eloop_ssl.h"
#include "bufio.h"

#include <stdint.h>
#include <setjmp.h>

#ifndef ELOOPHTTPDEF
#define ELOOPHTTPDEF
#endif

enum http_err {
    HTTP_ERR_NONE,
    HTTP_ERR_TRANSPORT,
    HTTP_ERR_PARSE,
    HTTP_ERR_PARSE_LIMIT
};

enum {
    HTTP_TRANSPORT_F_NONE = 0x00,
    HTTP_TRANSPORT_F_CHUNKED_ENCODING = 0x01,
    /* TODO: gzip encoding and etc, via user callbacks (interface is unclear) */
    HTTP_TRANSPORT_F_CONNECTION_KEEP_ALIVE = 0x02 /* if not set connection is close */
};

struct http_transport {
    void *io;
    jmp_buf *onerror;
    int flags;
    struct bufio bio;
};

struct http_resp {
    int status_code;
    int64_t content_length;
};

struct http_hdr {
    const char *header;
    char *dest;
    size_t cap;
    int _matched;
};

typedef struct http_transport http_transport;
typedef void (*http_routine)(http_transport *);

ELOOPHTTPDEF int http_connect(http_transport *, enet_socket *, jmp_buf *);
ELOOPHTTPDEF int https_connect(http_transport *, essl_ctx_t, enet_socket *, jmp_buf *);

ELOOPHTTPDEF void http_send_req(http_transport *, const char *method, const char *path);
ELOOPHTTPDEF void http_send_header(http_transport *, const char *name, const char *value);
ELOOPHTTPDEF void http_end_request(http_transport *);

ELOOPHTTPDEF void http_recv_resp(http_transport *, struct http_resp *, struct http_hdr *hdrs);

ELOOPHTTPDEF void http_recv(http_transport *, void *, int);
ELOOPHTTPDEF void http_send(http_transport *, void const *, int);

#endif /* ELOOP_HTTP_H_ */

#ifdef ELOOP_HTTP_IMPLEMENTATION

#include <stdarg.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

#define HTTP__ARRAY_LEN(xs) (sizeof (xs) / sizeof (xs)[0])

static void http__write(http_transport *http, void const *buf, int len)
{
    bio_send(&http->bio, buf, len);
}

static void http__read(http_transport *http, void *buf, int len)
{
    bio_read(&http->bio, buf, len);
}

static void http__write_cstr(http_transport *http, const char *cstr)
{
    http__write(http, cstr, strlen(cstr));
}

static void http__cleanup_https(void *transport)
{
    http_transport *http = transport;
    essl_t *ssl = http->io;
    essl_free(ssl);
    free(ssl);
}

static int http__sock_read(void *hnd, void *buf, int len)
{
    int ret;
    http_transport *http = hnd;
    enet_socket *sock = http->io;
    ret = esock_recv(sock, buf, len);
    if (ret < 0) longjmp(*http->onerror, HTTP_ERR_TRANSPORT);
    return ret;
}

static void http__sock_write(void *hnd, void const *buf, int len)
{
    int ret;
    http_transport *http = hnd;
    enet_socket *sock = http->io;
    assert(http->onerror);
    ret = esock_send_ex(sock, buf, len, ENET_EX_F_WAITALL);
    if (ret < 0) longjmp(*http->onerror, HTTP_ERR_TRANSPORT);
}

static int http__ssl_read(void *hnd, void *buf, int len)
{
    int ret;
    http_transport *http = hnd;
    essl_t *ssl = http->io;
    assert(http->onerror);
    ret = essl_recv(ssl, buf, len);
    if (ret <= 0) longjmp(*http->onerror, HTTP_ERR_TRANSPORT);
    return ret;
}

static void http__ssl_write(void *hnd, void const *buf, int len)
{
    int ret, off;
    http_transport *http = hnd;
    essl_t *ssl = http->io;
    assert(http->onerror);
    for (off = 0; off < len; ) {
        ret = essl_send(ssl, (char *)buf + off, len - off);
        if (ret < 0) longjmp(*http->onerror, HTTP_ERR_TRANSPORT);
        off += ret;
    }
}

ELOOPHTTPDEF int http_connect(http_transport *http, enet_socket *sock, jmp_buf *onerror)
{
    memset(http, 0, sizeof *http);

    http->onerror = onerror;
    http->io = sock;
    bio_init(&http->bio, http, http__sock_read, http__sock_write);
    esock_setup_transport(sock, http, NULL);
    return 0;
}

ELOOPHTTPDEF int https_connect(http_transport *http, essl_ctx_t ctx, enet_socket *sock, jmp_buf *onerror)
{
    essl_t *ssl;
    ssl = malloc(sizeof *ssl);

    memset(http, 0, sizeof *http);

    if (essl_connect(ctx, ssl, sock) < 0) {
        free(ssl);
        return -1;
    }

    http->onerror = onerror;
    http->io = ssl;
    bio_init(&http->bio, http, http__ssl_read, http__ssl_write);
    esock_setup_transport(sock, http, http__cleanup_https);
    return 0;
}

ELOOPHTTPDEF void http_send_req(http_transport *http, const char *method, const char *path)
{
    http__write_cstr(http, method);
    http__write_cstr(http, " ");
    http__write_cstr(http, path);
    http__write_cstr(http, " HTTP/1.1\r\n");
}

ELOOPHTTPDEF void http_send_header(http_transport *http, const char *name, const char *value)
{
    http__write_cstr(http, name);
    http__write_cstr(http, ": ");
    http__write_cstr(http, value);
    http__write_cstr(http, "\r\n");
}

ELOOPHTTPDEF void http_end_request(http_transport *http)
{
    http__write_cstr(http, "\r\n");
    bio_flush(&http->bio);
}

struct http__header_value_pattern {
    const char *value;
    int bit;
};

static const struct http__header_value_pattern http__connection_patterns[] = {
    {"close", 0},
    {"keep-alive", HTTP_TRANSPORT_F_CONNECTION_KEEP_ALIVE},
};

static const struct http__header_value_pattern http__transfer_encoding_patterns[] = {
    {"chunked", HTTP_TRANSPORT_F_CHUNKED_ENCODING},
};

struct http__parser {
    http_transport *http;
    struct http_resp *resp;
    struct http_hdr *hdrs;
    void (*header_value_parse)(struct http__parser *);
    void (*header_value_parse_done)(struct http__parser *);

    union {
        int64_t content_length;
        int transfer_encoding_offs[HTTP__ARRAY_LEN(http__transfer_encoding_patterns)];
        int connection_offs[HTTP__ARRAY_LEN(http__connection_patterns)];
    } header_value_parser_state;

    char window[4];
};

static void http__parser_init(struct http__parser *parser, http_transport *http, struct http_resp *resp, struct http_hdr *hdrs)
{
    memset(parser, 0, sizeof *parser);
    parser->http = http;
    parser->resp = resp;
    parser->hdrs = hdrs;

    resp->content_length = -1;

    /* Initial read, so peek and get return actual data, not 0 */
    http__read(parser->http, parser->window + sizeof parser->window - 1, sizeof parser->window[0]);
}

static int http__parser_peek_char(struct http__parser *parser)
{
    if (memcmp(parser->window, "\r\n\r\n", 4) == 0) return 0;
    if (memcmp(parser->window + sizeof parser->window - 2, "\n\n", 2) == 0) return 0;
    return parser->window[sizeof parser->window - 1];
}

static int http__parser_get_char(struct http__parser *parser)
{
    int ret;
    ret = http__parser_peek_char(parser);
    memmove(parser->window, parser->window + 1, sizeof parser->window - 1);
    http__read(parser->http, parser->window + sizeof parser->window - 1, sizeof parser->window[0]);
    return ret;
}

static int http__parser_parse_cstr(struct http__parser *parser, const char *p)
{
    while (*p) {
        char c = http__parser_peek_char(parser);
        if (c != *p++) return 0;
        http__parser_get_char(parser);
    }
    return 1;
}

static int http__parser_number(struct http__parser *parser)
{
    int i, result = 0;
    for (i = 0;; i++) {
        char c = http__parser_peek_char(parser);
        if (c == 0)
            longjmp(*parser->http->onerror, HTTP_ERR_PARSE);
        if ('9' < c || c < '0') break;
        http__parser_get_char(parser);
        result = result*10 + c - '0';
    }
    if (i == 0)
        longjmp(*parser->http->onerror, HTTP_ERR_PARSE);
    return result;
}

static void http__parser_skip_ws(struct http__parser *parser)
{
    int i;
    for (i = 0; i < 10000; ++i) {
        char c = http__parser_peek_char(parser);
        if (c != ' ' && c != '\t') return;
        http__parser_get_char(parser);
    }
    longjmp(*parser->http->onerror, HTTP_ERR_PARSE_LIMIT);
}

static void http__parser_skip_newline(struct http__parser *parser)
{
    int i;
    for (i = 0; i < 10000; ++i) {
        switch (http__parser_get_char(parser)) {
        case 0:
        case '\n':
            return;
        }
    }
    longjmp(*parser->http->onerror, HTTP_ERR_PARSE_LIMIT);
}

static void http__parser_status_line(struct http__parser *parser)
{
    if (!http__parser_parse_cstr(parser, "HTTP/1."))
        longjmp(*parser->http->onerror, HTTP_ERR_PARSE);

    switch (http__parser_number(parser)) {
    case 0: case 1: break;
    default: longjmp(*parser->http->onerror, HTTP_ERR_PARSE);
    }

    if (http__parser_get_char(parser) != ' ')
        longjmp(*parser->http->onerror, HTTP_ERR_PARSE);

    parser->resp->status_code = http__parser_number(parser);
    http__parser_skip_newline(parser);
}

static void http__parser_content_length_parse(struct http__parser *parser)
{
    int64_t *cl = &parser->header_value_parser_state.content_length;
    char c      = http__parser_peek_char(parser);
    if ('0' <= c && c <= '9') *cl = *cl * 10 + c - '0';
    else longjmp(*parser->http->onerror, HTTP_ERR_PARSE);
}

static void http__parser_content_length_parse_done(struct http__parser *parser)
{
    int64_t cl = parser->header_value_parser_state.content_length;
    int64_t *dst = &parser->resp->content_length;
    if (*dst >= 0)
        /* Error if Content-Length is duplicated */
        longjmp(*parser->http->onerror, HTTP_ERR_PARSE);
    *dst = cl;
}

static void http__parser_connection_parse(struct http__parser *parser)
{
    char c = http__parser_peek_char(parser);
    size_t i;
    int bit = 0;
    for (i = 0; i < HTTP__ARRAY_LEN(http__connection_patterns); ++i) {
        struct http__header_value_pattern pat = http__connection_patterns[i];
        int *idx = parser->header_value_parser_state.connection_offs + i;
        if (*idx < 0 || !pat.value[*idx]) continue;
        if (pat.value[*idx] != tolower(c)) {
            *idx = -1;
            continue;
        }
        if (!pat.value[++*idx]) bit = pat.bit;
    }
    parser->http->flags |= bit;
}

static void http__parser_transfer_encoding_parse(struct http__parser *parser)
{
    char c = http__parser_peek_char(parser);
    size_t i;
    int bit = 0;
    int *offs = parser->header_value_parser_state.transfer_encoding_offs;
    for (i = 0; i < HTTP__ARRAY_LEN(http__transfer_encoding_patterns); ++i) {
        struct http__header_value_pattern pat = http__transfer_encoding_patterns[i];
        int *idx = offs + i;
        if (*idx < 0 || !pat.value[*idx]) continue;
        switch (c) {
            size_t j;
        case ',': case ' ':
            for (j = 0; j < HTTP__ARRAY_LEN(http__transfer_encoding_patterns); ++j)
                if (offs[j] < 0) offs[j] = 0;
            break;
        default:
            if (pat.value[++*idx] != tolower(c)) *idx = -1;
            else if (!pat.value[*idx]) bit |= pat.bit;
        }
    }
    parser->http->flags |= bit;
}

static struct http_hdr *http__parser_find_header(struct http__parser *parser)
{
    static const struct internal_header_parser {
        const char *name;
        void (*parse)(struct http__parser *);
        void (*parse_done)(struct http__parser *);
    } internal_header_parsers[] = {
        {"content-length", http__parser_content_length_parse, http__parser_content_length_parse_done},
        {"transfer-encoding", http__parser_transfer_encoding_parse, NULL},
        {"connection", http__parser_connection_parse, NULL},
    };
    int internal_offs[HTTP__ARRAY_LEN(internal_header_parsers)] = {0};
    size_t i;

    struct http_hdr *hdrs, *result = NULL;

    for (hdrs = parser->hdrs; hdrs && hdrs->header; ++hdrs) hdrs->_matched = 0;
    parser->header_value_parse = NULL;
    parser->header_value_parse_done = NULL;
    memset(&parser->header_value_parser_state, 0, sizeof parser->header_value_parser_state);

    for (;;) {
        int stuck = 1;
        char c = http__parser_peek_char(parser);
        for (i = 0; i < HTTP__ARRAY_LEN(internal_header_parsers); ++i) {
            struct internal_header_parser const *p = internal_header_parsers + i;
            int *off = internal_offs + i;
            if (*off < 0) continue;
            if (!p->name[*off]) continue;
            if (p->name[*off] != tolower(c)) {
                *off = -1;
                continue;
            }
            if (!p->name[++(*off)]) {
                parser->header_value_parse = p->parse;
                parser->header_value_parse_done = p->parse_done;
            }
            else stuck = 0;
        }
        for (hdrs = parser->hdrs; hdrs && hdrs->header; ++hdrs) {
            if (hdrs->_matched < 0) continue;
            if (!hdrs->header[hdrs->_matched]) continue;
            if (tolower(hdrs->header[hdrs->_matched]) != tolower(c)) {
                hdrs->_matched = -1;
                continue;
            }
            if (!hdrs->header[++hdrs->_matched]) result = hdrs;
            else stuck = 0;
        }
        http__parser_get_char(parser);
        if (stuck) break;
    }
    return result;
}

static void http__parser_header_value_internal(struct http__parser *parser)
{
    if (!parser->header_value_parse) return;
    for (;;) {
        char c = http__parser_peek_char(parser);
        if (c == 0 || c == '\n' || c == '\r')
            break;
        parser->header_value_parse(parser);
        http__parser_get_char(parser);
    }
    if (parser->header_value_parse_done)
        parser->header_value_parse_done(parser);
}

static void http__parser_header_value(struct http__parser *parser, struct http_hdr *hdr)
{
    size_t off;
    char c;
    for (off = 0; ; off++) {
        c = http__parser_peek_char(parser);
        if (c == 0 || c == '\n' || c == '\r')
            break;
        if (off + 1 >= hdr->cap)
            longjmp(*parser->http->onerror, HTTP_ERR_PARSE_LIMIT);
        if (parser->header_value_parse)
            parser->header_value_parse(parser);
        http__parser_get_char(parser);
        hdr->dest[off] = c;
    }
    hdr->dest[off] = 0;
    if (parser->header_value_parse_done)
        parser->header_value_parse_done(parser);
}

static void http__parser_headers(struct http__parser *parser)
{
    struct http_hdr *hdr;

    for (hdr = parser->hdrs; hdr && hdr->header; ++hdr)
        if (hdr->cap) *hdr->dest = 0;

    for (;;) {
        switch (http__parser_peek_char(parser)) {
        case '\r':
            http__parser_get_char(parser);
            /* fallthrough */
        case '\n': case 0:
            return;
        }

        hdr = http__parser_find_header(parser);
        if (http__parser_get_char(parser) == ':') {
            http__parser_skip_ws(parser);
            if (hdr) http__parser_header_value(parser, hdr);
            else http__parser_header_value_internal(parser);
        }
        http__parser_skip_newline(parser);
    }
}

ELOOPHTTPDEF void http_recv_resp(http_transport *http, struct http_resp *resp, struct http_hdr *hdrs)
{
    struct http__parser parser = {0};
    memset(resp, 0, sizeof *resp);
    http__parser_init(&parser, http, resp, hdrs);
    http__parser_status_line(&parser);
    http__parser_headers(&parser);
}

ELOOPHTTPDEF void http_recv(http_transport *http, void *buf, int len)
{
    /* TODO: chunked encoding */
    http__read(http, buf, len);
}

ELOOPHTTPDEF void http_send(http_transport *http, void const *buf, int len)
{
    /* TODO: chunked encoding */
    http__write(http, buf, len);
}

#endif /* ELOOP_HTTP_IMPLEMENTATION */
