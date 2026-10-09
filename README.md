# Asynchronous event loop C library

## Usage example

```c
#define EVENT_LOOP2_IMPLEMENTATION
#include "event_loop2.h"

void task1()
{
    int i;
    for (i = 0; i < 10; ++i) {
        printf("Task1: %d\n", i);
        ev_yield();
    }
}

void task2()
{
    int i;
    for (i = 0; i < 10; ++i) {
        printf("Task2: %d\n", i);
        ev_yield();
    }
}

int main()
{
    event_loop loop;
    eloop_init(&loop);

    eloop_add(&loop, task1);
    eloop_add(&loop, task2);

    eloop_run(&loop);
    return 0;
}
```

Compilation:

```shell
$ cc -o example example.c -I./libco ./libco/libco.c
```

Running:

```shell
$ ./example
Task2: 0
Task1: 0
Task2: 1
Task1: 1
Task2: 2
Task1: 2
Task2: 3
Task1: 3
```

## Layers progress

Currently HTTP and Telegram layers being WIP.

Runtime (`event_loop2.h`), tcp/udp (`eloop_net.h`) and ssl wrapper are ready and maintained. Though not heavily tested yet.
