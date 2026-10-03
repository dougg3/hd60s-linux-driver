#!/bin/sh
# Install the driver with DKMS so it is rebuilt on kernel updates and loaded
# at boot. Run as root from the source directory.
set -e
cd "$(dirname "$0")"
VER=$(sed -n 's/^PACKAGE_VERSION="\(.*\)"/\1/p' dkms.conf)

command -v dkms >/dev/null || { echo "dkms is not installed"; exit 1; }

# Unbind first: PipeWire/WirePlumber keep the ALSA device open, which
# otherwise makes rmmod fail.
if lsmod | grep -q '^hd60s '; then
	for i in /sys/bus/usb/drivers/hd60s/*:*; do
		[ -e "$i" ] && basename "$i" > /sys/bus/usb/drivers/hd60s/unbind
	done
	rmmod hd60s
fi

dkms remove hd60s/"$VER" --all 2>/dev/null || true
rm -rf /usr/src/hd60s-"$VER"
mkdir -p /usr/src/hd60s-"$VER"
cp Makefile dkms.conf *.c *.h LICENSE.txt /usr/src/hd60s-"$VER"/
dkms install hd60s/"$VER"
echo hd60s > /etc/modules-load.d/hd60s.conf
modprobe hd60s
dkms status hd60s
