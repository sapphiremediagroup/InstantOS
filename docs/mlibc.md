# mlibc Port

InstantOS keeps the existing `ilibcxx` runtime as the default libc while mlibc is
ported incrementally. The mlibc integration is opt-in and uses a pinned upstream
checkout instead of vendoring the full source tree into this repository.

## Source Checkout

Run `tools/fetch-mlibc.sh` to clone mlibc into
`outside/iUserApps/outside/mlibc`. The script pins the checkout to upstream
commit `c367b780a47dc1d6c70d19a8379f4a74e5a7ed96`.

## Build

Run `tools/build-mlibc.sh` after fetching, or configure CMake with
`-DINSTANTOS_ENABLE_MLIBC=ON` and build the `mlibc` target. The build script
copies `outside/iUserApps/outside/mlibc-sysdeps/instantos` into the mlibc
checkout and invokes Meson with `tools/mlibc/instantos-x86_64.ini`.

Run `tools/build-mlibc-hello.sh` after building mlibc to produce a minimal
mlibc-linked dynamic executable at `build/mlibc-hello`. The executable uses
`/lib/ld-instantos.so` as its interpreter by default; use the `OUTPUT` and
`MLIBC_ROOT` environment variables to redirect paths while testing.

## Current Scope

The scaffold builds and installs mlibc under `build/mlibc-root`. It includes
syscall entry wrappers, `-errno` translation, startup entry glue, FS-base
thread-pointer support, and initial sysdeps for write/read, open/close,
anonymous memory, time, process id, sleep/yield, and logging.

It additionally implements the process/terminal sysdeps required by an
interactive shell: `Fork`, `Execve`, `Waitpid`, `Dup`, `Dup2`, `Pipe`,
`Openat`, `Ioctl`, `Tcgetattr`, `Tcsetattr`, and a real `Isatty` (probing via
`TIOCGWINSZ`). These map onto the kernel's PTY/TTY subsystem (`/dev/ptmx`,
`/dev/pts/N`), `fork()` with address-space copy, and fd inheritance across
`spawn`/`exec`. The `crt1.S` entry stub is position-independent so binaries link
as PIE against the sysroot.

### Filesystem/process sysdeps for coreutils ("Tier 1")

The following sysdeps are thin wrappers over kernel syscalls that already exist,
added to unblock the GNU coreutils core (`mv`, `ln`, `chmod`, `touch`, and the
`-p`/`-l`/`-s` flags of `cp`):

* `Chmod`/`Fchmod`        -> kernel `Chmod` (94), by-path and by-handle.
* `Truncate`/`Ftruncate`  -> kernel `Truncate` (92).
* `Rename`                -> kernel `Rename` (93).
* `Link`                  -> kernel `Link` (97).
* `Symlink`               -> kernel `Symlink` (98).
* `Readlink`              -> kernel `Readlink` (99).
* `Utimensat`             -> kernel `Utime` (95). Resolves `UTIME_NOW`/
  `UTIME_OMIT` and a null `times` in userspace (the kernel is second-resolution
  and has no OMIT concept; OMIT reads the current value via `stat`/`lstat`).
* `Kill`                  -> kernel `Kill` (11).
* `Fchdir`                -> returns `ENOSYS` (the kernel has no fd->path lookup;
  `chdir` is path-based).

The `*at` variants are distinct mlibc sysdeps (NOT auto-routed to the base
calls): coreutils invokes `renameat`, `symlinkat`, `fchmodat`, `linkat`,
`readlinkat`, `faccessat`, so each is implemented as its own sysdep that ignores
`dirfd` (AT_FDCWD-relative only) and delegates to the same kernel syscall.
`Umask` is tracked in libc (process-local), since the kernel has no umask.

Two registration steps are required for any new sysdep and are easy to miss:
1. add the syscall number to
   `mlibc-sysdeps/instantos/include/bits/syscall.h`, and
2. add the tag to the `InstantOSSysdepTags` inheritance list in
   `mlibc-sysdeps/instantos/include/mlibc/sysdeps.hpp` — otherwise
   `Sysdeps<Tag>` resolves to `NoImpl` and the out-of-line definition fails to
   compile ("does not match any declaration in `mlibc::NoImpl`").

### close() of std streams

The `Close` sysdep returns success (not `EBADF`) for fd 0/1/2. The kernel
refuses to close the standard handles, but libc stdio teardown
(`fclose(stdout)` / coreutils' `close_stdout`) closes them to flush and detect
errors; returning `EBADF` produced spurious "error closing file" / "write
error" diagnostics from every forked utility.

### FAT32 rename / chmod / utime / statfs

`FAT32FS` now implements `rename` (move/rename within the volume, preserving the
cluster chain — no data copy; replaces an existing destination; fixes a moved
directory's `..`). `chmod` and `utime` are accepted as no-ops (FAT has no Unix
permission bits and only coarse timestamps), mirroring Linux's `vfat` so
`chmod`/`touch`/`cp -p` succeed. Symlinks remain unsupported on FAT32. The
kernel `sys_rename` maps VFS failures to accurate errnos (`EXDEV` cross-device,
`ENOSYS` if the filesystem lacks rename, else `ENOENT`).

`statfs` is implemented end-to-end: a new `statfs` op on `VNodeOps`
(`FsStats`), a kernel `Statfs` syscall (121, by-path or by-handle), FAT32
`nodeStatfs` (cluster counts from the BPB + a FAT free-cluster scan), and
`Statvfs`/`Fstatvfs` sysdeps that translate the kernel `KernelStatfs` into
`struct statvfs`. A non-ABI-breaking `f_type` field was added to mlibc's
`struct statvfs` (repurposing a spare slot) so coreutils' configure selects the
`statvfs` path (`USE_STATVFS=1`) and `stat`/`df` compile. This unblocks `df` and
`stat -f`. (Note: every `VNodeOps` instance is now value-initialized — `ops{}` —
so the new trailing op pointers default to null.)

### File ownership (chown)

File ownership is plumbed end-to-end: `FileStats` carries `uid`/`gid`, a new
`chown` op on `VNodeOps`, `VFS::chown` (by-path with follow/no-follow, and
by-fd), and a kernel `Chown` syscall (122). RamFS stores `uid`/`gid` on the node
and reports them via `stat`; FAT32 accepts `chown` as a no-op (FAT has no Unix
ownership, like Linux's `vfat`). `copyStatToUser` now reports the real
`st_uid`/`st_gid`. mlibc routes `chown`/`lchown`/`fchown`/`fchownat` all through
a single `Fchownat` sysdep (AT_FDCWD path, AT_SYMLINK_NOFOLLOW for lchown,
AT_EMPTY_PATH for fchown). This unblocks `chown`, `chgrp`, and `cp -p`.

### Special files (mknod / mkfifo) and /tmp tmpfs

`FileStats` carries `rdev`; a new `mknod` op on `VNodeOps`, `VFS::mknod`, and a
kernel `Mknod` syscall (123) create FIFO and char/block device nodes (type
selected from the `S_IFMT` bits of `mode`). RamFS implements `nodeMknod` (FIFO,
char/block with `rdev`) and reports `rdev` in `stat`; FAT32 returns `ENOSYS`
(no special files on FAT). `copyStatToUser` reports `st_rdev` and the block/char
mode bits. mlibc's `mkfifo`→`Mkfifoat` (forces `S_IFIFO`) and `mknod`→`Mknodat`
route to the syscall.

Because FAT32 cannot represent Unix ownership or special files, the kernel now
mounts an in-memory **RamFS at `/tmp`** (tmpfs) at boot, giving a writable
surface with the full Unix semantics (`chown` readback, `mkfifo`, `mknod`).

### FIFO I/O (named pipes)

Named FIFOs created by `mkfifo` are now functional IPC channels, not just nodes.
A FIFO registry (`gFifos`, keyed by filesystem + inode) gives every open of the
same FIFO path one shared `PipeObject` ring buffer, reusing the existing pipe
read/write/poll machinery. `sys_open` intercepts `FileType::Pipe` nodes and
applies POSIX rendezvous semantics: `open(O_RDONLY)` blocks until a writer is
present and `open(O_WRONLY)` blocks until a reader is present (unless
`O_NONBLOCK`); reads block until data or EOF (all writers closed), writes block
until space. Verified with a cross-process producer/consumer
(`cat fifo & ... echo > fifo`).

### Entropy (getentropy / getrandom, /dev/urandom)

A kernel entropy source (`common/krandom.cpp`, `kernel_fill_entropy`) mixes the
cycle counter (`rdtsc`), the millisecond timer, and the RTC unix time through a
persistent SplitMix64 pool, advancing state each call so output never repeats
(best-effort until a hardware RNG is wired). It backs two paths:
* a kernel `GetEntropy` syscall (124) and the `GetEntropy` sysdep, which
  `getentropy()`/`getrandom()` route through; and
* `/dev/urandom` and `/dev/random` char devices in DevFS (gnulib's `getrandom`
  falls back to opening these). DevFS also gained `/dev/null` and `/dev/zero`.

This unblocks `shuf`, `shred`, and `mktemp` (random temp suffixes).

### Process limits / credentials / timing sysdeps

A batch of cheap sysdeps, previously unimplemented (so they aborted with
"missing sysdep"), now return sane values — fully quieting that noise for
`sort`, `id`, `date`, etc.:
* `GetRlimit`/`SetRlimit` — report effectively-unlimited limits
  (`RLIMIT_NOFILE` = 256, matching the userspace fd table); set is a no-op.
* `Times` — zero CPU time, millisecond real-time tick.
* `ClockSet` — accepts as a no-op (the RTC is not settable from userspace).
* `ClockGetres` — 1 ms resolution.
* `GetSid` — kernel `GetSessionID` (28).
* `GetGroups` — the single primary gid.
* `GetResuid`/`GetResgid` — real == effective == saved (kernel tracks one id).
* `Fadvise` — `posix_fadvise` is advisory, so a no-op is correct.

None required kernel changes beyond a `GetSessionID` syscall-number constant;
they wrap existing `GetUid`/`GetGid`/`GetSessionID`/`GetTime` syscalls or return
fixed values in libc.

### User/group database (/etc)

The kernel mounts a small in-memory **RamFS at `/etc`** at boot and seeds it with
`/etc/passwd` (`root:x:0:0:root:/:/bin/bash`), `/etc/group` (`root:x:0:`), and
`/etc/hostname`. libc's `getpwuid()`/`getgrgid()` `fopen()` these standard
colon-delimited files, so `id`, `whoami`, and `ls -l` now resolve numeric ids to
names (`uid=0(root) gid=0(root)`, `whoami` -> `root`) instead of bare numbers. A
`writeSystemFile()` boot helper creates the files via the VFS.

### Mount table (/etc/mtab, df, getmntent)

The kernel seeds `/etc/mtab` at boot from the live VFS mount table
(`VFS::forEachMount`), one standard `fsname dir type opts freq passno` line per
mount. The `getmntent`/`setmntent`/`getmntent_r` family (in the sysdeps
`builtins.c`) is now a real musl-derived parser that reads this file, replacing
the empty stub. So `df` with no arguments enumerates every mount (instead of
warning "cannot read table of mounted file systems"), and `mount` can list them.

For `df` to attribute paths to the right filesystem, each `nodeStat` now reports
a distinct `st_dev` (the `FileSystem*` pointer) — without it df deduped all
mounts to device 0 and mis-reported (`df /` showed `/bin`). InitrdFS and DevFS
also gained a `statfs` op so df can stat pseudo-filesystems without erroring.

### Load average (uptime, getloadavg)

`GetLoadavg` is implemented (reports `0.00, 0.00, 0.00` — the kernel keeps no
load accounting), so `uptime`/`getloadavg()` print the load averages.

### Login records (utmp) — who, users, uptime duration

`who`, `users`, and `uptime`'s "up" duration work via a real utmp database:
* The `abis/linux/utmpx.h` + `utmp-defines.h` headers are installed into the
  sysroot (they define `struct utmpx` and `BOOT_TIME`/`USER_PROCESS`), so
  coreutils' `readutmp` selects the utmpx path (`HAVE_UTMPX_H=1`).
* The kernel mounts a `/var` tmpfs and seeds `/var/run/utmp` (= `_PATH_UTMP`)
  with a `BOOT_TIME` record (boot wall-clock) and a `root` `USER_PROCESS`
  record. `writeUtmpRecord` writes the exact 400-byte `struct utmpx` layout at
  fixed offsets (`ut_type`@0, `ut_user`@44, `ut_tv.tv_sec`@344).

A prerequisite fix: `ClockGet` (backing `time()`/`clock_gettime`) returned the
millisecond *uptime* for `CLOCK_REALTIME`, so `time()` reported seconds-since-
boot — making uptime compute a negative duration (`up ????`) and all timestamps
wrong. It now returns wall-clock Unix time (`GetUnixTime`) for `CLOCK_REALTIME`
and the uptime counter only for `CLOCK_MONOTONIC`/`_BOOTTIME`. Result:
`uptime` -> ` 19:45:42  up 0:00, 1 user, load average: 0.00, 0.00, 0.00`,
`who` -> `root console Jun 13 19:45`.

### coreutils validation

`tools/run-coreutils-smoke.sh` cross-builds real GNU coreutils 9.5 against mlibc
(`tools/build-coreutils.sh`), bundles them into the initrd, and drives them
through `bash -i` over a PTY against a writable FAT32 root disk. It verifies
`cat`, `cp`, `mv` (FAT32 rename, data preserved), `chmod`, `touch`, `ls`, `wc`,
`head`, `rm`, `[ -r ]` (the `Access` syscall), `df` (per-path and no-arg
`/etc/mtab` enumeration) and `stat -f` (the `statvfs` path), `chown`/`chgrp` (the `Chown` syscall), `id`, `mkfifo`/`mknod` (the
`Mknod` syscall, on the `/tmp` tmpfs; FIFOs are functional IPC channels), `shuf` (`/dev/urandom`), `mktemp`
(`getentropy`), `sort` (`GetRlimit`) and `id`/`whoami` (`GetResuid`/`GetGroups`/
`GetSid` + `/etc/passwd` name resolution, `uid=0(root)`), and
`uptime`/`who`/`users` (utmp login records: `up 0:00, 1 user`).
The smoke also asserts that no "missing sysdep" abort is logged for the
implemented sysdeps. Passing.

## TTY / PTY subsystem

The kernel provides a Linux-style teletype layer:

* `/dev/ptmx` opens a new PTY master; `ioctl(TIOCGPTN)` returns the slave index.
* `/dev/pts/N` is the matching slave, with in-kernel line discipline (canonical
  and raw modes, echo, `ICRNL`/`OPOST`/`ONLCR`, `VERASE`/`VKILL`/`VEOF`, and
  `VINTR`/`VQUIT` signal generation).
* termios/winsize ioctls: `TCGETS`/`TCSETS{,W,F}`, `TIOCGWINSZ`/`TIOCSWINSZ`,
  `TIOCGPTN`, `TIOCG/SPGRP`, `TIOCSCTTY`.
* `fork()` deep-copies the user address space; `spawn` inherits the parent's
  file descriptors so a shell's children share its controlling terminal.

The graphics terminal app (`outside/iUserApps/terminal`) is a PTY master with an
ANSI/VT parser: it opens `/dev/ptmx`, binds `/dev/pts/N` to a spawned shell's
stdio, renders the cell grid, and forwards keystrokes to the master.

## bash port (working end-to-end, incl. interactive PTY)

`tools/build-bash.sh` fetches GNU bash 5.2.21 and cross-builds it against the
mlibc sysroot, producing a dynamically-linked PIE bash that uses
`/lib/mlibc/ld-instantos.so` and links `libc.so`/`libdl.so`. Configure now
passes the compiler/PIE checks, the build links with **zero unresolved dynamic
symbols** against the mlibc runtime, and bash boots and runs inside the guest:
it reaches its mlibc entry point, executes a `-c` script (builtins and
arithmetic), writes its output to the console, and exits cleanly with status 0.

The `snprintf`/`vsnprintf`/`uselocale` "missing function" reports from older
notes were a misdiagnosis: those functions are present in `libc.so`. The actual
runtime blocker was a `#UD` ("Unknown Instruction") crash in the dynamic linker
at startup — see "rtld own-constructor crash" below.

### rtld own-constructor crash (fixed)

The mlibc rtld (`options/rtld/generic/main.cpp`) panics via `__builtin_trap()`
(which emits `ud2`, surfacing as the kernel's "exception: Unknown Instruction")
if `ld-instantos.so` contains any of its own global constructors, i.e. a
non-empty `.init_array`. The InstantOS sysdeps file
(`mlibc-sysdeps/instantos/sysdeps.cpp`) was emitting a static constructor
(`_GLOBAL__sub_I_sysdeps.cpp`) to default-initialize its file-scope `fdTable`
and `dirStreams` globals, which got linked into the rtld and tripped the panic.

The fix is to force compile-time initialization with `constinit` on those
globals so no constructor is generated. **Any new file-scope object in the
sysdeps translation unit must likewise be constant-initialized** (use
`constinit` / value-initialization) or it will reintroduce the crash.

### stdout lost on the console (fixed)

After the rtld fix, bash ran and exited cleanly but its `echo` output never
appeared: every write logged "mlibc: fwrite() I/O errors are not handled" and
no `write()` syscall was issued. The cause was in `fd_file::determine_type()`
(`options/ansi/generic/file-io.cpp`): mlibc probes a stream with
`lseek(fd, 0, SEEK_CUR)` to classify it as file-like vs pipe-like. It only
treats `ESPIPE` as "not seekable"; any other error fails the whole write before
`io_write()` runs.

The kernel's `sys_write`/`sys_ioctl`/`sys_seek` special-case bare stdio fds
0/1/2 as the console when no real `File` handle is bound to that slot, but
`sys_seek` returned `EBADF` for that case instead of `ESPIPE`. The fix
(`src/cpu/syscall/fs.cpp`): when a bare stdio fd has no bound `File`, `sys_seek`
returns `ESPIPE` (the console is a non-seekable, pipe-like stream), so mlibc
classifies it correctly and buffered writes flush to the console.

### Validation

* `tools/run-mlibc-smoke.sh` boots a minimal mlibc binary as the init process
  and checks for a `mlibc-hello: serial-done` marker over serial. Passing.
* `tools/run-bash-smoke.sh` boots a tiny freestanding launcher (in
  `tools/bash-smoke/launcher.c`) that `spawn`s `/bin/bash -c '<script>'`, waits
  for it, and reports markers over serial. It asserts bash reaches main, that
  its stdout (`BASH_SMOKE_OK`, `arith=42`, `shell=bash`) reaches the console,
  that bash exits with status 0, and that no user-process crash occurred.
  Passing.
* `tools/run-pty-smoke.sh` boots a PTY launcher (`tools/bash-smoke/pty_launcher.c`)
  that mirrors the graphics terminal: it opens `/dev/ptmx`, binds the
  `/dev/pts/N` slave to a spawned `bash -i`, then scripts commands to the master
  and forwards bash's output back to serial. It asserts the full interactive
  loop works — line-discipline echo, command output (`PTY_HELLO`),
  variable+arithmetic expansion (`val=21`), `for` loops (`loop1`..`loop3`),
  `pwd`, and a clean `exit` (status 0). Passing. This exercises the same kernel
  PTY/line-discipline/tty-buffering path the graphics terminal uses.

  Note: the PTY harness must call `poll(fds, nfds, /*timeoutMs=*/0)` for a
  non-blocking scan; the kernel `Poll` syscall blocks for any non-zero timeout
  (and forever for `-1`). Passing a garbage third argument deadlocks the master
  reader against bash's blocking slave read.

### Known follow-ups

* **Job control / process groups.** Under `bash -i`, bash logs
  `cannot set terminal process group (-1): Bad file descriptor` and
  `no job control in this shell`, then continues. The kernel has no real process
  groups/sessions yet (`SetPgid` is a no-op, `GetPgid` returns the pid), so
  `tcsetpgrp()`/foreground-group signal delivery (Ctrl-C to the foreground job,
  `fg`/`bg`) are not functional. Interactive command entry, echo, expansion,
  builtins, pipelines, and exit all work without it. Implementing real process
  groups + sessions + foreground-group signal routing is the next substantial
  piece for full interactive fidelity.
* `getpeername`/`getsockname` (`Peername`/`Sockname`) sysdeps are not
  overridable in the current mlibc option set, so bash's startup
  `getpeername(0)` socket probe logs a cosmetic `sysdep_or_enosys` message;
  execution continues correctly.
* Remaining POSIX breadth (locale corners, additional sysdeps) as bash and
  other software exercise them.

