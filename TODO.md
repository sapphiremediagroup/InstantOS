# TODO

## Desktop

- [x] Auto-start `terminal` after `graphics-compositor` is ready.
- [x] Add a `Super+T` compositor shortcut to launch `terminal`.
- [x] Make the taskbar center area clickable for Terminal, Files, Backgrounds, and Cube.
- [x] Add a small app launcher that lists launchable `/bin` programs and spawns the selected app.

## Kernel and Services

- [x] Implement `ProcInfo` and make `process-manager` report real process data.
- [x] Implement blocking `wait()` for child process termination.
- [x] Implement `pipe()` with readable/writable handles.
- [x] Make `storage-manager` real.
  - [x] Detect the persistent block device and report capacity, sector size, and read/write support.
  - [x] Add a minimal disk format path for an uninitialized persistent disk.
  - [x] Add a mount path for an already formatted persistent disk.
  - [x] Persist and reload filesystem metadata across boots.
  - [x] Expose disk state and mount errors through `storage-manager` APIs or diagnostics.
  - [x] Add boot-time smoke tests for empty, formatted, and corrupted disk images.
- [x] Implement the existing VirtIO-net path and `Net*` syscalls.
  - [x] Bring up VirtIO-net device discovery, feature negotiation, and queue initialization.
  - [x] Implement packet transmit and receive buffers with interrupt or polling integration.
  - [x] Add MAC address discovery and link-state reporting.
  - [x] Implement basic Ethernet, ARP, IPv4, ICMP, UDP, and TCP plumbing required by sockets.
    - [x] Add Ethernet, ARP, IPv4, and ICMP echo plumbing for `NetPing`.
    - [x] Add UDP packet routing between the network stack and datagram sockets.
    - [x] Add TCP connection state and stream socket integration.
  - [x] Wire `Net*` syscalls to the networking service with clear error returns.
  - [x] Add QEMU networking smoke tests for ping, UDP loopback, and simple TCP connections.

## POSIX Compatibility

- [x] Define a stable POSIX-facing syscall/error contract: return `-errno` or set `errno` consistently instead of exposing raw `UINT64_MAX` failures to libc.
- [x] Port or vendor mlibc with an `instantos` sysdeps layer.
  - [x] Decide whether mlibc is vendored, submoduled, or fetched by the build.
  - [x] Add the `instantos` sysdeps build configuration and toolchain integration.
  - [x] Implement syscall entry wrappers and `errno` translation.
  - [x] Implement process startup, argv/envp handoff, auxv, and program entry glue.
  - [x] Implement TLS setup for the main thread and spawned threads.
  - [x] Implement basic file, directory, time, memory, process, and signal sysdeps.
  - [x] Build and run a minimal dynamically or statically linked mlibc hello-world binary.
- [x] Replace userland headers with POSIX-compatible `<unistd.h>`, `<fcntl.h>`, `<sys/stat.h>`, `<sys/mman.h>`, `<dirent.h>`, `<time.h>`, `<signal.h>`, and `<pthread.h>` coverage.
- [x] Implement real `waitpid()` semantics: block until child exit, report status bits, reap terminated children, and handle `WNOHANG`.
- [x] Implement `fork()` or document a `posix_spawn()`-first model and make common software build paths prefer `posix_spawn()`.
- [x] Implement `execve()` semantics that replace the current process image, preserve required file descriptors, and pass argv/envp/auxv in a libc-friendly format.
- [x] Implement `pipe()` and descriptor inheritance, including close-on-exec, duplicated descriptors, EOF behavior, and blocking reads/writes.
- [x] Add terminal/TTY support: canonical input, echo, basic `termios`, process groups, foreground job control, and `isatty()`.
- [x] Add `poll()`/`select()` support for stdio, regular files, and pipes.
- [x] Extend native `poll()` readiness to event queues.
- [x] Extend `poll()`/`select()` readiness to network sockets.
- [x] Add sockets on top of the networking stack.
  - [x] Add socket descriptor allocation, lifetime management, duplication, close-on-exec, and `close()` behavior.
  - [x] Implement `socket()` for the first supported domains and types, starting with `AF_INET` and `SOCK_STREAM`/`SOCK_DGRAM`.
  - [x] Implement `bind()` and local port/address allocation, including conflict errors.
  - [x] Implement `connect()` for TCP and UDP sockets, including nonblocking/in-progress behavior if supported.
  - [x] Implement `listen()` and backlog tracking for TCP server sockets.
  - [x] Implement `accept()` with blocking and readiness behavior.
  - [x] Implement `send()`, `recv()`, `sendto()`, and `recvfrom()` with EOF, partial I/O, and error semantics.
  - [x] Implement `getsockopt()` and `setsockopt()` for the options needed by common software.
  - [x] Integrate socket readiness with `poll()`/`select()` for connect, accept, read, write, hangup, and error states.
  - [x] Add socket smoke tests for descriptor behavior, TCP client/server, UDP datagrams, and readiness.
- [x] Complete POSIX filesystem behavior.
  - [x] Normalize absolute and relative paths, including `.`, `..`, repeated slashes, and trailing slash rules.
  - [x] Implement per-process current working directory and `chdir()`/`getcwd()` semantics.
  - [x] Implement path traversal errors for missing components, non-directories, loops, permission denial, and too-long paths.
  - [x] Implement ownership, permission bits, mode creation masks, and access checks for files and directories.
  - [x] Implement `open()` flag semantics for `O_CREAT`, `O_EXCL`, `O_TRUNC`, `O_APPEND`, `O_DIRECTORY`, `O_NOFOLLOW`, and invalid flag combinations.
  - [x] Implement `stat()`, `fstat()`, `lstat()`, and stable inode/device metadata.
  - [x] Implement `mkdir()`, `rmdir()`, `unlink()`, and directory emptiness checks.
  - [x] Implement `link()` and hard-link reference counting.
  - [x] Implement `symlink()` and `readlink()` if symlinks are in scope for the first POSIX pass.
  - [x] Implement `rename()` semantics for files and directories, including replacement and cross-directory cases.
  - [x] Implement `truncate()` and `ftruncate()` for growing, shrinking, sparse regions if supported, and open descriptor interactions.
  - [x] Implement POSIX directory iteration with `opendir()`, `readdir()`, `rewinddir()`, `closedir()`, and stable `.`/`..` behavior.
  - [x] Implement atime, mtime, ctime updates and `utimensat()`/`futimens()` behavior.
  - [x] Implement `fsync()`/`fdatasync()` behavior or document durable-write limitations.
  - [x] Add filesystem tests for path resolution, permissions, metadata, links, rename, truncate, directory iteration, timestamps, and error edge cases.
- [x] Make `mmap()` support POSIX flags and file-backed mappings.
  - [x] Define the supported address-space layout and page alignment rules for `mmap()` and `munmap()`.
  - [x] Implement anonymous mappings for `MAP_PRIVATE` and `MAP_SHARED` where applicable.
  - [x] Implement file-backed read-only and writable mappings.
  - [x] Implement copy-on-write behavior for `MAP_PRIVATE` file-backed mappings.
  - [x] Implement shared writeback behavior for `MAP_SHARED` mappings.
  - [x] Implement `MAP_FIXED` replacement semantics and strict error handling for invalid ranges.
  - [x] Enforce `PROT_READ`, `PROT_WRITE`, `PROT_EXEC`, and file descriptor permission interactions.
  - [x] Implement `msync()` or document unsupported persistence semantics.
  - [x] Add tests for protection faults, partial unmap, file growth/truncation interactions, and invalid flag combinations.
- [x] Implement robust signals.
  - [x] Define signal numbers, default actions, ignored signals, and unblockable signals.
  - [x] Implement per-thread signal masks and `sigprocmask()`/`pthread_sigmask()` behavior.
  - [x] Track pending process-directed and thread-directed signals.
  - [x] Implement `sigaction()` handlers, flags, signal sets, and old-action returns.
  - [x] Deliver signals on user return paths with correct saved context and `sigreturn` handling.
  - [x] Implement interrupted syscall behavior and `SA_RESTART` restart rules.
  - [x] Implement `kill()`, `raise()`, `pthread_kill()`, and process-group delivery if job control needs it.
  - [x] Implement alternate signal stacks with `sigaltstack()`.
  - [x] Add tests for masks, pending delivery, handler ordering, syscall interruption, alternate stacks, and thread-directed signals.
- [x] Implement pthread-grade threading.
  - [x] Define kernel thread IDs, process membership, lifecycle, and exit/join state.
  - [x] Implement thread creation with user stack setup, TLS, and libc handoff.
  - [x] Implement `pthread_join()` and `pthread_detach()` resource cleanup semantics.
  - [x] Implement mutexes, including normal, recursive, error-checking, and robust behavior if required.
  - [x] Implement condition variables with timed waits and correct wakeup ordering guarantees.
  - [x] Implement rwlocks and semaphores with blocking scheduler integration.
  - [x] Implement thread-local `errno` and TLS destructors.
  - [x] Define cancellation points and implement deferred cancellation boundaries.
  - [x] Integrate thread blocking, wakeups, priorities if any, and signal delivery with the scheduler.
  - [x] Add tests for creation, join/detach, TLS, mutexes, condvars, rwlocks, semaphores, cancellation, and signal interactions.
- [ ] Add libc conformance tests.
  - [x] Keep focused `posix-smoke` tests for filesystem, `mmap`, pipe readiness, signal masks, pthread/semaphore primitives, and socket descriptor behavior.
  - [ ] Add separate smoke binaries for each major POSIX area so regressions identify the failing subsystem quickly.
  - [x] Add mlibc test integration once the `instantos` sysdeps layer can run basic programs.
  - [ ] Add libc-test integration for the supported syscall and libc surface area.
  - [x] Port a small set of POSIX utilities that exercise real-world filesystem, process, terminal, and socket behavior.
  - [ ] Run the conformance suite in CI or a reproducible QEMU script with clear pass/fail artifacts.

## Userland: bash + coreutils (mlibc)

Done this pass — GNU bash 5.2 and ~98 coreutils cross-build against mlibc and run
on InstantOS. Smoke harnesses: `tools/run-bash-smoke.sh`, `tools/run-pty-smoke.sh`,
`tools/run-coreutils-smoke.sh`.

- [x] Boot GNU bash (`-c` scripts and interactive over a PTY) on InstantOS.
- [x] Fix the rtld `#UD` crash (sysdeps `constinit` globals) and console stdout (`sys_seek` ESPIPE).
- [x] Fix `fork()`+`execve()` of dynamically-linked children (validUserState scheduler path; page tables from PMM not the buddy heap).
- [x] Tier-1 fs/process sysdeps (`Chmod`, `Truncate`, `Rename`, `Link`, `Symlink`, `Readlink`, `Utimensat`, `Kill`, plus the `*at` variants).
- [x] `Access`/`Faccessat` (new kernel `Access` syscall).
- [x] FAT32 `rename`/`chmod`/`utime` (rename real; chmod/utime vfat-style no-ops) + accurate rename errno (EXDEV/ENOSYS/ENOENT).
- [x] `Close(fd 0/1/2)` returns success so libc stdio teardown doesn't spuriously error.
- [x] `statfs`/`statvfs` (kernel `Statfs` syscall, FAT32/RamFS/InitrdFS/DevFS `statfs` ops, per-mount `st_dev`) -> `df`, `stat -f`.
- [x] File ownership: `uid`/`gid` on FileStats + VNode, kernel `Chown` syscall, `Fchownat` sysdep -> `chown`/`chgrp`.
- [x] Special files: kernel `Mknod` syscall + RamFS `nodeMknod` (FIFO/char/block, `rdev`) -> `mkfifo`/`mknod`; `/tmp` tmpfs.
- [x] Entropy: kernel RNG (`krandom`), `GetEntropy` syscall + sysdep, `/dev/urandom`/`/dev/random`/`/dev/null`/`/dev/zero` -> `shuf`/`mktemp`.
- [x] Cheap process/credential sysdeps: `GetRlimit`/`SetRlimit`/`Times`/`ClockSet`/`ClockGetres`/`GetSid`/`GetGroups`/`GetResuid`/`GetResgid`/`GetLoadavg`/`Fadvise`.
- [x] `/etc/passwd` + `/etc/group` (RamFS `/etc`) -> `id`/`whoami` resolve names.
- [x] Mount table: seed `/etc/mtab` from the VFS mount list + real `getmntent` parser -> `df` no-args / `mount`.
- [x] FIFO I/O: named FIFOs are real IPC channels (shared ring buffer, blocking open/read/write).
- [x] utmp login records: `/var` tmpfs + `/var/run/utmp` BOOT_TIME/USER_PROCESS -> `uptime` duration, `who`, `users`. Fixed `ClockGet` to return wall-clock time for `CLOCK_REALTIME`.

### Remaining polish / follow-ups

- [ ] `df` device names: show a real device (e.g. `/dev/ahci0`) instead of `-` (currently `mnt_fsname` is the fs type).
- [ ] `shred`: smoke-test the multi-pass data-overwrite path (entropy/`/dev/urandom` already works).
- [ ] FIFO edge cases: `poll()`/`O_NONBLOCK` on FIFOs, multiple concurrent readers/writers, SIGPIPE on write-to-closed-reader.
- [ ] Persistence: `/etc`, `/etc/mtab`, `/var/run/utmp` are RAM-seeded each boot; writes don't persist and `mtab`/`utmp` are boot snapshots. Add a live `/proc`-style view or FAT32 copy-back.
- [ ] utmp liveness: wire `pututxline` into the login/session flow so `/var/run/utmp` reflects real logins at runtime (currently a static boot seed).
- [ ] Expand the verified coreutils set and try a larger program (e.g. real GNU `make`, `grep`, or `coreutils` test suite).
- [ ] `Sysinfo`/`getrandom` and other `linux`-option sysdeps are unavailable because the mlibc linux option is disabled; revisit if a program needs them.

## TCC Port

- [ ] Port TinyCC (`tcc`) as the first on-system C compiler target.
  - [x] Define an InstantOS target/sysroot layout with headers, CRT objects, `libinstant.so`, and `/lib/ld-instantos.so`.
  - [x] Add initial fetch, sysroot, and gated build scripts for the TCC port.
  - [x] Cross-build `tcc` from the host as a native InstantOS user app before attempting self-hosting.
  - [x] Add a C-compatible CRT object for C programs that export unmangled `main`.
  - [x] Package `/bin/tcc` and `/bin/tcc-hello.c` into initrd when `INSTANTOS_ENABLE_TCC=ON`.
  - [ ] Patch or configure `tcc` to emit InstantOS-compatible ELF executables and dynamic-linker metadata.
  - [x] Build or package `libtcc1.a` without executing target binaries on the host.
  - [ ] Add the minimum missing libc/POSIX APIs needed by `tcc` and its runtime helpers.
    - [x] Add initial APIs found by the TCC build: `sys/time.h`, `gettimeofday`, `inttypes.h`, `fprintf`, `fputs`, `fdopen`, `execvp`, integer parsers, and basic local time.
  - [ ] Support compiler output files, temporary files, include path lookup, and executable permissions on the InstantOS filesystem.
  - [x] Package a small C sysroot into the initrd or persistent disk image for in-OS compilation.
  - [ ] Validate `tcc hello.c -o hello` inside InstantOS and run the produced binary.
  - [ ] Add smoke tests for preprocessing, compile-only, link, and run workflows.
  - [ ] Attempt building `tcc` inside InstantOS only after hosted `tcc` can build and run simple programs reliably.

## Hardware Acceleration

- [ ] Benchmark scalar, SSE2, AVX2, and AVX-512 paths in QEMU and on real hardware before enabling each path by default.

### 1: Last Tasks

- [ ] Audit the current ACPI implementation against the ACPI 6.0 specification, especially RSDP/XSDT/RSDT discovery, table revision handling, and checksum validation.
- [ ] Extend 64-bit GAS preference to any future FADT register consumers instead of reading legacy 32-bit fields directly.
- [x] Add duplicate ACPI table handling and mapped-address bounds checks.
- [x] Fill gaps in the AML interpreter needed by ACPI 6.0 DSDT/SSDT device enumeration and power methods.
- [x] Review platform table support needed by current drivers, including MADT, MCFG, HPET, DSDT, SSDT, and FACS.
- [ ] Add boot/test fixtures for representative ACPI 1.0, 2.0, and 6.0 firmware layouts to prevent regressions.
