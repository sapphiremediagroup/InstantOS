// What GNU tar's calls on a symlink return here (tools/install-test): tar's
// messages hide the errno ("Unknown system error"), so this prints the numbers.
// tar ignores only ENOSYS/EOPNOTSUPP (95) for links; anything else is an error.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void show(const char* what, int ret) {
    printf("[linkprobe] %s -> %d errno=%d\n", what, ret, ret ? errno : 0);
}

int main(void) {
    unlink("/tmp/lp-dangling");
    unlink("/tmp/lp-good");
    show("symlink dangling", symlink("lp-missing", "/tmp/lp-dangling"));
    show("symlink good", symlink("/bin/bash", "/tmp/lp-good"));
    struct stat st;
    int r = lstat("/tmp/lp-good", &st);
    printf("[linkprobe] lstat good -> %d mode=%o\n", r, (unsigned)st.st_mode);
    r = lstat("/tmp/lp-dangling", &st);
    printf("[linkprobe] lstat dangling -> %d mode=%o\n", r, (unsigned)st.st_mode);
    show("fchmodat good NOFOLLOW", fchmodat(AT_FDCWD, "/tmp/lp-good", 0777, AT_SYMLINK_NOFOLLOW));
    show("fchmodat dangling NOFOLLOW", fchmodat(AT_FDCWD, "/tmp/lp-dangling", 0777, AT_SYMLINK_NOFOLLOW));
    if (chdir("/tmp") == 0) {
        show("fchmodat relative NOFOLLOW", fchmodat(AT_FDCWD, "lp-good", 0777, AT_SYMLINK_NOFOLLOW));
    }
    struct timespec ts[2] = {{0, UTIME_OMIT}, {1700000000, 0}};
    show("utimensat good NOFOLLOW", utimensat(AT_FDCWD, "/tmp/lp-good", ts, AT_SYMLINK_NOFOLLOW));
    show("utimensat dangling NOFOLLOW", utimensat(AT_FDCWD, "/tmp/lp-dangling", ts, AT_SYMLINK_NOFOLLOW));
    show("lchown good", lchown("/tmp/lp-good", 0, 0));
    printf("[linkprobe] strerror(95) = %s\n", strerror(EOPNOTSUPP));
    return 0;
}
