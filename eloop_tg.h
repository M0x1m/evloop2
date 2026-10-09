#ifndef ELOOP_TG_
#define ELOOP_TG_

#ifndef ELOOPTGDEF
#define ELOOPTGDEF
#endif

#include "eloop_http.h"

#include <stdint.h>

struct telegram_bot;
union telegram_event;

typedef void (*tg_command)(struct telegram_bot *);
typedef void (*tg_handler)(struct telegram_bot *, union telegram_event);

enum telegram_event_kind {
    TG_EVENT_TEXT_MESSAGE,
    __TG_EVENT_KIND_COUNT
};

union telegram_event {
    enum telegram_event_kind kind;
    struct {
        enum telegram_event_kind kind;
        int64_t sender;
        const char *message;
    } text_message;
};

ELOOPTGDEF struct telegram_bot *tg_new_bot(const char *token, void *userdata);
ELOOPTGDEF void tg_register_command(struct telegram_bot *, const char *, tg_command, const char *);
ELOOPTGDEF void tg_set_event_handler(struct telegram_bot *, enum telegram_event_kind, tg_handler);
ELOOPTGDEF void tg_set_socks5_proxy(struct telegram_bot *, const char *host, int port);
ELOOPTGDEF void tg_dispatch(struct telegram_bot *);

ELOOPTGDEF void tg_send_message(struct telegram_bot *, int64_t, const char *, int);

#endif /* ELOOP_TG_ */

#ifdef ELOOP_TG_IMPLEMENTAION

#include <assert.h>
#include <string.h>
#include <stdlib.h>

#define ELOOP_TG_DA_APPEND(da, x)                                       \
    do {                                                                \
        if ((da)->count >= (da)->capacity) {                            \
            (da)->capacity = (da)->capacity ? (da)->capacity * 2 : 1;   \
            (da)->items = realloc((da)->items, sizeof *(da)->items * (da)->capacity); \
            assert((da)->items);                                        \
        }                                                               \
        (da)->items[(da)->count++] = (x);                               \
    } while (0)

typedef void (*tg__task)(struct telegram_bot *, void *);

struct telegram_command {
    const char *name;
    const char *desc;
    tg_command handler;
};

struct telegram_commands {
    struct telegram_command *items;
    size_t count;
    size_t capacity;
};

struct telegram_bot {
    event_loop loop;
    const char *token;
    void *userdata;
    struct {
        const char *host;
        int port;
    } socks5_proxy;
    struct telegram_commands commands;
    tg_handler event_handlers[__TG_EVENT_KIND_COUNT];
};

ELOOPTGDEF struct telegram_bot *tg_new_bot(const char *token, void *userdata)
{
    struct telegram_bot *bot = malloc(sizeof *bot);
    memset(bot, 0, sizeof *bot);
    eloop_init(&bot->loop);
    bot->token = token;
    bot->userdata = userdata;
    return bot;
}

ELOOPTGDEF void tg_register_command(struct telegram_bot *bot, const char *cmd_name, tg_command handler, const char *desc)
{
    struct telegram_command command;
    memset(&command, 0xcc, sizeof command);
    command.name = cmd_name;
    command.desc = desc;
    command.handler = handler;
    da_append(&bot->commands, command);
}

ELOOPTGDEF void tg_set_event_handler(struct telegram_bot *bot, enum telegram_event_kind kind, tg_handler handler)
{
    assert(kind < __TG_EVENT_KIND_COUNT);
    bot->event_handlers[kind] = handler;
}

struct tg__task_wrapper_closure {
    tg__task task;
    void *data;
    struct telegram_bot *bot;
};

static void tg__task_wrapper(void)
{
    struct tg__task_wrapper_closure c;
    memcpy(&c, ev_get_data(), sizeof c);
    ev_end_init();
    c.task(c.bot, c.data);
}

static void tg__run_task(struct telegram_bot *bot, tg__task task, void *data)
{
    ev_task *task;
    struct tg__task_wrapper_closure c;

    task = eloop_add(&bot->loop, tg__task_wrapper);
    ev_set_task_data(task, &c);
    c.task = task;
    c.data = data;
    ev_run_init(task);
}

static void tg__set_command_descriptions(struct telegram_bot *bot, void *data)
{
    (void) data;

}

ELOOPTGDEF void tg_dispatch(struct telegram_bot *bot)
{
    tg__run_task(bot, tg__set_command_descriptions, NULL);
    eloop_run(&bot->loop);
}

#endif /* ELOOP_TG_IMPLEMENTAION */
