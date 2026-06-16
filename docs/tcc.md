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

## Next Work

- Add an InstantOS target backend/configuration patch for TinyCC.
- Validate the packaged `/lib/tcc/include` sysroot from inside InstantOS.
- Validate that TCC's default linker path can find `/lib/crt1.o`, `/lib/crti.o`, `/lib/crtn.o`, `/lib/tcc/libtcc1.a`, and `/lib/libc.so`.
- Make TCC emit `/lib/ld-instantos.so` as the ELF interpreter by default.
- Make `-lc` resolve to `libinstant.so` or a future mlibc libc.
- Add a link mode equivalent to the current user app link flags: PIE, `_start`,
  SysV hash, no build-id, and `-z max-page-size=0x1000`.
- Package a minimal writable test workspace in InstantOS and validate:

```sh
tcc hello.c -o hello
./hello
```
