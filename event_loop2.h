#ifndef EVENT_LOOP2_H_
#define EVENT_LOOP2_H_

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 199309L
#endif

#include <stddef.h>
#include <stdint.h>
#include "libco.h"

#ifndef EVENTLOOP2DEF
#define EVENTLOOP2DEF
#endif

#define EV_NO_TIMEOUT (-1)

struct ev_task;
typedef struct ev_task ev_task;

typedef void (*ev_routine)(void);

typedef struct {
    cothread_t env;
    ev_task *alive, *dead;
    size_t stack_size;
    int flags;

    void *ator;
    void *(*alloc)(void *ator, size_t);
    void (*free)(void *ator, void *, size_t);
} event_loop;

enum {
    EV_F_NONE         = 0x00,
    EV_F_READ         = 0x01,
    EV_F_WRITE        = 0x02,
    EV_F_TIMEOUT      = 0x04,
    EV_F_RECV_MESSAGE = 0x08,
    EV_F_SEND_MESSAGE = 0x10,
    EV_F_ERROR        = 0x20,
    EV_F_NOTIFY       = 0x40,
    EV_F_EXCEPTIONS   = EV_F_ERROR | EV_F_TIMEOUT
};

EVENTLOOP2DEF void     eloop_init(event_loop *);
EVENTLOOP2DEF void     eloop_set_ator(event_loop *, void *, void *(*)(void *, size_t), void (*)(void *, void *, size_t));
EVENTLOOP2DEF void     eloop_set_stack_size(event_loop *, size_t);
EVENTLOOP2DEF ev_task *eloop_add(event_loop *ev, ev_routine);
EVENTLOOP2DEF ev_task *eloop_add_ex(event_loop *ev, ev_routine, void *, size_t);
EVENTLOOP2DEF void     eloop_run(event_loop *);

EVENTLOOP2DEF void ev_enable_stub(ev_task *);

EVENTLOOP2DEF void ev_set_task_data(ev_task *, void *);
EVENTLOOP2DEF void ev_set_task_data_to_transport_handle_cb(ev_task *, intptr_t (*)(void *));
EVENTLOOP2DEF void ev_set_task_flags(ev_task *, int);
EVENTLOOP2DEF void ev_set_task_timeout(ev_task *, int ms);
EVENTLOOP2DEF void ev_set_task_cleanup_cb(ev_task *, void (*)(ev_task *));

EVENTLOOP2DEF int   ev_yield(void);
EVENTLOOP2DEF int   ev_send_message(ev_task *dst, void *);
EVENTLOOP2DEF void *ev_recv_message(void);
EVENTLOOP2DEF void *ev_pull_message(void);

EVENTLOOP2DEF void  ev_notify(ev_task *);
EVENTLOOP2DEF void  ev_task_terminate(ev_task *);

EVENTLOOP2DEF ev_task *ev_current(void);

EVENTLOOP2DEF void  *ev_get_task_data(ev_task *);
EVENTLOOP2DEF int    ev_get_task_timeout(ev_task *);
EVENTLOOP2DEF int    ev_get_task_flags(ev_task *);

EVENTLOOP2DEF event_loop *ev_get_task_loop(ev_task *);

EVENTLOOP2DEF size_t ev_get_task_size(ev_task *);
EVENTLOOP2DEF void  *ev_get_task_base(ev_task *);

EVENTLOOP2DEF void ev_end_init(void);
EVENTLOOP2DEF void ev_run_init(ev_task *task);

#define ev_set_data(arg) ev_set_task_data(NULL, arg)
#define ev_set_data_to_transport_handle_cb(arg) ev_set_task_data_to_transport_handle_cb(NULL, arg)
#define ev_set_flags(arg) ev_set_task_flags(NULL, arg)
#define ev_set_timeout(arg) ev_set_task_timeout(NULL, arg)
#define ev_set_cleanup_cb(arg) ev_set_task_cleanup_cb(NULL, arg)

#define ev_terminate() ev_task_terminate(NULL)

#define ev_get_loop() ev_get_task_loop(NULL)
#define ev_get_data() ev_get_task_data(NULL)
#define ev_get_timeout() ev_get_task_timeout(NULL)
#define ev_get_flags() ev_get_task_flags(NULL)

#endif /* EVENT_LOOP2_H_ */

#ifdef EVENT_LOOP2_IMPLEMENTATION

#ifndef EVENT_LOOP2_DEFAULT_STACK_SIZE
#define EVENT_LOOP2_DEFAULT_STACK_SIZE 65536
#endif

#include <string.h>
#include <limits.h>

#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <assert.h>

#include <poll.h>
#include <time.h>

enum {
    ELOOP_F_DIRTY           = 0x01,
    ELOOP_F_DISTURB_MESSAGE = 0x02,
    ELOOP_F_DISTURB_NOTIF   = 0x04
};

enum {
    EV_TASK_F_NORMAL    = 0x00,
    EV_TASK_F_USERALLOC = 0x01,
    EV_TASK_F_STUB      = 0x02,
    EV_TASK_F_DEAD      = 0x04,
    EV_TASK_F_INIT      = 0x08
};

struct ev_task {
    cothread_t env;
    cothread_t parent;
    ev_routine routine;

    event_loop *loop;
    ev_task *next;
    size_t size;

    struct {
        void *data;
        ev_task *partner;
    } message;

    void *data;
    intptr_t (*data_to_transport_handle)(void*);
    void (*cleanup)(ev_task *);

    int timeout;
    int elapsed;
    int wait_flags;
    int wake_flags;
    int flags;
};

#define EV_TASK_BASE(task) ((void *)(task))
#define EV_IS_DEAD(task) ((task)->flags & EV_TASK_F_DEAD)

EVENTLOOP2DEF void ev_task_terminate(register ev_task *task)
{
    register event_loop *loop;
    if (!task) task = ev_current();
    loop = task->loop;
    task->flags |= EV_TASK_F_DEAD;
    loop->flags |= ELOOP_F_DIRTY;
    if (task->env == co_active())
        co_switch(loop->env);
}

/* TODO: Some compilers may nuke call to this with -O1 and I dont know
   how much other ABI's would clobber user stack while we terminating
   the task.
   After some testing there is some results:

   +--------------------+----------+----------------------------+
   |Compiler            |Opt-Level |Stack                       |
   +--------------------+----------+----------------------------+
   |GCC 16.2.1 (amd64)  |-O0       |Clobbered (first 40 bytes)  |
   |Linux               |          |Stubbing helps              |
   |                    +----------+----------------------------+
   |                    |-O1 and   |Not touched                 |
   |                    |above     |                            |
   +--------------------+----------+----------------------------+
   |GCC 16.2.1 (i386)   |-O0       |Clobbered (first 32 bytes)  |
   |Linux               |          |Stubbing helps              |
   |                    +----------+----------------------------+
   |                    |-O1 and   |Not touched                 |
   |                    |above     |                            |
   +--------------------+----------+----------------------------+
   |Clang 22.1.8 (amd64)|-O0       |Clobbered (first 56 bytes)  |
   |Linux               |          |Stubbing helps              |
   |                    +----------+----------------------------+
   |                    |-O1 and   |Not touched                 |
   |                    |above     |                            |
   +--------------------+----------+----------------------------+
   |Clang 22.1.8 (i386) |-O0       |Clobbered (first 36 bytes)  |
   |Linux               |          |Stubbing helps              |
   +--------------------+----------+----------------------------+
   |TCC (amd64)         |          |Clobbered (first 64 bytes)  |
   |Linux               |          |Stubbing helps              |
   +--------------------+----------+----------------------------+
*/
static void eloop__stub(register ev_routine routine)
{
    volatile void *stub[12];
    (void) stub;
    routine();
}

static void eloop__libco_wrapper(void)
{
    ev_task *task = ev_current();
    co_switch(task->parent);
    if (task->flags & EV_TASK_F_STUB) eloop__stub(task->routine);
    else task->routine();
    ev_task_terminate(task);
}

static void *eloop__alloc(event_loop const *ev, size_t size)
{
    if (ev->ator) return ev->alloc(ev->ator, size);
    else          return malloc(size);
}

static void eloop__free(event_loop const *ev, void *p, size_t size)
{
    if (!ev->ator) free(p);
    else if (ev->free) ev->free(ev->ator, p, size);
}

EVENTLOOP2DEF void eloop_init(event_loop *ev)
{
    memset(ev, 0, sizeof *ev);
    ev->env = co_active();
    ev->stack_size = EVENT_LOOP2_DEFAULT_STACK_SIZE;
}

EVENTLOOP2DEF void eloop_set_stack_size(event_loop *ev, size_t ss)
{
    ev->stack_size = ss;
}

static ev_task *eloop__add_init(event_loop *ev, ev_routine routine, void *base, size_t size)
{
    size_t stack_size;
    char  *stack_base;
    ev_task *task;

    stack_size = size - sizeof *task;
    stack_base = (char *)base + sizeof *task;
    task = base;

    memset(task, 0, sizeof *task);

    task->env     = co_derive(stack_base, stack_size, eloop__libco_wrapper);
    task->routine = routine;
    task->parent  = co_active();
    task->loop    = ev;
    task->next    = ev->alive;
    task->size    = size;

    ev->alive = task;

    co_switch(task->env);
    return task;
}

EVENTLOOP2DEF ev_task *eloop_add_ex(event_loop *ev, ev_routine routine, void *mem, size_t size)
{
    ev_task *task;
    if (size < sizeof *task) return NULL;
    task = eloop__add_init(ev, routine, mem, size);
    task->flags |= EV_TASK_F_USERALLOC;
    return task;
}

EVENTLOOP2DEF ev_task *eloop_add(event_loop *ev, ev_routine routine)
{
    size_t size;
    void *base;
    ev_task *task;

    if (ev->dead) {
        task     = ev->dead;
        ev->dead = task->next;
        base     = EV_TASK_BASE(task);
        size     = task->size;
    } else {
        size     = ev->stack_size + sizeof *task;
        base     = eloop__alloc(ev, size);
    }

    task = eloop__add_init(ev, routine, base, size);

    return task;
}

static int eloop__poll_flags_to_flags(short flags)
{
    int result = 0;
    short errorous = POLLERR|POLLHUP|POLLNVAL;
    if (flags & POLLIN)   result |= EV_F_READ;
    if (flags & POLLOUT)  result |= EV_F_WRITE;
    if (flags & errorous) result |= EV_F_ERROR;
    return result;
}

static short eloop__flags_to_poll_flags(int flags)
{
    short result = 0;
    if (flags & EV_F_READ) result |= POLLIN;
    if (flags & EV_F_WRITE) result |= POLLOUT;
    return result;
}

static void eloop__cancel_randevouz_if_dead(ev_task *task)
{
    ev_task *partner = task->message.partner;
    if (!partner) return;
    if (!EV_IS_DEAD(partner)) return;
    task->message.partner = NULL;
    task->wake_flags = EV_F_ERROR;
}

static void eloop__cleanup(event_loop *ev)
{
    ev_task *task, *prev;
    if (!(ev->flags & ELOOP_F_DIRTY)) return;
    ev->flags &= ~ELOOP_F_DIRTY;
    for (prev = NULL, task = ev->alive; task; ) {
        ev_task *dead;

        if (!EV_IS_DEAD(task)) {
            eloop__cancel_randevouz_if_dead(task);
            prev = task;
            task = task->next;
            continue;
        }

        dead = task;
        task = dead->next;

        if (prev) prev->next = task;
        else      ev->alive  = task;

        if (dead->cleanup) dead->cleanup(dead);
        if (dead->flags & EV_TASK_F_USERALLOC) continue;

        dead->next = ev->dead;
        ev->dead = dead;
    }
}

static size_t eloop__populate_fildes(event_loop *ev, struct pollfd *fildes)
{
    size_t i;
    ev_task *task;
    for (i = 0, task = ev->alive; task; task = task->next) {
        struct pollfd *pfd;

        if (!task->data_to_transport_handle) continue;

        pfd         = &fildes[i++];
        pfd->fd     = task->data_to_transport_handle(task->data);
        pfd->events = eloop__flags_to_poll_flags(task->wait_flags);
    }
    return i;
}

struct poll_events_result {
    int wakey_guys;
    int elapsed_ms;
};

static struct poll_events_result eloop__poll(struct pollfd *fildes, int nfds, int timeout_ms)
{
    struct poll_events_result res = {0};
    struct timespec begin, end;

    /* TODO: windows bullshit api */
    clock_gettime(CLOCK_MONOTONIC, &begin);
    res.wakey_guys = poll(fildes, nfds, timeout_ms);
    clock_gettime(CLOCK_MONOTONIC, &end);
    res.elapsed_ms = (end.tv_sec - begin.tv_sec) * 1000 + (end.tv_nsec - begin.tv_nsec) / 1000000;
    return res;
}

static int eloop__compute_timeout(event_loop *ev)
{
    ev_task *task;
    int remain, timeout = INT_MAX;

    int disturb_flags = ELOOP_F_DISTURB_MESSAGE
        | ELOOP_F_DISTURB_NOTIF;

    if (ev->flags & disturb_flags) {
        ev->flags &= ~disturb_flags;
        return 0;
    }

    for (task = ev->alive; task; task = task->next) {
        if (task->timeout == EV_NO_TIMEOUT) continue;

        remain = task->timeout - task->elapsed;
        if (remain < 0) remain = 0;
        if (remain < timeout) timeout = remain;
    }
    return timeout == INT_MAX ? -1 : timeout;
}

static void eloop__wake_on_events(event_loop *ev, int elapsed, struct pollfd *fildes)
{
    size_t i;
    ev_task *task;
    for (i = 0, task = ev->alive; task; task = task->next) {
        struct pollfd *pfd = NULL;
        if (task->data_to_transport_handle) {
            assert(task->data_to_transport_handle(task->data) == fildes[i].fd);
            pfd = &fildes[i++];
        }

        if (task->timeout != EV_NO_TIMEOUT) {
            task->elapsed += elapsed;
            if (task->elapsed >= task->timeout) {
                task->elapsed = 0;
                task->wake_flags |= EV_F_TIMEOUT;
            }
        }

        if (pfd) task->wake_flags |= eloop__poll_flags_to_flags(pfd->revents);
    }
}

static void eloop__dispatch(event_loop *ev)
{
    ev_task *task = ev->alive;
    while (task) {
        ev_task *next = task->next;
        assert(!(task->flags & EV_TASK_F_INIT) && "ev_run_init() should be called right after creating of the task");
        if (task->wake_flags & (task->wait_flags | EV_F_EXCEPTIONS)) {
            co_switch(task->env);
            task->wake_flags = 0;
        }
        task = next;
    }
}

static ev_task *ev__get_randezvous_partner_if_ready(ev_task *sender)
{
    ev_task *receiver;
    if (!(sender->wait_flags & EV_F_SEND_MESSAGE)) return NULL;

    receiver = sender->message.partner;
    if (!receiver) return NULL;
    if (!(receiver->wait_flags & EV_F_RECV_MESSAGE)) return NULL;
    return receiver;
}

static void eloop__match_randezvous(event_loop *ev)
{
    ev_task *sender;
    for (sender = ev->alive; sender; sender = sender->next) {
        ev_task *receiver = ev__get_randezvous_partner_if_ready(sender);
        if (!receiver) continue;

        ev->flags |= ELOOP_F_DISTURB_MESSAGE;

        receiver->message.data = sender->message.data;
        receiver->message.partner = sender;

        sender->wait_flags &= ~EV_F_SEND_MESSAGE;
        receiver->wait_flags &= ~EV_F_RECV_MESSAGE;

        sender->wake_flags = EV_F_SEND_MESSAGE;
        receiver->wake_flags = EV_F_RECV_MESSAGE;
    }
}

static void eloop__free_task_list(event_loop *ev, ev_task *task)
{
    ev_task *next;
    for (; task; task = next) {
        next = task->next;
        if (task->flags & EV_TASK_F_USERALLOC) continue;
        eloop__free(ev, EV_TASK_BASE(task), task->size);
    }
}

static void eloop__tasks_free(event_loop *ev)
{
    eloop__free_task_list(ev, ev->alive);
    eloop__free_task_list(ev, ev->dead);
}

EVENTLOOP2DEF void eloop_run(event_loop *ev)
{
    struct pollfd *fildes = NULL;
    size_t cap = 1, length;

    while (ev->alive) {
        struct poll_events_result poll_result;
        ev_task *task;
        int timeout, elapsed = 0;
        size_t sanity;

        length = 0;

        for (task = ev->alive; task; task = task->next) {
            if (task->data_to_transport_handle) length++;
            if (task->wake_flags & task->wait_flags & EV_F_NOTIFY)
                ev->flags |= ELOOP_F_DISTURB_NOTIF;
        }

        if (length >= cap) {
            eloop__free(ev, fildes, sizeof *fildes * cap);
            while (length >= cap) cap *= 2;
            fildes = eloop__alloc(ev, sizeof *fildes * cap);
        }

        eloop__match_randezvous(ev);

        sanity = eloop__populate_fildes(ev, fildes);
        assert(sanity == length);
        timeout = eloop__compute_timeout(ev);
        poll_result = eloop__poll(fildes, length, timeout);

        if (poll_result.wakey_guys == -1) {
            if (errno == EINTR) poll_result.wakey_guys = 0;
            else {
                perror("poll");
                abort();
            }
        }

        elapsed = poll_result.elapsed_ms;

        eloop__wake_on_events(ev, elapsed, fildes);
        eloop__dispatch(ev);
        eloop__cleanup(ev);
    }

    eloop__tasks_free(ev);
    eloop__free(ev, fildes, sizeof *fildes * cap);
}

EVENTLOOP2DEF void ev_set_task_data(ev_task *task, void *data)
{
    if (!task) task = ev_current();
    task->data = data;
}

EVENTLOOP2DEF void ev_set_task_data_to_transport_handle_cb(ev_task *task, intptr_t (*cb)(void *))
{
    if (!task) task = ev_current();
    task->data_to_transport_handle = cb;
}

EVENTLOOP2DEF void ev_set_task_flags(ev_task *task, int flags)
{
    if (!task) task = ev_current();
    task->wait_flags = flags;
}

EVENTLOOP2DEF void ev_set_task_timeout(ev_task *task, int timeout_ms)
{
    if (!task) task = ev_current();
    task->timeout = timeout_ms;
}

EVENTLOOP2DEF int ev_yield(void)
{
    ev_task *task = ev_current();
    int ret;
    assert(!(task->flags & EV_TASK_F_INIT) && "ev_end_init() should be called before any yield");
    co_switch(task->loop->env);
    ret = task->wake_flags;
    task->wake_flags = 0;
    return ret;
}

EVENTLOOP2DEF int ev_send_message(ev_task *dst, void *msg)
{
    ev_task *sender = ev_current();
    int old_flags = sender->wait_flags;
    sender->message.data = msg;
    sender->message.partner = dst;
    sender->wait_flags = EV_F_SEND_MESSAGE;

    while (sender->wait_flags & EV_F_SEND_MESSAGE) {
        int flags = ev_yield();
        if (flags & EV_F_SEND_MESSAGE) break;
        if (flags & (EV_F_TIMEOUT|EV_F_ERROR)) {
            sender->wait_flags = old_flags;
            sender->message.partner = NULL;
            return -1;
        }
    }

    sender->wait_flags = old_flags;
    return 0;
}

EVENTLOOP2DEF void *ev_recv_message(void)
{
    ev_task *receiver = ev_current();
    void *msg;
    int old_flags = receiver->wait_flags;
    receiver->wait_flags = EV_F_RECV_MESSAGE;

    while (receiver->wait_flags & EV_F_RECV_MESSAGE) {
        int flags = ev_yield();
        if (flags & EV_F_RECV_MESSAGE) break;
        if (flags & (EV_F_TIMEOUT|EV_F_ERROR)) {
            receiver->wait_flags = old_flags;
            return NULL;
        }
    }

    msg = receiver->message.data;
    receiver->wait_flags = old_flags;
    receiver->message.data = NULL;
    receiver->message.partner = NULL;
    return msg;
}

EVENTLOOP2DEF void *ev_pull_message(void)
{
    ev_task *task = ev_current();
    void *msg = NULL;
    if (task->wake_flags & EV_F_RECV_MESSAGE) {
        msg = task->message.data;
        task->wake_flags &= ~EV_F_RECV_MESSAGE;
        task->wait_flags &= ~EV_F_RECV_MESSAGE;
        task->message.data = NULL;
        task->message.partner = NULL;
    }
    return msg;
}

EVENTLOOP2DEF void *ev_get_task_data(ev_task *task)
{
    if (!task) task = ev_current();
    return task->data;
}

EVENTLOOP2DEF int ev_get_task_flags(ev_task *task)
{
    if (!task) task = ev_current();
    return task->wake_flags;
}

EVENTLOOP2DEF void ev_set_task_cleanup_cb(ev_task *task, void (*cleanup)(ev_task *))
{
    if (!task) task = ev_current();
    task->cleanup = cleanup;
}

EVENTLOOP2DEF void ev_notify(ev_task *task)
{
    task->wake_flags |= EV_F_NOTIFY;
}

EVENTLOOP2DEF void eloop_set_ator(event_loop *ev, void *ator, void *(*alloc)(void *, size_t), void (*free)(void *, void *, size_t))
{
    ev->ator = ator;
    ev->alloc = alloc;
    ev->free = free;
}

EVENTLOOP2DEF void *ev_get_task_base(ev_task *task)
{
    return EV_TASK_BASE(task);
}

EVENTLOOP2DEF size_t ev_get_task_size(ev_task *task)
{
    return task->size;
}

EVENTLOOP2DEF void ev_enable_stub(ev_task *task)
{
    task->flags |= EV_TASK_F_STUB;
}

EVENTLOOP2DEF event_loop *ev_get_task_loop(ev_task *task)
{
    if (!task) task = ev_current();
    return task->loop;
}

EVENTLOOP2DEF void ev_end_init(void)
{
    ev_task *task = ev_current();
    assert((task->flags & EV_TASK_F_INIT) && "ev_run_init() should be called right after creating of the task");
    co_switch(task->parent);
    assert(!(task->flags & EV_TASK_F_INIT));
}

EVENTLOOP2DEF void ev_run_init(ev_task *task)
{
    task->parent = co_active();
    task->flags |= EV_TASK_F_INIT;
    co_switch(task->env);
    task->flags &= ~EV_TASK_F_INIT;
}

EVENTLOOP2DEF int ev_get_task_timeout(ev_task *task)
{
    return task->timeout;
}

EVENTLOOP2DEF ev_task *ev_current(void)
{
    cothread_t active = co_active();
    return (void*)((char*)active - sizeof(ev_task));
}

#endif /* EVENT_LOOP2_IMPLEMENTATION */
