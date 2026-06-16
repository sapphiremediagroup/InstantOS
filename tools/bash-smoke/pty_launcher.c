// Headless PTY harness for bash interactive validation.
//
// Replicates what the graphics terminal does, but headless and scripted:
//   1. open("/dev/ptmx")            -> PTY master
//   2. ioctl(master, TIOCSWINSZ)    -> set geometry
//   3. ioctl(master, TIOCGPTN)      -> slave index N
//   4. open("/dev/pts/N")           -> PTY slave
//   5. dup2(slave -> 0,1,2); spawn("/bin/bash")
//   6. write commands to the master, poll+read bash's output back
//
// All bash output (prompt, command echo, and command results) flows back
// through the master read; we forward it to the serial console (syscall 88)
// with a [PTY>] prefix so the smoke runner can assert on it.
//
// Uses only raw InstantOS syscalls (no libc) so the test isolates the bash +
// mlibc + kernel-PTY runtime.

typedef unsigned long u64;
typedef long i64;

static u64 sc1(u64 n, u64 a) {
    u64 r;
    register u64 rax asm("rax") = n;
    register u64 rbx asm("rbx") = a;
    asm volatile("syscall" : "=a"(r) : "a"(rax), "r"(rbx) : "rcx", "r11", "memory");
    return r;
}
static u64 sc2(u64 n, u64 a, u64 b) {
    u64 r;
    register u64 rax asm("rax") = n;
    register u64 rbx asm("rbx") = a;
    register u64 r10 asm("r10") = b;
    asm volatile("syscall" : "=a"(r) : "a"(rax), "r"(rbx), "r"(r10) : "rcx", "r11", "memory");
    return r;
}
static u64 sc3(u64 n, u64 a, u64 b, u64 c) {
    u64 r;
    register u64 rax asm("rax") = n;
    register u64 rbx asm("rbx") = a;
    register u64 r10 asm("r10") = b;
    register u64 rdx asm("rdx") = c;
    asm volatile("syscall" : "=a"(r) : "a"(rax), "r"(rbx), "r"(r10), "r"(rdx)
                 : "rcx", "r11", "memory");
    return r;
}

enum {
    SYS_EXIT = 2, SYS_WRITE = 3, SYS_READ = 4, SYS_OPEN = 5, SYS_CLOSE = 6,
    SYS_WAIT = 10, SYS_SLEEP = 15, SYS_DUP2 = 37, SYS_SPAWN = 40,
    SYS_SERIAL = 88, SYS_POLL = 91, SYS_IOCTL = 119,
};

enum {
    O_RDWR = 2,
    TIOCSWINSZ = 0x5414,
    TIOCGPTN = 0x80045430,
    POLLIN = 0x0001,
};

struct Winsize { unsigned short row, col, xp, yp; };
struct PollFD { i64 fd; short events; short revents; };

static unsigned slen(const char* s) { unsigned n = 0; while (s[n]) n++; return n; }
static void serial(const char* s) { sc2(SYS_SERIAL, (u64)s, slen(s)); }
static void serial_n(const char* s, u64 n) { sc2(SYS_SERIAL, (u64)s, n); }

static void put_hex(u64 v) {
    char b[19]; b[0]='0'; b[1]='x';
    for (int i = 0; i < 16; i++) {
        unsigned nib = (v >> ((15 - i) * 4)) & 0xf;
        b[2+i] = (char)(nib < 10 ? '0'+nib : 'a'+nib-10);
    }
    b[18] = '\0';
    serial(b);
}

static void build_pts(char* out, unsigned n) {
    const char* p = "/dev/pts/";
    int i = 0; while (p[i]) { out[i] = p[i]; i++; }
    char d[12]; int k = 0;
    if (n == 0) d[k++] = '0';
    while (n) { d[k++] = (char)('0' + n % 10); n /= 10; }
    while (k) out[i++] = d[--k];
    out[i] = '\0';
}

// Drain master output for a while, forwarding to serial. Returns total bytes.
static u64 drain(i64 master, int iters) {
    char buf[512];
    u64 total = 0;
    for (int it = 0; it < iters; ++it) {
        struct PollFD pfd; pfd.fd = master; pfd.events = POLLIN; pfd.revents = 0;
        sc3(SYS_POLL, (u64)&pfd, 1, 0);  // timeoutMs=0 -> non-blocking scan
        if (!(pfd.revents & POLLIN)) {
            sc1(SYS_SLEEP, 20);
            continue;
        }
        i64 n = (i64)sc3(SYS_READ, (u64)master, (u64)buf, sizeof(buf));
        if (n <= 0) break;
        serial_n(buf, (u64)n);
        total += (u64)n;
    }
    return total;
}

void _start(void) {
    serial("\n[PTYTEST] start\n");

    i64 master = (i64)sc3(SYS_OPEN, (u64)"/dev/ptmx", O_RDWR, 0);
    if (master < 0) { serial("[PTYTEST] ptmx-open-failed\n"); sc1(SYS_EXIT, 1); }
    serial("[PTYTEST] ptmx-open ok\n");

    struct Winsize ws; ws.row = 24; ws.col = 80; ws.xp = 0; ws.yp = 0;
    sc3(SYS_IOCTL, (u64)master, TIOCSWINSZ, (u64)&ws);

    unsigned ptn = 0;
    if ((i64)sc3(SYS_IOCTL, (u64)master, TIOCGPTN, (u64)&ptn) < 0) {
        serial("[PTYTEST] gptn-failed\n"); sc1(SYS_EXIT, 1);
    }

    char slavePath[32];
    build_pts(slavePath, ptn);
    serial("[PTYTEST] slave="); serial(slavePath); serial("\n");

    i64 slave = (i64)sc3(SYS_OPEN, (u64)slavePath, O_RDWR, 0);
    if (slave < 0) { serial("[PTYTEST] slave-open-failed\n"); sc1(SYS_EXIT, 1); }

    // Bind the slave to the child's stdio slots, then spawn bash.
    sc2(SYS_DUP2, (u64)slave, 0);
    sc2(SYS_DUP2, (u64)slave, 1);
    sc2(SYS_DUP2, (u64)slave, 2);

    static const char* const argv[] = { "bash", "-i", 0 };
    static const char* const envp[] = {
        "PATH=/bin", "HOME=/", "TERM=dumb", "PS1=ISH$ ", 0,
    };
    u64 pid = sc3(SYS_SPAWN, (u64)"/bin/bash", (u64)argv, (u64)envp);

    // Restore our own stdio so we don't keep the slave bound here, then close it.
    sc1(SYS_CLOSE, (u64)slave);

    if ((i64)pid < 0 || pid == (u64)-1) {
        serial("[PTYTEST] spawn-failed rc="); put_hex(pid); serial("\n"); sc1(SYS_EXIT, 1);
    }
    serial("[PTYTEST] spawned pid="); put_hex(pid); serial("\n");

    // Let bash start up and print its prompt.
    drain(master, 40);

    // Feed a sequence of commands. Each ends with '\n' so the line discipline
    // delivers a complete line to bash.
    serial("[PTYTEST] -- sending commands --\n");
    static const char* cmds[] = {
        "echo PTY_HELLO\n",
        "X=7; echo val=$((X*3))\n",
        "for i in 1 2 3; do echo loop$i; done\n",
        "pwd\n",
        "exit\n",
        0,
    };
    for (int i = 0; cmds[i]; ++i) {
        sc3(SYS_WRITE, (u64)master, (u64)cmds[i], slen(cmds[i]));
        drain(master, 25);
    }

    // Final drain to capture any trailing output / EOF.
    drain(master, 30);

    int status = 0;
    sc3(SYS_WAIT, pid, (u64)&status, 0);
    serial("\n[PTYTEST] bash-exit status="); put_hex((u64)(unsigned)status); serial("\n");
    serial("[PTYTEST] done\n");
    sc1(SYS_EXIT, 0);
}
