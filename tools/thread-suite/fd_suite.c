// fd-suite: many open files through libc (mlibc in InstantOS). RLIMIT_NOFILE
// (1024 at first, EMFILE at it, raised to 10000 and kept by fork and exec),
// fds well past the old 256 that read, write, dup2, fcntl(F_DUPFD), poll and
// select, the lowest free fd taken again, threads opening at the same time.
// Run by tools/run-thread-suite.sh; exits 0 only if everything passed.
// "fd-suite --child FD LIMIT" is the exec half: FD must still be open and
// RLIMIT_NOFILE still LIMIT.
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/select.h>
#include <sys/wait.h>
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

static int set_nofile(rlim_t cur, rlim_t max) {
    struct rlimit r = {cur, max};
    return setrlimit(RLIMIT_NOFILE, &r);
}

static int g_pipe[2];
static int g_fds[12000];
static int g_count;

static void close_all(void) {
    for (int i = 0; i < g_count; i++) close(g_fds[i]);
    g_count = 0;
}

static void test_limit(void) {
    section("RLIMIT_NOFILE");
    struct rlimit r;
    getrlimit(RLIMIT_NOFILE, &r);
    checkv(r.rlim_cur == 1024 && r.rlim_max == 65536, "it starts at 1024, 65536", (long)r.rlim_cur);
    checkv(sysconf(_SC_OPEN_MAX) == 1024, "sysconf(_SC_OPEN_MAX) says 1024", sysconf(_SC_OPEN_MAX));
    pipe(g_pipe);
    int fd, last = -1;
    while ((fd = dup(g_pipe[0])) >= 0) {
        g_fds[g_count++] = fd;
        last = fd;
        if (g_count > 2000) break;
    }
    checkv(fd < 0 && errno == EMFILE, "dup fails with EMFILE at the limit", fd);
    checkv(last == 1023, "the last fd is 1023", last);
    errno = 0;
    checkv(dup2(g_pipe[0], 1024) < 0 && errno == EBADF, "dup2 to 1024 (the limit): EBADF", errno);
    checkv(fcntl(g_pipe[0], F_DUPFD, 1024) < 0 && errno == EINVAL, "F_DUPFD from the limit: EINVAL", errno);
    close_all();

    checkv(set_nofile(20000, 10000) < 0 && errno == EINVAL, "setrlimit with cur > max: EINVAL", errno);
    checkv(set_nofile(1024, 70000) < 0 && errno == EPERM, "setrlimit above 65536: EPERM", errno);
    checkv(set_nofile(10000, 65536) == 0, "setrlimit(10000)", errno);
    checkv(sysconf(_SC_OPEN_MAX) == 10000, "sysconf follows it", sysconf(_SC_OPEN_MAX));
}

static void test_many(void) {
    section("thousands of fds");
    int ok = 1;
    for (int i = 0; i < 9000; i++) {
        int fd = dup(i % 2 ? g_pipe[1] : g_pipe[0]);
        if (fd < 0) { ok = 0; break; }
        if (g_count && fd != g_fds[g_count - 1] + 1) ok = 0;   // each the lowest free one
        g_fds[g_count++] = fd;
    }
    checkv(ok && g_count == 9000, "9000 fds, each the next number", g_count);
    int high_w = g_fds[g_count - 1];   // odd i: write ends
    int high_r = g_fds[g_count - 2];
    checkv(high_w > 9000, "fds past 9000", high_w);
    char c = 0;
    checkv(write(high_w, "a", 1) == 1 && read(high_r, &c, 1) == 1 && c == 'a', "read and write through them", c);

    int mid = g_fds[500];
    close(mid);
    int again = dup(g_pipe[0]);
    checkv(again == mid, "the lowest free fd is taken again", again);
    g_fds[500] = again;

    checkv(dup2(g_pipe[1], 9999) == 9999, "dup2 to 9999", errno);
    checkv(write(9999, "b", 1) == 1 && read(g_pipe[0], &c, 1) == 1 && c == 'b', "... works", c);
    close(9999);
    int dupfd = fcntl(g_pipe[0], F_DUPFD, 9500);
    checkv(dupfd == 9500, "F_DUPFD from 9500 gives 9500", dupfd);
    close(dupfd);

    // poll on 2000 of them, with data waiting in the pipe.
    write(g_pipe[1], "p", 1);
    struct pollfd* pfds = calloc(2000, sizeof *pfds);
    for (int i = 0; i < 2000; i++) {
        pfds[i].fd = g_fds[g_count - 2 - 2 * i];   // read ends from the top down
        pfds[i].events = POLLIN;
    }
    int n = poll(pfds, 2000, 1000);
    checkv(n == 2000 && (pfds[1999].revents & POLLIN), "poll on 2000 fds", n);
    free(pfds);
    // select on fds up to FD_SETSIZE.
    fd_set set;
    FD_ZERO(&set);
    int maxfd = 0;
    for (int i = 0; i < g_count && g_fds[i] < FD_SETSIZE; i += 2) {
        FD_SET(g_fds[i], &set);
        if (g_fds[i] > maxfd) maxfd = g_fds[i];
    }
    struct timeval tv = {1, 0};
    n = select(maxfd + 1, &set, NULL, NULL, &tv);
    checkv(n > 400 && FD_ISSET(g_fds[0], &set), "select on 500 fds", n);
    read(g_pipe[0], &c, 1);
    struct timeval nap = {0, 30000};
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    select(0, NULL, NULL, NULL, &nap);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    long ms = (t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_nsec - t0.tv_nsec) / 1000000;
    checkv(ms >= 25, "select with no fds sleeps (ms)", ms);

    // fork and exec keep them, and the limit.
    pid_t pid = fork();
    if (pid == 0) {
        _exit(write(high_w, "f", 1) == 1 ? 0 : 1);
    }
    int st = 0;
    waitpid(pid, &st, 0);
    checkv(WIFEXITED(st) && WEXITSTATUS(st) == 0 && read(g_pipe[0], &c, 1) == 1 && c == 'f',
           "a fork child writes through fd 9000+", st);
    pid = fork();
    if (pid == 0) {
        char fdText[16];
        snprintf(fdText, sizeof fdText, "%d", high_w);
        execl("/bin/fd-suite", "fd-suite", "--child", fdText, "10000", (char*)NULL);
        _exit(99);
    }
    waitpid(pid, &st, 0);
    checkv(WIFEXITED(st) && WEXITSTATUS(st) == 0, "an exec'd program keeps fd 9000+ and the limit", st);
    checkv(read(g_pipe[0], &c, 1) == 1 && c == 'e', "... and writes through it", c);
    close_all();

    // The limit lowered under open fds: they stay, new ones stop at it.
    int keep = dup2(g_pipe[0], 5000);
    checkv(set_nofile(100, 65536) == 0, "setrlimit(100) with fd 5000 open", errno);
    checkv(fcntl(keep, F_GETFD) >= 0, "fd 5000 is still open", keep);
    while ((g_fds[g_count] = dup(g_pipe[0])) >= 0) g_count++;
    checkv(errno == EMFILE && g_count > 80 && g_count < 100 && g_fds[g_count - 1] == 99, "new fds stop at 100", g_count);
    close_all();
    close(keep);
    set_nofile(10000, 65536);
}

#define THREADS 8
#define PER_THREAD 1000
static int g_thread_fds[THREADS][PER_THREAD];
static void* opener(void* arg) {
    int* out = arg;
    for (int i = 0; i < PER_THREAD; i++) out[i] = dup(g_pipe[0]);
    return NULL;
}
static int cmp_int(const void* a, const void* b) {
    return *(const int*)a - *(const int*)b;
}

static void test_threads(void) {
    section("threads opening at once");
    pthread_t t[THREADS];
    for (int i = 0; i < THREADS; i++) pthread_create(&t[i], NULL, opener, g_thread_fds[i]);
    for (int i = 0; i < THREADS; i++) pthread_join(t[i], NULL);
    static int all[THREADS * PER_THREAD];
    int n = 0, bad = 0;
    for (int i = 0; i < THREADS; i++) {
        for (int j = 0; j < PER_THREAD; j++) {
            if (g_thread_fds[i][j] < 0) bad++;
            else all[n++] = g_thread_fds[i][j];
        }
    }
    qsort(all, n, sizeof all[0], cmp_int);
    int dups = 0;
    for (int i = 1; i < n; i++) dups += all[i] == all[i - 1];
    checkv(bad == 0 && n == THREADS * PER_THREAD, "8 threads get 1000 fds each", bad);
    checkv(dups == 0, "no fd given twice", dups);
    for (int i = 0; i < n; i++) close(all[i]);
}

int main(int argc, char** argv) {
    if (argc == 4 && strcmp(argv[1], "--child") == 0) {
        int fd = atoi(argv[2]);
        struct rlimit r;
        getrlimit(RLIMIT_NOFILE, &r);
        if ((long)r.rlim_cur != atol(argv[3])) return 2;
        return write(fd, "e", 1) == 1 ? 0 : 1;
    }
    test_limit();
    test_many();
    test_threads();
    printf("fd-suite: %d of %d passed\n", g_total - g_failed, g_total);
    return g_failed ? 1 : 0;
}
