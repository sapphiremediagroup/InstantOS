#!/usr/bin/env bash
# Build a QEMU (the distro's 8.2.2) with the RTL8168H test model in rtl8168.c
# and its RTL8125B variant, and an igc device (igb under the I225-V's IDs,
# igc.c), for QEMU_NIC=rtl8168 / rtl8125 / igc runs of the network suite. QEMU has no model of this
# chip, which is what most PC boards carry. It also gains a trace event for the
# lights a USB keyboard is told to show (hid_kbd_set_leds; PS/2 already has
# ps2_set_ledstate), for the keyboard checks.
#
# The tree stays outside the checkout (C: is short on space, drvfs is slow):
#   QEMU_RTL_DIR (default ~/qemu-rtl)/qemu-8.2.2, binary in $QEMU_RTL_DIR/bin.
# Nothing is installed; $QEMU_RTL_DIR/share/qemu links to the distro's ROMs.
# Needs libglib2.0-dev libpixman-1-dev libslirp-dev ninja-build python3-venv.
#
#   tools/qemu-rtl8168/build-qemu.sh
#   QEMU=~/qemu-rtl/bin/qemu-system-x86_64 QEMU_NIC=rtl8168 tools/run-net-suite.sh
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
VERSION=8.2.2
DIR="${QEMU_RTL_DIR:-$HOME/qemu-rtl}"
SRC="$DIR/qemu-$VERSION"

mkdir -p "$DIR"
if [ ! -d "$SRC" ]; then
  if [ ! -f "$DIR/qemu-$VERSION.tar.xz" ]; then
    curl -sSfL -o "$DIR/qemu-$VERSION.tar.xz" "https://download.qemu.org/qemu-$VERSION.tar.xz"
  fi
  tar -C "$DIR" -xf "$DIR/qemu-$VERSION.tar.xz"
fi

cp "$HERE/rtl8168.c" "$SRC/hw/net/rtl8168.c"
cp "$HERE/igc.c" "$SRC/hw/net/igc.c"
if ! grep -q "igc.c" "$SRC/hw/net/meson.build"; then
  printf "system_ss.add(when: 'CONFIG_IGB_PCI_EXPRESS', if_true: files('igc.c'))
" >> "$SRC/hw/net/meson.build"
fi
if ! grep -q RTL8168_PCI "$SRC/hw/net/Kconfig"; then
  printf '\nconfig RTL8168_PCI\n    bool\n    default y if PCI_DEVICES\n    depends on PCI\n' >> "$SRC/hw/net/Kconfig"
  printf "system_ss.add(when: 'CONFIG_RTL8168_PCI', if_true: files('rtl8168.c'))\n" >> "$SRC/hw/net/meson.build"
fi

# usb-net never tells the guest about the cable (connection stays 1): give it
# link_status_changed, so the CDC-ECM driver's link notifications are checked
# by the network suite's link flap (QMP set_link).
if ! grep -q usbnet_set_link_status "$SRC/hw/usb/dev-network.c"; then
  python3 - "$SRC/hw/usb/dev-network.c" <<'PY'
import sys
p = sys.argv[1]
s = open(p).read()
s = s.replace("static NetClientInfo net_usbnet_info = {", """static void usbnet_set_link_status(NetClientState *nc)
{
    USBNetState *s = qemu_get_nic_opaque(nc);

    s->connection = !nc->link_down;
    usb_wakeup(s->intr, 0);
}

static NetClientInfo net_usbnet_info = {
    .link_status_changed = usbnet_set_link_status,""", 1)
open(p, "w").write(s)
PY
fi

if ! grep -q hid_kbd_set_leds "$SRC/hw/input/trace-events"; then
  printf 'hid_kbd_set_leds(int leds) "leds 0x%%x"\n' >> "$SRC/hw/input/trace-events"
  sed -i 's/^        hs->kbd.leds = buf\[0\];$/&\n        trace_hid_kbd_set_leds(buf[0]);/' "$SRC/hw/input/hid.c"
fi

if [ ! -f "$SRC/build/build.ninja" ]; then
  (cd "$SRC" && ./configure --target-list=x86_64-softmmu --prefix=/usr \
     --enable-slirp --disable-docs --disable-werror --disable-gtk --disable-sdl \
     --disable-vnc --disable-opengl --disable-virglrenderer --disable-xen)
fi
ninja -C "$SRC/build" qemu-system-x86_64
mkdir -p "$DIR/bin" "$DIR/share"
cp "$SRC/build/qemu-system-x86_64" "$DIR/bin/qemu-system-x86_64"
# QEMU looks for its ROMs relative to the binary, in ../share/qemu; the
# distro's are spread over three directories.
rm -rf "$DIR/share/qemu"
mkdir -p "$DIR/share/qemu"
for d in /usr/share/qemu /usr/share/seabios /usr/lib/ipxe/qemu; do
  [ -d "$d" ] && ln -sf "$d"/* "$DIR/share/qemu/"
done
"$DIR/bin/qemu-system-x86_64" -device help | grep -q '"rtl8168"'
"$DIR/bin/qemu-system-x86_64" -device help | grep -q '"rtl8125"'
"$DIR/bin/qemu-system-x86_64" -device help | grep -q '"igc"'
echo "qemu with rtl8168, rtl8125, igc and usb-net link changes: $DIR/bin/qemu-system-x86_64"
