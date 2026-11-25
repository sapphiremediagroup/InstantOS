// Between Rust's std (x86_64-unknown-linux-musl) and mlibc, for the two types
// whose layouts differ (see tools/rust-port/README.md for the whole comparison):
//
//   pthread_attr_t   56 bytes in musl, 200 in mlibc: std's stack copy holds a
//                    pointer to a heap-allocated mlibc one instead.
//   struct sigaction handler, mask, flags, restorer in musl and glibc;
//                    handler, flags, restorer, mask (the kernel's order) in mlibc.
//
// Only Rust's references go through here: with -Wl,--wrap=NAME (programs,
// tools/rust-port/mlibc-cc), or with the symbols of a Rust static library
// renamed to __wrap_NAME (llvm-objcopy, tools/rust-port/redirect-archive.sh).
// Everything else keeps using mlibc's functions and its layouts.
#define _GNU_SOURCE
#include <pthread.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>

// Without --wrap (renamed symbols), the real function has its own name.
#ifdef RUST_COMPAT_DIRECT
#define REAL(name) name
#else
#define REAL(name) __real_##name
int __real_pthread_attr_init(pthread_attr_t *);
int __real_pthread_attr_destroy(pthread_attr_t *);
int __real_pthread_attr_getstack(const pthread_attr_t *, void **, size_t *);
int __real_pthread_attr_setstack(pthread_attr_t *, void *, size_t);
int __real_pthread_attr_getstacksize(const pthread_attr_t *, size_t *);
int __real_pthread_attr_setstacksize(pthread_attr_t *, size_t);
int __real_pthread_attr_getguardsize(const pthread_attr_t *, size_t *);
int __real_pthread_attr_setguardsize(pthread_attr_t *, size_t);
int __real_pthread_attr_getdetachstate(const pthread_attr_t *, int *);
int __real_pthread_attr_setdetachstate(pthread_attr_t *, int);
int __real_pthread_create(pthread_t *, const pthread_attr_t *, void *(*)(void *), void *);
int __real_pthread_getattr_np(pthread_t, pthread_attr_t *);
int __real_sigaction(int, const struct sigaction *, struct sigaction *);
#endif

// musl's pthread_attr_t (56 bytes, alignment 8). A zeroed or never initialised
// one has no mlibc attribute behind it yet.
typedef struct {
    pthread_attr_t *real;
    unsigned long pad[6];
} musl_attr;
_Static_assert(sizeof(musl_attr) == 56, "musl's pthread_attr_t");

static int real_attr(musl_attr *a, pthread_attr_t **out) {
    if (!a->real) {
        pthread_attr_t *made = malloc(sizeof *made);
        if (!made) return 12;   // ENOMEM
        int e = REAL(pthread_attr_init)(made);
        if (e) {
            free(made);
            return e;
        }
        a->real = made;
    }
    *out = a->real;
    return 0;
}

int __wrap_pthread_attr_init(musl_attr *a) {
    pthread_attr_t *real;
    a->real = NULL;
    return real_attr(a, &real);
}

int __wrap_pthread_attr_destroy(musl_attr *a) {
    if (!a->real) return 0;
    int e = REAL(pthread_attr_destroy)(a->real);
    free(a->real);
    a->real = NULL;
    return e;
}

#define GET1(fn, T)                                                            \
    int __wrap_##fn(const musl_attr *a, T *v) {                                \
        pthread_attr_t *real;                                                  \
        int e = real_attr((musl_attr *)a, &real);                              \
        return e ? e : REAL(fn)(real, v);                                      \
    }
#define SET1(fn, T)                                                            \
    int __wrap_##fn(musl_attr *a, T v) {                                       \
        pthread_attr_t *real;                                                  \
        int e = real_attr(a, &real);                                           \
        return e ? e : REAL(fn)(real, v);                                      \
    }
GET1(pthread_attr_getstacksize, size_t)
SET1(pthread_attr_setstacksize, size_t)
GET1(pthread_attr_getguardsize, size_t)
SET1(pthread_attr_setguardsize, size_t)
GET1(pthread_attr_getdetachstate, int)
SET1(pthread_attr_setdetachstate, int)

int __wrap_pthread_attr_getstack(const musl_attr *a, void **addr, size_t *size) {
    pthread_attr_t *real;
    int e = real_attr((musl_attr *)a, &real);
    return e ? e : REAL(pthread_attr_getstack)(real, addr, size);
}

int __wrap_pthread_attr_setstack(musl_attr *a, void *addr, size_t size) {
    pthread_attr_t *real;
    int e = real_attr(a, &real);
    return e ? e : REAL(pthread_attr_setstack)(real, addr, size);
}

int __wrap_pthread_create(pthread_t *t, const musl_attr *a, void *(*start)(void *), void *arg) {
    pthread_attr_t *real = NULL;
    if (a) {
        int e = real_attr((musl_attr *)a, &real);
        if (e) return e;
    }
    return REAL(pthread_create)(t, real, start, arg);
}

int __wrap_pthread_getattr_np(pthread_t t, musl_attr *a) {
    pthread_attr_t *real;
    a->real = NULL;
    int e = real_attr(a, &real);
    return e ? e : REAL(pthread_getattr_np)(t, real);
}

// musl's struct sigaction.
typedef struct {
    union {
        void (*handler)(int);
        void (*sigaction)(int, siginfo_t *, void *);
    } u;
    sigset_t mask;
    int flags;
    void (*restorer)(void);
} musl_sigaction;
_Static_assert(sizeof(musl_sigaction) == 152, "musl's struct sigaction");

int __wrap_sigaction(int sig, const musl_sigaction *act, musl_sigaction *old) {
    struct sigaction in, out;
    if (act) {
        memset(&in, 0, sizeof in);
        in.sa_handler = act->u.handler;
        in.sa_mask = act->mask;
        in.sa_flags = act->flags;
    }
    int r = REAL(sigaction)(sig, act ? &in : NULL, old ? &out : NULL);
    if (r == 0 && old) {
        memset(old, 0, sizeof *old);
        old->u.handler = out.sa_handler;
        old->mask = out.sa_mask;
        old->flags = (int)out.sa_flags;
    }
    return r;
}
