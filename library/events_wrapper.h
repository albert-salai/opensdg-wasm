#ifndef INTERNAL_EVENTS_WRAPPER_H
#define INTERNAL_EVENTS_WRAPPER_H

#include <semaphore.h>

typedef sem_t event_t;

static inline void event_init(event_t *ev)
{
    sem_init(ev, 0, 0);
}

static inline void event_destroy(event_t *ev)
{
    sem_destroy(ev);
}

static inline void event_wait(event_t *ev)
{
    sem_wait(ev);
}

static inline void event_post(event_t *ev)
{
    sem_post(ev);
}

#endif
