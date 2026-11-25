// Phase 7 P7.3 - the stage3 gcc rebuilds the InstantOS userland, in-OS.
//
// Runs on the P7.2 disk, whose /work/gcc-build holds a finished bootstrap
// (stage3 current), with the world sources added at /src/world-src.tar.gz
// (tools/gcc-selfhost/make-world-src.sh):
//
//   install : make install of the bootstrap tree (--prefix=/usr), so the stage3
//             gcc replaces the disk's cross-built one as the system compiler;
//             checked by comparing the installed cc1/cc1plus with stage3's
//   untar   : /src/world-src.tar.gz -> /world
//   userland: /world/build-userland.sh: ld-instantos.so, libinstant.so, the
//             managers and apps, every compile and link by the stage3 g++
//   suite   : /world/build-suite.sh: the feature suite's runner and helper
//   mlibc   : /world/build-mlibc.sh: mlibc, installed into /world/mlibc-root
//   ports   : /world/build-ports.sh: bash (on that mlibc) and tinycc (on a tcc
//             sysroot made from the userland above), each by its own
//             configure + make
//
// The results stay on the disk (/world/iUserApps-build/{libinstant.so,out/},
// /world/suite, /world/mlibc-root, /world/out, /world/tcc-sysroot); the host
// copies them off and builds the test images from them
// (tools/gcc-selfhost/run-world-suite.sh, run-world-ladders.sh). A rerun on the same disk skips the
// install once it is checked; the world is rebuilt from scratch every time.
// GCCSELF_WORLD_OK == every expected program was built.

#include "launcher-common.h"

static const char* const ENVP[]={
    "PATH=/usr/bin:/bin","HOME=/","TMPDIR=/work/tmp","SHELL=/bin/sh","CONFIG_SHELL=/bin/sh",
    "MAKEINFO=true","LC_ALL=C","LANG=C",0
};

#define LIBEXEC "/usr/libexec/gcc/x86_64-unknown-instantos/13.3.0"
static const char* const ARGV_INSTALL[] = {"sh","-c",
    "cd /work/gcc-build && make install MAKEINFO=true STMP_FIXINC= > /work/tmp/install.log 2>&1; "
    "rc=$?; tail -30 /work/tmp/install.log; exit $rc",0};
// The installed compiler proper must be stage3's (install does not strip).
static const char* const ARGV_CHECK_INSTALL[] = {"sh","-c",
    "x=/work/gcc-build/gcc; [ -d /work/gcc-build/stage3-gcc ] && x=/work/gcc-build/stage3-gcc; "
    "cmp " LIBEXEC "/cc1 $x/cc1 && cmp " LIBEXEC "/cc1plus $x/cc1plus && "
    "gcc --version | head -1 > /dev/console && touch /work/tmp/p73-installed",0};
static const char* const ARGV_UNTAR[] = {"sh","-c",
    "rm -rf /world && gzip -dc /src/world-src.tar.gz | tar -x -C /",0};
static const char* const ARGV_USERLAND[] = {"sh","-c",
    "sh /world/build-userland.sh > /work/tmp/userland.log 2>&1; "
    "rc=$?; tail -40 /work/tmp/userland.log; exit $rc",0};
static const char* const ARGV_SUITE[] = {"sh","-c","sh /world/build-suite.sh",0};
static const char* const ARGV_MLIBC[] = {"sh","-c",
    "sh /world/build-mlibc.sh > /work/tmp/mlibc.log 2>&1; "
    "rc=$?; tail -40 /work/tmp/mlibc.log; exit $rc",0};
static const char* const ARGV_PORTS[] = {"sh","-c",
    "sh /world/build-ports.sh > /work/tmp/ports.log 2>&1; "
    "rc=$?; tail -60 /work/tmp/ports.log; exit $rc",0};
static const char* const ARGV_PROBE[] = {"sh","-c","sh -x /src/world-probe.sh 2>&1",0};
// What was built, for the log (the host checks the files themselves).
static const char* const ARGV_LIST[] = {"sh","-c",
    "cd /world && ls -l iUserApps-build/libinstant.so iUserApps-build/out suite mlibc-root/lib out "
    "tcc-sysroot/lib tcc-sysroot/lib/tcc > /dev/console",0};

static const char* const EXPECTED[] = {
    "/world/iUserApps-build/libinstant.so",
    "/world/iUserApps-build/out/ld-instantos.so",
    "/world/iUserApps-build/out/input-manager",
    "/world/iUserApps-build/out/storage-manager",
    "/world/iUserApps-build/out/process-manager",
    "/world/iUserApps-build/out/network-manager",
    "/world/iUserApps-build/out/font-manager",
    "/world/iUserApps-build/out/session-manager",
    "/world/suite/runner",
    "/world/suite/ostest-helper",
    "/world/mlibc-root/lib/libc.so",
    "/world/mlibc-root/lib/ld-instantos.so",
    "/world/mlibc-root/lib/libdl.so",
    "/world/mlibc-root/lib/libm.so",
    "/world/mlibc-root/lib/crt1.o",
    "/world/out/bash",
    "/world/out/tcc",
    "/world/tcc-sysroot/lib/crt0.o",
    0
};
// Not ELF: present is enough.
static const char* const EXPECTED_FILES[] = {
    "/world/mlibc-root/include/stdio.h",
    "/world/tcc-sysroot/lib/tcc/libtcc1.a",
    0
};

static int step(const char* name, const char* const* argv){
    serial("[GCCSELF] "); serial(name); serial("\n");
    i64 rc = spawn_wait("/bin/sh", argv, ENVP);
    serial("GCCSELF_WORLD_"); serial(name); serial("_RC="); put_dec(exit_code(rc)); serial("\n");
    return exit_code(rc) == 0;
}

void _start(void){
    serial("\n[GCCSELF] start: the stage3 gcc rebuilds the userland (P7.3)\n");
    setup_stdio();
    sc3(SYS_MKDIR,(u64)"/work/tmp",0755,0);

    // A quick in-OS experiment instead of the build: put a script at
    // /src/world-probe.sh on the disk (debugfs write) and only it runs.
    if(file_exists("/src/world-probe.sh")){
        step("PROBE", ARGV_PROBE);
        serial("GCCSELF_DONE\n");
        sc1(SYS_EXIT,0);
    }

    int ok = 1;
    if(!file_exists("/work/tmp/p73-installed")){
        ok = step("INSTALL", ARGV_INSTALL) && step("CHECK_INSTALL", ARGV_CHECK_INSTALL);
    }else{
        serial("GCCSELF_WORLD_INSTALL_RESUMED\n");
    }
    serial(ok ? "GCCSELF_WORLD_STAGE3_INSTALLED\n" : "GCCSELF_WORLD_STAGE3_INSTALL_FAIL\n");

    if(ok) ok = step("UNTAR", ARGV_UNTAR) && file_exists("/world/build-userland.sh");
    if(ok) ok = step("USERLAND", ARGV_USERLAND);
    if(ok) ok = step("SUITE", ARGV_SUITE);
    if(ok) ok = step("MLIBC", ARGV_MLIBC);
    if(ok) ok = step("PORTS", ARGV_PORTS);
    spawn_wait("/bin/sh", ARGV_LIST, ENVP);

    int all = ok;
    for(int i = 0; EXPECTED[i]; i++){
        if(!is_elf(EXPECTED[i])){ serial("GCCSELF_WORLD_MISSING "); serial(EXPECTED[i]); serial("\n"); all = 0; }
    }
    for(int i = 0; EXPECTED_FILES[i]; i++){
        if(!file_exists(EXPECTED_FILES[i])){ serial("GCCSELF_WORLD_MISSING "); serial(EXPECTED_FILES[i]); serial("\n"); all = 0; }
    }
    if(all) serial("GCCSELF_WORLD_OK\n");
    serial("GCCSELF_DONE\n");
    sc1(SYS_EXIT,0);
}
