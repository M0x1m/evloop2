#ifndef ELOOP_SSL_H_
#define ELOOP_SSL_H_

#include "eloop_net.h"
#include <openssl/ssl.h>

#ifndef ELOOPSSLDEF
#define ELOOPSSLDEF
#endif

#ifndef ELOOP_SSL_IMPLEMENTATION
struct essl_ctx;
typedef struct essl_ctx *essl_ctx_t;
#else
typedef SSL_CTX *essl_ctx_t;
#endif

typedef struct essl essl_t;

typedef struct {
    void (*onclose)(essl_t *);
    void (*onerror)(essl_t *);
    void (*ontimeout)(essl_t *);
    void (*onfree)(essl_t *);
} essl_callbacks;

typedef enum {
    ESSL_ERR_NONE,
    ESSL_ERR_HANDSHAKE,
    ESSL_ERR_CERT,
    ESSL_ERR_IO,
    ESSL_ERR_TIMEOUT,
    ESSL_ERR_OTHER
} essl_error;

struct essl {
    SSL *ssl;
    void *data;
    essl_callbacks cbs;
    essl_error error;
};

ELOOPSSLDEF essl_ctx_t essl_init(void);
ELOOPSSLDEF void essl_deinit(essl_ctx_t);

ELOOPSSLDEF int essl_ctx_use_certificate_file(essl_ctx_t, const char *);
ELOOPSSLDEF int essl_ctx_use_private_key_file(essl_ctx_t, const char *);
ELOOPSSLDEF void essl_ctx_set_verify(essl_ctx_t, int, int (*)(int, X509_STORE_CTX *));

ELOOPSSLDEF int essl_accept(essl_ctx_t, essl_t *, enet_socket *);
ELOOPSSLDEF int essl_connect(essl_ctx_t, essl_t *, enet_socket *);

ELOOPSSLDEF int essl_recv(essl_t *, void *, int);
ELOOPSSLDEF int essl_send(essl_t *, const void *, int);

ELOOPSSLDEF void essl_free(essl_t *);
ELOOPSSLDEF void essl_close(essl_t *);

ELOOPSSLDEF void *essl_get_data(essl_t *);
ELOOPSSLDEF SSL  *essl_get_ssl(essl_t *);
ELOOPSSLDEF essl_error   essl_get_error(essl_t *);
ELOOPSSLDEF essl_callbacks essl_get_callbacks(essl_t *);
ELOOPSSLDEF void essl_set_data(essl_t *, void *);
ELOOPSSLDEF void essl_set_callbacks(essl_t *, essl_callbacks);
ELOOPSSLDEF void essl_attach(enet_socket *, essl_t *);
ELOOPSSLDEF int  essl_pending(essl_t *);
ELOOPSSLDEF int  essl_wait(essl_t *, int);

#endif /* ELOOP_SSL_H_ */

#ifdef ELOOP_SSL_IMPLEMENTATION

#include <assert.h>
#include <string.h>

static void openssl_init_once(void)
{
    static int inited = 0;
    if (inited) return;
    inited = 1;
    SSL_library_init();
    OpenSSL_add_all_algorithms();
    SSL_load_error_strings();
}

ELOOPSSLDEF essl_ctx_t essl_init(void)
{
    SSL_CTX *ctx;
    openssl_init_once();

    ctx = SSL_CTX_new(TLS_method());
    if (!ctx) return NULL;
    SSL_CTX_set_options(ctx, SSL_OP_NO_SSLv2 | SSL_OP_NO_SSLv3);

    return ctx;
}

ELOOPSSLDEF void essl_deinit(essl_ctx_t ctx)
{
    if (!ctx) return;
    SSL_CTX_free(ctx);
}

ELOOPSSLDEF int essl_ctx_use_certificate_file(essl_ctx_t ctx, const char *file)
{
    if (!ctx || !file) return -1;
    return SSL_CTX_use_certificate_file(ctx, file, SSL_FILETYPE_PEM) ? 0 : -1;
}

ELOOPSSLDEF int essl_ctx_use_private_key_file(essl_ctx_t ctx, const char *file)
{
    if (!ctx || !file) return -1;
    return SSL_CTX_use_PrivateKey_file(ctx, file, SSL_FILETYPE_PEM) ? 0 : -1;
}

ELOOPSSLDEF void essl_ctx_set_verify(essl_ctx_t ctx, int mode, int (*cb)(int, X509_STORE_CTX *))
{
    if (ctx) SSL_CTX_set_verify(ctx, mode, cb);
}

static int essl__io_yield(int ret, essl_t *ssl)
{
    switch (SSL_get_error(ssl->ssl, ret)) {
    case SSL_ERROR_WANT_READ:  return essl_wait(ssl, EV_F_READ); break;
    case SSL_ERROR_WANT_WRITE: return essl_wait(ssl, EV_F_WRITE); break;
    default:
        ssl->error = ESSL_ERR_IO;
        if (ssl->cbs.onerror) ssl->cbs.onerror(ssl);
        return -1;
    }
    assert(0 && "unreachable");
}

static int essl__new_ssl(essl_ctx_t ctx, essl_t *ssl, enet_socket *sock)
{
    int fd;
    SSL *openssl_ssl;

    memset(ssl, 0, sizeof *ssl);

    fd = esock_get_fd(sock);
    if (fd < 0) {
        ssl->error = ESSL_ERR_IO;
        return -1;
    }
    openssl_ssl = SSL_new(ctx);
    if (!openssl_ssl) {
        ssl->error = ESSL_ERR_OTHER;
        return -1;
    }
    ssl->ssl = openssl_ssl;

    SSL_set_fd(ssl->ssl, fd);
    return 0;
}

static int essl__abstract_handshake(essl_ctx_t ctx, essl_t *ssl, enet_socket *sock, int (*f)(SSL *))
{
    int ret;

    ret = essl__new_ssl(ctx, ssl, sock);
    if (ret < 0) return -1;

    for (;;) {
        ret = f(ssl->ssl);
        if (ret == 1) break;
        if (ret == 0) ssl->error = ESSL_ERR_HANDSHAKE;
        else if (essl__io_yield(ret, ssl) < 0) return -1;
        else continue;
        essl_free(ssl);
        return -1;
    }

    return 0;
}

ELOOPSSLDEF int essl_accept(essl_ctx_t ctx, essl_t *ssl, enet_socket *sock)
{
    return essl__abstract_handshake(ctx, ssl, sock, SSL_accept);
}

ELOOPSSLDEF int essl_connect(essl_ctx_t ctx, essl_t *ssl, enet_socket *sock)
{
    return essl__abstract_handshake(ctx, ssl, sock, SSL_connect);
}

ELOOPSSLDEF void essl_close(essl_t *ssl)
{
    if (ssl->cbs.onclose) ssl->cbs.onclose(ssl);
    SSL_set_shutdown(ssl->ssl, SSL_RECEIVED_SHUTDOWN | SSL_SENT_SHUTDOWN);
    SSL_shutdown(ssl->ssl);
    essl_free(ssl);
}

ELOOPSSLDEF void essl_free(essl_t *ssl)
{
    if (ssl->cbs.onfree) ssl->cbs.onfree(ssl);
    SSL_free(ssl->ssl);
    ssl->ssl = NULL;
}

ELOOPSSLDEF void *essl_get_data(essl_t *ssl)
{
    return ssl->data;
}

ELOOPSSLDEF SSL *essl_get_ssl(essl_t *ssl)
{
    return ssl->ssl;
}

ELOOPSSLDEF essl_error essl_get_error(essl_t *ssl)
{
    return ssl->error;
}

ELOOPSSLDEF essl_callbacks essl_get_callbacks(essl_t *ssl)
{
    return ssl->cbs;
}

ELOOPSSLDEF void essl_set_data(essl_t *ssl, void *data)
{
    ssl->data = data;
}

ELOOPSSLDEF void essl_set_callbacks(essl_t *ssl, essl_callbacks cbs)
{
    ssl->cbs = cbs;
}

ELOOPSSLDEF int essl_recv(essl_t *ssl, void *buf, int len)
{
    int ret;
    for (;;) {
        ret = SSL_read(ssl->ssl, buf, len);
        if (ret > 0) break;
        if (ret == 0) {
            if (ssl->cbs.onclose) ssl->cbs.onclose(ssl);
            return 0;
        }
        if (essl__io_yield(ret, ssl) < 0) return -1;
    }
    return ret;
}

ELOOPSSLDEF int essl_send(essl_t *ssl, const void *buf, int len)
{
    int ret;
    for (;;) {
        ret = SSL_write(ssl->ssl, buf, len);
        if (ret > 0) break;
        if (ret == 0) {
            if (ssl->cbs.onclose) ssl->cbs.onclose(ssl);
            return 0;
        }
        if (essl__io_yield(ret, ssl) < 0) return -1;
    }
    return ret;
}

ELOOPSSLDEF void essl_attach(enet_socket *sock, essl_t *ssl)
{
    esock_setup_transport(sock, ssl, (void (*)(void*))essl_close);
}

ELOOPSSLDEF int essl_pending(essl_t *ssl)
{
    return SSL_pending(ssl->ssl);
}

ELOOPSSLDEF int essl_wait(essl_t *ssl, int flags)
{
    int ret;
    if (flags & EV_F_READ && essl_pending(ssl))
        return EV_F_READ;

    ev_set_flags(flags);

    ret = ev_yield();
    if (ret & EV_F_TIMEOUT) {
        ssl->error = ESSL_ERR_TIMEOUT;
        if (ssl->cbs.ontimeout) ssl->cbs.ontimeout(ssl);
        return -1;
    }
    if (ret & EV_F_ERROR) {
        ssl->error = ESSL_ERR_OTHER;
        if (ssl->cbs.onerror) ssl->cbs.onerror(ssl);
        return -1;
    }

    return ret;
}

#endif /* ELOOP_SSL_IMPLEMENTATION */
