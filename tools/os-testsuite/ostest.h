// Shared preamble of the freestanding in-OS test programs (runner.c,
// netsuite.c): raw syscalls, syscall numbers, libc-free helpers, serial
// output and the pass/fail tally behind the "SCORE ..." line.
#pragma once

typedef unsigned long u64; typedef long i64;
typedef unsigned int u32; typedef int i32;
typedef unsigned short u16; typedef unsigned char u8; typedef short i16;

// ---- syscall convention: num=rax, args=rbx,r10,rdx,r8,r9, ret=rax ----
static i64 syscall5(i64 n, i64 a, i64 b, i64 c, i64 d, i64 e){
    i64 r;
    register i64 rax asm("rax")=n;
    register i64 rbx asm("rbx")=a;
    register i64 r10 asm("r10")=b;
    register i64 rdx asm("rdx")=c;
    register i64 r8  asm("r8")=d;
    register i64 r9  asm("r9")=e;
    asm volatile("syscall":"=a"(r):"a"(rax),"r"(rbx),"r"(r10),"r"(rdx),"r"(r8),"r"(r9):"rcx","r11","memory");
    return r;
}
#define SC0(n)              syscall5((n),0,0,0,0,0)
#define SC1(n,a)            syscall5((n),(i64)(a),0,0,0,0)
#define SC2(n,a,b)          syscall5((n),(i64)(a),(i64)(b),0,0,0)
#define SC3(n,a,b,c)        syscall5((n),(i64)(a),(i64)(b),(i64)(c),0,0)
#define SC4(n,a,b,c,d)      syscall5((n),(i64)(a),(i64)(b),(i64)(c),(i64)(d),0)
#define SC5(n,a,b,c,d,e)    syscall5((n),(i64)(a),(i64)(b),(i64)(c),(i64)(d),(i64)(e))

// syscall numbers (enum ordinals)
enum {
 N_OSINFO=0,N_PROCINFO=1,N_EXIT=2,N_WRITE=3,N_READ=4,N_OPEN=5,N_CLOSE=6,N_GETPID=7,
 N_FORK=8,N_EXEC=9,N_WAIT=10,N_KILL=11,N_MMAP=12,N_MUNMAP=13,N_YIELD=14,N_SLEEP=15,N_GETTIME=16,
 N_FBINFO=18,N_FBMAP=19,N_SIGNAL=20,N_GETUID=24,N_GETGID=25,N_GETSESSIONID=28,N_GETSESSIONINFO=29,
 N_CHDIR=30,N_GETCWD=31,N_MKDIR=32,N_RMDIR=33,N_UNLINK=34,N_STAT=35,N_DUP=36,N_DUP2=37,
 N_PIPE=38,N_GETPPID=39,N_SPAWN=40,N_GETUSERINFO=41,N_READDIR=42,
 N_SHAREDALLOC=44,N_SHAREDMAP=45,N_SHAREDFREE=46,N_SURFCREATE=47,N_SURFMAP=48,N_SURFCOMMIT=49,
 N_QUEUECREATE=60,N_QUEUESEND=61,N_QUEUERECV=62,N_SERVICEREG=65,N_SERVICECONN=66,
 N_NETGETMAC=67,N_NETLINK=70,N_THREADCREATE=74,N_THREADEXIT=75,N_THREADJOIN=76,N_SEEK=77,
 N_GETUNIXTIME=87,N_SERIAL=88,N_FCNTL=89,N_MPROTECT=90,N_POLL=91,N_TRUNCATE=92,N_RENAME=93,
 N_CHMOD=94,N_UTIME=95,N_FSTAT=96,N_LINK=97,N_SYMLINK=98,N_READLINK=99,N_LSTAT=100,
 N_SIGPROCMASK=101,N_SOCKET=102,N_BIND=103,N_CONNECT=104,N_LISTEN=105,N_ACCEPT=106,
 N_SEND=107,N_RECV=108,N_SHUTDOWN=109,N_GETSOCKOPT=110,N_SETSOCKOPT=111,N_STORAGEINFO=112,
 N_SIGACTION=115,N_IOCTL=119,N_ACCESS=120,N_STATFS=121,N_CHOWN=122,N_GETENTROPY=124,
 N_GETSOCKNAME=125,N_GETPEERNAME=126,N_PIPE2=129,N_FSYNC=130,N_MMAPFILE=131,N_MSYNC=132,
 N_FDPATH=133,N_SETAFFINITY=137,N_GETAFFINITY=138,N_GETCPU=139,N_CPUINFO=140,
 N_GETUNIXTIMENS=148,N_SETUNIXTIME=149,N_KERNELLOG=151
};

// flags / constants
enum { O_RDONLY=0,O_WRONLY=1,O_RDWR=2,O_CREAT=0100,O_EXCL=0200,O_TRUNC=01000,O_APPEND=02000,
       O_NONBLOCK=04000,O_DIRECTORY=0200000,O_CLOEXEC=02000000 };
enum { SEEK_SET=0,SEEK_CUR=1,SEEK_END=2 };
enum { F_OK=0,X_OK=1,W_OK=2,R_OK=4 };
enum { F_GETFD=1,F_SETFD=2,FD_CLOEXEC=1 };
enum { POLLIN=1,POLLOUT=4 };
enum { PROT_R=1,PROT_W=2,PROT_X=4 };
enum { MAP_SHARED=1,MAP_PRIVATE=2,MAP_FIXED=0x10,MAP_ANON=0x20 };
enum { E_NOENT=2,E_CHILD=10,E_BADF=9,E_AGAIN=11,E_ACCES=13,E_EXIST=17,E_NOTDIR=20,E_ISDIR=21,E_INVAL=22,
       E_NAMETOOLONG=36,E_SPIPE=29,E_NOTEMPTY=39,E_OPNOTSUPP=95,E_AFNOSUPPORT=97,E_NOTCONN=107 };
enum { WNOHANG=1,WUNTRACED=2,WCONTINUED=8 }; // wait() options (kernel: kWaitOptionMask)
enum { SIG_USR1=10,SIG_KILL=9,SIG_PIPE=13 };
enum { AF_INET=2,AF_UNIX=1,SOCK_STREAM=1,SOCK_DGRAM=2,SOCK_RAW=3,SOCK_NONBLOCK=04000,
       SOL_SOCKET=1,SO_REUSEADDR=2,SO_TYPE=3,SHUT_RDWR=2 };
enum { TIOCGWINSZ=0x5413,TIOCSWINSZ=0x5414,TIOCGPTN=0x80045430,TCGETS=0x5401,TCSETS=0x5402 };
#define S_IFMT 0170000
#define S_IFREG 0100000
#define S_IFDIR 0040000
#define S_IFCHR 0020000
#define S_IFLNK 0120000

struct Stat { u64 dev,ino; u32 mode,nlink,uid,gid; u64 rdev,size,blksize,blocks,atime,mtime,ctime; };
struct DirEntry { char name[256]; u64 inode; i32 type; };
struct PollFD { i64 fd; i16 events; i16 revents; };
struct Winsize { u16 row,col,xp,yp; };
struct Termios { u32 iflag,oflag,cflag,lflag; u8 line; u8 cc[32]; u32 ibaud,obaud; };
struct sockaddr_in { u16 sin_family; u16 sin_port; u32 sin_addr; u8 zero[8]; };
struct IPCMessage { u64 id; u32 senderPID; u16 flags; u16 reserved; u64 size; u8 data[256]; };
// Argument block for the file-backed mmap syscall (must match kernel MmapFileArgs).
struct MmapFileArgs { u64 addr,length,prot,flags,fd,offset; };

// freestanding primitives (clang lowers some inits to these)
void* memset(void* d,int c,u64 n){u8* p=(u8*)d;for(u64 i=0;i<n;i++)p[i]=(u8)c;return d;}
void* memcpy(void* d,const void* s,u64 n){u8* a=(u8*)d;const u8* b=(const u8*)s;for(u64 i=0;i<n;i++)a[i]=b[i];return d;}
static u64 slen(const char* s){u64 n=0;while(s[n])n++;return n;}
static int smemcmp(const void* a,const void* b,u64 n){const u8* x=(const u8*)a;const u8* y=(const u8*)b;for(u64 i=0;i<n;i++){if(x[i]!=y[i])return (int)x[i]-(int)y[i];}return 0;}
static int sstreq(const char* a,const char* b){u64 i=0;for(;a[i]&&b[i];i++)if(a[i]!=b[i])return 0;return a[i]==b[i];}
static int contains(const char* hay,u64 hn,const char* needle){u64 nn=slen(needle);if(nn>hn)return 0;for(u64 i=0;i+nn<=hn;i++){if(smemcmp(hay+i,needle,nn)==0)return 1;}return 0;}

// serial output
static void sw(const char* s){SC2(N_SERIAL,s,slen(s));}
static void swn(const char* s,u64 n){SC2(N_SERIAL,s,n);}
static void sdec(i64 v){char b[24];int i=23;b[23]=0;if(v==0){sw("0");return;}int neg=0;u64 u=(u64)v;if(v<0){neg=1;u=(u64)(-v);}while(u){b[--i]=(char)('0'+u%10);u/=10;}if(neg)b[--i]='-';sw(&b[i]);}

// ---- scoring ----
static int g_total=0,g_pass=0,g_secT=0,g_secP=0; static const char* g_sec="";
static void section(const char* name){
    if(g_sec[0]){ sw("  ["); sw(g_sec); sw("] "); sdec(g_pass-g_secP); sw("/"); sdec(g_total-g_secT);
                  sw("  (cum "); sdec(g_pass); sw("/"); sdec(g_total); sw(")\n"); }
    g_sec=name; g_secT=g_total; g_secP=g_pass;
    if(name[0]) { sw("\n== "); sw(name); sw(" ==\n"); }
}
static void check(int cond,const char* name){
    g_total++;
    if(cond){ g_pass++; }
    else { sw("  FAIL: "); sw(name); sw("\n"); }
}
// on-fail print the observed value (helps triage)
static void checkv(int cond,const char* name,i64 got){
    g_total++;
    if(cond){ g_pass++; }
    else { sw("  FAIL: "); sw(name); sw(" got="); sdec(got); sw("\n"); }
}
static void ok(i64 ret,const char* name){ checkv(ret>=0,name,ret); }        // success (>=0)
static void neg(i64 ret,const char* name){ checkv(ret<0,name,ret); }        // any failure
static void err(i64 ret,i64 e,const char* name){ checkv(ret==-e,name,ret); }// exact -errno

// ---- syscall helpers ----
static i64 t_open(const char* p,i64 fl,i64 m){ return SC3(N_OPEN,p,fl,m); }
static i64 t_close(i64 h){ return SC1(N_CLOSE,h); }
static i64 t_write(i64 h,const void* b,u64 n){ return SC3(N_WRITE,h,b,n); }
static i64 t_read(i64 h,void* b,u64 n){ return SC3(N_READ,h,b,n); }
static i64 t_unlink(const char* p){ return SC1(N_UNLINK,p); }
static void yield(void){ SC0(N_YIELD); }
static void msleep(i64 ms){ SC1(N_SLEEP,ms); }

static u16 htons(u16 v){ return (u16)((v<<8)|(v>>8)); }

// write a whole small file on any writable fs; returns 0 on success
static int put_file(const char* path,const char* data,u64 n){
    i64 h=t_open(path,O_WRONLY|O_CREAT|O_TRUNC,0644);
    if(h<0) return -1;
    i64 w=t_write(h,data,n); t_close(h);
    return (w==(i64)n)?0:-1;
}

// fd->path (SyscallNumber::FdPath); writes NUL-terminated abs path, returns len or -errno
static i64 t_fd_path(i64 h,char* buf,u64 n){ return SC3(N_FDPATH,h,buf,n); }

// file-backed mmap (SyscallNumber::MmapFile); returns mapped address (>0) or -errno
static i64 t_mmap_file(i64 fd,u64 len,u64 prot,u64 flags,u64 off){
    struct MmapFileArgs a; a.addr=0; a.length=len; a.prot=prot; a.flags=flags; a.fd=(u64)fd; a.offset=off;
    return SC1(N_MMAPFILE,&a);
}

// build "<pfx><n>" into buf (n < 1e9); used for mass file creation tests
static void iname(char* buf,const char* pfx,int n){
    int i=0; while(pfx[i]){buf[i]=pfx[i];i++;}
    char tmp[12]; int j=0; if(n==0)tmp[j++]='0'; while(n){tmp[j++]=(char)('0'+n%10);n/=10;}
    while(j>0)buf[i++]=tmp[--j]; buf[i]='\0';
}
