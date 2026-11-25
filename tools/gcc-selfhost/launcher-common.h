// Shared helpers of the gcc-selfhost launchers (launcher-binutils.c, launcher-gcc.c):
// raw syscalls, serial output, stdio on /dev/console, spawn+wait.
#pragma once

typedef unsigned long u64;
typedef long i64;

static u64 sc1(u64 n, u64 a){u64 r;register u64 rax asm("rax")=n;register u64 rbx asm("rbx")=a;asm volatile("syscall":"=a"(r):"a"(rax),"r"(rbx):"rcx","r11","memory");return r;}
static u64 sc2(u64 n,u64 a,u64 b){u64 r;register u64 rax asm("rax")=n;register u64 rbx asm("rbx")=a;register u64 r10 asm("r10")=b;asm volatile("syscall":"=a"(r):"a"(rax),"r"(rbx),"r"(r10):"rcx","r11","memory");return r;}
static u64 sc3(u64 n,u64 a,u64 b,u64 c){u64 r;register u64 rax asm("rax")=n;register u64 rbx asm("rbx")=a;register u64 r10 asm("r10")=b;register u64 rdx asm("rdx")=c;asm volatile("syscall":"=a"(r):"a"(rax),"r"(rbx),"r"(r10),"r"(rdx):"rcx","r11","memory");return r;}

enum {
    SYS_EXIT=2, SYS_WRITE=3, SYS_READ=4, SYS_OPEN=5, SYS_CLOSE=6,
    SYS_WAIT=10, SYS_SLEEP=15, SYS_MKDIR=32, SYS_UNLINK=34, SYS_DUP2=37, SYS_SPAWN=40, SYS_SERIAL=88,
};
enum { O_RDONLY=0, O_WRONLY=1, O_RDWR=2, O_CREAT=0100, O_TRUNC=01000 };

static unsigned slen(const char* s){unsigned n=0;while(s[n])n++;return n;}
static void serial(const char* s){sc2(SYS_SERIAL,(u64)s,slen(s));}
static void serial_n(const char* s,u64 n){sc2(SYS_SERIAL,(u64)s,n);}
static void put_dec(i64 v){char b[24];int i=23;b[23]=0;if(v==0){serial("0");return;}int neg=0;u64 u=(u64)v;if(v<0){neg=1;u=(u64)(-v);}while(u){b[--i]=(char)('0'+u%10);u/=10;}if(neg)b[--i]='-';serial(&b[i]);}
static void put_hex(u64 v){char b[19];b[0]='0';b[1]='x';for(int i=0;i<16;i++){int nib=(int)((v>>((15-i)*4))&0xf);b[2+i]=(char)(nib<10?'0'+nib:'a'+nib-10);}b[18]=0;serial(b);}
static u64 rd16(const unsigned char* b){return (u64)b[0]|((u64)b[1]<<8);}
static u64 rd32(const unsigned char* b){return (u64)b[0]|((u64)b[1]<<8)|((u64)b[2]<<16)|((u64)b[3]<<24);}
static u64 rd64(const unsigned char* b){u64 v=0;for(int i=0;i<8;i++)v|=((u64)b[i])<<(8*i);return v;}


// Bind /dev/console (a real, dup-able char device) to stdin/stdout/stderr.
// autoconf configure dups all three (`exec 6>&1`, `exec 7<&0`); the bare implicit
// kernel console has no backing handle and cannot be duplicated. /dev/console
// writes to the framebuffer console + serial and reads as EOF; being a char
// device it is line-buffered (no block-buffered-file seek-on-flush issues).
static void setup_stdio(void){
    // Open /dev/console once read-write and install it as stdin/stdout/stderr, so
    // all three carry both READ and WRITE rights (autoconf dup()s and both reads
    // and writes them). read() returns EOF; write() -> console + serial.
    i64 fd=(i64)sc3(SYS_OPEN,(u64)"/dev/console",O_RDWR,0);
    if(fd<0) return;
    sc2(SYS_DUP2,(u64)fd,0);
    sc2(SYS_DUP2,(u64)fd,1);
    sc2(SYS_DUP2,(u64)fd,2);
    if(fd!=0 && fd!=1 && fd!=2) sc1(SYS_CLOSE,(u64)fd);
}
static int file_exists(const char* path){
    i64 fd=(i64)sc3(SYS_OPEN,(u64)path,O_RDONLY,0);
    if(fd<0) return 0; sc1(SYS_CLOSE,(u64)fd); return 1;
}
static i64 spawn_wait(const char* path, const char* const* argv, const char* const* envp){
    u64 pid=sc3(SYS_SPAWN,(u64)path,(u64)argv,(u64)envp);
    if((i64)pid<0||pid==(u64)-1) return -1;
    int status=0;
    sc3(SYS_WAIT,pid,(u64)&status,0);
    return (i64)(int)status;
}
static i64 exit_code(i64 status){ return (status>>8)&0xFF; }
static int is_elf(const char* path){
    i64 fd=(i64)sc3(SYS_OPEN,(u64)path,O_RDONLY,0);
    if(fd<0) return 0;
    char m[4]={0};
    i64 n=(i64)sc3(SYS_READ,(u64)fd,(u64)m,4);
    sc1(SYS_CLOSE,(u64)fd);
    return n==4 && m[0]==0x7f && m[1]=='E' && m[2]=='L' && m[3]=='F';
}
static void dump_file(const char* path){
    serial("\n---DUMP "); serial(path); serial("---\n");
    i64 fd=(i64)sc3(SYS_OPEN,(u64)path,O_RDONLY,0);
    if(fd<0){ serial("(open failed)\n"); return; }
    char buf[2048]; i64 n; while((n=(i64)sc3(SYS_READ,(u64)fd,(u64)buf,sizeof(buf)))>0) serial_n(buf,(u64)n);
    sc1(SYS_CLOSE,(u64)fd);
    serial("\n---ENDDUMP---\n");
}
