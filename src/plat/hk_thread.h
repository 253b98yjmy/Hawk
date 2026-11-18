/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Threads, locks, and handing work to the main (UI) thread. */
#ifndef HK_THREAD_H
#define HK_THREAD_H

#include "base/hk_common.h"

typedef struct hk_thread hk_thread;
typedef void (*hk_thread_fn)(void *arg);

hk_thread *hk_thread_start(const char *name, hk_thread_fn fn, void *arg);
void hk_thread_join(hk_thread *t);

/* A mutex and a condition variable (no allocation; zero-initialized structs are ready to use). */
typedef struct hk_mutex {
    void *opaque;
} hk_mutex;

typedef struct hk_cond {
    void *opaque;
} hk_cond;

void hk_mutex_lock(hk_mutex *m);
void hk_mutex_unlock(hk_mutex *m);
void hk_cond_wait(hk_cond *c, hk_mutex *m);
void hk_cond_signal(hk_cond *c);
void hk_cond_broadcast(hk_cond *c);

/*
 * Runs fn(arg) on the main thread from its event loop. Safe to call from any thread.
 * Tasks run in the order they were posted.
 */
void hk_main_post(void (*fn)(void *arg), void *arg);

#endif
