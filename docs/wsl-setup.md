# InstantOS WSL2 Build Environment Setup

Ubuntu 24.04 (WSL2) toolchain for building InstantOS on `/mnt/d/projects/InstantOS`.

## One-time setup

```bash
# Core toolchain (clang 18 is fine for kernel/userland/TCC; bash port needs clang >= 19)
sudo apt update
sudo apt install -y clang lld llvm clang-20 lld-20 llvm-20 \
                    cmake ninja-build meson nasm make xorriso mtools \
                    qemu-system-x86 ovmf build-essential git

# Scripts expect the Arch-style OVMF path; Ubuntu ships /usr/share/OVMF
sudo mkdir -p /usr/share/edk2/x64
sudo ln -sf /usr/share/OVMF/OVMF_CODE_4M.fd /usr/share/edk2/x64/OVMF_CODE.4m.fd
sudo ln -sf /usr/share/OVMF/OVMF_VARS_4M.fd /usr/share/edk2/x64/OVMF_VARS.4m.fd
```

## Building

```bash
cd /mnt/d/projects/InstantOS
rm -rf build
cmake -S . -B build
cmake --build build --target iso --parallel 4
```

Result: `build/iboot.iso` (bootable UEFI image).

Options:
- `-DINSTANTOS_ENABLE_TCC=ON` — bundle the TinyCC port (enabled automatically by `./build.sh` when `outside/iUserApps/outside/tinycc` exists)
- `tools/build-mlibc.sh` + `tools/build-bash.sh` — optional GNU bash + mlibc runtime (needs clang >= 19: `export PATH=/usr/lib/llvm-20/bin:$PATH`)

## Running

WSLg is unavailable on this machine (no /mnt/wslg), so use either:

```bash
# Headless smoke test inside WSL (no KVM available - use TCG)
QEMU_ACCEL=tcg QEMU_DISPLAY=none USB_MODE=xhci ./run.sh

# Or: Windows-native QEMU with WHPX accel + GUI (Git Bash/MSYS)
./run-win.sh
```

- No `/dev/kvm` in this WSL — always set `QEMU_ACCEL=tcg` for in-WSL runs.
- `run-win.sh` needs QEMU for Windows in `C:\Program Files\qemu` and picks up
  `build/iboot.iso` + `build/ahci.img` from the shared `/mnt/d` path.

## Notes

- Submodules must be checked out (`git clone --recurse-submodules`), including
  the nested ones under `outside/iUserApps/outside/` (ilibcxx, tinycc, mlibc,
  mlibc-sysdeps).
- The bash port cross-build is slow (10+ minutes) and cached in `.bash-cache/`.
- `build.sh` = clean configure + build + run in one command.
