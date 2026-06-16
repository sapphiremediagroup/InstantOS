// Network-enabled NetSurf launcher for InstantOS http(s) testing.
// Runs as the boot "login" process and, in order:
//   1. spawns /bin/network-manager  (its main loop pumps NetProcessPackets,
//      which the kernel TCP/IP stack needs to drain RX + run TCP timers; curl
//      cannot do this itself)
//   2. spawns /bin/graphics-compositor (owns the framebuffer)
//   3. spawns /bin/nsfb pointed at NETSURF_START_URL (compiled in below)
// then waits on nsfb. Raw syscalls only (no libc).
//
// Syscalls: Spawn(40), Wait(10), Sleep(15), SerialWrite(88), Exit(2).

#ifndef NETSURF_START_URL
#define NETSURF_START_URL "about:welcome"
#endif

typedef unsigned long u64;

static u64 sc1(u64 n, u64 a) {
    u64 r; register u64 rax asm("rax") = n; register u64 rbx asm("rbx") = a;
    asm volatile("syscall" : "=a"(r) : "a"(rax), "r"(rbx) : "rcx", "r11", "memory");
    return r;
}
static u64 sc2(u64 n, u64 a, u64 b) {
    u64 r; register u64 rax asm("rax") = n; register u64 rbx asm("rbx") = a;
    register u64 r10 asm("r10") = b;
    asm volatile("syscall" : "=a"(r) : "a"(rax), "r"(rbx), "r"(r10) : "rcx", "r11", "memory");
    return r;
}
static u64 sc3(u64 n, u64 a, u64 b, u64 c) {
    u64 r; register u64 rax asm("rax") = n; register u64 rbx asm("rbx") = a;
    register u64 r10 asm("r10") = b; register u64 rdx asm("rdx") = c;
    asm volatile("syscall" : "=a"(r) : "a"(rax), "r"(rbx), "r"(r10), "r"(rdx)
                 : "rcx", "r11", "memory");
    return r;
}

static unsigned slen(const char* s) { unsigned n = 0; while (s[n]) n++; return n; }
static void serial(const char* s) { sc2(88, (u64)s, slen(s)); }
static void put_hex(u64 v) {
    char b[19]; b[0]='0'; b[1]='x';
    for (int i=0;i<16;i++){ unsigned n=(v>>((15-i)*4))&0xf; b[2+i]=(char)(n<10?'0'+n:'a'+n-10);}
    b[18]=0; serial(b);
}

void _start(void) {
    serial("netsurf-net-launcher: start url=" NETSURF_START_URL "\n");

    /* 1. network-manager: pumps NetProcessPackets so sockets/DNS/TCP work. */
    {
        static const char* const nargv[] = { "network-manager", 0 };
        static const char* const nenvp[] = { "HOME=/", 0 };
        u64 npid = sc3(40, (u64)"/bin/network-manager", (u64)nargv, (u64)nenvp);
        serial("netsurf-net-launcher: network-manager pid="); put_hex(npid); serial("\n");
        sc1(15, 800);
    }

    /* 2. graphics compositor. */
    {
        static const char* const cargv[] = { "graphics-compositor", 0 };
        static const char* const cenvp[] = { "HOME=/", 0 };
        u64 cpid = sc3(40, (u64)"/bin/graphics-compositor", (u64)cargv, (u64)cenvp);
        serial("netsurf-net-launcher: compositor pid="); put_hex(cpid); serial("\n");
        sc1(15, 500);
    }

    /* 3. nsfb pointed at the test URL. */
    const char* path = "/bin/nsfb";
    static const char* const argv[] = {
        "nsfb",
        "-v",
        "-w", "1024",
        "-h", "768",
        NETSURF_START_URL,
        0,
    };
    static const char* const envp[] = {
        "HOME=/",
        "NETSURFRES=/netsurf/res/",
        "TERM=dumb",
        0,
    };

    u64 pid = sc3(40, (u64)path, (u64)argv, (u64)envp);
    if ((long)pid < 0 || pid == (u64)-1) {
        serial("netsurf-net-launcher: spawn-failed rc="); put_hex(pid); serial("\n");
        sc1(2, 1);
        return;
    }
    serial("netsurf-net-launcher: spawned nsfb pid="); put_hex(pid); serial("\n");

    int status = 0;
    u64 w = sc3(10, pid, (u64)&status, 0);
    serial("netsurf-net-launcher: waited rc="); put_hex(w);
    serial(" status="); put_hex((u64)(unsigned)status); serial("\n");
    serial("netsurf-net-launcher: done\n");
    sc1(2, 0);
}
