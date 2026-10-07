#!/bin/sh
# Install the HCodec H.264 encoder + patched meson-vdec for the running kernel.
# Undo: rm the files listed below, then depmod -a.
set -e
K=$(uname -r)
SRC=/home/ray

strip --strip-debug -o /tmp/amvenc_avc.ko "$SRC/venc/avc/amvenc_avc.ko"
strip --strip-debug -o /tmp/meson-vdec.ko "$SRC/vdecclean/meson-vdec.ko"
# updates/ takes precedence over kernel/ for modprobe
install -Dm644 /tmp/amvenc_avc.ko "/lib/modules/$K/updates/amvenc_avc.ko"
install -Dm644 /tmp/meson-vdec.ko "/lib/modules/$K/updates/meson-vdec.ko"
install -Dm644 "$SRC/venc/ga_h264_enc_cabac.bin" /lib/firmware/meson/venc/ga_h264_enc_cabac.bin
install -Dm755 "$SRC/venc/avc/venc" /usr/local/bin/venc
echo 'KERNEL=="amvenc_avc", GROUP="video", MODE="0660"' > /etc/udev/rules.d/99-amvenc.rules
echo amvenc_avc > /etc/modules-load.d/amvenc.conf
depmod -a "$K"
sync
echo "installed for $K:"
modinfo -n amvenc_avc meson_vdec
