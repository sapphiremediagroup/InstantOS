// Phase 7 P7.1 - in-OS gcc rebuilds binutils (bash-free driver).
//
// The HOSTED gcc on the gcchost ext4 (/usr/bin/gcc) + the GNU userland ports on
// the initrd (/bin: sh make sed grep tar gzip awk cmp + coreutils) rebuild GNU
// binutils-2.42 from source, ON InstantOS:
//
//   untar    : gzip -dc /bin/binutils-insrc.tar.gz | tar -x  -> /work/binutils-2.42
//   configure: native (build=host=target=x86_64-unknown-instantos); huge sh/sed/
//              grep/awk/expr + gcc compile+link probe pass across bfd/opcodes/gas/ld
//   make     : compile the whole tree with the in-OS gcc -> gas/as-new, ld/ld-new
//   sanity   : run the freshly built as/ld (--version) in-OS
//
// Existing as/ar/ranlib (hosted binutils in /usr/bin) bootstrap the new build.
// Children write stdout/stderr to /dev/console (-> framebuffer + serial). Progress
// via GCCSELF_* serial markers (syscall 88). GCCSELF_BINUTILS_OK == P7.1 green.

#include "launcher-common.h"


// gcc + as/ar/ranlib on the ext4 (/usr/bin); the GNU userland ports on the
// initrd (/bin). Both on PATH so configure/make find every tool.
static const char* const ENVP[]={
    "PATH=/usr/bin:/bin","HOME=/","TMPDIR=/work/tmp","SHELL=/bin/sh","CONFIG_SHELL=/bin/sh",
    "MAKEINFO=true","LC_ALL=C","LANG=C",0
};

static const char* const ARGV_UNTAR[] = {"sh","-c",
    "gzip -dc /bin/binutils-insrc.tar.gz | tar -x -C /work",0};
static const char* const ARGV_CONFIGURE[] = {"sh","-c",
    "cd /work/bu-build && /work/binutils-2.42/configure "
    "--host=x86_64-unknown-instantos --build=x86_64-unknown-instantos "
    "--target=x86_64-unknown-instantos --prefix=/usr --disable-nls --disable-werror "
    "--without-zstd --without-debuginfod --without-msgpack --disable-gdb --disable-gold "
    "--disable-gprofng --disable-libctf --disable-plugins --without-isl --disable-lto "
    "--disable-bootstrap --enable-default-hash-style=sysv",0};
static const char* const ARGV_MAKE[] = {"sh","-c",
    "cd /work/bu-build && make -j2 MAKEINFO=true all-gas all-ld all-binutils",0};
static const char* const ARGV_AS_V[] = {"as-new","--version",0};
static const char* const ARGV_LD_V[] = {"ld-new","--version",0};

// --- FAST REPRO for the fork/exec frame-reuse UAF (bug #4) --------------------
// Skips the 108MB untar/configure/make; just hammers bash->gcc->cc1->collect2->ld
// links so the churn-triggered collect2 corruption reproduces in ~minutes.
#define REPRO_BUG4 0
static const char* const ARGV_LINK[] = {"sh","-c","gcc /tmp/t.c -o /tmp/t_out",0};
static void repro_bug4(void){
    // write a trivial translation unit
    i64 fd=(i64)sc3(SYS_OPEN,(u64)"/tmp/t.c",O_WRONLY|O_CREAT|O_TRUNC,0644);
    if(fd>=0){ const char* s="int main(void){return 0;}\n"; sc3(SYS_WRITE,(u64)fd,(u64)s,slen(s)); sc1(SYS_CLOSE,(u64)fd); }
    serial("[REPRO] hammering bash->gcc->collect2->ld links (churn UAF probe)\n");
    for(int i=1;i<=150;i++){
        i64 rc = spawn_wait("/bin/sh", ARGV_LINK, ENVP);
        int ok = is_elf("/tmp/t_out");
        serial("[REPRO] i="); put_dec(i); serial(" rc="); put_dec(rc); serial(ok?" ELF_OK\n":" ELF_BAD\n");
        if(!ok){ serial("REPRO_FIRST_FAIL_AT="); put_dec(i); serial("\n"); }
    }
    serial("REPRO_DONE\n");
    sc1(SYS_EXIT,0);
}

// --- EXEC DIAG: why do freshly-linked binaries fail to run? -------------------
#define REPRO_EXEC 0
static void inspect_elf(const char* path){
    serial("[EXEC] inspect "); serial(path); serial("\n");
    i64 fd=(i64)sc3(SYS_OPEN,(u64)path,O_RDONLY,0);
    if(fd<0){ serial("  open FAILED rc="); put_dec((i64)fd); serial("\n"); return; }
    static unsigned char buf[8192];
    i64 n=(i64)sc3(SYS_READ,(u64)fd,(u64)buf,sizeof(buf));
    sc1(SYS_CLOSE,(u64)fd);
    if(n<64){ serial("  short read n="); put_dec(n); serial("\n"); return; }
    if(!(buf[0]==0x7f&&buf[1]=='E'&&buf[2]=='L'&&buf[3]=='F')){ serial("  not ELF\n"); return; }
    u64 etype=rd16(buf+16), entry=rd64(buf+24), phoff=rd64(buf+32);
    u64 phentsize=rd16(buf+54), phnum=rd16(buf+56);
    serial("  e_type="); put_dec((i64)etype); serial(" (2=EXEC 3=DYN) entry="); put_hex(entry);
    serial(" phnum="); put_dec((i64)phnum); serial("\n");
    for(u64 i=0;i<phnum && phoff+(i+1)*phentsize<=(u64)n;i++){
        const unsigned char* ph=buf+phoff+i*phentsize;
        u64 ptype=rd32(ph), poff=rd64(ph+8), pvaddr=rd64(ph+16), pfilesz=rd64(ph+32);
        if(ptype==3){ serial("  PT_INTERP off="); put_hex(poff); serial(" len="); put_dec((i64)pfilesz);
            serial(" str=\""); if(poff+pfilesz<=(u64)n && pfilesz>0) serial_n((const char*)(buf+poff),pfilesz-1); serial("\"\n"); }
        if(ptype==1){ serial("  PT_LOAD vaddr="); put_hex(pvaddr); serial("\n"); }
    }
}
static const char* const ARGV_CC_T[] = {"sh","-c","gcc /tmp/t.c -o /tmp/t_out 2>/tmp/cc.err; echo cc_exit=$?",0};
static const char* const ARGV_TOUT[]  = {"/tmp/t_out",0};
static const char* const ARGV_TOUT_SH[]= {"sh","-c","/tmp/t_out; echo t_out_via_sh_exit=$?",0};
static const char* const ARGV_CFG[]    = {"sh","-c",
    "cd /tmp/cfgt && echo 'int main(void){return 0;}' > conftest.c && "
    "gcc conftest.c -o conftest && echo compiled && ./conftest; echo relexit=$?",0};
static const char* const ARGV_DOTSLASH[]={"sh","-c","cd /tmp && ./t_out; echo dot_slash_exit=$?",0};
static void repro_exec(void){
    i64 fd=(i64)sc3(SYS_OPEN,(u64)"/tmp/t.c",O_WRONLY|O_CREAT|O_TRUNC,0644);
    if(fd>=0){ const char* s="int main(void){return 42;}\n"; sc3(SYS_WRITE,(u64)fd,(u64)s,slen(s)); sc1(SYS_CLOSE,(u64)fd); }
    serial("[EXEC] compile /tmp/t.c -> /tmp/t_out\n");
    i64 crc=spawn_wait("/bin/sh",ARGV_CC_T,ENVP);
    serial("[EXEC] compile rc="); put_dec(crc); serial(is_elf("/tmp/t_out")?" ELF_OK\n":" ELF_BAD\n");
    dump_file("/tmp/cc.err");
    inspect_elf("/tmp/t_out");
    serial("[EXEC] spawn /tmp/t_out directly...\n");
    i64 rc=spawn_wait("/tmp/t_out",ARGV_TOUT,ENVP);
    serial("[EXEC] direct spawn_wait rc="); put_dec(rc); serial(" exit="); put_dec(exit_code(rc)); serial("\n");
    serial("[EXEC] run /tmp/t_out via sh (absolute)...\n");
    spawn_wait("/bin/sh",ARGV_TOUT_SH,ENVP);
    // Mimic configure EXACTLY: compile in a build dir, then run via RELATIVE ./path.
    sc3(SYS_MKDIR,(u64)"/tmp/cfgt",0755,0);
    serial("[EXEC] configure-style: cd /tmp/cfgt; gcc conftest.c -o conftest; ./conftest\n");
    spawn_wait("/bin/sh",ARGV_CFG,ENVP);
    serial("[EXEC] relative exec from /tmp...\n");
    spawn_wait("/bin/sh",ARGV_DOTSLASH,ENVP);
    serial("EXEC_DONE\n");
    sc1(SYS_EXIT,0);
}

// --- cc1 crash probe (LAUNCHER_CFLAGS=-DREPRO_CC1=1) -------------------------
// During make, cc1 crashed now and then reading a small bad pointer (0x78xx) in
// unrelated code. This compiles zlib's 15 files all at once, 40 rounds, so the
// crash (if it is load- or churn-related) shows up in minutes, not hours.
#ifndef REPRO_CC1
#define REPRO_CC1 0
#endif
static const char* const ARGV_CC1_STRESS[] = {"sh","-c",
    "gzip -dc /bin/binutils-insrc.tar.gz | tar -x -C /work binutils-2.42/zlib && "
    "cd /work/binutils-2.42/zlib && fails=0 && i=0 && "
    "while [ $i -lt ${ROUNDS:-40} ]; do i=$((i+1)); "
    "for f in adler32 compress crc32 deflate gzclose gzlib gzread gzwrite infback "
    "inffast inflate inftrees trees uncompr zutil; do "
    "( gcc -O2 -DHAVE_UNISTD_H -c $f.c -o /work/tmp/$f.o || echo CC1STRESS_FAIL round=$i file=$f ) & "
    "done; wait; echo CC1STRESS_ROUND=$i; done",0};

// --- leak probe (LAUNCHER_CFLAGS=-DREPRO_LEAK=1) ------------------------------
// P7.1 ran out of memory: ~360 KB of RAM (~58 KB of kernel heap) lost per exec.
// Each phase runs one kind of process churn; the kernel's "[mem] execs=" line
// (every 256 execs) shows which one leaks.
#ifndef REPRO_LEAK
#define REPRO_LEAK 0
#endif
static const char* const ARGV_TRUE[] = {"/bin/true",0};
static const char* const ARGV_LEAK_SH[] = {"sh","-c",
    "i=0; while [ $i -lt 1024 ]; do /bin/true; i=$((i+1)); done",0};
static const char* const ARGV_LEAK_PIPE[] = {"sh","-c",
    "i=0; while [ $i -lt 512 ]; do /bin/true | /bin/true; i=$((i+1)); done",0};
static const char* const ARGV_LEAK_SUBSHELL[] = {"sh","-c",
    "i=0; while [ $i -lt 1024 ]; do x=$(echo a); i=$((i+1)); done",0};
static const char* const ARGV_LEAK_GCC[] = {"sh","-c",
    "echo 'int f(int x){return x+1;}' > /work/tmp/l.c; i=0; "
    "while [ $i -lt 200 ]; do gcc -c /work/tmp/l.c -o /work/tmp/l.o; i=$((i+1)); done",0};
static void repro_leak(void){
    sc3(SYS_MKDIR,(u64)"/work",0755,0);
    sc3(SYS_MKDIR,(u64)"/work/tmp",0755,0);
    serial("[LEAK] phase spawn: /bin/true x1024 from the launcher\n");
    for(int i=0;i<1024;i++) spawn_wait("/bin/true", ARGV_TRUE, ENVP);
    serial("[LEAK] phase sh: fork+exec /bin/true x1024\n");
    spawn_wait("/bin/sh", ARGV_LEAK_SH, ENVP);
    serial("[LEAK] phase pipe: /bin/true | /bin/true x512\n");
    spawn_wait("/bin/sh", ARGV_LEAK_PIPE, ENVP);
    serial("[LEAK] phase subshell: $(echo) x1024 (fork, no exec)\n");
    spawn_wait("/bin/sh", ARGV_LEAK_SUBSHELL, ENVP);
    serial("[LEAK] phase gcc: gcc -c x200\n");
    spawn_wait("/bin/sh", ARGV_LEAK_GCC, ENVP);
    serial("[LEAK] phase spawn again: /bin/true x1024\n");
    for(int i=0;i<1024;i++) spawn_wait("/bin/true", ARGV_TRUE, ENVP);
    serial("LEAK_DONE\nGCCSELF_DONE\n");
    sc1(SYS_EXIT,0);
}

void _start(void){
    serial("\n[GCCSELF] start: in-OS gcc rebuilds binutils (P7.1)\n");
    setup_stdio();
#if REPRO_LEAK
    repro_leak();
#endif
#if REPRO_CC1
    sc3(SYS_MKDIR,(u64)"/work",0755,0);
    sc3(SYS_MKDIR,(u64)"/work/tmp",0755,0);
    serial("[GCCSELF] cc1 stress: zlib x15 in parallel, 40 rounds\n");
    {
        i64 src = spawn_wait("/bin/sh", ARGV_CC1_STRESS, ENVP);
        serial("CC1STRESS_RC="); put_dec(src); serial("\n");
    }
    serial("GCCSELF_DONE\n");
    sc1(SYS_EXIT,0);
#endif
#if REPRO_EXEC
    repro_exec();
#endif
#if REPRO_BUG4
    repro_bug4();
#endif

    sc3(SYS_MKDIR,(u64)"/work",0755,0);
    sc3(SYS_MKDIR,(u64)"/work/tmp",0755,0);
    sc3(SYS_MKDIR,(u64)"/work/bu-build",0755,0);
    serial("[GCCSELF] untar binutils-2.42 source\n");
    i64 rc = spawn_wait("/bin/sh", ARGV_UNTAR, ENVP);
    serial("GCCSELF_BU_UNTAR_RC="); put_dec(rc); serial("\n");
    int untar_ok = file_exists("/work/binutils-2.42/configure");
    serial(untar_ok ? "GCCSELF_BU_UNTAR_OK\n" : "GCCSELF_BU_UNTAR_FAIL\n");

    serial("[GCCSELF] configure (native instantos; sh+sed+grep+awk+gcc probes)\n");
    rc = spawn_wait("/bin/sh", ARGV_CONFIGURE, ENVP);
    serial("GCCSELF_BU_CONFIGURE_RC="); put_dec(rc); serial("\n");
    // The top level configures only itself; gas/, ld/, bfd/... are configured
    // by make (all-gas -> configure-gas), so their Makefiles do not exist yet.
    int cfg_ok = exit_code(rc) == 0 && file_exists("/work/bu-build/Makefile")
              && file_exists("/work/bu-build/config.status");
    serial(cfg_ok ? "GCCSELF_BU_CONFIGURE_OK\n" : "GCCSELF_BU_CONFIGURE_FAIL\n");
    if(!cfg_ok) dump_file("/work/bu-build/config.log");

    serial("[GCCSELF] make (in-OS gcc compiles bfd/opcodes/gas/ld/binutils)\n");
    rc = spawn_wait("/bin/sh", ARGV_MAKE, ENVP);
    serial("GCCSELF_BU_MAKE_RC="); put_dec(rc); serial("\n");
    int as_elf = is_elf("/work/bu-build/gas/as-new");
    int ld_elf = is_elf("/work/bu-build/ld/ld-new");
    serial(as_elf ? "GCCSELF_BU_AS_ELF_OK\n" : "GCCSELF_BU_AS_ELF_BAD\n");
    serial(ld_elf ? "GCCSELF_BU_LD_ELF_OK\n" : "GCCSELF_BU_LD_ELF_BAD\n");

    i64 as_run = -1, ld_run = -1;
    if(as_elf){ as_run = spawn_wait("/work/bu-build/gas/as-new", ARGV_AS_V, ENVP);
        serial("GCCSELF_BU_AS_RUN_RC="); put_dec(as_run); serial("\n"); }
    if(ld_elf){ ld_run = spawn_wait("/work/bu-build/ld/ld-new", ARGV_LD_V, ENVP);
        serial("GCCSELF_BU_LD_RUN_RC="); put_dec(ld_run); serial("\n"); }
    if(as_elf && exit_code(as_run)==0) serial("GCCSELF_BU_AS_OK\n");
    if(ld_elf && exit_code(ld_run)==0) serial("GCCSELF_BU_LD_OK\n");

    if(untar_ok && cfg_ok && rc==0 && as_elf && ld_elf &&
       exit_code(as_run)==0 && exit_code(ld_run)==0)
        serial("GCCSELF_BINUTILS_OK\n");

    serial("GCCSELF_DONE\n");
    sc1(SYS_EXIT,0);
}
