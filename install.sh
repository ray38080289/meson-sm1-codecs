#!/bin/sh
# Install what `make` built for the running kernel: meson-vdec (decoder),
# amvenc_avc (H.264 encoder), amvenc_hevc (HEVC encoder), their firmware and
# the venc CLI.
#   sudo ./install.sh              install
#   sudo ./install.sh --uninstall  remove (firmware is left in place)
set -e
cd "$(dirname "$0")"
K=$(uname -r)
D=/lib/modules/$K/updates
MODS="vdec/meson-vdec.ko venc-h264/amvenc_avc.ko venc-hevc/amvenc_hevc.ko"

if [ "${1:-}" = --uninstall ]; then
	for m in $MODS; do rm -f "$D/${m##*/}"; done
	rm -f /usr/local/bin/venc /etc/udev/rules.d/99-amvenc.rules \
	      /etc/modules-load.d/amvenc.conf
	depmod -a "$K"
	echo "removed; reboot to get the in-kernel meson-vdec back"
	exit 0
fi

for m in $MODS venc-h264/venc; do
	[ -f "$m" ] || { echo "$m missing: run make first" >&2; exit 1; }
done

# updates/ takes precedence over the in-kernel meson-vdec for modprobe
for m in $MODS; do
	strip --strip-debug -o "/tmp/${m##*/}" "$m"
	install -Dm644 "/tmp/${m##*/}" "$D/${m##*/}"
	rm -f "/tmp/${m##*/}"
done

# firmware from its public upstreams, md5-checked (see get-firmware.py)
python3 get-firmware.py

install -Dm755 venc-h264/venc /usr/local/bin/venc

# legacy character devices of the encoders (the V4L2 nodes are /dev/video*)
cat > /etc/udev/rules.d/99-amvenc.rules <<'EOF'
KERNEL=="amvenc_avc", GROUP="video", MODE="0660"
KERNEL=="HevcEnc", GROUP="video", MODE="0660"
EOF
printf 'amvenc_avc\namvenc_hevc\n' > /etc/modules-load.d/amvenc.conf
depmod -a "$K"
sync
echo "installed for $K:"
modinfo -n meson_vdec amvenc_avc amvenc_hevc
echo "reboot (or reload meson_vdec and load amvenc_avc, amvenc_hevc) to use them"
