/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "plat/hk_thread.h"
#include "base/hk_mem.h"
#include "base/hk_vec.h"

#include <windows.h>

#include "hk_win32_internal.h"

struct hk_thread {
    HANDLE handle;
    hk_thread_fn fn;
    void *arg;
};

static DWORD WINAPI thread_main(LPVOID p)
{
    hk_thread *t = p;
    t->fn(t->arg);
    return 0;
}

hk_thread *hk_thread_start(const char *name, hk_thread_fn fn, void *arg)
{
    hk_thread *t = hk_alloc_zero(sizeof *t);
    t->fn = fn;
    t->arg = arg;
    t->handle = CreateThread(NULL, 0, thread_main, t, 0, NULL);
    HK_ASSERT(t->handle != NULL);
    if (name) {
        wchar_t wname[64];
        int n = MultiByteToWideChar(CP_UTF8, 0, name, -1, wname, 64);
        if (n > 0) SetThreadDescription(t->handle, wname);
    }
    return t;
}

void hk_thread_join(hk_thread *t)
{
    if (!t) return;
    WaitForSingleObject(t->handle, INFINITE);
    CloseHandle(t->handle);
    hk_free(t, sizeof *t);
}

/* SRWLOCK and CONDITION_VARIABLE are pointer-sized and zero-initialized. */
_Static_assert(sizeof(SRWLOCK) == sizeof(void *), "SRWLOCK size");
_Static_assert(sizeof(CONDITION_VARIABLE) == sizeof(void *), "CONDITION_VARIABLE size");

void hk_mutex_lock(hk_mutex *m)
{
    AcquireSRWLockExclusive((PSRWLOCK)&m->opaque);
}

void hk_mutex_unlock(hk_mutex *m)
{
    ReleaseSRWLockExclusive((PSRWLOCK)&m->opaque);
}

void hk_cond_wait(hk_cond *c, hk_mutex *m)
{
    SleepConditionVariableSRW((PCONDITION_VARIABLE)&c->opaque, (PSRWLOCK)&m->opaque, INFINITE, 0);
}

void hk_cond_signal(hk_cond *c)
{
    WakeConditionVariable((PCONDITION_VARIABLE)&c->opaque);
}

void hk_cond_broadcast(hk_cond *c)
{
    WakeAllConditionVariable((PCONDITION_VARIABLE)&c->opaque);
}

/* --- main thread task queue ---------------------------------------------------------- */

typedef struct task {
    void (*fn)(void *);
    void *arg;
} task;

static hk_mutex g_lock;
static HK_VEC(task) g_tasks;
static DWORD g_main_thread_id;
static bool g_wake_pending;

void hk_win32_main_queue_init(void)
{
    g_main_thread_id = GetCurrentThreadId();
}

void hk_main_post(void (*fn)(void *arg), void *arg)
{
    hk_mutex_lock(&g_lock);
    task t = {fn, arg};
    hk_vec_push(&g_tasks, t);
    bool wake = !g_wake_pending;
    g_wake_pending = true;
    hk_mutex_unlock(&g_lock);
    if (wake) PostThreadMessageW(g_main_thread_id, HK_WM_TASKS, 0, 0);
}

void hk_win32_run_main_tasks(void)
{
    for (;;) {
        hk_mutex_lock(&g_lock);
        g_wake_pending = false;
        if (g_tasks.len == 0) {
            hk_mutex_unlock(&g_lock);
            return;
        }
        /* Take the whole batch so tasks may post new tasks without deadlocking. */
        HK_VEC(task) batch = {g_tasks.data, g_tasks.len, g_tasks.cap};
        g_tasks.data = NULL;
        g_tasks.len = g_tasks.cap = 0;
        hk_mutex_unlock(&g_lock);
        for (uint32_t i = 0; i < batch.len; i++) batch.data[i].fn(batch.data[i].arg);
        hk_vec_free(&batch);
    }
}
