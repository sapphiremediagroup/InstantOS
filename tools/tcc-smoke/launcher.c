// Headless TinyCC validation over a PTY, driving /bin/tcc via bash.
//
// Same PTY mechanism as the coreutils smoke launcher (tools/coreutils-smoke/
// launcher.c): open /dev/ptmx, spawn bash on the slave, write a scripted command
// ladder, and forward all output to the serial console so the smoke runner can
// assert on real results.
//
// The command ladder is a diagnostic staircase so the FIRST failing rung is
// unambiguous:
//   1. tcc -v            does the compiler binary load and run at all?
//   2. tcc -E            preprocess: include-path lookup + header reads
//   3. tcc -c            compile-only: codegen + writing a .o
//   4. tcc <src> -o exe  link: find crt*.o / libtcc1.a / libc.so + ELF emit
//   5. ./exe             does the InstantOS loader accept tcc's output ELF?
// Each rung prints a TCC_*_RC=<code> sentinel so the runner can pinpoint the
// exact stage that breaks.

typedef unsigned long u64;
typedef long i64;

static u64 sc1(u64 n, u64 a) {
    u64 r; register u64 rax asm("rax")=n; register u64 rbx asm("rbx")=a;
    asm volatile("syscall":"=a"(r):"a"(rax),"r"(rbx):"rcx","r11","memory"); return r;
}
static u64 sc2(u64 n, u64 a, u64 b) {
    u64 r; register u64 rax asm("rax")=n; register u64 rbx asm("rbx")=a; register u64 r10 asm("r10")=b;
    asm volatile("syscall":"=a"(r):"a"(rax),"r"(rbx),"r"(r10):"rcx","r11","memory"); return r;
}
static u64 sc3(u64 n, u64 a, u64 b, u64 c) {
    u64 r; register u64 rax asm("rax")=n; register u64 rbx asm("rbx")=a; register u64 r10 asm("r10")=b; register u64 rdx asm("rdx")=c;
    asm volatile("syscall":"=a"(r):"a"(rax),"r"(rbx),"r"(r10),"r"(rdx):"rcx","r11","memory"); return r;
}

enum {
    SYS_EXIT=2, SYS_WRITE=3, SYS_READ=4, SYS_OPEN=5, SYS_CLOSE=6,
    SYS_WAIT=10, SYS_SLEEP=15, SYS_DUP2=37, SYS_SPAWN=40,
    SYS_SERIAL=88, SYS_POLL=91, SYS_IOCTL=119,
};
enum { O_RDWR=2, TIOCSWINSZ=0x5414, TIOCGPTN=0x80045430, POLLIN=0x0001 };

struct Winsize { unsigned short row, col, xp, yp; };
struct PollFD { i64 fd; short events; short revents; };

static unsigned slen(const char* s){unsigned n=0;while(s[n])n++;return n;}
static void serial(const char* s){sc2(SYS_SERIAL,(u64)s,slen(s));}
static void serial_n(const char* s,u64 n){sc2(SYS_SERIAL,(u64)s,n);}
static void put_hex(u64 v){char b[19];b[0]='0';b[1]='x';for(int i=0;i<16;i++){unsigned nib=(v>>((15-i)*4))&0xf;b[2+i]=(char)(nib<10?'0'+nib:'a'+nib-10);}b[18]='\0';serial(b);}

static void build_pts(char* out, unsigned n){const char* p="/dev/pts/";int i=0;while(p[i]){out[i]=p[i];i++;}char d[12];int k=0;if(n==0)d[k++]='0';while(n){d[k++]=(char)('0'+n%10);n/=10;}while(k)out[i++]=d[--k];out[i]='\0';}

static u64 drain(i64 master, int iters){
    char buf[512]; u64 total=0;
    for(int it=0; it<iters; ++it){
        struct PollFD pfd; pfd.fd=master; pfd.events=POLLIN; pfd.revents=0;
        sc3(SYS_POLL,(u64)&pfd,1,0);
        if(!(pfd.revents&POLLIN)){ sc1(SYS_SLEEP,20); continue; }
        i64 n=(i64)sc3(SYS_READ,(u64)master,(u64)buf,sizeof(buf));
        if(n<=0) break;
        serial_n(buf,(u64)n); total+=(u64)n;
    }
    return total;
}

void _start(void){
    serial("\n[TCCTEST] start\n");

    i64 master=(i64)sc3(SYS_OPEN,(u64)"/dev/ptmx",O_RDWR,0);
    if(master<0){serial("[TCCTEST] ptmx-open-failed\n");sc1(SYS_EXIT,1);}
    serial("[TCCTEST] ptmx-open ok\n");

    struct Winsize ws; ws.row=24; ws.col=80; ws.xp=0; ws.yp=0;
    sc3(SYS_IOCTL,(u64)master,TIOCSWINSZ,(u64)&ws);

    unsigned ptn=0;
    if((i64)sc3(SYS_IOCTL,(u64)master,TIOCGPTN,(u64)&ptn)<0){serial("[TCCTEST] gptn-failed\n");sc1(SYS_EXIT,1);}
    char slavePath[32]; build_pts(slavePath, ptn);

    i64 slave=(i64)sc3(SYS_OPEN,(u64)slavePath,O_RDWR,0);
    if(slave<0){serial("[TCCTEST] slave-open-failed\n");sc1(SYS_EXIT,1);}

    sc2(SYS_DUP2,(u64)slave,0);
    sc2(SYS_DUP2,(u64)slave,1);
    sc2(SYS_DUP2,(u64)slave,2);

    static const char* const argv[]={"bash","-i",0};
    static const char* const envp[]={"PATH=/bin","HOME=/","TERM=dumb","PS1=TCC$ ",0};
    u64 pid=sc3(SYS_SPAWN,(u64)"/bin/bash",(u64)argv,(u64)envp);
    sc1(SYS_CLOSE,(u64)slave);
    if((i64)pid<0||pid==(u64)-1){serial("[TCCTEST] spawn-failed\n");sc1(SYS_EXIT,1);}
    serial("[TCCTEST] spawned pid="); put_hex(pid); serial("\n");

    drain(master, 40);

    serial("[TCCTEST] -- commands --\n");
    // Diagnostic ladder. /tmp is a writable tmpfs (RamFS). This initrd has only
    // bash + tcc (no coreutils), so every rung uses bash builtins + absolute
    // paths. Each rung prints a sentinel + return code so the runner can
    // pinpoint the first failure.
    static const char* cmds[]={
        "cd /tmp && echo TCC_SETUP_OK\n",
        "tcc -v; echo TCC_VERSION_RC=$?\n",                                       // 1: binary runs
        "tcc -E /bin/tcc-hello.c > /tmp/pp.txt; echo TCC_PP_RC=$?\n",             // 2: preprocess
        "tcc -c /bin/tcc-hello.c -o /tmp/hello.o; echo TCC_COMPILE_RC=$?; [[ -s /tmp/hello.o ]] && echo TCC_OBJ_EXISTS\n", // 3: compile-only
        "tcc /bin/tcc-hello.c -o /tmp/hello; echo TCC_LINK_RC=$?; [[ -s /tmp/hello ]] && echo TCC_EXE_EXISTS\n",           // 4: link + ELF emit
        "read -n4 m < /tmp/hello; [[ \"${m:1:3}\" == ELF ]] && echo TCC_ELF_MAGIC_OK || echo TCC_ELF_MAGIC_BAD\n",         // ELF magic via builtin
        "/tmp/hello; echo TCC_RUN_RC=$?\n",                                       // 5: loader accepts output
        "echo TCC_LADDER_DONE\n",
        "exit\n",
        0,
    };
    for(int i=0; cmds[i]; ++i){
        sc3(SYS_WRITE,(u64)master,(u64)cmds[i],slen(cmds[i]));
        drain(master, 60);
    }
    drain(master, 40);

    int status=0;
    sc3(SYS_WAIT,pid,(u64)&status,0);
    serial("\n[TCCTEST] bash-exit status="); put_hex((u64)(unsigned)status); serial("\n");
    serial("[TCCTEST] done\n");
    sc1(SYS_EXIT,0);
}
