# InstantOS
A rewrite of the closed source graphical x86_64 operating system

# Features
- [x] Terminal Emulator (with ANSI)
- [x] Userland!
- [x] C/C++ and Rust Support
- [x] Keyboard support
- [x] Mouse support
- [x] Window Manager

# Upcoming features
- [ ] Custom FileSystem

# Building
## Requirements
- `clang` and `clang++`
- `cmake`
- `nasm`
- `make`
- `lld-link` through LLVM
- `xorriso`
- `mtools` (`mformat`, `mmd`, `mcopy`)
- `qemu-system-x86_64`
- OVMF firmware files

InstantOS also expects the bundled dependencies under `outside/` to be present, So clone this Repository with --recurse-submodules.

## Build
Clean and configure the build directory:

```bash
rm -rf build
cmake -S . -B build
```

Build the bootloader, kernel, userland programs, initrd, and ISO:

```bash
cmake --build build --target iso --parallel 4
```

The final bootable image is written to:

```text
build/iboot.iso
```

## Run
Boot the generated image in QEMU with:

```bash
./run.sh
```

`run.sh` creates `build/ahci.img` on first run and copies `OVMF_VARS.4m.fd` into `build/` if needed.

## build and run
If you want the full workflow in one command, use:

```bash
./build.sh
```