#!/bin/bash
# Usage: stress.sh alone|both N  — hw-decode /tmp/a.h264, optionally while encoding.
mode=$1; n=${2:-10}; cd "$(dirname "$0")"
irq() { awk '/ vdec$/ {print $2}' /proc/interrupts; }
for r in $(seq 1 "$n"); do
	i0=$(irq)
	timeout -s KILL 30 ffmpeg -hide_banner -v info -stats -c:v h264_v4l2m2m \
		-i /tmp/a.h264 -f null - 2>&1 | tr '\r' '\n' > /tmp/dec.log &
	dec=$!
	enc=-
	if [ "$mode" = both ]; then
		sleep 0.$((r % 9))
		./venc_test 1280 720 600 /tmp/b.h264 > /tmp/enc.log
		enc=$(grep -c "stage 9" /tmp/enc.log)
	fi
	wait $dec
	frames=$(grep -o 'frame= *[0-9]*' /tmp/dec.log | tail -1 | tr -dc 0-9)
	echo "round $r: dec_frames=${frames:-0} vdec_irqs=$(( $(irq) - i0 )) enc_frames=$enc"
done
