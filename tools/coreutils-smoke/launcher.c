// Headless coreutils validation over a PTY, driving real GNU coreutils via bash.
//
// Same PTY mechanism as pty_launcher.c, but the scripted commands exercise the
// cross-built coreutils binaries in /bin (and thus the sysdeps they rely on:
// Access, Chmod, Rename, Link, Symlink, Readlink, Truncate, Utimensat, ...).
// All shell + command output flows back through the master and is forwarded to
// the serial console so the smoke runner can assert on real results.

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
    serial("\n[CUTEST] start\n");

    i64 master=(i64)sc3(SYS_OPEN,(u64)"/dev/ptmx",O_RDWR,0);
    if(master<0){serial("[CUTEST] ptmx-open-failed\n");sc1(SYS_EXIT,1);}
    serial("[CUTEST] ptmx-open ok\n");

    struct Winsize ws; ws.row=24; ws.col=80; ws.xp=0; ws.yp=0;
    sc3(SYS_IOCTL,(u64)master,TIOCSWINSZ,(u64)&ws);

    unsigned ptn=0;
    if((i64)sc3(SYS_IOCTL,(u64)master,TIOCGPTN,(u64)&ptn)<0){serial("[CUTEST] gptn-failed\n");sc1(SYS_EXIT,1);}
    char slavePath[32]; build_pts(slavePath, ptn);

    i64 slave=(i64)sc3(SYS_OPEN,(u64)slavePath,O_RDWR,0);
    if(slave<0){serial("[CUTEST] slave-open-failed\n");sc1(SYS_EXIT,1);}

    sc2(SYS_DUP2,(u64)slave,0);
    sc2(SYS_DUP2,(u64)slave,1);
    sc2(SYS_DUP2,(u64)slave,2);

    static const char* const argv[]={"bash","-i",0};
    static const char* const envp[]={"PATH=/bin","HOME=/","TERM=dumb","PS1=CU$ ",0};
    u64 pid=sc3(SYS_SPAWN,(u64)"/bin/bash",(u64)argv,(u64)envp);
    sc1(SYS_CLOSE,(u64)slave);
    if((i64)pid<0||pid==(u64)-1){serial("[CUTEST] spawn-failed\n");sc1(SYS_EXIT,1);}
    serial("[CUTEST] spawned pid="); put_hex(pid); serial("\n");

    drain(master, 40);

    serial("[CUTEST] -- commands --\n");
    // Each command exercises real coreutils binaries and the sysdeps they use.
    static const char* cmds[]={
        "cd / && mkdir -p cu && cd cu && pwd\n",                 // mkdir + pwd (/ is writable RamFS)
        "echo hello > a.txt; cat a.txt\n",                       // redirection + cat
        "cp a.txt b.txt; cat b.txt\n",                           // cp (Truncate/Chmod/Utimensat)
        "mv b.txt c.txt && cat c.txt && echo CU_MV_OK\n",        // mv (Rename) preserves data
        "ls\n",                                                   // list after move
        "chmod 600 c.txt && echo CU_CHMOD_OK\n",                 // chmod (no-op on FAT, succeeds)
        "touch -d @1000000000 c.txt && echo CU_TOUCH_OK\n",      // touch (no-op on FAT, succeeds)
        "[ -r a.txt ] && echo CU_ACCESS_OK\n",                   // test -r (Access)
        "wc -c a.txt\n",                                          // wc
        "head -c 3 a.txt; echo\n",                               // head
        "df -P / && echo CU_DF_OK\n",                            // df (statvfs)
        "df 2>/dev/null | wc -l && echo CU_DFALL_OK\n",          // df (no args -> /etc/mtab enumeration)
        "stat -f -c 'bs=%s blocks=%b' / && echo CU_STATF_OK\n", // stat -f (statvfs)
        "chown 0:0 c.txt && echo CU_CHOWN_OK\n",                 // chown (Fchownat -> Chown syscall)
        "id -u && echo CU_ID_OK\n",                              // id (uid) - GetResuid/GetGroups/GetSid
        "id && echo CU_IDFULL_OK\n",                             // full id (now with names from /etc)
        "whoami && echo CU_WHOAMI_OK\n",                         // whoami (/etc/passwd lookup)
        "mkfifo /tmp/fifo && stat -c '%F' /tmp/fifo && echo CU_FIFO_OK\n", // mkfifo on tmpfs (RamFS)
        "cat /tmp/fifo > /tmp/fout.txt & sleep 1; echo PIPED > /tmp/fifo; sleep 1; cat /tmp/fout.txt; echo CU_FIFOIO_OK\n", // FIFO IPC (blocking read/write)
        "mknod /tmp/cdev c 5 1 && stat -c '%F %t:%T' /tmp/cdev && echo CU_MKNOD_OK\n", // mknod char dev
        "rm /tmp/fifo /tmp/cdev /tmp/fout.txt\n",                // cleanup tmpfs nodes
        "shuf -e a b c d e > /tmp/sh.txt && wc -l /tmp/sh.txt && echo CU_SHUF_OK\n", // shuf (getentropy)
        "mktemp /tmp/tmpXXXXXX && echo CU_MKTEMP_OK\n",          // mktemp (getentropy)
        "printf 'c\\na\\nb\\n' > /tmp/u.txt && sort /tmp/u.txt > /tmp/s.txt && cat /tmp/s.txt && echo CU_SORT_OK\n", // sort (GetRlimit)
        "uptime && echo CU_UPTIME_OK\n",                         // uptime (utmp BOOT_TIME + getloadavg)
        "ls -l /var/run/utmp; wc -c /var/run/utmp\n",            // DEBUG: utmp file exists/size
        "who && echo CU_WHO_OK\n",                               // who (utmp USER_PROCESS)
        "users && echo CU_USERS_OK\n",                           // users (utmp)
        "rm c.txt a.txt; ls; echo CU_DONE\n",                    // rm (unlink) + ls
        "exit\n",
        0,
    };
    for(int i=0; cmds[i]; ++i){
        sc3(SYS_WRITE,(u64)master,(u64)cmds[i],slen(cmds[i]));
        drain(master, 30);
    }
    drain(master, 30);

    int status=0;
    sc3(SYS_WAIT,pid,(u64)&status,0);
    serial("\n[CUTEST] bash-exit status="); put_hex((u64)(unsigned)status); serial("\n");
    serial("[CUTEST] done\n");
    sc1(SYS_EXIT,0);
}
