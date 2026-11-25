// signal-suite: signals through libc (mlibc in InstantOS). Masks name the right
// signals, handlers get siginfo_t, a signal reaches a thread that makes no
// syscalls (pthread_kill, asynchronous pthread_cancel, alarm), handlers leave
// the floating-point state alone, faults run their handlers (siglongjmp out,
// also from a stack overflow on the alternate stack), sigwait/sigtimedwait/
// sigsuspend, SA_RESTART, and wait reports deaths by signal.
// Run by tools/run-thread-suite.sh; exits 0 only if everything passed.
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <errno.h>
#include <pthread.h>
#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int g_total, g_failed;
static void checkv(int ok, const char* what, long got) {
    g_total++;
    if (!ok) {
        g_failed++;
        printf("  FAIL: %s (got %ld, errno %d %s)\n", what, got, errno, strerror(errno));
    }
    fflush(stdout);
}
static void section(const char* name) {
    printf("== %s\n", name);
    fflush(stdout);
}
static void sleep_ms(int ms) {
    struct timespec t = {ms / 1000, (ms % 1000) * 1000000L};
    nanosleep(&t, NULL);
}
static long now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000L + t.tv_nsec / 1000000L;
}
static void on(int sig, void (*fn)(int, siginfo_t*, void*), int flags) {
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = fn;
    sa.sa_flags = SA_SIGINFO | flags;
    sigemptyset(&sa.sa_mask);
    sigaction(sig, &sa, NULL);
}
static void dfl(int sig) {
    signal(sig, SIG_DFL);
}

static volatile sig_atomic_t g_hits;
static volatile int g_code, g_pid, g_uid, g_signo;
static volatile pthread_t g_where;
static void record(int sig, siginfo_t* si, void* uc) {
    (void)uc;
    g_signo = sig;
    g_code = si->si_code;
    g_pid = si->si_pid;
    g_uid = si->si_uid;
    g_where = pthread_self();
    g_hits++;
}
static void reset(void) {
    g_hits = 0;
    g_code = g_pid = g_uid = g_signo = -1;
    g_where = 0;
}

static void test_masks(void) {
    section("masks and siginfo");
    reset();
    on(SIGUSR1, record, 0);
    sigset_t set, old, pend;
    sigemptyset(&set);
    sigaddset(&set, SIGUSR1);
    sigprocmask(SIG_BLOCK, &set, &old);
    raise(SIGUSR1);
    checkv(g_hits == 0, "a blocked SIGUSR1 waits", g_hits);
    sigemptyset(&pend);
    sigpending(&pend);
    checkv(sigismember(&pend, SIGUSR1) == 1 && sigismember(&pend, SIGUSR2) == 0 &&
           sigismember(&pend, SIGSEGV) == 0, "sigpending names SIGUSR1 and nothing near it", 0);
    sigset_t cur;
    sigprocmask(SIG_BLOCK, NULL, &cur);
    checkv(sigismember(&cur, SIGUSR1) == 1 && sigismember(&cur, SIGSEGV) == 0 && sigismember(&cur, SIGUSR2) == 0,
           "sigprocmask reads back the signal it blocked", 0);
    sigprocmask(SIG_SETMASK, &old, NULL);
    checkv(g_hits == 1, "unblocking delivers it", g_hits);
    // mlibc's raise() is kill(getpid(), sig).
    checkv(g_code == SI_USER && g_pid == getpid(), "raise: SI_USER from this process", g_code);

    reset();
    kill(getpid(), SIGUSR1);
    checkv(g_hits == 1 && g_code == SI_USER && g_pid == getpid() && g_uid == (int)getuid(),
           "kill: SI_USER with the sender's pid and uid", g_code);

    // sa_mask holds a signal while the handler runs.
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = record;
    sa.sa_flags = SA_SIGINFO;
    sigemptyset(&sa.sa_mask);
    sigaddset(&sa.sa_mask, SIGUSR2);
    sigaction(SIGUSR1, &sa, NULL);
    struct sigaction back;
    sigaction(SIGUSR1, NULL, &back);
    checkv(sigismember(&back.sa_mask, SIGUSR2) == 1 && sigismember(&back.sa_mask, SIGUSR1) == 0,
           "sigaction reads back its sa_mask", 0);
    dfl(SIGUSR1);
}

static volatile int g_spin_stop;
static volatile int g_spin_started;
static void* spinner(void* arg) {
    (void)arg;
    g_spin_started = 1;
    while (!g_spin_stop) {
    }
    return NULL;
}

static void* async_cancel_me(void* arg) {
    (void)arg;
    int old;
    pthread_setcanceltype(PTHREAD_CANCEL_ASYNCHRONOUS, &old);
    g_spin_started = 1;
    for (;;) {
    }
    return NULL;
}

static void test_threads(void) {
    section("threads");
    pthread_t t;
    g_spin_stop = 0;
    g_spin_started = 0;
    pthread_create(&t, NULL, spinner, NULL);
    while (!g_spin_started) sleep_ms(1);
    // Installed after the thread started: dispositions belong to the process.
    reset();
    on(SIGUSR2, record, 0);
    pthread_kill(t, SIGUSR2);
    long t0 = now_ms();
    while (!g_hits && now_ms() - t0 < 2000) sleep_ms(1);
    checkv(g_hits == 1, "pthread_kill reaches a thread in a loop without syscalls", g_hits);
    checkv(pthread_equal(g_where, t), "... and runs the handler there", 0);
    checkv(g_code == SI_TKILL && g_pid == getpid(), "... with SI_TKILL", g_code);

    // kill() to the process: the thread that does not block it takes it.
    sigset_t set, old;
    sigemptyset(&set);
    sigaddset(&set, SIGUSR2);
    pthread_sigmask(SIG_BLOCK, &set, &old);
    reset();
    kill(getpid(), SIGUSR2);
    t0 = now_ms();
    while (!g_hits && now_ms() - t0 < 2000) sleep_ms(1);
    checkv(g_hits == 1 && pthread_equal(g_where, t), "a process signal goes to the thread that does not block it", g_hits);
    pthread_sigmask(SIG_SETMASK, &old, NULL);
    g_spin_stop = 1;
    pthread_join(t, NULL);
    dfl(SIGUSR2);

    // Asynchronous cancellation of a thread that never makes a syscall.
    g_spin_started = 0;
    pthread_create(&t, NULL, async_cancel_me, NULL);
    while (!g_spin_started) sleep_ms(1);
    t0 = now_ms();
    pthread_cancel(t);
    void* result = NULL;
    pthread_join(t, &result);
    checkv(result == PTHREAD_CANCELED, "pthread_cancel ends a looping thread (asynchronous)", (long)result);
    checkv(now_ms() - t0 < 2000, "... at once (ms)", now_ms() - t0);
}

// A loop of double arithmetic: handlers that use the vector registers too
// must not change its result.
static volatile double g_sink;
static void fp_handler(int sig, siginfo_t* si, void* uc) {
    (void)sig; (void)si; (void)uc;
    double x = 1.0;
    for (int i = 0; i < 200; i++) x = x * 1.0000001 + 0.5;
    g_sink = x;
    g_hits++;
}
static double fp_work(void) {
    double a = 0.0, b = 1.0;
    for (int i = 1; i < 6000000; i++) {
        a += b / (double)i;
        b = -b * 0.999999999;
    }
    return a;
}

static sigjmp_buf g_jump;
static void jump_out(int sig, siginfo_t* si, void* uc) {
    (void)sig; (void)uc;
    g_code = si->si_code;
    g_signo = sig;
    g_hits++;
    siglongjmp(g_jump, 1);
}

static void test_timer_and_fp(void) {
    section("timers, floating point, siglongjmp");
    double expect = fp_work();
    reset();
    on(SIGALRM, fp_handler, 0);
    struct itimerval it = {{0, 1000}, {0, 1000}};
    setitimer(ITIMER_REAL, &it, NULL);
    double got = fp_work();
    struct itimerval off = {{0, 0}, {0, 0}};
    setitimer(ITIMER_REAL, &off, NULL);
    checkv(g_hits >= 5, "SIGALRM interrupts a computation", g_hits);
    checkv(got == expect, "the computation's result is the same", (long)((got - expect) * 1e9));
    dfl(SIGALRM);

    // alarm() ends a loop that would run for ever; siglongjmp out restores the mask.
    reset();
    on(SIGALRM, jump_out, 0);
    volatile long spins = 0;
    long t0 = now_ms();
    if (sigsetjmp(g_jump, 1) == 0) {
        alarm(1);
        for (;;) spins++;
    }
    long took = now_ms() - t0;
    checkv(g_hits == 1 && took >= 900 && took < 3000, "alarm(1) stops an endless loop (ms)", took);
    sigset_t cur;
    sigprocmask(SIG_BLOCK, NULL, &cur);
    checkv(sigismember(&cur, SIGALRM) == 0, "siglongjmp out of the handler gives the mask back", 0);
    dfl(SIGALRM);
}

static volatile int g_deep;
static int recurse(int n) {
    volatile char pad[4096];
    pad[0] = (char)n;
    g_deep = n;
    return recurse(n + 1) + pad[0];
}

static void test_faults(void) {
    section("faults");
    reset();
    on(SIGSEGV, jump_out, 0);
    for (int round = 0; round < 2; round++) {
        if (sigsetjmp(g_jump, 1) == 0) {
            *(volatile int*)(uintptr_t)0x18 = 1;
            checkv(0, "a write to an unmapped address faults", 0);
        }
    }
    checkv(g_hits == 2 && g_signo == SIGSEGV && g_code == SEGV_MAPERR, "SIGSEGV twice, SEGV_MAPERR, recovered with siglongjmp", g_hits);

    // A write to a read-only page: SEGV_ACCERR.
    char* ro = mmap(NULL, 4096, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    reset();
    if (sigsetjmp(g_jump, 1) == 0) {
        ro[5] = 1;
    }
    checkv(g_hits == 1 && g_code == SEGV_ACCERR, "a write to a read-only page: SEGV_ACCERR", g_code);
    munmap(ro, 4096);

    reset();
    on(SIGFPE, jump_out, 0);
    volatile int zero = 0;
    volatile int r = 0;
    if (sigsetjmp(g_jump, 1) == 0) {
        r = 10 / zero;
    }
    checkv(g_hits == 1 && g_signo == SIGFPE && g_code == FPE_INTDIV, "dividing by zero: SIGFPE, FPE_INTDIV", g_code);
    (void)r;
    dfl(SIGFPE);

    // Running out of stack: the handler can only run on the alternate stack.
    stack_t ss;
    ss.ss_sp = malloc(65536);
    ss.ss_size = 65536;
    ss.ss_flags = 0;
    sigaltstack(&ss, NULL);
    reset();
    on(SIGSEGV, jump_out, SA_ONSTACK);
    pid_t pid = fork();
    if (pid == 0) {
        if (sigsetjmp(g_jump, 1) == 0) {
            recurse(0);
        }
        _exit(g_hits == 1 && g_deep > 100 ? 0 : 3);
    }
    int st = 0;
    waitpid(pid, &st, 0);
    checkv(WIFEXITED(st) && WEXITSTATUS(st) == 0, "a stack overflow is caught on the alternate stack", st);
    dfl(SIGSEGV);
    stack_t dis = {0};
    dis.ss_flags = SS_DISABLE;
    sigaltstack(&dis, NULL);
    free(ss.ss_sp);

    // No handler: wait says which signal it died of.
    pid = fork();
    if (pid == 0) {
        *(volatile int*)(uintptr_t)0x18 = 1;
        _exit(0);
    }
    waitpid(pid, &st, 0);
    checkv(WIFSIGNALED(st) && WTERMSIG(st) == SIGSEGV, "an unhandled fault: WIFSIGNALED, SIGSEGV", st);
    pid = fork();
    if (pid == 0) {
        raise(SIGTERM);
        _exit(0);
    }
    waitpid(pid, &st, 0);
    checkv(WIFSIGNALED(st) && WTERMSIG(st) == SIGTERM, "raise(SIGTERM): WIFSIGNALED, SIGTERM", st);
    pid = fork();
    if (pid == 0) {
        for (;;) {
        }
    }
    sleep_ms(30);
    long t0 = now_ms();
    kill(pid, SIGINT);
    waitpid(pid, &st, 0);
    checkv(WIFSIGNALED(st) && WTERMSIG(st) == SIGINT && now_ms() - t0 < 1000, "SIGINT ends a busy child at once", st);
    pid = fork();
    if (pid == 0) _exit(7);
    waitpid(pid, &st, 0);
    checkv(WIFEXITED(st) && WEXITSTATUS(st) == 7, "a normal exit is still WIFEXITED", st);
}

static void* send_later(void* arg) {
    sleep_ms(40);
    pthread_kill(*(pthread_t*)arg, SIGUSR1);
    return NULL;
}

static void test_waits(void) {
    section("sigwait, sigtimedwait, sigsuspend, SA_RESTART");
    sigset_t set, old;
    sigemptyset(&set);
    sigaddset(&set, SIGUSR1);
    pthread_sigmask(SIG_BLOCK, &set, &old);
    pthread_t self = pthread_self(), t;
    pthread_create(&t, NULL, send_later, &self);
    int sig = 0;
    int rc = sigwait(&set, &sig);
    pthread_join(t, NULL);
    checkv(rc == 0 && sig == SIGUSR1, "sigwait takes a signal sent later", sig);

    struct timespec ts = {0, 50 * 1000000L};
    long t0 = now_ms();
    rc = sigtimedwait(&set, NULL, &ts);
    checkv(rc == -1 && errno == EAGAIN && now_ms() - t0 >= 45, "sigtimedwait times out with EAGAIN", now_ms() - t0);
    pthread_kill(pthread_self(), SIGUSR1);
    siginfo_t si;
    memset(&si, 0, sizeof si);
    rc = sigwaitinfo(&set, &si);
    checkv(rc == SIGUSR1 && si.si_signo == SIGUSR1 && si.si_code == SI_TKILL, "sigwaitinfo fills siginfo_t", si.si_code);

    reset();
    on(SIGUSR1, record, 0);
    pthread_create(&t, NULL, send_later, &self);
    sigset_t none;
    sigemptyset(&none);
    rc = sigsuspend(&none);
    pthread_join(t, NULL);
    checkv(rc == -1 && errno == EINTR && g_hits == 1, "sigsuspend returns after the handler", g_hits);
    sigset_t cur;
    pthread_sigmask(SIG_BLOCK, NULL, &cur);
    checkv(sigismember(&cur, SIGUSR1) == 1, "sigsuspend gives the old mask back", 0);
    pthread_sigmask(SIG_SETMASK, &old, NULL);
    dfl(SIGUSR1);

    for (int restart = 1; restart >= 0; restart--) {
        int fds[2];
        pipe(fds);
        reset();
        on(SIGALRM, record, restart ? SA_RESTART : 0);
        struct itimerval once = {{0, 0}, {0, 30000}};
        setitimer(ITIMER_REAL, &once, NULL);
        pid_t pid = fork();
        if (pid == 0) {
            sleep_ms(150);
            write(fds[1], "x", 1);
            _exit(0);
        }
        char c = 0;
        errno = 0;
        ssize_t n = read(fds[0], &c, 1);
        int e = errno;
        waitpid(pid, NULL, 0);
        if (restart) {
            checkv(n == 1 && c == 'x' && g_hits == 1, "SA_RESTART: read goes on after the handler", n);
        } else {
            errno = e;
            checkv(n == -1 && e == EINTR && g_hits == 1, "without SA_RESTART: read fails EINTR", n);
        }
        close(fds[0]);
        close(fds[1]);
    }
    dfl(SIGALRM);
}

int main(void) {
    test_masks();
    test_threads();
    test_timer_and_fp();
    test_faults();
    test_waits();
    printf("signal-suite: %d of %d passed\n", g_total - g_failed, g_total);
    return g_failed ? 1 : 0;
}
