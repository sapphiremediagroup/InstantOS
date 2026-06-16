// Standalone bash smoke launcher. Runs as the boot "login" process, spawns
// /bin/bash with a -c script, waits for it, and reports pass/fail markers over
// the serial console (syscall 88). Uses only raw InstantOS syscalls so it does
// not depend on any libc, isolating the test to the bash + mlibc runtime.
//
// Syscalls used: Spawn(40), Wait(10), SerialWrite(88), Exit(11).

typedef unsigned long u64;

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

static unsigned slen(const char* s) {
    unsigned n = 0;
    while (s[n]) n++;
    return n;
}
static void serial(const char* s) { sc2(88, (u64)s, slen(s)); }

static void put_hex(u64 v) {
    char buf[19];
    buf[0] = '0'; buf[1] = 'x';
    for (int i = 0; i < 16; i++) {
        unsigned nib = (v >> ((15 - i) * 4)) & 0xf;
        buf[2 + i] = (char)(nib < 10 ? '0' + nib : 'a' + nib - 10);
    }
    buf[18] = '\0';
    serial(buf);
}

void _start(void) {
    serial("bash-smoke: start\n");

    // Probe: write directly to fd 1 to confirm the inherited stdio slot reaches
    // the console/serial (syscall 3 = Write). This isolates kernel stdout
    // routing from bash's own buffered fwrite path.
    {
        static const char probe[] = "bash-smoke: fd1-probe FD1_OK\n";
        sc3(3, 1, (u64)probe, sizeof(probe) - 1);
    }

    const char* path = "/bin/bash";
    // bash -c '<script>': exercise builtins, $((arithmetic)), and a child exec.
    // static so the compiler emits these as data, not a stack memcpy (we are
    // freestanding with no libc memcpy available).
    static const char* const argv[] = {
        "bash",
        "-c",
        "echo BASH_SMOKE_OK; echo arith=$((6*7)); echo shell=$0; exit 0",
        0,
    };
    static const char* const envp[] = {
        "PATH=/bin",
        "HOME=/",
        "TERM=dumb",
        "PS1=$ ",
        0,
    };

    u64 pid = sc3(40, (u64)path, (u64)argv, (u64)envp);
    if ((long)pid < 0 || pid == (u64)-1) {
        serial("bash-smoke: spawn-failed rc=");
        put_hex(pid);
        serial("\n");
        sc1(2, 1); // Exit(1)
        return;
    }
    serial("bash-smoke: spawned pid=");
    put_hex(pid);
    serial("\n");

    int status = 0;
    u64 w = sc3(10, pid, (u64)&status, 0);
    serial("bash-smoke: waited rc=");
    put_hex(w);
    serial(" status=");
    put_hex((u64)(unsigned)status);
    serial("\n");

    serial("bash-smoke: done\n");
    sc1(2, 0); // Exit(0)
}
