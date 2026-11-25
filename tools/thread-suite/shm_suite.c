// shm-suite: memory shared between processes through libc (mlibc in InstantOS):
// MAP_SHARED|MAP_ANONYMOUS, memfd_create, shm_open, passing a memfd over a
// socket, and pthread objects and semaphores that work between processes in it.
// Run by tools/run-thread-suite.sh; exits 0 only if everything passed.
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <semaphore.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#ifndef F_ADD_SEALS
#define F_ADD_SEALS 1033
#define F_GET_SEALS 1034
#endif
#ifndef MFD_CLOEXEC
#define MFD_CLOEXEC 1U
#define MFD_ALLOW_SEALING 2U
#endif

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
static int exit_of(pid_t pid) {
    int st = 0;
    if (waitpid(pid, &st, 0) != pid) return -1;
    return WIFEXITED(st) ? WEXITSTATUS(st) : 1000 + WTERMSIG(st);
}
static void sleep_ms(int ms) {
    struct timespec t = {ms / 1000, (ms % 1000) * 1000000L};
    nanosleep(&t, NULL);
}

static void test_basics(void) {
    section("mmap");
    int* a = mmap(NULL, 65536, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    checkv(a != MAP_FAILED, "MAP_SHARED|MAP_ANONYMOUS", (long)a);
    pid_t pid = fork();
    if (pid == 0) {
        a[0] = 5;
        a[10000] = 6;
        _exit(0);
    }
    exit_of(pid);
    checkv(a[0] == 5 && a[10000] == 6, "the child's writes are here", a[10000]);
    munmap(a, 65536);

    // A hint without MAP_FIXED must not replace what is there.
    char* p = mmap(NULL, 8192, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    p[0] = 'x';
    char* q = mmap(p, 8192, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    checkv(q != MAP_FAILED && q != p && p[0] == 'x', "a hint does not replace a mapping", p[0]);
    char* f = mmap(p, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    checkv(f == p && p[0] == 0, "MAP_FIXED does", (long)f);
    munmap(p, 8192);
    munmap(q, 8192);

    // PROT_NONE reserves; parts are opened with mprotect.
    p = mmap(NULL, 1 << 20, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    checkv(p != MAP_FAILED && mprotect(p + 65536, 4096, PROT_READ | PROT_WRITE) == 0, "reserve with PROT_NONE, open a page", (long)p);
    p[65536] = 1;
    pid = fork();
    if (pid == 0) {
        p[65536 + 4096] = 1;
        _exit(0);
    }
    int c = exit_of(pid);
    checkv(c != 0, "the page after it crashes", c);
    munmap(p, 1 << 20);
}

static void test_memfd(void) {
    section("memfd and shm_open");
    int fd = memfd_create("buffer", MFD_CLOEXEC);
    checkv(fd >= 0, "memfd_create", fd);
    checkv(ftruncate(fd, 100000) == 0, "ftruncate", 0);
    struct stat st;
    fstat(fd, &st);
    checkv(st.st_size == 100000 && S_ISREG(st.st_mode), "fstat", st.st_size);
    char* a = mmap(NULL, 100000, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    checkv(a != MAP_FAILED, "mmap", (long)a);
    strcpy(a + 50000, "through the mapping");
    char b[32] = {0};
    checkv(pread(fd, b, 19, 50000) == 19 && strcmp(b, "through the mapping") == 0, "pread sees it", 0);
    checkv(pwrite(fd, "written", 7, 99990) == 7 && memcmp(a + 99990, "written", 7) == 0, "the mapping sees pwrite", 0);
    checkv(lseek(fd, 0, SEEK_END) == 100000, "lseek to the end", lseek(fd, 0, SEEK_END));
    checkv((fcntl(fd, F_GETFD) & FD_CLOEXEC) != 0, "MFD_CLOEXEC", 0);
    munmap(a, 100000);
    close(fd);
    fd = memfd_create("sealed", MFD_ALLOW_SEALING);
    ftruncate(fd, 4096);
    checkv(fcntl(fd, F_ADD_SEALS, 2 | 4) == 0 && fcntl(fd, F_GET_SEALS) == 6, "seals", fcntl(fd, F_GET_SEALS));
    checkv(ftruncate(fd, 0) == -1 && errno == EPERM, "a sealed size -> EPERM", errno);
    close(fd);

    char name[64];
    snprintf(name, sizeof name, "/shm-%ld", (long)getpid());
    fd = shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0600);
    checkv(fd >= 0, "shm_open", fd);
    checkv(shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0600) == -1 && errno == EEXIST, "again with O_EXCL -> EEXIST", errno);
    ftruncate(fd, 8192);
    int fd2 = shm_open(name, O_RDWR, 0);
    int* x = mmap(NULL, 8192, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    int* y = mmap(NULL, 8192, PROT_READ | PROT_WRITE, MAP_SHARED, fd2, 0);
    x[1500] = 77;
    checkv(y[1500] == 77, "two opens are the same memory", y[1500]);
    checkv(shm_unlink(name) == 0, "shm_unlink", errno);
    checkv(shm_open(name, O_RDWR, 0) == -1 && errno == ENOENT, "the name is gone", errno);
    close(fd);
    close(fd2);
    y[0] = 88;
    checkv(x[0] == 88, "the memory is not", x[0]);
    munmap(x, 8192);
    munmap(y, 8192);
}

// A memfd sent to another process, which maps it (how a browser's processes
// hand each other images).
static void test_pass(void) {
    section("a memfd over a socket");
    int sv[2];
    checkv(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0, "socketpair", errno);
    pid_t pid = fork();
    if (pid == 0) {
        close(sv[0]);
        char byte, control[CMSG_SPACE(sizeof(int))];
        struct iovec iov = {&byte, 1};
        struct msghdr msg = {.msg_iov = &iov, .msg_iovlen = 1, .msg_control = control, .msg_controllen = sizeof control};
        if (recvmsg(sv[1], &msg, 0) != 1) _exit(1);
        struct cmsghdr* c = CMSG_FIRSTHDR(&msg);
        if (!c || c->cmsg_type != SCM_RIGHTS) _exit(2);
        int fd;
        memcpy(&fd, CMSG_DATA(c), sizeof fd);
        struct stat st;
        fstat(fd, &st);
        uint32_t* p = mmap(NULL, (size_t)st.st_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        close(fd);
        if (p == MAP_FAILED) _exit(3);
        for (size_t i = 0; i < (size_t)st.st_size / 4; i++) {
            if (p[i] != (uint32_t)(i * 2654435761u)) _exit(4);
            p[i] ^= 0xffffffffu;
        }
        write(sv[1], "k", 1);
        _exit(60);
    }
    close(sv[1]);
    const size_t size = 3 << 20;
    int fd = memfd_create("image", MFD_CLOEXEC);
    ftruncate(fd, (off_t)size);
    uint32_t* p = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    for (size_t i = 0; i < size / 4; i++) p[i] = (uint32_t)(i * 2654435761u);
    char byte = 'f', control[CMSG_SPACE(sizeof(int))] = {0};
    struct iovec iov = {&byte, 1};
    struct msghdr msg = {.msg_iov = &iov, .msg_iovlen = 1, .msg_control = control, .msg_controllen = sizeof control};
    struct cmsghdr* c = CMSG_FIRSTHDR(&msg);
    c->cmsg_level = SOL_SOCKET;
    c->cmsg_type = SCM_RIGHTS;
    c->cmsg_len = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(c), &fd, sizeof fd);
    checkv(sendmsg(sv[0], &msg, 0) == 1, "send it", errno);
    close(fd);
    checkv(read(sv[0], &byte, 1) == 1 && byte == 'k', "the other side is done", byte);
    int good = 1;
    for (size_t i = 0; i < size / 4; i++) {
        if (p[i] != ((uint32_t)(i * 2654435761u) ^ 0xffffffffu)) good = 0;
    }
    checkv(good, "3 MiB went there and came back changed", good);
    checkv(exit_of(pid) == 60, "the child's checks", 0);
    munmap(p, size);
    close(sv[0]);
}

struct shared {
    pthread_mutex_t m;
    pthread_cond_t c;
    pthread_rwlock_t rw;
    pthread_barrier_t bar;
    sem_t sem, back;
    long count, queued, taken;
    int turn, phase, bad;
};
static void test_sync(void) {
    section("pthread objects between processes");
    struct shared* s = mmap(NULL, sizeof *s, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    pthread_mutexattr_t ma;
    pthread_mutexattr_init(&ma);
    checkv(pthread_mutexattr_setpshared(&ma, PTHREAD_PROCESS_SHARED) == 0, "mutexattr_setpshared", 0);
    pthread_mutex_init(&s->m, &ma);
    pthread_condattr_t ca;
    pthread_condattr_init(&ca);
    pthread_condattr_setpshared(&ca, PTHREAD_PROCESS_SHARED);
    pthread_cond_init(&s->c, &ca);
    pthread_rwlockattr_t ra;
    pthread_rwlockattr_init(&ra);
    pthread_rwlockattr_setpshared(&ra, PTHREAD_PROCESS_SHARED);
    pthread_rwlock_init(&s->rw, &ra);
    pthread_barrierattr_t ba;
    pthread_barrierattr_init(&ba);
    pthread_barrierattr_setpshared(&ba, PTHREAD_PROCESS_SHARED);
    pthread_barrier_init(&s->bar, &ba, 4);
    checkv(sem_init(&s->sem, 1, 0) == 0 && sem_init(&s->back, 1, 0) == 0, "sem_init between processes", errno);

    pid_t kids[4];
    for (int k = 0; k < 4; k++) {
        kids[k] = fork();
        if (kids[k] == 0) {
            for (int i = 0; i < 5000; i++) {
                pthread_mutex_lock(&s->m);
                long v = s->count;
                if ((i & 255) == 0) sched_yield();
                s->count = v + 1;
                pthread_mutex_unlock(&s->m);
            }
            _exit(0);
        }
    }
    int good = 1;
    for (int k = 0; k < 4; k++) good &= exit_of(kids[k]) == 0;
    checkv(good && s->count == 20000, "4 processes x 5000 under a mutex", s->count);

    pid_t pid = fork();
    for (int i = 0, me = pid == 0; i < 2000; i++) {
        pthread_mutex_lock(&s->m);
        while (s->turn != me) {
            struct timespec d;
            clock_gettime(CLOCK_REALTIME, &d);
            d.tv_sec += 5;
            if (pthread_cond_timedwait(&s->c, &s->m, &d) == ETIMEDOUT) {
                s->bad++;
                break;
            }
        }
        s->turn = !me;
        pthread_cond_signal(&s->c);
        pthread_mutex_unlock(&s->m);
        if (s->bad) break;
    }
    if (pid == 0) _exit(0);
    exit_of(pid);
    checkv(s->bad == 0, "4000 turns on a condition variable", s->bad);

    pid = fork();
    for (int i = 0; i < 2000; i++) {
        if (pid == 0) {
            sem_wait(&s->sem);
            sem_post(&s->back);
        } else {
            sem_post(&s->sem);
            sem_wait(&s->back);
        }
    }
    if (pid == 0) _exit(0);
    exit_of(pid);
    checkv(1, "4000 turns on semaphores", 0);

    for (int k = 0; k < 3; k++) {
        kids[k] = fork();
        if (kids[k] == 0) {
            for (int i = 0; i < 300; i++) {
                __atomic_add_fetch(&s->phase, 1, __ATOMIC_SEQ_CST);
                pthread_barrier_wait(&s->bar);
                if (__atomic_load_n(&s->phase, __ATOMIC_SEQ_CST) < 4 * (i + 1)) s->bad = 1;
                pthread_barrier_wait(&s->bar);
            }
            _exit(0);
        }
    }
    for (int i = 0; i < 300; i++) {
        __atomic_add_fetch(&s->phase, 1, __ATOMIC_SEQ_CST);
        pthread_barrier_wait(&s->bar);
        pthread_barrier_wait(&s->bar);
    }
    for (int k = 0; k < 3; k++) exit_of(kids[k]);
    checkv(!s->bad && s->phase == 1200, "300 rounds of a barrier of 4 processes", s->phase);

    s->count = 0;
    for (int k = 0; k < 4; k++) {
        kids[k] = fork();
        if (kids[k] == 0) {
            for (int i = 0; i < 2000; i++) {
                if (i % 4 == k) {
                    pthread_rwlock_wrlock(&s->rw);
                    s->queued++;
                    if (s->queued != s->taken + 1) s->bad = 1;
                    s->taken++;
                } else {
                    pthread_rwlock_rdlock(&s->rw);
                    if (s->queued != s->taken) s->bad = 1;
                }
                pthread_rwlock_unlock(&s->rw);
            }
            _exit(0);
        }
    }
    for (int k = 0; k < 4; k++) exit_of(kids[k]);
    checkv(!s->bad && s->queued == 2000, "a rwlock of 4 processes", s->queued);

    // A process that sleeps in it is killed: the others go on.
    pid = fork();
    if (pid == 0) {
        sem_wait(&s->sem);
        _exit(0);
    }
    sleep_ms(30);
    kill(pid, SIGKILL);
    exit_of(pid);
    sem_post(&s->sem);
    checkv(sem_trywait(&s->sem) == 0, "a killed waiter takes nothing with it", errno);
    munmap(s, sizeof *s);
}

int main(void) {
    test_basics();
    test_memfd();
    test_pass();
    test_sync();
    printf("shm-suite: %d of %d passed\n", g_total - g_failed, g_total);
    return g_failed ? 1 : 0;
}
