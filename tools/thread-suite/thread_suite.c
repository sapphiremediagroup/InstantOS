// thread-suite: every way a pthread can wait, on mlibc in InstantOS (they sleep
// on kernel futexes). Prints "  FAIL: ..." lines and a summary; exits 0 only if
// everything passed. Run by tools/run-thread-suite.sh.
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <semaphore.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <threads.h>
#include <time.h>
#include <unistd.h>

static int g_total, g_failed;
static void checkv(int ok, const char* what, long got) {
    g_total++;
    if (!ok) {
        g_failed++;
        printf("  FAIL: %s (got %ld)\n", what, got);
    }
    fflush(stdout);
}
#define check(cond, what) checkv((cond), (what), 0)
static void section(const char* name) {
    printf("== %s\n", name);
    fflush(stdout);
}

static int64_t now_ns(clockid_t clock) {
    struct timespec t;
    clock_gettime(clock, &t);
    return t.tv_sec * 1000000000LL + t.tv_nsec;
}
static struct timespec at(clockid_t clock, int64_t ms) {
    int64_t t = now_ns(clock) + ms * 1000000;
    return (struct timespec){t / 1000000000, t % 1000000000};
}
static void sleep_ms(int ms) {
    struct timespec t = {ms / 1000, (ms % 1000) * 1000000L};
    nanosleep(&t, NULL);
}
static uint64_t tsc(void) {
    uint32_t a, d;
    __asm__ volatile("rdtsc" : "=a"(a), "=d"(d));
    return ((uint64_t)d << 32) | a;
}
#define MAXT 32
static pthread_t g_threads[MAXT];
static void start(int n, void* (*fn)(void*)) {
    for (long i = 0; i < n; i++) {
        int e = pthread_create(&g_threads[i], NULL, fn, (void*)i);
        if (e) {
            printf("  FAIL: pthread_create %s\n", strerror(e));
            exit(1);
        }
    }
}
static long join_all(int n) {
    long sum = 0;
    for (int i = 0; i < n; i++) {
        void* r = NULL;
        pthread_join(g_threads[i], &r);
        sum += (long)r;
    }
    return sum;
}

// ---- ids ----
static void* id_thread(void* arg) {
    (void)arg;
    return (void*)(long)getpid();
}
static void* fork_thread(void* arg) {   // the child belongs to the process, not the thread
    (void)arg;
    pid_t child = fork();
    if (child == 0) {
        _exit(getppid() > 0 ? 21 : 1);
    }
    return (void*)(long)child;
}
static void* wait_thread(void* arg) {
    int st = 0;
    pid_t r = waitpid((pid_t)(long)arg, &st, 0);
    return (void*)(long)(r == (pid_t)(long)arg && WIFEXITED(st) ? WEXITSTATUS(st) : -1);
}
static void test_ids(void) {
    section("ids, fork and wait in threads");
    pthread_t t;
    void* r;
    pthread_create(&t, NULL, id_thread, NULL);
    pthread_join(t, &r);
    checkv((long)r == getpid(), "getpid is the process's in a thread", (long)r);
    pthread_create(&t, NULL, fork_thread, NULL);
    pthread_join(t, &r);   // the thread that forked is gone
    int st = 0;
    pid_t child = (pid_t)(long)r;
    pid_t w = waitpid(child, &st, 0);
    checkv(w == child && WIFEXITED(st) && WEXITSTATUS(st) == 21, "the process waits for a child a dead thread forked", w == child ? st : -errno);
    child = fork();
    if (child == 0) {
        sleep_ms(50);
        _exit(33);
    }
    pthread_create(&t, NULL, wait_thread, (void*)(long)child);
    pthread_join(t, &r);
    checkv((long)r == 33, "a thread waits for the main thread's child", (long)r);
    checkv(pthread_kill(pthread_self(), 0) == 0, "pthread_kill(self, 0)", 0);
}

// ---- mutexes ----
static pthread_mutex_t m1 = PTHREAD_MUTEX_INITIALIZER;
static long g_count;
enum { LOCKERS = 8, LOCKS = 20000 };
static void* locker(void* arg) {
    for (int i = 0; i < LOCKS; i++) {
        pthread_mutex_lock(&m1);
        long v = g_count;
        if ((i & 255) == (int)(long)arg) sched_yield();
        g_count = v + 1;
        pthread_mutex_unlock(&m1);
    }
    return NULL;
}
static void* trylocker(void* arg) {
    (void)arg;
    long n = 0;
    while (n < 5000) {
        if (pthread_mutex_trylock(&m1) == 0) {
            g_count++;
            n++;
            pthread_mutex_unlock(&m1);
        } else {
            sched_yield();
        }
    }
    return NULL;
}
static atomic_int g_flag;
static void* hold_100ms(void* arg) {
    pthread_mutex_lock(arg);
    atomic_store(&g_flag, 1);
    sleep_ms(100);
    pthread_mutex_unlock(arg);
    return NULL;
}
static void* unlock_foreign(void* arg) {
    return (void*)(long)pthread_mutex_unlock(arg);
}
static void* trylock_foreign(void* arg) {
    int e = pthread_mutex_trylock(arg);
    if (e == 0) pthread_mutex_unlock(arg);
    return (void*)(long)e;
}
static void test_mutexes(void) {
    section("mutexes");
    int64_t t0 = now_ns(CLOCK_MONOTONIC);
    start(LOCKERS, locker);
    join_all(LOCKERS);
    checkv(g_count == (long)LOCKERS * LOCKS, "8 threads x 20000 under a mutex", g_count);
    printf("  (%lld ms)\n", (long long)(now_ns(CLOCK_MONOTONIC) - t0) / 1000000);
    g_count = 0;
    start(4, trylocker);
    join_all(4);
    checkv(g_count == 20000, "trylock", g_count);

    uint64_t best = ~0ULL;
    for (int k = 0; k < 5; k++) {
        uint64_t c0 = tsc();
        for (int i = 0; i < 10000; i++) {
            pthread_mutex_lock(&m1);
            pthread_mutex_unlock(&m1);
        }
        uint64_t c = (tsc() - c0) / 10000;
        if (c < best) best = c;
    }
    printf("  (lock and unlock without contention: %llu TSC ticks)\n", (unsigned long long)best);

    pthread_t t;
    void* r;
    atomic_store(&g_flag, 0);
    pthread_create(&t, NULL, hold_100ms, &m1);
    while (!atomic_load(&g_flag)) sleep_ms(1);
    struct timespec deadline = at(CLOCK_REALTIME, 30);
    t0 = now_ns(CLOCK_MONOTONIC);
    int e = pthread_mutex_timedlock(&m1, &deadline);
    int64_t d = (now_ns(CLOCK_MONOTONIC) - t0) / 1000;
    checkv(e == ETIMEDOUT, "timedlock of a held mutex -> ETIMEDOUT", e);
    checkv(d >= 29000 && d < 95000, "after 30 ms (us)", d);
    deadline = at(CLOCK_REALTIME, 5000);
    e = pthread_mutex_timedlock(&m1, &deadline);
    checkv(e == 0, "timedlock gets it once it is free", e);
    if (e == 0) pthread_mutex_unlock(&m1);
    pthread_join(t, NULL);

    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_t rec;
    pthread_mutex_init(&rec, &attr);
    check(pthread_mutex_lock(&rec) == 0 && pthread_mutex_lock(&rec) == 0 && pthread_mutex_trylock(&rec) == 0, "recursive: 3 times");
    pthread_create(&t, NULL, trylock_foreign, &rec);
    pthread_join(t, &r);
    checkv((long)r == EBUSY, "recursive: another thread gets EBUSY", (long)r);
    pthread_create(&t, NULL, unlock_foreign, &rec);
    pthread_join(t, &r);
    checkv((long)r == EPERM, "recursive: another thread's unlock -> EPERM", (long)r);
    pthread_mutex_unlock(&rec);
    pthread_mutex_unlock(&rec);
    pthread_create(&t, NULL, trylock_foreign, &rec);
    pthread_join(t, &r);
    checkv((long)r == EBUSY, "recursive: still held after 2 of 3 unlocks", (long)r);
    pthread_mutex_unlock(&rec);
    pthread_create(&t, NULL, trylock_foreign, &rec);
    pthread_join(t, &r);
    checkv((long)r == 0, "recursive: free after the third", (long)r);

    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_ERRORCHECK);
    pthread_mutex_t ec;
    pthread_mutex_init(&ec, &attr);
    e = pthread_mutex_unlock(&ec);
    checkv(e == EPERM, "errorcheck: unlock of a free mutex -> EPERM", e);
    pthread_mutex_lock(&ec);
    e = pthread_mutex_lock(&ec);
    checkv(e == EDEADLK, "errorcheck: lock twice -> EDEADLK", e);
    pthread_create(&t, NULL, unlock_foreign, &ec);
    pthread_join(t, &r);
    checkv((long)r == EPERM, "errorcheck: another thread's unlock -> EPERM", (long)r);
    pthread_create(&t, NULL, trylock_foreign, &ec);
    pthread_join(t, &r);
    checkv((long)r == EBUSY, "errorcheck: and it is still held", (long)r);
    checkv(pthread_mutex_unlock(&ec) == 0, "errorcheck: unlock", 0);
    pthread_mutex_destroy(&ec);
    pthread_mutex_destroy(&rec);
}

// ---- condition variables ----
static pthread_mutex_t cm = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv = PTHREAD_COND_INITIALIZER, cv2 = PTHREAD_COND_INITIALIZER;
static int g_ready, g_waiting, g_woken;
static void* cond_waiter(void* arg) {
    (void)arg;
    pthread_mutex_lock(&cm);
    g_waiting++;
    while (!g_ready) pthread_cond_wait(&cv, &cm);
    if (g_ready > 0 && g_ready < 1000) g_ready--;
    g_woken++;
    pthread_mutex_unlock(&cm);
    return NULL;
}
static void wait_for(int* v, int want) {
    for (int i = 0; i < 5000; i++) {
        pthread_mutex_lock(&cm);
        int now = *v;
        pthread_mutex_unlock(&cm);
        if (now == want) return;
        sleep_ms(1);
    }
}
enum { ITEMS = 20000, QUEUE = 8 };
static int g_queue[QUEUE], g_head, g_used, g_made, g_taken;
static long g_sum;
static void* producer(void* arg) {
    (void)arg;
    for (;;) {
        pthread_mutex_lock(&cm);
        while (g_used == QUEUE) pthread_cond_wait(&cv2, &cm);
        if (g_made == ITEMS) {
            pthread_cond_broadcast(&cv);
            pthread_mutex_unlock(&cm);
            return NULL;
        }
        g_queue[(g_head + g_used++) % QUEUE] = ++g_made;
        pthread_cond_signal(&cv);
        pthread_mutex_unlock(&cm);
    }
}
static void* consumer(void* arg) {
    (void)arg;
    for (;;) {
        pthread_mutex_lock(&cm);
        while (g_used == 0 && g_made < ITEMS) pthread_cond_wait(&cv, &cm);
        if (g_used == 0) {
            pthread_cond_broadcast(&cv2);
            pthread_mutex_unlock(&cm);
            return NULL;
        }
        g_sum += g_queue[g_head];
        g_head = (g_head + 1) % QUEUE;
        g_used--;
        g_taken++;
        pthread_cond_signal(&cv2);
        pthread_mutex_unlock(&cm);
    }
}
static int g_turn;
static void* ping(void* arg) {
    int me = (int)(long)arg, lost = 0;
    pthread_mutex_lock(&cm);
    for (int i = 0; i < 5000; i++) {
        while (g_turn != me) {
            struct timespec d = at(CLOCK_REALTIME, 5000);
            if (pthread_cond_timedwait(me ? &cv2 : &cv, &cm, &d) == ETIMEDOUT && g_turn != me) {
                lost = 1;
                g_turn = me;
            }
        }
        g_turn = !me;
        pthread_cond_signal(me ? &cv : &cv2);
    }
    pthread_mutex_unlock(&cm);
    return (void*)(long)lost;
}
// broadcast, then destroy and free at once: the waiters may still be leaving
struct gate {
    pthread_mutex_t m;
    pthread_cond_t c;
    int open, in;
};
static struct gate* g_gate;
static pthread_mutex_t gm = PTHREAD_MUTEX_INITIALIZER;
static void* gate_waiter(void* arg) {
    (void)arg;
    pthread_mutex_lock(&gm);
    struct gate* g = g_gate;
    g->in++;
    while (!g->open) pthread_cond_wait(&g->c, &gm);
    pthread_mutex_unlock(&gm);
    return NULL;
}
static void test_conds(void) {
    section("condition variables");
    g_ready = g_waiting = g_woken = 0;
    start(8, cond_waiter);
    wait_for(&g_waiting, 8);
    pthread_mutex_lock(&cm);
    g_ready = 1;
    pthread_cond_signal(&cv);
    pthread_mutex_unlock(&cm);
    wait_for(&g_woken, 1);
    sleep_ms(20);
    checkv(g_woken == 1, "signal lets one of 8 go on", g_woken);
    pthread_mutex_lock(&cm);
    g_ready = 1000;
    pthread_cond_broadcast(&cv);
    pthread_mutex_unlock(&cm);
    join_all(8);
    checkv(g_woken == 8, "broadcast the rest", g_woken);

    start(3, consumer);
    pthread_t p[3];
    for (int i = 0; i < 3; i++) pthread_create(&p[i], NULL, producer, NULL);
    for (int i = 0; i < 3; i++) pthread_join(p[i], NULL);
    join_all(3);
    checkv(g_taken == ITEMS && g_sum == (long)ITEMS * (ITEMS + 1) / 2, "bounded queue, 3 producers, 3 consumers", g_taken);

    int64_t t0 = now_ns(CLOCK_MONOTONIC);
    start(2, ping);
    checkv(join_all(2) == 0, "10000 turns, no signal lost", 0);
    printf("  (%lld ns a turn)\n", (long long)(now_ns(CLOCK_MONOTONIC) - t0) / 10000);

    pthread_mutex_lock(&cm);
    for (int k = 0; k < 2; k++) {
        clockid_t clock = k ? CLOCK_MONOTONIC : CLOCK_REALTIME;
        pthread_condattr_t ca;
        pthread_cond_t c;
        pthread_condattr_init(&ca);
        pthread_condattr_setclock(&ca, clock);
        pthread_cond_init(&c, &ca);
        struct timespec d = at(clock, 40);
        t0 = now_ns(CLOCK_MONOTONIC);
        int e = pthread_cond_timedwait(&c, &cm, &d);
        int64_t us = (now_ns(CLOCK_MONOTONIC) - t0) / 1000;
        checkv(e == ETIMEDOUT, k ? "timedwait (monotonic) -> ETIMEDOUT" : "timedwait (realtime) -> ETIMEDOUT", e);
        checkv(us >= 39000 && us < 140000, "after 40 ms (us)", us);
        d = at(clock, -20);
        e = pthread_cond_timedwait(&c, &cm, &d);
        checkv(e == ETIMEDOUT, "a deadline in the past -> ETIMEDOUT", e);
        pthread_cond_destroy(&c);
    }
    pthread_mutex_unlock(&cm);

    uint64_t c0 = tsc();
    for (int i = 0; i < 10000; i++) pthread_cond_signal(&cv);
    printf("  (a signal nobody waits for: %llu TSC ticks)\n", (unsigned long long)(tsc() - c0) / 10000);

    for (int round = 0; round < 200; round++) {
        g_gate = calloc(1, sizeof(*g_gate));
        pthread_cond_init(&g_gate->c, NULL);
        start(4, gate_waiter);
        for (;;) {
            pthread_mutex_lock(&gm);
            int in = g_gate->in;
            pthread_mutex_unlock(&gm);
            if (in == 4) break;
            sched_yield();
        }
        pthread_mutex_lock(&gm);
        g_gate->open = 1;
        pthread_cond_broadcast(&g_gate->c);
        struct gate* g = g_gate;
        g_gate = NULL;
        pthread_cond_destroy(&g->c);   // the mutex is still held, as POSIX's example has it
        pthread_mutex_unlock(&gm);
        (void)g;                       // freed once all have left the loop that reads it
        join_all(4);
        free(g);
    }
    check(1, "200 times broadcast and destroy");
}

// ---- semaphores ----
static sem_t sa, sb;
static void* sem_ping(void* arg) {
    for (int i = 0; i < 10000; i++) {
        if (arg) {
            sem_wait(&sb);
            sem_post(&sa);
        } else {
            sem_post(&sb);
            sem_wait(&sa);
        }
    }
    return NULL;
}
static atomic_long g_taken_sem;
static void* sem_taker(void* arg) {
    (void)arg;
    for (int i = 0; i < 2000; i++) {
        while (sem_wait(&sa) != 0) {
        }
        atomic_fetch_add(&g_taken_sem, 1);
    }
    return NULL;
}
static void on_signal(int s) { (void)s; }
static void* sem_eintr(void* arg) {
    atomic_store(&g_flag, 1);
    int r = sem_wait(arg);
    return (void*)(long)(r == -1 ? errno : 0);
}
static void test_semaphores(void) {
    section("semaphores");
    sem_init(&sa, 0, 0);
    sem_init(&sb, 0, 0);
    start(2, sem_ping);
    join_all(2);
    check(1, "10000 turns");
    sem_init(&sa, 0, 0);
    start(6, sem_taker);
    for (int i = 0; i < 12000; i++) {
        sem_post(&sa);
        if ((i & 127) == 0) sched_yield();
    }
    join_all(6);
    checkv(atomic_load(&g_taken_sem) == 12000, "12000 posts, 6 takers", atomic_load(&g_taken_sem));
    int v = -1;
    sem_getvalue(&sa, &v);
    checkv(v == 0, "value 0 after all are taken", v);
    checkv(sem_trywait(&sa) == -1 && errno == EAGAIN, "trywait of 0 -> EAGAIN", errno);
    sem_post(&sa);
    sem_post(&sa);
    sem_getvalue(&sa, &v);
    checkv(v == 2, "value 2", v);
    checkv(sem_trywait(&sa) == 0, "trywait takes one, also after contention", errno);
    checkv(sem_wait(&sa) == 0, "wait takes the other", errno);
    struct timespec d = at(CLOCK_REALTIME, 30);
    int64_t t0 = now_ns(CLOCK_MONOTONIC);
    int r = sem_timedwait(&sa, &d);
    int64_t us = (now_ns(CLOCK_MONOTONIC) - t0) / 1000;
    checkv(r == -1 && errno == ETIMEDOUT, "timedwait -> ETIMEDOUT", errno);
    checkv(us >= 29000 && us < 130000, "after 30 ms (us)", us);
    struct sigaction act = {.sa_handler = on_signal}, old;
    sigaction(SIGUSR1, &act, &old);
    pthread_t t;
    void* e;
    atomic_store(&g_flag, 0);
    pthread_create(&t, NULL, sem_eintr, &sa);
    while (!atomic_load(&g_flag)) sleep_ms(1);
    sleep_ms(20);
    checkv(pthread_kill(t, SIGUSR1) == 0, "pthread_kill a waiting thread", 0);
    pthread_join(t, &e);
    checkv((long)e == EINTR, "sem_wait interrupted by a signal -> EINTR", (long)e);
    sigaction(SIGUSR1, &old, NULL);
}

// ---- rwlocks ----
static pthread_rwlock_t rw = PTHREAD_RWLOCK_INITIALIZER;
static atomic_int g_readers, g_writers, g_most, g_bad;
static void* rw_user(void* arg) {
    unsigned seed = (unsigned)(long)arg + 1;
    for (int i = 0; i < 4000; i++) {
        seed = seed * 1664525u + 1013904223u;
        if ((seed >> 16) % 8) {
            pthread_rwlock_rdlock(&rw);
            int n = atomic_fetch_add(&g_readers, 1) + 1;
            if (n > atomic_load(&g_most)) atomic_store(&g_most, n);
            if (atomic_load(&g_writers)) atomic_store(&g_bad, 1);
            if ((i & 63) == 0) sched_yield();
            atomic_fetch_sub(&g_readers, 1);
        } else {
            pthread_rwlock_wrlock(&rw);
            if (atomic_fetch_add(&g_writers, 1) || atomic_load(&g_readers)) atomic_store(&g_bad, 1);
            g_count++;
            atomic_fetch_sub(&g_writers, 1);
        }
        pthread_rwlock_unlock(&rw);
    }
    return NULL;
}
static void* rw_holder(void* arg) {
    if (arg) pthread_rwlock_wrlock(&rw);
    else pthread_rwlock_rdlock(&rw);
    atomic_store(&g_flag, 1);
    sleep_ms(120);
    pthread_rwlock_unlock(&rw);
    return NULL;
}
static void test_rwlocks(void) {
    section("rwlocks");
    start(8, rw_user);
    join_all(8);
    check(!atomic_load(&g_bad), "readers and writers keep apart (8 threads x 4000)");
    for (long w = 0; w < 2; w++) {
        pthread_t t;
        atomic_store(&g_flag, 0);
        pthread_create(&t, NULL, rw_holder, (void*)w);
        while (!atomic_load(&g_flag)) sleep_ms(1);
        struct timespec d = at(CLOCK_REALTIME, 30);
        int e = pthread_rwlock_timedwrlock(&rw, &d);
        checkv(e == ETIMEDOUT, w ? "timedwrlock against a writer -> ETIMEDOUT" : "timedwrlock against a reader -> ETIMEDOUT", e);
        e = pthread_rwlock_tryrdlock(&rw);
        checkv(w ? e == EBUSY : e == 0, w ? "tryrdlock against a writer -> EBUSY" : "a second reader is let in", e);
        if (e == 0) pthread_rwlock_unlock(&rw);
        d = at(CLOCK_REALTIME, 5000);
        e = w ? pthread_rwlock_timedrdlock(&rw, &d) : pthread_rwlock_wrlock(&rw);
        checkv(e == 0, "it is had once the holder lets go", e);
        if (e == 0) pthread_rwlock_unlock(&rw);
        pthread_join(t, NULL);
    }
}

// ---- barriers, once ----
static pthread_barrier_t bar;
static atomic_int g_phase, g_serial;
static void* barrier_user(void* arg) {
    (void)arg;
    for (int i = 0; i < 1000; i++) {
        atomic_fetch_add(&g_phase, 1);
        int r = pthread_barrier_wait(&bar);
        if (r == PTHREAD_BARRIER_SERIAL_THREAD) atomic_fetch_add(&g_serial, 1);
        if (atomic_load(&g_phase) < 6 * (i + 1)) atomic_store(&g_bad, 1);
        pthread_barrier_wait(&bar);
    }
    return NULL;
}
static pthread_once_t once = PTHREAD_ONCE_INIT;
static int g_inits;
static void slow_init(void) {
    sleep_ms(60);
    g_inits++;
}
static void* once_user(void* arg) {
    (void)arg;
    pthread_once(&once, slow_init);
    return (void*)(long)g_inits;
}
static void test_barrier_once(void) {
    section("barrier and once");
    atomic_store(&g_bad, 0);
    pthread_barrier_init(&bar, NULL, 6);
    start(6, barrier_user);
    join_all(6);
    checkv(atomic_load(&g_serial) == 1000 && !atomic_load(&g_bad), "6 threads x 1000 rounds, one serial thread each", atomic_load(&g_serial));
    checkv(pthread_barrier_destroy(&bar) == 0, "destroy", 0);
    start(8, once_user);
    long sum = join_all(8);
    checkv(g_inits == 1 && sum == 8, "once: 8 threads wait for one init", sum);
}

// ---- join, detach, exit ----
static pthread_key_t key;
static atomic_int g_destroyed, g_cleanups;
static void key_gone(void* v) {
    (void)v;
    atomic_fetch_add(&g_destroyed, 1);
}
static void cleanup(void* arg) {
    (void)arg;
    atomic_fetch_add(&g_cleanups, 1);
}
static void* returns(void* arg) { return (char*)arg + 100; }
static void* sleeps_returns(void* arg) {
    sleep_ms(80);
    return (char*)arg + 7;
}
static void* exits(void* arg) {
    pthread_setspecific(key, (void*)1);
    pthread_cleanup_push(cleanup, NULL);
    pthread_exit((char*)arg + 1);
    pthread_cleanup_pop(0);
    return NULL;
}
static void* detached(void* arg) {
    (void)arg;
    atomic_fetch_add(&g_flag, 1);
    return NULL;
}
static __thread int tls_value = 41;
static void* tls_user(void* arg) {
    (void)arg;
    return (void*)(long)(tls_value++);
}
static void test_join(void) {
    section("join, detach, exit");
    pthread_t t;
    void* r;
    pthread_create(&t, NULL, returns, (void*)5);
    sleep_ms(30);
    pthread_join(t, &r);
    checkv((long)r == 105, "join a thread that ended", (long)r);
    pthread_create(&t, NULL, sleeps_returns, (void*)1);
    int64_t t0 = now_ns(CLOCK_MONOTONIC);
    pthread_join(t, &r);
    checkv((long)r == 8, "join a thread that runs", (long)r);
    checkv(now_ns(CLOCK_MONOTONIC) - t0 > 60000000, "which waits for it", 0);
    pthread_key_create(&key, key_gone);
    pthread_create(&t, NULL, exits, (void*)9);
    pthread_join(t, &r);
    checkv((long)r == 10 && atomic_load(&g_destroyed) == 1 && atomic_load(&g_cleanups) == 1,
           "pthread_exit runs the cleanup handlers and key destructors", (long)r);
    atomic_store(&g_flag, 0);
    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
    for (int i = 0; i < 50; i++) {
        pthread_create(&t, &a, detached, NULL);
        pthread_create(&t, NULL, detached, NULL);
        pthread_detach(t);
    }
    for (int i = 0; i < 3000 && atomic_load(&g_flag) < 100; i++) sleep_ms(1);
    checkv(atomic_load(&g_flag) == 100, "100 detached threads run", atomic_load(&g_flag));
    long sum = 0;
    for (int i = 0; i < 20; i++) {
        pthread_create(&t, NULL, tls_user, NULL);
        pthread_join(t, &r);
        sum += (long)r;
    }
    checkv(sum == 41 * 20 && tls_value == 41, "each thread has its own fresh TLS", sum);
    t0 = now_ns(CLOCK_MONOTONIC);
    int ok = 1;
    for (int i = 0; i < 2000; i++) {
        if (pthread_create(&t, NULL, returns, (void*)(long)i) != 0) {
            ok = 0;
            break;
        }
        pthread_join(t, &r);
        if ((long)r != i + 100) ok = 0;
    }
    check(ok, "2000 threads one after the other");
    printf("  (%lld us a thread)\n", (long long)(now_ns(CLOCK_MONOTONIC) - t0) / 2000 / 1000);
}

// ---- what a thread leaves behind ----
static long used_mb(void) {
    return (sysconf(_SC_PHYS_PAGES) - sysconf(_SC_AVPHYS_PAGES)) / 256;
}
static void* uses_stack(void* arg) {
    volatile char big[20000];
    for (int i = 0; i < 20000; i += 512) big[i] = (char)(long)arg;
    return (void*)(long)(big[512] == (char)(long)arg);
}
static void* counts_down(void* arg) {
    uses_stack(arg);
    atomic_fetch_add(&g_flag, 1);
    return NULL;
}
static int too_deep(int n) {
    volatile char frame[4000];
    frame[0] = (char)n;
    return too_deep(n + 1) + frame[0];
}
static void* overflows(void* arg) {
    (void)arg;
    return (void*)(long)too_deep(0);
}
static void test_memory(void) {
    section("what a thread leaves behind");
    pthread_t t;
    void* r;
    long before = used_mb(), sum = 0;
    int64_t t0 = now_ns(CLOCK_MONOTONIC);
    for (long i = 0; i < 6000; i++) {
        if (pthread_create(&t, NULL, uses_stack, (void*)(i & 63))) break;
        pthread_join(t, &r);
        sum += (long)r;
    }
    checkv(sum == 6000, "6000 threads made and joined", sum);
    long grown = used_mb() - before;
    checkv(grown < 12, "which leave no memory behind (MiB)", grown);
    printf("  (%lld us a thread, %ld MiB more)\n", (long long)(now_ns(CLOCK_MONOTONIC) - t0) / 6000 / 1000, grown);
    // (The kernel's heap grows by 16 MiB at a time and keeps that: one step is
    // no leak; 8000 threads that left their stacks behind would be 300 MiB.)
    before = used_mb();
    for (int round = 0; round < 2; round++) {
        atomic_store(&g_flag, 0);
        int made = 0;
        for (int i = 0; i < 4000; i++) {
            if (pthread_create(&t, NULL, counts_down, NULL)) break;
            made++;
            pthread_detach(t);
            while (made - atomic_load(&g_flag) > 20) sched_yield();
        }
        for (int i = 0; i < 5000 && atomic_load(&g_flag) < made; i++) sleep_ms(1);
        checkv(made == 4000 && atomic_load(&g_flag) == 4000, "4000 detached threads", atomic_load(&g_flag));
        grown = used_mb() - before;
        printf("  (%ld MiB more)\n", grown);
    }
    checkv(grown < 40, "which leave no memory behind either (MiB)", grown);
    before = used_mb();
    int ok = 1;
    for (int round = 0; round < 30; round++) {
        start(MAXT, uses_stack);
        if (join_all(MAXT) != MAXT) ok = 0;
    }
    check(ok, "30 times 32 threads at once");
    grown = used_mb() - before;
    checkv(grown < 20, "nor do they (MiB)", grown);
    // detached after it ended
    for (int i = 0; i < 50; i++) {
        pthread_create(&t, NULL, uses_stack, NULL);
        sleep_ms(1);
        if (pthread_detach(t)) ok = 0;
    }
    check(ok, "50 threads detached after they ended");
    pthread_create(&t, NULL, uses_stack, NULL);
    sleep_ms(30);
    int e = pthread_kill(t, 0);
    checkv(e == ESRCH, "pthread_kill of an ended thread -> ESRCH", e);
    pthread_join(t, NULL);
    // a stack of the program's own
    pthread_attr_t a;
    pthread_attr_init(&a);
    char* mine = malloc(256 * 1024 + 4096);
    char* aligned = (char*)(((uintptr_t)mine + 4095) & ~4095UL);
    pthread_attr_setstack(&a, aligned, 256 * 1024);
    for (int i = 0; i < 20; i++) {
        e = pthread_create(&t, &a, uses_stack, (void*)3L);
        if (e) break;
        pthread_join(t, &r);
    }
    checkv(e == 0 && (long)r == 1, "a stack the program made, 20 times", e);
    free(mine);
    pthread_attr_destroy(&a);
    // other sizes
    pthread_attr_init(&a);
    for (size_t size = 64 * 1024; size <= 16 * 1024 * 1024; size *= 4) {
        pthread_attr_setstacksize(&a, size);
        e = pthread_create(&t, &a, uses_stack, (void*)2L);
        if (e) break;
        pthread_join(t, &r);
    }
    checkv(e == 0, "stacks of 64 KiB to 16 MiB", e);
    // the guard page
    pid_t child = fork();
    if (child == 0) {
        pthread_create(&t, NULL, overflows, NULL);
        pthread_join(t, NULL);
        _exit(0);
    }
    int st = 0;
    waitpid(child, &st, 0);
    printf("  (the crash above is the stack overflow test's)\n");
    checkv(!(WIFEXITED(st) && WEXITSTATUS(st) == 0), "a thread that overflows its stack crashes", st);
}

// ---- C11 ----
static mtx_t cmtx;
static cnd_t ccnd;
static once_flag cflag = ONCE_FLAG_INIT;
static int c_count, c_ready, c_inits;
static void c_init(void) { c_inits++; }
static int c_worker(void* arg) {
    call_once(&cflag, c_init);
    mtx_lock(&cmtx);
    while (!c_ready) cnd_wait(&ccnd, &cmtx);
    c_count++;
    mtx_unlock(&cmtx);
    return (int)(long)arg * 3;
}
static void test_c11(void) {
    section("C11 threads");
    mtx_init(&cmtx, mtx_plain);
    cnd_init(&ccnd);
    thrd_t t[4];
    for (long i = 0; i < 4; i++) thrd_create(&t[i], c_worker, (void*)i);
    sleep_ms(20);
    mtx_lock(&cmtx);
    c_ready = 1;
    cnd_broadcast(&ccnd);
    mtx_unlock(&cmtx);
    int sum = 0;
    for (int i = 0; i < 4; i++) {
        int r = 0;
        thrd_join(t[i], &r);
        sum += r;
    }
    checkv(sum == 18 && c_count == 4 && c_inits == 1, "thrd, mtx, cnd, call_once", sum);
    mtx_lock(&cmtx);
    struct timespec d = at(CLOCK_REALTIME, 20);
    int e = cnd_timedwait(&ccnd, &cmtx, &d);
    checkv(e == thrd_timedout, "cnd_timedwait -> thrd_timedout", e);
    mtx_unlock(&cmtx);
}

// ---- signals and cancellation against waits ----
static atomic_int g_hits;
static void count_signal(int s) {
    (void)s;
    atomic_fetch_add(&g_hits, 1);
}
static void* waits_for_mutex(void* arg) {
    atomic_store(&g_flag, 1);
    int e = pthread_mutex_lock(arg);
    if (e == 0) pthread_mutex_unlock(arg);
    return (void*)(long)e;
}
static void* waits_for_cond(void* arg) {
    (void)arg;
    pthread_mutex_lock(&cm);
    atomic_store(&g_flag, 1);
    while (!g_ready) pthread_cond_wait(&cv, &cm);
    pthread_mutex_unlock(&cm);
    return (void*)77;
}
static void unlock_cm(void* arg) {
    pthread_mutex_unlock(arg);
    atomic_fetch_add(&g_cleanups, 1);
}
static void* cancelled_cond(void* arg) {
    (void)arg;
    pthread_mutex_lock(&cm);
    atomic_store(&g_flag, 1);
    pthread_cleanup_push(unlock_cm, &cm);
    for (;;) pthread_cond_wait(&cv, &cm);
    pthread_cleanup_pop(0);
    return NULL;
}
static void* cancelled_loop(void* arg) {
    (void)arg;
    atomic_store(&g_flag, 1);
    for (;;) {
        pthread_testcancel();
        sched_yield();
    }
    return NULL;
}
static void* sleeps_long(void* arg) {
    (void)arg;
    sleep_ms(300);
    return NULL;
}
static void* cancelled_join(void* arg) {
    atomic_store(&g_flag, 1);
    pthread_join(*(pthread_t*)arg, NULL);
    return NULL;
}
static void test_signals(void) {
    section("signals and cancellation");
    struct sigaction act = {.sa_handler = count_signal}, old;
    sigaction(SIGUSR2, &act, &old);
    pthread_t t;
    void* r;
    atomic_store(&g_flag, 0);
    pthread_mutex_lock(&m1);
    pthread_create(&t, NULL, waits_for_mutex, &m1);
    while (!atomic_load(&g_flag)) sleep_ms(1);
    for (int i = 0; i < 5; i++) {
        sleep_ms(10);
        pthread_kill(t, SIGUSR2);
    }
    sleep_ms(20);
    checkv(atomic_load(&g_hits) == 5, "a thread waiting for a mutex runs its handlers", atomic_load(&g_hits));
    pthread_mutex_unlock(&m1);
    pthread_join(t, &r);
    checkv((long)r == 0, "and then gets the mutex", (long)r);
    g_ready = 0;
    atomic_store(&g_flag, 0);
    pthread_create(&t, NULL, waits_for_cond, NULL);
    while (!atomic_load(&g_flag)) sleep_ms(1);
    for (int i = 0; i < 5; i++) {
        sleep_ms(10);
        pthread_kill(t, SIGUSR2);
    }
    sleep_ms(20);
    checkv(atomic_load(&g_hits) == 10, "a thread in cond_wait too", atomic_load(&g_hits));
    pthread_mutex_lock(&cm);
    g_ready = 1;
    pthread_cond_signal(&cv);
    pthread_mutex_unlock(&cm);
    pthread_join(t, &r);
    checkv((long)r == 77, "and is then signalled", (long)r);
    sigaction(SIGUSR2, &old, NULL);

    atomic_store(&g_cleanups, 0);
    for (int i = 0; i < 20; i++) {
        atomic_store(&g_flag, 0);
        pthread_create(&t, NULL, cancelled_cond, NULL);
        while (!atomic_load(&g_flag)) sched_yield();
        if (i & 1) sleep_ms(5);
        int e = pthread_cancel(t);
        if (e) checkv(0, "pthread_cancel", e);
        pthread_join(t, &r);
        if (r != PTHREAD_CANCELED) checkv(0, "cancelled in cond_wait", (long)r);
    }
    checkv(atomic_load(&g_cleanups) == 20, "20 threads cancelled in cond_wait, mutex held in the cleanup", atomic_load(&g_cleanups));
    atomic_store(&g_flag, 0);
    pthread_create(&t, NULL, cancelled_loop, NULL);
    while (!atomic_load(&g_flag)) sleep_ms(1);
    pthread_cancel(t);
    pthread_join(t, &r);
    check(r == PTHREAD_CANCELED, "cancelled at pthread_testcancel");
    pthread_t slow;
    pthread_create(&slow, NULL, sleeps_long, NULL);
    atomic_store(&g_flag, 0);
    pthread_create(&t, NULL, cancelled_join, &slow);
    while (!atomic_load(&g_flag)) sleep_ms(1);
    sleep_ms(10);
    int64_t t0 = now_ns(CLOCK_MONOTONIC);
    pthread_cancel(t);
    pthread_join(t, &r);
    checkv(r == PTHREAD_CANCELED && now_ns(CLOCK_MONOTONIC) - t0 < 150000000, "cancelled in pthread_join", (long)r);
    pthread_join(slow, NULL);
    // none of them may be counted as a waiter any more
    pthread_cond_destroy(&cv);
    check(1, "cond_destroy after the cancellations");
}

// ---- fork with threads, malloc, stdio ----
static void* allocator(void* arg) {
    unsigned seed = (unsigned)(long)arg + 7;
    void* keep[32] = {0};
    for (int i = 0; i < 20000; i++) {
        seed = seed * 1664525u + 1013904223u;
        unsigned k = (seed >> 8) % 32;
        free(keep[k]);
        size_t n = 1 + (seed >> 12) % 2000;
        keep[k] = malloc(n);
        memset(keep[k], (int)k, n);
    }
    for (int k = 0; k < 32; k++) free(keep[k]);
    return NULL;
}
static FILE* g_file;
static void* writer(void* arg) {
    for (int i = 0; i < 500; i++) fprintf(g_file, "line of thread %ld number %03d ends\n", (long)arg, i);
    return NULL;
}
static pthread_mutex_t fm = PTHREAD_MUTEX_INITIALIZER;
static void fork_prepare(void) { pthread_mutex_lock(&fm); }
static void fork_done(void) { pthread_mutex_unlock(&fm); }
static void test_libc(void) {
    section("malloc, stdio and fork with threads");
    start(8, allocator);
    join_all(8);
    check(1, "8 threads malloc and free");
    g_file = fopen("/tmp/thread-lines", "w");
    if (g_file) {
        start(4, writer);
        join_all(4);
        fclose(g_file);
        g_file = fopen("/tmp/thread-lines", "r");
        char line[128];
        int good = 0, bad = 0;
        while (fgets(line, sizeof line, g_file)) {
            if (strncmp(line, "line of thread ", 15) == 0 && strstr(line, " ends\n")) good++;
            else bad++;
        }
        fclose(g_file);
        checkv(good == 2000 && bad == 0, "4 threads print whole lines to one FILE", good);
    }
    pthread_atfork(fork_prepare, fork_done, fork_done);
    pid_t child = fork();
    if (child == 0) {
        int e = pthread_mutex_trylock(&fm);   // the child handler unlocked it
        if (e == 0) pthread_mutex_unlock(&fm);
        g_count = 0;
        start(4, trylocker);
        join_all(4);
        _exit(e == 0 && g_count == 20000 ? 51 : 1);
    }
    int st = 0;
    waitpid(child, &st, 0);
    checkv(WIFEXITED(st) && WEXITSTATUS(st) == 51, "fork: atfork handlers, and threads in the child", st);
}

// ---- waiting costs nothing ----
static void* idle(void* arg) {
    (void)arg;
    pthread_mutex_lock(&cm);
    g_waiting++;
    while (!g_ready) pthread_cond_wait(&cv2, &cm);
    pthread_mutex_unlock(&cm);
    return NULL;
}
static uint64_t syscall_cost(void) {
    uint64_t best = ~0ULL;
    for (int k = 0; k < 5; k++) {
        uint64_t t0 = tsc();
        for (int i = 0; i < 20000; i++) getppid();
        uint64_t t = (tsc() - t0) / 20000;
        if (t < best) best = t;
    }
    return best;
}
static void test_idle(void) {
    section("waiting costs nothing");
    uint64_t alone = syscall_cost();
    g_ready = g_waiting = 0;
    start(8, idle);
    wait_for(&g_waiting, 8);
    sleep_ms(20);
    uint64_t beside = syscall_cost();
    pthread_mutex_lock(&cm);
    g_ready = 1000;
    pthread_cond_broadcast(&cv2);
    pthread_mutex_unlock(&cm);
    join_all(8);
    checkv(beside < 2 * alone, "a syscall beside 8 threads in cond_wait (TSC ticks)", (long)beside);
    printf("  (a syscall: %llu TSC ticks alone, %llu beside 8 waiting threads)\n", (unsigned long long)alone, (unsigned long long)beside);
}

int main(void) {
    test_ids();
    test_mutexes();
    test_conds();
    test_semaphores();
    test_rwlocks();
    test_barrier_once();
    test_join();
    test_memory();
    test_c11();
    test_signals();
    test_libc();
    test_idle();
    printf("thread-suite: %d of %d passed\n", g_total - g_failed, g_total);
    return g_failed ? 1 : 0;
}
