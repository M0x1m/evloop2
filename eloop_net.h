#ifndef ELOOP_NET_H_
#define ELOOP_NET_H_

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200112L
#endif

#include "event_loop2.h"
#include <sys/socket.h>

#ifndef ELOOPNETDEF
#define ELOOPNETDEF
#endif

enum {
    ENET_EX_F_NONE           = 0x00,
    ENET_EX_F_NOWAIT         = 0x01,
    ENET_EX_F_WAITALL        = 0x02,
    ENET_EX_F_NOT_REUSE_PORT = 0x04,
    ENET_EX_F_NOT_REUSE_ADDR = 0x08
};

typedef enum {
    ENET_ERR_NONE,
    ENET_ERR_TIMEOUT,
    ENET_ERR_CLOSED,
    ENET_ERR_RESOLV,
    ENET_ERR_REFUSED,
    ENET_ERR_INVAL,
    ENET_ERR_OTHER
} enet_error;

enum enet_socktype {
    ENET_SOCK_TCP,
    ENET_SOCK_UDP
};

struct enet_socket;
typedef struct enet_socket enet_socket;

typedef struct {
    void (*onclose)(enet_socket *);
    void (*onerror)(enet_socket *);
    void (*ontimeout)(enet_socket *);
    void (*oncleanup)(enet_socket *);
} esock_callbacks;

typedef void (*enet_routine)(enet_socket *);

ELOOPNETDEF void enet_connect_tcp(event_loop *, const char *, int, enet_routine, void *);
ELOOPNETDEF void enet_connect_udp(event_loop *, const char *, int, enet_routine, void *);
ELOOPNETDEF int enet_bind_tcp(event_loop *, int, enet_routine acceptor, void *);
ELOOPNETDEF int enet_bind_udp(event_loop *, int, enet_routine acceptor, void *);

struct enet_connect_ex_opts {
    enum enet_socktype socktype;
    int timeout;
    int flags;
};

struct enet_bind_ex_opts {
    enum enet_socktype socktype;
    int backlog;
    int flags;
    const char *host;
};

ELOOPNETDEF void enet_connect_ex(event_loop *, const char *, int, enet_routine acceptor, void *, struct enet_connect_ex_opts);
ELOOPNETDEF int enet_bind_ex(event_loop *, int, enet_routine acceptor, void *, struct enet_bind_ex_opts);

ELOOPNETDEF int esock_accept(enet_socket *, enet_routine, void *);

ELOOPNETDEF int esock_recv(enet_socket *, void *, int);
ELOOPNETDEF int esock_recv_ex(enet_socket *, void *, int, int);
ELOOPNETDEF int esock_recvfrom(enet_socket *, void *, int, int, struct sockaddr *, unsigned int *);

ELOOPNETDEF int esock_send(enet_socket *, const void *, int);
ELOOPNETDEF int esock_send_ex(enet_socket *, const void *, int, int);
ELOOPNETDEF int esock_sendto(enet_socket *, const void *, int, int, struct sockaddr const *, unsigned int);

ELOOPNETDEF void esock_close(enet_socket *);

ELOOPNETDEF int  esock_wait(enet_socket *, int);

ELOOPNETDEF intptr_t esock_get_fd(enet_socket *sock);
ELOOPNETDEF struct sockaddr *esock_get_addr(enet_socket *sock, unsigned int *len);
ELOOPNETDEF enet_error esock_get_error(enet_socket *);
ELOOPNETDEF void *esock_get_data(enet_socket *);
ELOOPNETDEF void *esock_get_transport(enet_socket *);
ELOOPNETDEF void esock_set_data(enet_socket *, void *);
ELOOPNETDEF void esock_set_callbacks(enet_socket *, esock_callbacks);

ELOOPNETDEF void esock_setup_transport(enet_socket *, void *, void (*)(void *));

ELOOPNETDEF int esock_socks5_connect(enet_socket *sock, const char *host, int port);

#endif /* ELOOP_NET_H_ */

#ifdef ELOOP_NET_IMPLEMENTATION

#include <assert.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>

#include <arpa/inet.h>
#include <netdb.h>
#include <fcntl.h>
#include <unistd.h>

struct enet_socket {
    intptr_t   fd;
    enum enet_socktype type;
    enet_error error;
    struct sockaddr_storage addr;
    unsigned int addrlen;

    void *data;
    esock_callbacks cbs;

    void *transport_state;
    void (*transport_cleanup)(void *);
};

static void enet__sock_cleanup(ev_task *task)
{
    enet_socket *sock = ev_get_task_data(task);
    if (sock->transport_cleanup) sock->transport_cleanup(sock->transport_state);
    if (sock->cbs.oncleanup) sock->cbs.oncleanup(sock);
    if (sock->fd != -1) close(sock->fd);
}

static intptr_t enet__get_fd_enet_socket(void *data)
{
    enet_socket *sock = data;
    return esock_get_fd(sock);
}

static intptr_t enet__get_fd_int(void *data)
{
    int *fd = data;
    return *fd;
}

struct enet__connect_task_closure {
    struct enet_connect_ex_opts opts;
    int port;
    const char *host;
    enet_routine routine;
    void *userdata;
};

static void enet__fd_make_nonblock(int fd)
{
    int flags = fcntl(fd, F_GETFL);
    fcntl(fd, F_SETFL, flags|O_NONBLOCK);
}

static int enet__socktype_to_system_st(enum enet_socktype type)
{
    switch (type) {
    case ENET_SOCK_TCP: return SOCK_STREAM;
    case ENET_SOCK_UDP: return SOCK_DGRAM;
    default:
        assert(0 && "unreachable: enet__socktype_to_system_st");
    }
}

static void enet__setup_sock_task(enet_socket *sock)
{
    intptr_t (*fd_getter)(void *) = enet__get_fd_enet_socket;
    if (sock->fd == -1) fd_getter = NULL;
    else ev_set_timeout(-1);
    ev_set_data(sock);
    ev_set_data_to_transport_handle_cb(fd_getter);
    ev_set_cleanup_cb(enet__sock_cleanup);
}

static void enet__connect_task_try(enet_socket *sock, enet_routine *routine)
{
    /* TODO: implement windows stuff */
    char port_str[16];
    struct addrinfo hints = {0};
    struct enet__connect_task_closure c;
    struct addrinfo *info, *cur;
    int zero = 0;
    int fd, ret;

    ev_set_data(&c);
    ev_end_init();

    sock->fd = -1;
    sock->data = c.userdata;
    *routine = c.routine;

    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = enet__socktype_to_system_st(c.opts.socktype);

    if (c.port > 65535 || c.port < 0) {
        sock->error = ENET_ERR_INVAL;
        return;
    }

    sprintf(port_str, "%u", c.port);

    ret = getaddrinfo(c.host, port_str, &hints, &info);
    if (ret != 0) {
        sock->error = ENET_ERR_RESOLV;
        return;
    }

    ev_set_data(&fd);
    ev_set_data_to_transport_handle_cb(enet__get_fd_int);

    for (cur = info; cur; cur = cur->ai_next) {
        fd = socket(cur->ai_family, cur->ai_socktype, cur->ai_protocol);
        if (fd < 0) continue;

        enet__fd_make_nonblock(fd);

        if (cur->ai_family == AF_INET6)
            setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &zero, sizeof zero);

        ret = connect(fd, cur->ai_addr, cur->ai_addrlen);
        if (ret == 0) {
        success:
            memcpy(&sock->addr, cur->ai_addr, cur->ai_addrlen);
            sock->addrlen = cur->ai_addrlen;
            sock->fd      = fd;
            sock->type    = c.opts.socktype;
            sock->error   = ENET_ERR_NONE;
            break;
        }

        switch (errno) {
            int so_error;
            socklen_t len;
        case EINPROGRESS:
            ev_set_flags(EV_F_WRITE);
            ev_set_timeout(c.opts.timeout);
            ret = ev_yield();
            if (ret & EV_F_TIMEOUT) {
                sock->error = ENET_ERR_TIMEOUT;
                break;
            }
            if (ret & EV_F_ERROR) {
                sock->error = ENET_ERR_OTHER;
                break;
            }
            so_error = 0;
            len = sizeof so_error;
            getsockopt(fd, SOL_SOCKET, SO_ERROR, &so_error, &len);
            if (so_error == 0) goto success;
            /* fallthrough */
        default:
            sock->error = ENET_ERR_REFUSED;
            break;
        }

        close(fd);
   }

    if (fd < 0) sock->error = ENET_ERR_OTHER;
    freeaddrinfo(info);
}

static void enet__connect_task(void)
{
    enet_routine routine;
    enet_socket sock;
    enet__connect_task_try(&sock, &routine);
    enet__setup_sock_task(&sock);
    routine(&sock);
}

static void enet__connect(event_loop *ev, const char *host, int port, enet_routine routine, void *userdata, struct enet_connect_ex_opts opts)
{
    struct enet__connect_task_closure *c;
    ev_task *task;

    task = eloop_add(ev, enet__connect_task);
#ifndef ELOOP_NET_NOSTUB
    ev_enable_stub(task);
#endif
    ev_run_init(task);
    c = ev_get_task_data(task);
    c->opts    = opts;
    c->port    = port;
    c->host    = host;
    c->routine = routine;
    c->userdata = userdata;
}

ELOOPNETDEF void enet_connect_tcp(event_loop *ev, const char *host, int port, enet_routine routine, void *userdata)
{
    struct enet_connect_ex_opts opts;
    opts.socktype = ENET_SOCK_TCP;
    opts.flags    = ENET_EX_F_NONE;
    opts.timeout  = -1;
    enet__connect(ev, host, port, routine, userdata, opts);
}

ELOOPNETDEF void enet_connect_udp(event_loop *ev, const char *host, int port, enet_routine routine, void *userdata)
{
    struct enet_connect_ex_opts opts;
    opts.socktype = ENET_SOCK_UDP;
    opts.flags    = ENET_EX_F_NONE;
    opts.timeout  = -1;
    enet__connect(ev, host, port, routine, userdata, opts);
}

ELOOPNETDEF void enet_connect_ex(event_loop *ev, const char *host, int port, enet_routine routine, void *userdata, struct enet_connect_ex_opts opts)
{
    enet__connect(ev, host, port, routine, userdata, opts);
}

static int enet__bind_sock(struct enet_bind_ex_opts opts, int port, int *out_fd)
{
    /* TODO: windows */
    char port_str[16];
    const char *host = opts.host;
    struct addrinfo hints = {0}, *res, *rp;
    int fd = -1, ret;
    int reuse = 1;
    int v6only = 0;

    if (port > 65535 || port < 0) return -1;
    sprintf(port_str, "%u", port);

    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = enet__socktype_to_system_st(opts.socktype);
    hints.ai_flags = AI_PASSIVE;

    ret = getaddrinfo(host, port_str, &hints, &res);
    if (ret != 0) return -1;

    for (rp = res; rp != NULL; rp = rp->ai_next) {
        fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (fd < 0) continue;

        enet__fd_make_nonblock(fd);

        if (!(opts.flags & ENET_EX_F_NOT_REUSE_ADDR))
            setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
#ifdef SO_REUSEPORT
        if (!(opts.flags & ENET_EX_F_NOT_REUSE_PORT))
            setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &reuse, sizeof(reuse));
#endif

        if (rp->ai_family == AF_INET6) {
            if (!host || strcmp(host, "::") == 0 || strcmp(host, "0.0.0.0") == 0)
                setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &v6only, sizeof(v6only));
        }

        if (bind(fd, rp->ai_addr, rp->ai_addrlen) == 0) {
            if (opts.socktype == ENET_SOCK_TCP)
                listen(fd, opts.backlog > 0 ? opts.backlog : SOMAXCONN);

            *out_fd = fd;
            freeaddrinfo(res);
            return 0;
        }

        close(fd);
        fd = -1;
    }

    freeaddrinfo(res);
    if (fd != -1) close(fd);
    return -1;
}

struct enet__readysocket_task_closure {
    enet_socket sock;
    enet_routine routine;
    void *userdata;
};

static void enet__readysocket_task(void)
{
    struct enet__readysocket_task_closure closure = {0};
    ev_set_data(&closure);
    ev_end_init();
    enet__setup_sock_task(&closure.sock);
    esock_set_data(&closure.sock, closure.userdata);
    closure.routine(&closure.sock);
}

static enet_socket *enet__run_readysocket_task(event_loop *ev, int sock, enet_routine routine, void *userdata)
{
    ev_task *task;
    struct enet__readysocket_task_closure *closure;
    task = eloop_add(ev, enet__readysocket_task);
#ifndef ELOOP_NET_NOSTUB
    ev_enable_stub(task);
#endif

    ev_run_init(task);
    closure = ev_get_task_data(task);
    closure->sock.fd = sock;
    closure->routine = routine;
    closure->userdata = userdata;
    return &closure->sock;
}

ELOOPNETDEF int enet_bind_ex(event_loop *ev, int port, enet_routine acceptor, void *userdata, struct enet_bind_ex_opts opts)
{
    /* TODO: windows */
    enet_socket *sock;
    int fd;
    int ret = enet__bind_sock(opts, port, &fd);
    if (ret < 0) return ret;
    sock = enet__run_readysocket_task(ev, fd, acceptor, userdata);
    sock->type = opts.socktype;
    return ret;
}

ELOOPNETDEF int enet_bind_tcp(event_loop *ev, int port, enet_routine acceptor, void *userdata)
{
    struct enet_bind_ex_opts opts = {0};
    opts.socktype = ENET_SOCK_TCP;
    opts.backlog = 10;

    return enet_bind_ex(ev, port, acceptor, userdata, opts);
}

ELOOPNETDEF int enet_bind_udp(event_loop *ev, int port, enet_routine acceptor, void *userdata)
{
    struct enet_bind_ex_opts opts = {0};
    opts.socktype = ENET_SOCK_UDP;

    return enet_bind_ex(ev, port, acceptor, userdata, opts);
}

ELOOPNETDEF enet_error esock_get_error(enet_socket *sock)
{
    return sock->error;
}

ELOOPNETDEF void *esock_get_data(enet_socket *sock)
{
    return sock->data;
}

ELOOPNETDEF void esock_set_data(enet_socket *sock, void *data)
{
    sock->data = data;
}

ELOOPNETDEF int esock_accept(enet_socket *sock, enet_routine client, void *userdata)
{
    int fd, wake;
    enet_socket *new_sock;
    struct sockaddr_storage addr;
    socklen_t addrlen = sizeof addr;

    ev_set_flags(EV_F_READ);
    wake = ev_yield();
    if (wake & EV_F_TIMEOUT) {
        if (sock->cbs.ontimeout) sock->cbs.ontimeout(sock);
        sock->error = ENET_ERR_TIMEOUT;
        return -1;
    }
    if (wake & EV_F_ERROR) {
        if (sock->cbs.onerror) sock->cbs.onerror(sock);
        sock->error = ENET_ERR_OTHER;
        return -1;
    }

    fd = accept(sock->fd, (struct sockaddr *)&addr, &addrlen);
    if (fd < 0) return -1;

    enet__fd_make_nonblock(fd);

    new_sock = enet__run_readysocket_task(ev_get_loop(), fd, client, userdata);
    memcpy(&new_sock->addr, &addr, addrlen);
    new_sock->addrlen = addrlen;
    new_sock->type = sock->type;
    return 0;
}

ELOOPNETDEF void esock_close(enet_socket *sock)
{
    if (sock->type == ENET_SOCK_UDP || sock->fd == -1)
        ev_terminate();

    shutdown(sock->fd, SHUT_WR);
    ev_set_flags(EV_F_READ);
    ev_yield();
    ev_terminate();
}

ELOOPNETDEF intptr_t esock_get_fd(enet_socket *sock)
{
    return sock->fd;
}

ELOOPNETDEF struct sockaddr *esock_get_addr(enet_socket *sock, unsigned int *len)
{
    *len = sock->addrlen;
    return (struct sockaddr *)&sock->addr;
}

ELOOPNETDEF int esock_recv(enet_socket *sock, void *buf, int size)
{
    return esock_recvfrom(sock, buf, size, 0, NULL, NULL);
}

ELOOPNETDEF int esock_recv_ex(enet_socket *sock, void *buf, int size, int flags)
{
    return esock_recvfrom(sock, buf, size, flags, NULL, NULL);
}

ELOOPNETDEF int esock_send(enet_socket *sock, const void *buf, int size)
{
    return esock_sendto(sock, buf, size, 0, NULL, 0);
}

ELOOPNETDEF int esock_send_ex(enet_socket *sock, const void *buf, int size, int flags)
{
    return esock_sendto(sock, buf, size, flags, NULL, 0);
}

enum enet_io_arg_tag {
    ENET_IO_ARG_WRITE,
    ENET_IO_ARG_READ
};

union enet_io_arg {
    enum enet_io_arg_tag tag;
    struct {
        enum enet_io_arg_tag tag;
        const void *buf;
        int size;
        struct sockaddr const *dest;
        unsigned int addrlen;
    } write;
    struct {
        enum enet_io_arg_tag tag;
        void *buf;
        int size;
        struct sockaddr *source;
        unsigned int *addrlen;
    } read;
};

static int enet__io_get_size(union enet_io_arg const *arg)
{
    switch (arg->tag) {
    case ENET_IO_ARG_WRITE: return arg->write.size;
    case ENET_IO_ARG_READ:  return arg->read.size;
    default:
        assert(0 && "unreachable: enet__io_get_size");
    }
}

static int enet__io_has_address(union enet_io_arg const *arg)
{
    switch (arg->tag) {
    case ENET_IO_ARG_WRITE: return !!arg->write.dest;
    case ENET_IO_ARG_READ:  return !!arg->read.source;
    default:
        assert(0 && "unreachable: enet__io_has_address");
    }
}

struct enet_io {
    union enet_io_arg *arg;
    int (*func)(enet_socket *, union enet_io_arg *arg);
    int flag;
};

static int enet__abstract_io(enet_socket *sock, int flags, struct enet_io io)
{
    int ret, total;

    if (sock->fd == -1) {
        sock->error = ENET_ERR_INVAL;
        return -1;
    }

    if (sock->type == ENET_SOCK_UDP && flags & ENET_EX_F_WAITALL) {
        sock->error = ENET_ERR_INVAL;
        return -1;
    }

    if (sock->type == ENET_SOCK_TCP && enet__io_has_address(io.arg)) {
        sock->error = ENET_ERR_INVAL;
        return -1;
    }

    if (flags & ENET_EX_F_NOWAIT) {
        ret = io.func(sock, io.arg);
        if (ret < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                sock->error = ENET_ERR_NONE;
                return -1;
            }
            if (sock->cbs.onerror) sock->cbs.onerror(sock);
            sock->error = ENET_ERR_OTHER;
            return -1;
        }
        if (ret == 0) {
            if (sock->cbs.onclose) sock->cbs.onclose(sock);
            sock->error = ENET_ERR_CLOSED;
            return -1;
        }
        return ret;
    }

    total = 0;
    do {
        if (esock_wait(sock, io.flag) < 0) return -1;
        ret = io.func(sock, io.arg);
        if (ret < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                continue;
            }
            if (sock->cbs.onerror) sock->cbs.onerror(sock);
            sock->error = ENET_ERR_OTHER;
            return -1;
        }
        if (ret == 0) {
            if (total) return total;
            if (sock->cbs.onclose) sock->cbs.onclose(sock);
            sock->error = ENET_ERR_CLOSED;
            return -1;
        }
        total += ret;

        if (!(flags & ENET_EX_F_WAITALL)) break;
    } while (total < enet__io_get_size(io.arg));

    return total;
}

static int enet__io_read(enet_socket *sock, union enet_io_arg *arg)
{
    assert(arg->tag == ENET_IO_ARG_READ);
    return recvfrom(sock->fd, arg->read.buf, arg->read.size, 0, arg->read.source, (socklen_t *) arg->read.addrlen);
}

static int enet__io_write(enet_socket *sock, union enet_io_arg *arg)
{
    assert(arg->tag == ENET_IO_ARG_WRITE);
    return sendto(sock->fd, arg->write.buf, arg->write.size, 0, arg->write.dest, (socklen_t) arg->write.addrlen);
}

ELOOPNETDEF int esock_sendto(enet_socket *sock, const void *buf, int size, int flags, struct sockaddr const *dest, unsigned int addrlen)
{
    union enet_io_arg arg;
    struct enet_io io;

    arg.tag           = ENET_IO_ARG_WRITE;
    arg.write.buf     = buf;
    arg.write.size    = size;
    arg.write.dest    = dest;
    arg.write.addrlen = addrlen;

    io.arg  = &arg;
    io.func = enet__io_write;
    io.flag = EV_F_WRITE;

    return enet__abstract_io(sock, flags, io);
}

ELOOPNETDEF int esock_recvfrom(enet_socket *sock, void *buf, int size, int flags, struct sockaddr *source, unsigned int *addrlen)
{
    union enet_io_arg arg;
    struct enet_io io;

    arg.tag          = ENET_IO_ARG_READ;
    arg.read.buf     = buf;
    arg.read.size    = size;
    arg.read.source  = source;
    arg.read.addrlen = addrlen;

    io.arg  = &arg;
    io.func = enet__io_read;
    io.flag = EV_F_READ;

    return enet__abstract_io(sock, flags, io);
}

ELOOPNETDEF int esock_wait(enet_socket *sock, int flags)
{
    int wake;

    ev_set_flags(flags);
    wake = ev_yield();
    if (wake & EV_F_TIMEOUT) {
        if (sock->cbs.ontimeout) sock->cbs.ontimeout(sock);
        sock->error = ENET_ERR_TIMEOUT;
        return -1;
    }
    if (wake & EV_F_ERROR) {
        if (sock->cbs.onerror) sock->cbs.onerror(sock);
        sock->error = ENET_ERR_OTHER;
        return -1;
    }

    return wake;
}

ELOOPNETDEF void esock_setup_transport(enet_socket *sock, void *state, void (*cleanup)(void *))
{
    sock->transport_state = state;
    sock->transport_cleanup = cleanup;
}

ELOOPNETDEF void *esock_get_transport(enet_socket *sock)
{
    return sock->transport_state;
}

ELOOPNETDEF void esock_set_callbacks(enet_socket *sock, esock_callbacks cbs)
{
    sock->cbs = cbs;
}

ELOOPNETDEF int esock_socks5_connect(enet_socket *sock, const char *host, int port)
{
    unsigned char buf[256];
    int hostlen = strlen(host);
    int addrlen;

    memcpy(buf, "\x05\x01\x00", 3);
    if (esock_send(sock, buf, 3) < 0) return -1;
    if (esock_recv(sock, buf, 2) < 0) return -1;

    memcpy(buf, "\x05\x01\x00\x03", 4);
    buf[4] = hostlen;
    memcpy(buf + 5, host, hostlen);
    buf[5 + hostlen] = (port >> 8) & 0xFF;
    buf[6 + hostlen] = port & 0xFF;
    if (esock_send(sock, buf, 7 + hostlen) < 0) return -1;
    if (esock_recv(sock, buf, 4) < 0) return -1;

    switch (buf[3]) {
    case 0x01: addrlen = 4;          break;
    case 0x04: addrlen = 16;         break;
    default:   addrlen = 1 + buf[4]; break;
    }
    if (esock_recv(sock, buf, addrlen + 2) < 0) return -1;
    return 0;
}

#endif /* ELOOP_NET_IMPLEMENTATION */
