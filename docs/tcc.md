# TinyCC Port

InstantOS is starting with TinyCC (`tcc`) as the first in-OS C compiler target.
The initial goal is not self-hosting; it is a host-built `tcc` binary that runs in
InstantOS and can compile a small C file into an InstantOS executable.

## Current Scaffold

- `tools/fetch-tcc.sh` fetches a pinned TinyCC checkout into `outside/iUserApps/outside/tinycc`.
- `tools/build-tcc-sysroot.sh` creates `build/tcc-sysroot` from the current InstantOS runtime artifacts.
- `tools/build-tcc.sh` prepares the sysroot and gates the experimental native TCC build behind `TCC_NATIVE_BUILD=1`.
- `tools/tcc/hello.c` is the first in-OS compile smoke source.
- `cmake -DINSTANTOS_ENABLE_TCC=ON` enables a `tcc` build target and packages `/bin/tcc` plus `/bin/tcc-hello.c` into initrd.

## Sysroot Layout

The sysroot generated at `build/tcc-sysroot` contains:

- `include/`: ilibcxx C/POSIX headers.
- `lib/crt0.o`: InstantOS process entry object.
- `lib/crt1.o`: alias to `crt0.o` for hosted compiler conventions.
- `lib/Scrt1.o`: alias to `crt0.o` for PIE compiler conventions.
- `lib/crti.o` and `lib/crtn.o`: empty compatibility objects for TCC's default ELF link flow.
- `lib/libinstant.so`: InstantOS userland runtime/libc shim.
- `lib/libc.so`: alias to `libinstant.so` so `-lc` resolves.
- `lib/ld-instantos.so`: dynamic loader used by produced executables.
- `lib/tcc/libtcc1.a`: TinyCC runtime support archive after `tools/build-tcc.sh`.
- `/lib/tcc/include`: packaged C/POSIX headers in the initrd when TCC is enabled.
- `/lib/crt0.o`, `/lib/crt1.o`, `/lib/Scrt1.o`, `/lib/crti.o`, `/lib/crtn.o`, and `/lib/libc.so` are also packaged for TCC's default link flow.

Produced programs should be PIE ELF executables using `/lib/ld-instantos.so` as
their interpreter and `libinstant.so` as the runtime library.

## Commands

```sh
tools/fetch-tcc.sh
cmake --build build --target tcc-sysroot
TCC_NATIVE_BUILD=1 tools/build-tcc.sh
```

The final command currently stops after preparing the sysroot unless
`TCC_NATIVE_BUILD=1` is set. With that flag, the script builds the native
InstantOS `tcc` executable only. It intentionally does not run upstream `make
all` yet because that tries to execute the target `tcc` on the host to build
`libtcc1.a`.

The CMake path is:

```sh
cmake -S . -B build -DINSTANTOS_ENABLE_TCC=ON
cmake --build build --target tcc
cmake --build build --target iso
```

This currently produces `build/tcc`, an InstantOS PIE executable using
`/lib/ld-instantos.so`, and `build/tcc-sysroot/lib/tcc/libtcc1.a`.

## Status

`tcc hello.c -o hello && ./hello` works **inside InstantOS** as of 2026-06-16.
`tools/run-tcc-smoke.sh` boots a headless QEMU image (tcc + sysroot + bash over a
PTY) and asserts the full ladder: `tcc -v` -> `-E` -> `-c` -> link -> run.

Three fixes were needed to get from "host-built tcc" to "runs in-OS":

1. **`ld-instantos` glibc SONAME aliases.** tcc's `DT_NEEDED` lists `libm.so.6`
   (Makefile `LIBS=-lm`), which has no standalone file here. `loadDependency()`
   in `outside/iUserApps/ld-instantos/src/main.cpp` now aliases `libm.so.6` (and
   `libc.so.6`/`libpthread.so.0`/`libdl.so.2`/`librt.so.1`) to `libinstant.so`,
   which already exports the math symbols. Without it: `[ld-instantos] stat failed`
   and the process never starts (exit 127).
2. **`int64_t` redefinition.** ilibcxx `<stdint.h>` used a `long long` fallback when
   `__INT64_TYPE__` was undefined, but tcc's private `<stddef.h>` defines `int64_t`
   as `long` on LP64. The fixed-width typedefs are now wrapped in the shared
   `__int8_t_defined` guard and the 64-bit fallback selects `long` on LP64.
3. **Distinct CRT placeholder symbols.** `crti.o` and `crtn.o` were the same object
   (`cp`), so tcc's internal linker saw `__instant_tcc_empty_crt` "defined twice"
   (it collides on a repeated name even when local, unlike lld). `tools/tcc/empty-crt.c`
   now takes `-D__INSTANT_CRT_SYM=...` and the sysroot build compiles the two objects
   with distinct names.

Run it with: `tools/run-tcc-smoke.sh`

## Next Work

- Add the InstantOS target backend/configuration patch upstream (currently the
  host build wrapper supplies the link recipe; tcc's own defaults are configured
  via `--elfinterp`/`--crtprefix`/`--libpaths`).
- Exercise larger programs (multiple translation units, more of libc) and grow
  the SONAME alias table / sysdeps as new dependencies surface.
- Attempt building `tcc` inside InstantOS (self-hosting) now that hosted `tcc`
  compiles and runs simple programs reliably.
