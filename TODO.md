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
- [ ] `Sysinfo`/`getrandom` and other `linux`-option sysdeps are unavailable because the mlibc linux option is disabled; revisit if a program needs them.

## TCC Port

- [ ] Port TinyCC (`tcc`) as the first on-system C compiler target.
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
  - [ ] Attempt building `tcc` inside InstantOS only after hosted `tcc` can build and run simple programs reliably.

## Hardware Acceleration

- [ ] Benchmark scalar, SSE2, AVX2, and AVX-512 paths in QEMU and on real hardware before enabling each path by default.

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
- [ ] P7.1 First rung - in-OS gcc rebuilds binutils: untar binutils-2.42,
      `./configure` (exercises sh/sed/grep + gcc compile/link probes), `make`,
      produce working as/ld; sanity-run them. Oracle: `GCCSELF_BINUTILS_OK`.
      Swap the new binutils in; gcchost C/C++/tcc ladders still green.
- [ ] P7.2 Main event - gcc builds gcc (3-stage bootstrap): stage1 (in-OS gcc)
      builds stage2, stage2 builds stage3; `make compare` (stage2 vs stage3
      objects) passes; compare final cc1 byte-wise (pin -frandom-seed /
      timestamps for determinism as with tcc2==tcc3).
      Oracle: `SELFHOST_GCC_STAGE2_EQ_STAGE3`.
- [ ] P7.3 stage3 compiler rebuilds the OS userland (and optionally the kernel
      cross-image); os-testsuite 314/314 + all ladders green on the result.
- [ ] Cross-cutting: every latent kernel/libc bug shaken out by configure/cc1
      gets a minimized permanent check in os-testsuite. Track WHPX/TCG soak
      performance (a full bootstrap is long); use serial-marker resumability.

## Desktop polish (paused 2026-10-28)

Step 1 is done: the taskbar's centre pill lists the open windows (focus,
minimize, restore; launchers marked when their app runs). The rest:

- [ ] Live clock: the compositor blocks in `queue_receive` with no timeout, so
      the taskbar clock only redraws when something else changes. Wake once a
      minute (on the minute) and redraw just the clock.
- [ ] Apps follow their window's size: nothing handles
      `WindowEventAction::Resized`, so resized/maximized windows keep drawing at
      their first size. Terminal (re-wrap, `TIOCSWINSZ` so bash gets SIGWINCH),
      NetSurf (`NSFB_EVENT_RESIZE`), file browser, wallpaper picker.
- [ ] Keyboard window management: Alt/Super+Tab, Super+Up/Down
      (maximize/restore/minimize), Alt+F4; double-click a title bar to
      maximize; keep windows from being dragged off screen. Maximize should
      leave the taskbar uncovered (it is x=y=16, screen size minus 32 now).
- [ ] Display: the wallpaper is dithered (also under TCG); under WHPX the
      picture was ~960 px wide with `setScanout failed`; `-vga none` stalls
      the boot after `[iBoot] Framebuffer acquired.`
