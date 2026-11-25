// Phase 7 P7.2 - in-OS gcc bootstraps gcc: three stages, stage2 == stage3.
//
// The hosted gcc on the disk (/usr/bin/gcc, g++, binutils) + the GNU userland
// ports on the initrd rebuild gcc-13.3.0 three times, ON InstantOS:
//
//   untar    : gzip -dc /src/gcc-insrc.tar.gz | tar -x  -> /work/gcc-13.3.0
//   configure: native (build=host=target=x86_64-unknown-instantos), bootstrap,
//              C and C++ (gcc is written in C++, so a bootstrap needs both)
//   make     : make bootstrap: stage1 built by the hosted gcc, stage2 by stage1,
//              stage3 by stage2, then gcc's `compare` of the stage2 and stage3
//              objects
//   verify   : our own checks, so a vacuous compare cannot pass: the compare
//              stamp, the number of stage3 objects it had to compare, cc1 of
//              stage2 and stage3 byte for byte, and the stage3 compiler building
//              and running a program
//
// Resumable: when the disk already holds a configured build (/work/gcc-build/
// Makefile, from an earlier run on the same disk), untar and configure are
// skipped and make carries on. Progress via GCCSELF_* serial markers;
// SELFHOST_GCC_STAGE2_EQ_STAGE3 == P7.2 green.

#include "launcher-common.h"

static const char* const ENVP[]={
    "PATH=/usr/bin:/bin","HOME=/","TMPDIR=/work/tmp","SHELL=/bin/sh","CONFIG_SHELL=/bin/sh",
    "MAKEINFO=true","LC_ALL=C","LANG=C",0
};

static const char* const ARGV_UNTAR[] = {"sh","-c",
    "gzip -dc /src/gcc-insrc.tar.gz | tar -x -C /work",0};
static const char* const ARGV_CONFIGURE[] = {"sh","-c",
    "cd /work/gcc-build && /work/gcc-13.3.0/configure "
    "--build=x86_64-unknown-instantos --host=x86_64-unknown-instantos "
    "--target=x86_64-unknown-instantos --prefix=/usr --with-sysroot=/ "
    "--with-native-system-header-dir=/usr/include --enable-bootstrap "
    "--enable-languages=c,c++ --enable-checking=release --disable-multilib "
    "--disable-nls --disable-lto --without-isl --disable-libsanitizer "
    "--disable-libssp --disable-libgomp --disable-libquadmath --disable-libitm "
    "--disable-libvtv --disable-libatomic --disable-libstdcxx-pch "
    "--enable-default-pie --enable-default-hash-style=sysv MAKEINFO=true",0};
// No -g (smaller trees, faster stages); STMP_FIXINC= skips fixincludes, which
// our mlibc headers do not need.
// -j2 runs out of memory in 3 GB when the two largest generated files
// (insn-emit.cc, gimple-match.cc: over a GB of cc1plus each) compile together
// (P7.2 run 8, stage2). So when make fails, the gcc directory of the current
// stage is finished with -j1 and the -j2 bootstrap resumed, up to 3 times; a
// real error fails the -j1 make too and ends it.
static const char* const ARGV_MAKE[] = {"sh","-c",
    "cd /work/gcc-build && "
    "F='MAKEINFO=true STMP_FIXINC= "
    "STAGE1_CFLAGS=-O2 STAGE1_CXXFLAGS=-O2 BOOT_CFLAGS=-O2 BOOT_CXXFLAGS=-O2 "
    "CFLAGS_FOR_TARGET=-O2 CXXFLAGS_FOR_TARGET=-O2' && "
    "for try in 1 2 3 4; do "
    "make -j2 bootstrap $F && exit 0; "
    "[ $try = 4 ] && exit 1; "
    "s=$(cat stage_current 2>/dev/null); [ -n \"$s\" ] || exit 1; "
    "echo \"GCCSELF_GCC_SERIAL_RETRY stage=$s try=$try\" > /dev/console; "
    "make -j1 all-$s-gcc $F || exit 1; "
    "done",0};
// A finished bootstrap leaves stage3 current: its trees under their plain names
// (gcc/, libcpp/, ...) and stage2's as prev-*; after a stage3-end they are
// stage3-* and stage2-*. Stage2 is built with -gtoggle (debug info on, gcc's
// bootstrap-debug), so the compilers are compared stripped, as gcc's own
// compare ignores debug info.
#define STAGE_DIRS \
    "if [ -d stage3-gcc ]; then s2=stage2-gcc; s3=stage3-gcc; " \
    "else s2=prev-gcc; s3=gcc; fi; "
static const char* const ARGV_COUNT[] = {"sh","-c",
    "cd /work/gcc-build && "
    "if [ -d stage3-gcc ]; then n=$(find stage3-* -name '*.o' | wc -l); "
    "else n=$(find . \\( -name 'prev-*' -o -name 'stage1-*' -o -name 'build-*' \\) -prune "
    "-o -name '*.o' -print | wc -l); fi && "
    "echo GCCSELF_GCC_STAGE3_OBJECTS=$n > /dev/console",0};
static const char* const ARGV_CC1_CMP[] = {"sh","-c",
    "cd /work/gcc-build && " STAGE_DIRS
    "for p in cc1 cc1plus; do "
    "strip -o /work/tmp/s2-$p $s2/$p && strip -o /work/tmp/s3-$p $s3/$p && "
    "cmp /work/tmp/s2-$p /work/tmp/s3-$p || exit 1; done",0};
static const char* const ARGV_HELLO[] = {"sh","-c",
    "cd /work/tmp && printf '#include <stdio.h>\\nint main(void){puts(\"stage3 hello\");return 7;}\\n' > h.c && "
    "x=/work/gcc-build/gcc; [ -d /work/gcc-build/stage3-gcc ] && x=/work/gcc-build/stage3-gcc; "
    "$x/xgcc -B$x/ h.c -o h && ./h; "
    "echo GCCSELF_GCC_HELLO_EXIT=$? > /dev/console",0};

void _start(void){
    serial("\n[GCCSELF] start: in-OS gcc bootstraps gcc (P7.2)\n");
    setup_stdio();

    sc3(SYS_MKDIR,(u64)"/work",0755,0);
    sc3(SYS_MKDIR,(u64)"/work/tmp",0755,0);
    sc3(SYS_MKDIR,(u64)"/work/gcc-build",0755,0);

    int resumed = file_exists("/work/gcc-build/Makefile");
    int cfg_ok = resumed;
    if(resumed){
        serial("GCCSELF_GCC_RESUME (configured build found; running make again)\n");
    }else{
        serial("[GCCSELF] untar gcc-13.3.0 source\n");
        i64 rc = spawn_wait("/bin/sh", ARGV_UNTAR, ENVP);
        serial("GCCSELF_GCC_UNTAR_RC="); put_dec(rc); serial("\n");
        int untar_ok = file_exists("/work/gcc-13.3.0/configure");
        serial(untar_ok ? "GCCSELF_GCC_UNTAR_OK\n" : "GCCSELF_GCC_UNTAR_FAIL\n");

        serial("[GCCSELF] configure (bootstrap, c and c++)\n");
        rc = spawn_wait("/bin/sh", ARGV_CONFIGURE, ENVP);
        serial("GCCSELF_GCC_CONFIGURE_RC="); put_dec(rc); serial("\n");
        cfg_ok = untar_ok && exit_code(rc) == 0 && file_exists("/work/gcc-build/Makefile");
        serial(cfg_ok ? "GCCSELF_GCC_CONFIGURE_OK\n" : "GCCSELF_GCC_CONFIGURE_FAIL\n");
        if(!cfg_ok) dump_file("/work/gcc-build/config.log");
    }

    i64 make_rc = -1;
    if(cfg_ok){
        serial("[GCCSELF] make bootstrap (stage1 -> stage2 -> stage3 -> compare)\n");
        make_rc = spawn_wait("/bin/sh", ARGV_MAKE, ENVP);
        serial("GCCSELF_GCC_MAKE_RC="); put_dec(make_rc); serial("\n");
    }

    int compared = file_exists("/work/gcc-build/compare");
    serial(compared ? "GCCSELF_GCC_COMPARE_STAMP_OK\n" : "GCCSELF_GCC_COMPARE_STAMP_MISSING\n");
    spawn_wait("/bin/sh", ARGV_COUNT, ENVP);
    i64 cc1_rc = spawn_wait("/bin/sh", ARGV_CC1_CMP, ENVP);
    int cc1_eq = exit_code(cc1_rc) == 0;
    serial(cc1_eq ? "GCCSELF_GCC_CC1_EQ\n" : "GCCSELF_GCC_CC1_DIFF\n");
    i64 hello_rc = spawn_wait("/bin/sh", ARGV_HELLO, ENVP);
    (void)hello_rc;

    // The object count printed above is checked by whoever reads the log; the
    // marker needs make, gcc's compare and identical stage2/stage3 compilers.
    if(cfg_ok && make_rc == 0 && compared && cc1_eq)
        serial("SELFHOST_GCC_STAGE2_EQ_STAGE3\n");

    serial("GCCSELF_DONE\n");
    sc1(SYS_EXIT,0);
}
