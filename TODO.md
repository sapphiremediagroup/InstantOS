# TODO

## POSIX Compatibility
- [ ] Add libc conformance tests.
  - [x] Keep focused `posix-smoke` tests for filesystem, `mmap`, pipe readiness, signal masks, pthread/semaphore primitives, and socket descriptor behavior.
  - [x] Add separate smoke binaries for each major POSIX area so regressions identify the failing subsystem quickly.
  - [x] Add mlibc test integration once the `instantos` sysdeps layer can run basic programs.
  - [ ] Add libc-test integration for the supported syscall and libc surface area.
  - [x] Port a small set of POSIX utilities that exercise real-world filesystem, process, terminal, and socket behavior.
  - [ ] Run the conformance suite in CI or a reproducible QEMU script with clear pass/fail artifacts.

### Remaining polish / follow-ups

- [ ] `df` device names: show a real device (e.g. `/dev/ahci0`) instead of `-` (currently `mnt_fsname` is the fs type).
- [ ] `shred`: smoke-test the multi-pass data-overwrite path (entropy/`/dev/urandom` already works).
- [x] FIFO edge cases: `poll()`/`O_NONBLOCK` on FIFOs, multiple concurrent readers/writers, SIGPIPE on write-to-closed-reader. (`O_NONBLOCK` read/write return `EAGAIN` instead of blocking; `fcntl(F_GETFL/F_SETFL)` toggles it per endpoint; write-to-closed-reader raises `SIGPIPE` and returns `EPIPE`. Covered by the FIFO block in `posix-smoke` `test_ipc()`.)
- [ ] Persistence: `/etc`, `/etc/mtab`, `/var/run/utmp` are RAM-seeded each boot; writes don't persist and `mtab`/`utmp` are boot snapshots. Add a live `/proc`-style view or FAT32 copy-back.
- [ ] utmp liveness: wire `pututxline` into the login/session flow so `/var/run/utmp` reflects real logins at runtime (currently a static boot seed).
- [ ] Expand the verified coreutils set and try a larger program (e.g. real GNU `make`, `grep`, or `coreutils` test suite).
- [ ] `sysinfo` and other `linux`-option sysdeps are unavailable because the mlibc linux option is disabled; revisit if a program needs them. (`getrandom`/`getentropy` work: `Sysdeps<GetEntropy>`, the kernel CSPRNG.)
- [ ] `mmap` hints: an anonymous `mmap` passes only addr/length/prot to the kernel (`Sysdeps<VmMap>` drops the flags), and `sys_mmap` maps at any non-zero address it is given, so a plain hint acts like `MAP_FIXED` and can replace mappings. Pass the flags and treat a hint without `MAP_FIXED` as a hint (as Linux: elsewhere if the range is taken).

## TCC Port

- [x] Port TinyCC (`tcc`) as the first on-system C compiler target.
  - [x] Define an InstantOS target/sysroot layout with headers, CRT objects, `libinstant.so`, and `/lib/ld-instantos.so`.
  - [x] Add initial fetch, sysroot, and gated build scripts for the TCC port.
  - [x] Cross-build `tcc` from the host as a native InstantOS user app before attempting self-hosting.
  - [x] Add a C-compatible CRT object for C programs that export unmangled `main`.
  - [x] Package `/bin/tcc` and `/bin/tcc-hello.c` into initrd when `INSTANTOS_ENABLE_TCC=ON`.
  - [x] Patch or configure `tcc` to emit InstantOS-compatible ELF executables and dynamic-linker metadata.
  - [x] Build or package `libtcc1.a` without executing target binaries on the host.
  - [x] Add the minimum missing libc/POSIX APIs needed by `tcc` and its runtime helpers.
    - [x] Add initial APIs found by the TCC build: `sys/time.h`, `gettimeofday`, `inttypes.h`, `fprintf`, `fputs`, `fdopen`, `execvp`, integer parsers, and basic local time.
  - [x] Support compiler output files, temporary files, include path lookup, and executable permissions on the InstantOS filesystem.
  - [x] Package a small C sysroot into the initrd or persistent disk image for in-OS compilation.
  - [x] Validate `tcc hello.c -o hello` inside InstantOS and run the produced binary. (`tools/run-tcc-smoke.sh`)
  - [x] Add smoke tests for preprocessing, compile-only, link, and run workflows. (`tools/run-tcc-smoke.sh`)
  - [x] Attempt building `tcc` inside InstantOS only after hosted `tcc` can build and run simple programs reliably. (tcc self-hosts in-OS, tcc2 == tcc3: `SELFHOST_FIXPOINT_OK`, `tools/tcc-selfhost`; also when built by the in-OS stage3 gcc, P7.3.)

## Hardware Acceleration

- [x] Benchmark scalar, SSE2, AVX2, and AVX-512 paths in QEMU and on real hardware before enabling each path by default.
      DONE 2026-11-19: `MemBench` syscall 162 + `tools/membench` (`run-membench-win.sh whpx|kvm|tcg`)
      time the kernel's memcpy/memset/memset32/memcmp variants (rep, SSE2, AVX2; there is no
      AVX-512 path, and neither CPU here has AVX-512). On the i3-6100 (WHPX) and a Xeon E5 v4
      (KVM) `rep movsb/stosb` (ERMSB) beat the SSE2 loops the kernel used by up to 2x at every
      size; under TCG it is 4-10x slower than SIMD. So the kernel now times rep/SSE2/AVX2 on 4 KiB
      at boot and keeps the fastest (`memory_calibrate_acceleration`, `[mem] memcpy: rep ...` in
      the log); memcmp uses AVX2/SSE2 from 32/16 bytes (it used a byte loop below 128).
      Found on the way: page faults did not save the program's vector registers, so a fault on a
      file-backed page (ext4's SSE2 memcpy) changed xmm0-3 under the program; fixed in
      `exceptionHandler`, checked by the suite's "page faults keep the program's SSE registers".
- [ ] Bare-metal check of the calibration on the PC: its `/kernel.log` shows the `[mem]` picks
      after the next boot of a kernel with it (WHPX already runs on that CPU).
- [ ] memset/memset32 between 64 and 256 KiB: AVX2 is 15-30% faster than rep there on both
      CPUs (rep wins below and above); a size-dependent pick could take it.

### More network drivers (2026-11-20)

- [x] vmxnet3 (VMware) and AMD PCnet (VirtualBox, older VMware): `vmxnet3.cpp`, `pcnet.cpp`.
- [x] Realtek RTL8125A/B 2.5G in `rtl8169.cpp`, checked on an RTL8125B model added to the custom
      QEMU (`tools/qemu-rtl8168`).
- [x] Intel I225/I226 2.5G as `Family::Igc` in `e1000.cpp`; QEMU's `igc` test device is its igb
      model under the I225-V's id, so the GPY PHY and 2.5G are untested.
- [x] USB Ethernet, CDC-ECM on xHCI (`usb_net.cpp`, `setup_usb_ethernet` in `xhci.cpp`), checked
      on QEMU's usb-net (patched in the custom QEMU to report the cable).
      All five pass `run-net-suite.sh` 156/156 (`QEMU_NIC=vmxnet3|pcnet|rtl8125|igc|usb-net`).
- [ ] USB Ethernet with vendor protocols: AX88179/178A (ASIX) and RTL8152/8153 without their ECM
      configuration (r8152's framing); RNDIS/NCM (phone tethering). No QEMU model for the first two.
- [ ] USB Ethernet: OHCI (only xHCI does bulk), more than one adapter, unplug while running.
- [ ] None of the new chips is on the PC (it has an RTL8168): a real RTL8125/I225/USB adapter check.

### Installed system + Limine (started 2026-11-20)

The PC boots Limine by default, with InstantOS, Windows and Arch Linux entries, and InstantOS
runs from an installed ext4 root instead of the initrd. The InstantOS side is checked in QEMU on a
PC-shaped GPT disk (`tools/boot/make-boot-disk.sh`: firmware -> iboot directly, no Limine there).
- [x] 1. Limine on the PC: `tools/install-limine-windows.ps1` (Administrator; backs the ESP up,
      `\EFI\Limine\BOOTX64.EFI` + `limine.conf` from `tools/boot/limine.conf` with the
      `arch-linux*.efi` it finds, iboot/kernel/initrd copied, `iboot.cfg` with the INSTANTOS
      partition's GUID, a "Limine" firmware entry put first, Windows Boot Manager kept).
      DONE 2026-11-21: the user ran it. Limine's own binary is in `build\limine` (v10.x-binary).
- [x] 2. iboot reads `\EFI\InstantOS\iboot.cfg` (`root=<partition GUID>`, `root=none`) and its
      load options (a Limine entry's `cmdline`), passed in `BootInfo::config` behind
      `kBootConfigMagic`; `BootConfig` in the kernel, `gptFindPartitionByGuid`. DONE 2026-11-20:
      iboot.cfg alone mounts root=, load options override it, a root= naming a non-InstantOS
      partition is refused and the usual rule picks one.
- [x] 3. Installed mode (`/.installed` on the root: its `/bin`, `/lib`, `/include`, `/netsurf`,
      not the initrd's, which stays as the rescue system; the ext4 boot self-test skips GPT
      InstantOS roots). DONE 2026-11-23 (step 6's installed boot).
- [x] 4. `mke2fs`/`e2fsck` (`tools/build-e2fsprogs.sh`, `tools/e2fsprogs/mke2fs.conf`: the
      driver's tested feature set), `/dev/diskNpM` for InstantOS partitions only
      (`DiskRegistry`), syscalls `PartitionList`/`PartitionSetType`/`PartitionMount` (165-167),
      `root=none` live mode, and `instant-install` (net-tools). DONE 2026-11-23. mlibc's `sync()`
      had no sysdep (a no-op); it is now `StorageSync` All, which also flushes the drives' caches.
- [ ] 5. Logs under `/test/logs/` when it exists (log-saver; the installer makes it).
- [x] 6. End to end in QEMU (`tools/install-test/run-install-test.sh`, `QEMU_WIN=1` for WHPX:
      live boot installs onto the FAT32 INSTANTOS partition, the host checks the GPT type and
      e2fsck and puts the toolchain tarball on it, then the installed boot unpacks it with tar and
      builds and runs C and C++; PASSES 2026-11-23). On the PC: installed and booted 2026-11-23
      (Limine was not first in the firmware order: the script's id lookup matched {fwbootmgr}).
      The PC's partition was later found retyped to basic data (Windows showing it as F:), so it was
      refused as another system's: an ext4 volume labelled `InstantOS` on basic data now counts as
      ours too, and the test retypes, boots and reinstalls over it (2026-11-23). Reinstalling sets
      InstantOS's type again, so Windows drops the letter.
- [x] 7. Tools on the installed system: the GNU tools are in the initrd's /bin (all rebuilt with
      the corrected `strerror_r`/`fchmodat` answers), and the gcc toolchain comes with the PC's
      INITRD (`tools/make-pc-initrd.sh`, after the iso target; install-limine-windows.ps1 copies
      it): `instant-install` unpacks it. DONE 2026-11-23 in QEMU; on the PC pending.
      On the PC gcc said cc1plus was not found: the terminal's bash had no exported PATH (fixed, the
      terminal passes one; the install test set PATH itself and so missed it).
      Seen on the way, not fixed: under QEMU/WHPX with virtio-gpu a terminal line sometimes stays
      blank until something redraws it (the text is in the window's surface after a redraw).
      `instant-install --update diskNpM` (from the live entry) puts the image's system files and, when
      it is another one, the toolchain over an installed system and keeps everything else; the
      install test covers it (2026-11-23).
- [ ] 8. Files copied onto the installed partition from Windows (Ext2Fsd) are gone after a restart,
      with `Write-VolumeCache` too: Ext2Fsd or our journal replay, not known which. The boot log
      now says `[ext4] journal clean` or what was replayed.

### mbedTLS -> OpenSSL (started 2026-11-23)

OpenSSL 3.5 LTS (supported to April 2030; pinned 3.5.9 with its checksum). Shared
(`libssl.so.3`, `libcrypto.so.3` in `/lib/mlibc`), the `openssl` command shipped, headers and
libraries also in the gcc toolchain tarball.

- [x] 1. `tools/build-openssl.sh`: target `instantos-x86_64` (assembly, pthreads; no engines, async,
      QUIC, loadable modules, legacy provider) into the mlibc sysroot, bundled by CMake.
- [x] 2. Randomness from mlibc's `getentropy` only: the smoke's second program replaces it with one
      that fails, and every request for randomness must fail with it.
- [x] 3. `OPENSSLDIR=/etc/ssl`: `openssl.cnf`, and the CA bundle as `cert.pem`. 1-3 DONE 2026-11-23:
      `tools/run-openssl-smoke.sh` (QEMU_WIN=1) passes; nothing uses OpenSSL yet.
- [x] 4. curl on OpenSSL (same curl version). DONE 2026-11-23:
      the network suite with NET_INTERNET=1 is 170/170.
- [x] 5. libcurl and NetSurf (`NETSURF_USE_OPENSSL=YES`, `netsurf-ip-certificate.patch`: hosts that
      are IP addresses). DONE 2026-11-23: the NetSurf suite is 19/19, with bad certificates now.
- [x] 6. Tests: the self-test (1-3), the network suite's HTTPS tests (4), the NetSurf suite (5).
- [x] 7. curl 8.23.0, pinned by checksum (curl.se and GitHub serve the same file). DONE 2026-11-23:
      network suite 170/170, NetSurf suite 19/19.
- [x] 8. Headers and `-lssl -lcrypto` in `tools/make-dev-tarball.sh`; the install test builds and runs
      `/examples/openssl.c` in the OS. DONE 2026-11-23.
- [x] 9. mbedTLS removed (script, sysroot, every mention). DONE 2026-11-23.

### Kernel futexes and threads (started 2026-11-24)

mlibc's `FutexWait` is a yield and `FutexWake` does nothing, and Mesa has its own copy of that: a
thread waiting for anything spins through `Yield`, big kernel lock included. Wanted for Ladybird
(many processes and threads) and Node.

- [x] 1. Kernel: `FutexWait` (168), `FutexWake` (169), `src/cpu/syscall/futex.cpp`; feature-suite
      group "futexes" (46 checks). DONE 2026-11-24: the suite is 463/463 at 1, 2 and 4 CPUs (WHPX).
      Nothing in userland uses them yet. Found on the way and fixed: a driver module's `delayUs` never
      ended on one CPU without a TSC clock (WHPX), where the uptime stands still with interrupts off.
- [x] 2. A wake of many waiters spreads over the idle CPUs (`kickIdleCpu` kicked the first one each
      time and the others waited for their tick; `PerCPUData::idleKicked` now sends the next kick to
      another). DONE 2026-11-24: 3 CPUs under WHPX, until a thread runs on both other CPUs: 750-860 us
      before, 180-470 us after (host noise); at 4 CPUs 1.7-3.2 ms -> 1.0-2.3 ms, the first two
      threads at once and the third late, likely the 4-thread host (not proven: check on the PC, step
      8). The suite prints the times ("until all ran"); 465/465.
- [x] 3. Tests: feature-suite group "futex races and cost" (sleepers stay asleep through I/O and cost
      other threads nothing; wakes against timeouts, signals and kills; wake order; 96 words; shared
      pages). DONE 2026-11-24: 491/491 (1-2 CPUs), 493/493 (3-4) under WHPX, also TCG. 8 bugs put into
      the kernel on purpose are all caught (two of them only at 4 CPUs, being races). Beside 8 threads
      that wait by yielding, as mlibc's do, a syscall costs about 20 times as much at 4 CPUs.
- [x] 4. mlibc on them (`tools/mlibc/instantos-threads.patch` and the sysdeps): locks and condition
      variables make no syscall without contention and wake one waiter, semaphores fixed, pthread_kill
      and deferred pthread_cancel work, `getpid` in a thread is the process's (`GetTgid`, 170).
      `tools/run-thread-suite.sh` (new) passes at 1, 2 and 4 CPUs under WHPX, 2026-11-24.
- [x] 5. Mesa and libc++ on real futexes (`<instantos/futex.h>`; `thread_local` objects with destructors
      link now). Mesa's timed fence waits end (its shim's result was one util_queue ignored). DONE
      2026-11-24: softpipe GL and the C++ runtime pass in QEMU; iris and anv only on the PC (step 8).
- [x] 6. mlibc threads give back their stack and TCB on join and detach (some stacks are kept for new
      threads; a guard page below each); the kernel clears and wakes a word when a thread is gone
      (`ThreadExitNotify`, 171). 6000 joined threads leave 0 MiB behind. DONE 2026-11-24.
- [x] 7. libinstant's pthreads and semaphores, and the kernel's ThreadJoin, sleep too (native-threads in
      the thread suite). DONE 2026-11-24.
      Kernel bugs found by these tests and fixed, each with a check in the feature suite or the thread
      suite: threads had no saved vector registers (an interrupt, page fault or syscall left them the
      kernel's); fork from a thread was refused, its children and wait() now belong to the process; fork
      made other threads' kernel-made stacks (heap memory) copy-on-write; a process that exited while
      ended threads were not reaped yet never closed its files (a pipe's reader hung). mlibc's fd table
      was not thread-safe; kill() of no process said ENOENT; nanosleep below 1 ms did not sleep.
- [x] Regression 2026-11-24: threads pass (1, 2, 4 CPUs), feature 501/501 and 503/503, network 170/170,
      NetSurf 19/19, OpenSSL, GL; install test passed and keyboard 149/150 (a USB Shift+F5 lost once;
      50/50 on a rerun) before the last change (thread exit takes no lock). ISO and PC image built.
- [x] 8. On the PC (2026-11-24): gl-offscreen, vk-offscreen and drm-probe pass; gl-window 25.7 -> 29.7 fps,
      vk-window 24.9 -> 29.9 fps. CPU use was not measured. Now exactly every second vblank of 60 Hz:
      the compositor's frame (draw, glFinish, flip) takes more than 16.7 ms.
- [ ] GPU clock: the Gen9 driver never sets one (no RPNSWREQ/RPS at all), so the HD 530 may run at its
      boot clock (350 MHz of 1050?). Not proven. First read RPNSWREQ (0xA008), RPSTAT1 (0xA01C) and
      RP_STATE_CAP (MCHBAR mirror 0x145998) in `gpu-test state`; then a gpu-test experiment that requests
      the maximum (i915's intel_rps.c, Gen9) and gl-window again.

### Shared memory (2026-11-24)

- [x] `SharedPages` (`include/memory/shared_pages.hpp`): MAP_SHARED|MAP_ANONYMOUS, `memfd_create` with seals
      (`MemfdCreate`, 172), `/dev/shm` for `shm_open`, and MAP_SHARED maps of `/tmp` files as one memory;
      futexes, pthread objects (pshared) and semaphores work in it between processes, a memfd goes over a
      socket. RamFS files are made of it, and an open one keeps its memory when its name is unlinked.
      PROT_NONE is enforced for pages not there yet, an mmap hint no longer replaces a mapping (mlibc),
      mprotect keeps shared pages shared. Feature suite 586/586 and 588/588 (8 bugs put in on purpose are
      caught), `shm_suite.c` in the thread suite 35/35, network 170/170, NetSurf 19/19, install, OpenSSL.
- [ ] Not done: MAP_SHARED of an ext4/FAT32 file is still a copy per process (written back); MADV_DONTNEED,
      mremap, MAP_FIXED_NOREPLACE, sem_open; PROT_NONE on a page already there leaves it readable.

### For Ladybird, still missing

- [x] More than 256 open files per process. DONE 2026-11-25: the kernel's handle table is made of
      chunks of 256 slots, made as needed, up to 131072 (was a fixed 1024, 40 KiB a process); a
      dup or open beyond it is EMFILE, a bad handle EBADF. RLIMIT_NOFILE is kept with the table
      (`GetRlimit`/`SetRlimit`, 177/178: 1024 and 65536 at first, inherited, kept across exec).
      mlibc's fd table is chunks of one page up to 65536 under the soft limit (lowest free fd, a
      hint so opening is not a scan from 3), the exec stash is made to measure, `poll` takes more
      than 64 fds and `select` all of FD_SETSIZE (`select` with no fds now sleeps). Feature suite
      638/638 (5000 handles, the ceiling, fork), `fd_suite.c` 28/28 in the thread suite (EMFILE at
      the limit, 9000 fds, dup2/F_DUPFD, poll on 2000, fork and exec keep them, 8 threads).
- [x] Signals at any time, with siginfo (below).

### Ladybird port (started 2026-11-25)

Ladybird `6c19e86` (2026-11-25), source in WSL `~/ladybird-port/src`. Decisions (user): build on
hippo; Rust through the prebuilt `x86_64-unknown-linux-musl` std linked against mlibc; a native
InstantOS front end (Ladybird's only desktop UI is Qt). About 30 Rust crates (LibJS parser, LibWeb,
IPC, GC, allocator), ~35 vcpkg dependencies (Skia, ICU, HarfBuzz, FreeType, fontconfig, FFmpeg,
libavif, libwebp, curl with HTTP/2-3, sqlite3, simdutf, mimalloc, llvm-libc, ...).

- [ ] 1. Rust on InstantOS: Rust 1.98 with the musl target, its std linked against mlibc; a Rust
      program (threads, Mutex/Condvar, fs, time, process, sockets, panics with unwinding,
      thread_local) passes in the thread suite. mlibc gets a `syscall()` for what std calls
      directly (futex, getrandom; the rest ENOSYS so std falls back); musl and mlibc layouts of
      what std passes by pointer checked.
- [ ] 2. The dependencies: vcpkg with an `x64-instantos` triplet (clang 20, libc++ and mlibc,
      CMAKE_SYSTEM_NAME Linux) for Ladybird's vcpkg.json without Qt, D-Bus, ANGLE and Vulkan.
- [ ] 3. LibJS: Ladybird configured for InstantOS; `js` and test-js run in the OS.
- [ ] 4. All libraries and services; a page rendered headless in the OS (WebContent, RequestServer,
      ImageDecoder over IPC) to an image.
- [ ] 5. The front end (`UI/InstantOS` on the UI library): a window, tabs, address bar, the page
      from WebContent's shared bitmaps, input.
- [ ] 6. HTTPS, fonts, images and media in real use; then Skia on the GPU (Vulkan through Mesa's
      anv) and the PC.

### Signals (2026-11-25)

- [x] Delivered at every return to user mode, also from interrupts and exceptions (a loop with no
      syscalls gets SIGINT/SIGALRM; the sender kicks the target's CPU), with Linux's x86-64 frame:
      `siginfo_t` (SI_USER/SI_TKILL/SI_KERNEL/CLD_*, fault addresses), `ucontext_t` with every
      register, the mask and the XSAVE image, below the red zone or on the sigaltstack. sigreturn
      restores all of it and returns by iretq. SA_SIGINFO/NODEFER/RESETHAND/RESTART; user faults
      run their handler (SIGSEGV/BUS/ILL/FPE/TRAP); dispositions shared by all threads; a process
      signal goes to a thread that does not block it; `Sigsuspend`/`Sigpending`/`Sigtimedwait`/
      `Tgkill` (173-176); `wait` reports deaths by signal (WIFSIGNALED); pipes return EINTR.
      Bugs found on the way: mlibc's masks were one bit off (blocking SIGUSR1 blocked "SIGKILL", so
      nothing), `SFMASK` left DF set in the kernel, the interrupt-time switch lost r12-r15 and loaded
      the wrong vector state, fork dropped the sigaltstack, `times()` was in ms against a 1 MHz
      `CLK_TCK`. mlibc's pthread_cancel checks siginfo again. Feature suite 620/620 (1, 2 CPUs),
      622/622 (4); `signal_suite.c` 32/32 in the thread suite (1/2/4 CPUs); OpenSSL smoke with
      `openssl speed` back in; 8 of 9 bugs put back on purpose are caught (not the SFMASK one: under
      WHPX the kernel's memcpy is the SSE loop, which ignores DF).
- [x] The WHPX guest's clock ran at 40-70% of real time. DONE 2026-11-25: WHPX shows neither the
      invariant-TSC nor the hypervisor bit, so the kernel counted PIT ticks (losing those that come
      with interrupts off); the TSC is now trusted on Intel Core+/AMD 10h+ as Linux does, and its
      RTC calibration bounds each second's edge (it was up to 0.5% off when the vCPU was
      descheduled at an edge). Net suite under WHPX 157/158 (RTT, NTP and link checks pass), keyboard
      147/147. Still failing there: "connect to a closed port" (slirp on Windows).
- [ ] Not done: real-time signals queue only once (one pending instance, as standard signals);
      `sigqueue`/si_value; no per-process CPU time (`times`/`getrusage` report 0); mlibc's `raise()`
      is `kill(getpid())`, so in a thread it may land on another one.

### Modular drivers: .idrv (started 2026-11-20, docs/idrv.md)

- [x] 1. Format, `tools/mkidrv`, the kernel loader and driver API v1, PCnet moved out of the
      kernel as `modules/pcnet` (`/lib/drivers/pcnet.idrv`). DONE 2026-11-20: net suite
      QEMU_NIC=pcnet 156/156 with the module. Found on the way: PCnet's 32 receive descriptors
      overflowed on the 64 KiB UDP echo's 45 fragments (dropped frames); now 128. Modules use the
      kernel's memcpy/memset through the API (rep is byte-by-byte under TCG).
- [x] 2. Loading at run time. DONE 2026-11-20: `ModuleList` (163) / `ModuleLoad` (164, root),
      `lsmod [-v]` and `modload FILE` (mlibc tools in `net-tools`), the root disk's `/drivers`
      scanned at boot before `/lib/drivers` (one module per name, so a disk module replaces the
      built-in one; `F:\drivers` on the PC), devices one module took not offered to another.
      Checks: feature suite 417/417 with the new `test_modules` (runtime load of the test module
      `modules/idrvtest` from the ext4 disk, ENOENT/EINVAL/EEXIST/ENODEV, a damaged copy, its API
      checks in the log) and 416/416 with `IDRV_ON_DISK=1` (taken from `/drivers` at boot); net suite
      QEMU_NIC=pcnet 159/159 with `lsmod`/`modload` run under bash.
- [ ] 2b. A NIC module loaded after boot only joins the registry: the stack keeps the NIC it has.
      Switching to it (or using several NICs) is for later.
- [x] 3. Move the NIC drivers out. DONE 2026-11-20: every PCI NIC driver is a module
      (`modules/{virtio-net,e1000,rtl8169,vmxnet3,rtl8139,pcnet}`), the kernel copies deleted.
      The API gained `irqRegisterMsix` and `random` (appended; `IDRV_API_HAS`); no timer or CPU
      routing was needed (`PCI::register*` balances interrupts itself). Net suite with each module:
      virtio-net (MSI-X), e1000, e1000e, igb, igc, rtl8168, rtl8125, vmxnet3 158/158, rtl8139
      157/157; feature suite 417/417. Found on the way: rtl8139 had WRAP backwards (every frame
      across the ring's end garbled: large transfers failed), and `VMM::VirtualToPhysical`
      ignored large pages, so `PCI::mapMemoryBar` on a BAR above 4 GiB (virtio's, at 1 TiB on
      q35) faulted the kernel; it now uses `VirtualToPhysicalIn`.
- [ ] 3b. USB class drivers as modules (USB Ethernet first): needs a USB API (bulk/interrupt
      pipes, control transfers, attach callbacks) in `IdrvKernelApi`.
- [ ] 4. Unloading: `exit`, and the kernel taking back interrupts, NICs and DMA a module registered.
- [ ] 5. W^X for modules: text read-only + executable, data no-execute, in their own kernel range.

### 1: Last Tasks

- [x] Audit the current ACPI implementation against the ACPI 6.0 specification, especially RSDP/XSDT/RSDT discovery, table revision handling, and checksum validation. (RSDP v1/v2 checksums, XSDT-preferred-over-RSDT selection, per-table signature+checksum+bounds validation, and pointer-plausibility checks verified; pure logic extracted to `include/cpu/acpi/acpi_tables.hpp`.)
- [x] Extend 64-bit GAS preference to any future FADT register consumers instead of reading legacy 32-bit fields directly. (PM1a/b control, PM timer, GPE0/1, sleep-control, and reset registers all prefer the 64-bit GAS form with a length-guarded legacy fallback; DSDT resolution prefers `xDsdt`.)
- [x] Add duplicate ACPI table handling and mapped-address bounds checks.
- [x] Fill gaps in the AML interpreter needed by ACPI 6.0 DSDT/SSDT device enumeration and power methods.
- [x] Review platform table support needed by current drivers, including MADT, MCFG, HPET, DSDT, SSDT, and FACS.
- [x] Add boot/test fixtures for representative ACPI 1.0, 2.0, and 6.0 firmware layouts to prevent regressions. (Host-side fixtures in `tools/acpi-fixtures/` build synthetic 1.0/2.0/6.0 firmware images and drive the shared parsing logic; run with `tools/run-acpi-fixtures.sh`.)

## Phase 7 - Full self-host: GCC rebuilds GCC in-OS (roadmap finish line)

Exit criteria (DONE): `SELFHOST_GCC_STAGE2_EQ_STAGE3` printed from a headless
in-OS run, plus the full regression suite green using the stage3 compiler's
world. Harness lives in `tools/gcc-selfhost/` (launcher + ISO builder + serial
oracle), mirroring `tools/tcc-selfhost`. Storage note: heavy build trees stay in
WSL; keep the Windows `build/` tree lean (C: is tight).

- [x] P7.0 In-OS build environment (foundation): in-OS gcc + GNU make + /bin/sh
      (+ sed/grep/tar/gzip) build a multi-file C project via a Makefile, run it,
      and do an incremental (`mtime`) + parallel (`-j2`) rebuild.
      Oracle: `GCCSELF_MAKE_OK`. (gcc-driven analogue of `tools/buildworld`.)
      DONE: `tools/gcc-selfhost/` (launcher.c + build-iso.sh); reuses the gcchost
      ext4 (gcc world) at run time. Verified headless: GCCSELF_MAKE_OK green
      (configure=sh+sed+grep+gcc, make clean+incremental+`-j2`, app 0 then 7).
- [x] P7.1 First rung - in-OS gcc rebuilds binutils: untar binutils-2.42,
      `./configure` (exercises sh/sed/grep + gcc compile/link probes), `make`,
      produce working as/ld; sanity-run them. Oracle: `GCCSELF_BINUTILS_OK`.
      Swap the new binutils in; gcchost C/C++/tcc ladders still green.
      DONE 2026-11-07 (local WHPX run 9, `launcher-binutils.c`): make rc 0,
      as-new and ld-new run. The swap-in is not done (below).
- [ ] P7.1 swap-in: install the in-OS-built binutils over the disk's
      cross-built ones and rerun the gcchost C/C++/tcc ladders on them.
- [x] P7.2 Main event - gcc builds gcc (3-stage bootstrap): stage1 (in-OS gcc)
      builds stage2, stage2 builds stage3; `make compare` (stage2 vs stage3
      objects) passes; compare final cc1 byte-wise (pin -frandom-seed /
      timestamps for determinism as with tcc2==tcc3).
      Oracle: `SELFHOST_GCC_STAGE2_EQ_STAGE3`.
      DONE 2026-11-09 (local WHPX run 11, `launcher-gcc.c`): compare stamp,
      2305 stage3 objects, stripped cc1/cc1plus equal, stage3 hello exits 7.
- [ ] P7.2 clean run: that pass was one build tree resumed across runs 5-11,
      with the disk patched between runs (egrep added, stage1 target libraries
      removed). All the fixes are in the scripts now; do one unattended run
      from a fresh disk (`make-gcc-disk.sh`): bootstrap, then P7.3's world,
      suite and ladders (roughly 15 h under WHPX).
- [x] P7.3 stage3 compiler rebuilds the OS userland (and optionally the kernel
      cross-image); os-testsuite 314/314 + all ladders green on the result.
      DONE 2026-11-19 (local WHPX, on the P7.2 disk): `make install` of the
      bootstrap tree puts stage3 in /usr (installed cc1/cc1plus == stage3's),
      then in-OS it builds ld-instantos, libinstant, the managers and apps
      (the cross build's ninja commands, make-world-src.sh), the suite's
      runner/helper, mlibc (459 commands + install), bash and tinycc (their own
      configure + make on that mlibc / a tcc sysroot from that userland):
      GCCSELF_WORLD_OK (launcher-world.c). On those binaries: os-testsuite
      391/391 (run-world-suite.sh), BASHFORK_RESULT_OK and SELFHOST_FIXPOINT_OK
      (run-world-ladders.sh; the ladders' own /bin/login drivers are still
      compiled by the cross). Not rebuilt in-OS: the kernel (PE/COFF, clang),
      the GNU tools of the build images, binutils (the disk's cross-built ones
      link everything). Fixed on the way: mlibc __freadptr/__freadptrinc
      (coreutils cut), Fchmodat AT_SYMLINK_NOFOLLOW; the stale AF_UNIX suite
      check.
- [ ] GNU tar run as root cannot extract a symlink: gnulib's fchmodat
      (REPLACE_FCHMODAT) sets the link's mode via openat(O_PATH|O_NOFOLLOW) and
      fstatat(AT_EMPTY_PATH), and the kernel cannot open a symlink itself, so
      tar exits 2 ("Cannot change mode to rwxrwxrwx"). P7.3 packs its world
      tarball with tar -h meanwhile. Needs O_PATH|O_NOFOLLOW opening the link
      (as Linux) + a suite check. Also: those errors print "Unknown system
      error" (gnulib's text when strerror_r gives nothing) for known errnos.
      mlibc's Fchmodat already returns EOPNOTSUPP for AT_SYMLINK_NOFOLLOW on a
      link (2026-11-19).
- [ ] Regression checks for the P7.3 mlibc fixes (`__freadptr` with an
      `ungetc`'d byte, `fchmodat(AT_SYMLINK_NOFOLLOW)` on a link): the feature
      suite is freestanding, so they belong in `posix-smoke`.
- [ ] Cross-cutting: every latent kernel/libc bug shaken out by configure/cc1
      gets a minimized permanent check in os-testsuite. Track WHPX/TCG soak
      performance (a full bootstrap is long); use serial-marker resumability.

## gcc in the everyday system

- [x] QEMU: the desktop image with gcc on its root disk (2026-11-19). `tools/make-dev-disk.sh`
      (the OS-built stage3 gcc + binutils + mlibc dev files + libstdc++ under /usr, make,
      coreutils, GNU text tools, /examples), `/bin/sh` in the initrd, mlibc's loader also
      searching /usr/lib. `tools/run-dev-smoke-win.sh`: C, C++ and make run inside the desktop
      (DEVSMOKE_C_OK / _CXX_OK / _MAKE_OK, 0 crashes). Interactive: `DISK_IMG=build/dev-disk.img ./run-win.sh`.
- [ ] The PC: its InstantOS partition is FAT32, and the FAT32 driver reads and writes 8.3 names
      only, so a gcc install (`x86_64-unknown-instantos-gcc-13.3.0`, `libexec/gcc/...`) cannot
      live there. Either long file names (VFAT LFN entries) in the FAT32 driver, or an ext4
      InstantOS partition. Executable files on FAT32 need a mode too.
- [ ] `find`/`xargs` are only built on the build server; build them in WSL too
      (`tools/build-gnu-tool.sh find|xargs`) so the dev disk has them.
- [ ] A shortcut or icon for the terminal's compiler (e.g. open `/examples` in the file
      browser); a native text editor would make it a real development environment.

## Desktop polish (paused 2026-10-28)

Step 1 is done: the taskbar's centre pill lists the open windows (focus,
minimize, restore; launchers marked when their app runs). The rest:

- [ ] Live clock: the compositor blocks in `queue_receive` with no timeout, so
      the taskbar clock only redraws when something else changes. Wake once a
      minute (on the minute) and redraw just the clock.
- [ ] Apps follow their window's size: the UI library handles `Resized` (new
      surface, `on_size`, relayout), so the terminal (`TIOCSWINSZ`), file
      browser and wallpaper picker do. Left: NetSurf (`NSFB_EVENT_RESIZE`;
      `libnsfb/instant.c` drops window resize events).
- [ ] Keyboard window management: Alt/Super+Tab, Super+Up/Down
      (maximize/restore/minimize), Alt+F4; double-click a title bar to
      maximize; keep windows from being dragged off screen. (The compositor's
      only shortcuts are Super+T/F/B/C/W. Maximize already stops 12 px above
      the taskbar.)
- [ ] Display: the wallpaper is dithered (also under TCG); under WHPX the
      picture was ~960 px wide with `setScanout failed`; `-vga none` stalls
      the boot after `[iBoot] Framebuffer acquired.`
