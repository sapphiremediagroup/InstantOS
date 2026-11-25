// /bin/login of the install test (tools/install-test/run-install-test.sh), in
// the system initrd otherwise unchanged. Two boots of one disk:
//   1. the live system (iboot.cfg root=none): runs
//      `instant-install --yes disk0p3`, then restarts;
//   2. iboot.cfg root=<that partition>: the kernel finds /.installed and takes
//      /bin and /lib from the partition. This driver, copied there with the rest
//      of /bin, sees /.installed, checks the installed tree, and starts the
//      compositor from the partition.
// Markers: [installtest] ...
#include "../os-testsuite/ostest.h"

enum { N_REBOOT = 153 };

static int exists(const char* path) {
    static u8 st[512];
    return SC2(N_STAT, path, st) == 0;
}

static void report(int ok, const char* what) {
    sw(ok ? "[installtest] ok: " : "[installtest] FAIL: ");
    sw(what);
    sw("\n");
}

__attribute__((force_align_arg_pointer)) void _start(void) {
    sw("\n[installtest] BEGIN\n");
    const char* ev[] = {"HOME=/", "PATH=/bin", 0};

    if (!exists("/.installed")) {
#ifdef INSTALL_UPDATE
        sw("[installtest] live system: updating disk0p3\n");
        const char* av[] = {"/bin/instant-install", "--update", "--yes", "disk0p3", 0};
#else
        sw("[installtest] live system: installing onto disk0p3\n");
        const char* av[] = {"/bin/instant-install", "--yes", "disk0p3", 0};
#endif
        i64 pid = SC3(N_SPAWN, "/bin/instant-install", av, ev);
        int status = -1;
        if (pid > 0) SC3(N_WAIT, pid, &status, 0);
        const int code = (status >> 8) & 0xFF;
        sw("[installtest] instant-install exit ");
        sdec(pid > 0 ? code : -1);
        sw("\n");
        sw(pid > 0 && code == 0 ? "[installtest] INSTALL_OK\n" : "[installtest] INSTALL_FAILED\n");
        msleep(2000);   // the ext4 journal and the log reach the disk
        SC1(N_REBOOT, 0);
        for (;;) msleep(1000);
    }

    sw("[installtest] booted the installed system\n");
    report(exists("/bin/instant-install"), "/bin is the partition's (programs there)");
    report(exists("/lib/ld-instantos.so"), "/lib is the partition's");
    report(exists("/lib/drivers"), "the driver modules are there");
    report(exists("/test/logs"), "/test/logs exists");
    report(exists("/bin/mke2fs") && exists("/bin/e2fsck"), "mke2fs and e2fsck installed");
    report(exists("/bin/cat") && exists("/bin/tar") && exists("/bin/gzip"), "coreutils, tar and gzip installed");
    if (exists("/bin/link-probe")) {
        const char* pav[] = {"/bin/link-probe", 0};
        i64 ppid = SC3(N_SPAWN, "/bin/link-probe", pav, ev);
        int pstatus = -1;
        if (ppid > 0) SC3(N_WAIT, ppid, &pstatus, 0);
    }
    // The GNU tools' chmod works and says why when it does not.
    {
        const char* script =
            "cd / && echo x > /chmodtest && chmod 755 /chmodtest && [ \"$(chmod 755 /nope 2>&1)\" != '' ]"
            " && chmod 755 /nope 2>&1 | grep -q 'No such file' && rm /chmodtest";
        const char* cav[] = {"/bin/bash", "-c", script, 0};
        i64 cpid = SC3(N_SPAWN, "/bin/bash", cav, ev);
        int cstatus = -1;
        if (cpid > 0) SC3(N_WAIT, cpid, &cstatus, 0);
        report(cpid > 0 && ((cstatus >> 8) & 0xFF) == 0, "CHMOD: changes a mode, and names the error");
    }
    // The toolchain instant-install unpacked: gcc and g++ build and run the
    // examples.
    if (exists("/usr/bin/gcc")) {
        const char* script =
            "export PATH=/usr/bin:/bin; cd /tmp"
            " && gcc /examples/hello.c -o hello -lm && ./hello"
            " && g++ /examples/hello.cpp -o hellocpp && ./hellocpp"
            " && { [ ! -f /examples/openssl.c ] || { gcc /examples/openssl.c -o ossl -lssl -lcrypto && ./ossl; }; }";
        const char* bav[] = {"/bin/bash", "-c", script, 0};
        i64 bpid = SC3(N_SPAWN, "/bin/bash", bav, ev);
        int bstatus = -1;
        if (bpid > 0) SC3(N_WAIT, bpid, &bstatus, 0);
        report(bpid > 0 && ((bstatus >> 8) & 0xFF) == 0, "DEVTOOLS: gcc and g++ built and ran the examples (and with -lssl)");
    }
    const char* av[] = {"/bin/graphics-compositor", 0};
    i64 pid = SC3(N_SPAWN, "/bin/graphics-compositor", av, ev);
    sw(pid > 0 ? "[installtest] INSTALLED_BOOT_OK\n" : "[installtest] FAIL: no compositor\n");
    for (;;) msleep(1000);
}
