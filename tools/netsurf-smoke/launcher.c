// NetSurf launcher for InstantOS. Runs as the boot "login" process and spawns
// /bin/nsfb (the NetSurf framebuffer browser) with argv/envp via the Spawn
// syscall, then waits. Direct kernel init-load of a large mlibc dynamic binary
// trips the rtld; spawning (like the bash smoke test) uses the exec path that
// sets up the process/auxv correctly. Raw syscalls only (no libc).
//
// Syscalls: Spawn(40), Wait(10), SerialWrite(88), Exit(2).

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
    serial("netsurf-launcher: start\n");

    /* Ensure the graphics compositor is running (it owns the framebuffer and
     * composites app windows). Spawn it and give it a moment to register the
     * graphics.compositor service before launching the browser. */
    {
        static const char* const cargv[] = { "graphics-compositor", 0 };
        static const char* const cenvp[] = { "HOME=/", 0 };
        u64 cpid = sc3(40, (u64)"/bin/graphics-compositor", (u64)cargv, (u64)cenvp);
        serial("netsurf-launcher: compositor pid="); put_hex(cpid); serial("\n");
        /* Sleep ~500ms (Sleep syscall = 15) to let it come up. */
        sc1(15, 500);
    }

    const char* path = "/bin/nsfb";
    // -w/-h set the window size; final arg is the start URL (built-in page,
    // no network needed).
    static const char* const argv[] = {
        "nsfb",
        "-w", "1024",
        "-h", "768",
        "about:welcome",
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
        serial("netsurf-launcher: spawn-failed rc="); put_hex(pid); serial("\n");
        sc1(2, 1);
        return;
    }
    serial("netsurf-launcher: spawned pid="); put_hex(pid); serial("\n");

    int status = 0;
    u64 w = sc3(10, pid, (u64)&status, 0);
    serial("netsurf-launcher: waited rc="); put_hex(w);
    serial(" status="); put_hex((u64)(unsigned)status); serial("\n");
    serial("netsurf-launcher: done\n");
    sc1(2, 0);
}
