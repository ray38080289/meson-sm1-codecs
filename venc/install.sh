#!/bin/sh
# Install the HCodec H.264 encoder, the WAVE420L H.265 encoder and the
# patched meson-vdec for the running kernel.
# Undo: rm the files listed below, then depmod -a.
set -e
K=$(uname -r)
SRC=/home/ray

strip --strip-debug -o /tmp/amvenc_avc.ko "$SRC/venc/avc/amvenc_avc.ko"
strip --strip-debug -o /tmp/amvenc_hevc.ko "$SRC/henc/amvenc_hevc.ko"
strip --strip-debug -o /tmp/meson-vdec.ko "$SRC/vdecclean/meson-vdec.ko"
# updates/ takes precedence over kernel/ for modprobe
install -Dm644 /tmp/amvenc_avc.ko "/lib/modules/$K/updates/amvenc_avc.ko"
install -Dm644 /tmp/amvenc_hevc.ko "/lib/modules/$K/updates/amvenc_hevc.ko"
install -Dm644 /tmp/meson-vdec.ko "/lib/modules/$K/updates/meson-vdec.ko"

# H.264 (HCodec)
install -Dm644 "$SRC/venc/ga_h264_enc_cabac.bin" /lib/firmware/meson/venc/ga_h264_enc_cabac.bin
install -Dm755 "$SRC/venc/avc/venc" /usr/local/bin/venc

# H.265 (WAVE420L): Chips&Media sample + firmware (monet.bin, C&M binary
# licence: redistribution with the copyright notice) + config template
install -Dm755 "$SRC/w420/code/w4_enc_test" /usr/local/lib/henc/w4_enc_test
install -Dm644 "$SRC/w420/code/monet.bin" /usr/local/lib/henc/monet.bin
install -Dm644 "$SRC/w420/firmware/LICENSE.txt" /usr/local/lib/henc/LICENSE.monet.txt
install -Dm644 "$SRC/w420/code/cfg/encoder_defconfig.cfg" /usr/local/lib/henc/template.cfg
install -Dm755 "$SRC/henc/henc" /usr/local/bin/henc

cat > /etc/udev/rules.d/99-amvenc.rules <<'EOF'
KERNEL=="amvenc_avc", GROUP="video", MODE="0660"
KERNEL=="HevcEnc", GROUP="video", MODE="0660"
EOF
printf 'amvenc_avc\namvenc_hevc\n' > /etc/modules-load.d/amvenc.conf
depmod -a "$K"
sync
echo "installed for $K:"
modinfo -n amvenc_avc amvenc_hevc meson_vdec
