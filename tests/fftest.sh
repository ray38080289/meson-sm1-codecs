#!/bin/sh
# FFmpeg *_v4l2m2m hardware decode against ffmpeg software decode:
# frame counts and PSNR (inf = bit-exact). Container inputs, since raw
# elementary streams carry no timestamps for the decoder to pass through.
# Usage: sh fftest.sh [pattern]   (streams are made once in $D)
P=${1:-}
D=${VDECTEST_DIR:-$HOME/vdectest}
F="ffmpeg -nostdin -hide_banner -loglevel error -y"
SRC="-f lavfi -i testsrc2=size"
mkdir -p "$D"
cd "$D" || exit 1

gen() {	# file, encoder args...
	f=$1; shift
	[ -s "$f" ] || $F "$@" "$f"
}
gen c264.mkv  $SRC=1920x1080:rate=30 -frames:v 120 -c:v libx264 -preset fast -bf 3 -g 60
gen c265.mkv  $SRC=1920x1080:rate=30 -frames:v 60 -c:v libx265 -preset ultrafast -x265-params log-level=error:bframes=3
gen m2c.mkv   $SRC=1920x1080:rate=30 -frames:v 90 -c:v mpeg2video -bf 2 -g 15 -q:v 4
gen m2sdc.mkv $SRC=720x576:rate=25 -frames:v 75 -c:v mpeg2video -bf 2 -g 12 -q:v 3
gen m1c.mkv   $SRC=1280x720:rate=30 -frames:v 60 -c:v mpeg1video -bf 2 -g 15 -q:v 4
gen vp9.webm  $SRC=1920x1080:rate=30 -frames:v 30 -c:v libvpx-vp9 -deadline realtime -cpu-used 8 -b:v 4M -g 15
gen vp9sd.webm $SRC=720x576:rate=25 -frames:v 50 -c:v libvpx-vp9 -deadline realtime -cpu-used 8 -b:v 2M
gen vp9long.webm $SRC=1920x1080:rate=30 -frames:v 300 -c:v libvpx-vp9 -deadline realtime -cpu-used 8 -b:v 6M -g 60

for t in "c264.mkv h264_v4l2m2m" "c265.mkv hevc_v4l2m2m" "m2c.mkv mpeg2_v4l2m2m" \
	 "m2sdc.mkv mpeg2_v4l2m2m" "m1c.mkv mpeg1_v4l2m2m" "vp9.webm vp9_v4l2m2m" \
	 "vp9sd.webm vp9_v4l2m2m" "vp9long.webm vp9_v4l2m2m"; do
	set -- $t
	case $1 in *$P*) ;; *) continue ;; esac
	size=$(ffprobe -v error -select_streams v:0 -show_entries stream=width,height -of csv=s=x:p=0 "$1" | head -1)
	size=${size%x}
	$F -i "$1" -vsync passthrough -f rawvideo -pix_fmt yuv420p sw.yuv
	rm -f hw.yuv
	t0=$(date +%s)
	timeout -s KILL 120 ffmpeg -nostdin -hide_banner -loglevel error -y -c:v $2 -i "$1" \
		-vsync passthrough -f rawvideo -pix_fmt yuv420p hw.yuv 2>hw.err
	rc=$?
	fb=$(( ${size%x*} * ${size#*x} * 3 / 2 ))
	psnr=$(ffmpeg -nostdin -hide_banner -f rawvideo -pix_fmt yuv420p -s "$size" -i sw.yuv \
		-f rawvideo -pix_fmt yuv420p -s "$size" -i hw.yuv -lavfi "[0][1]psnr" -f null - 2>&1 |
		grep -o "average:[^ ]* min:[^ ]*")
	printf '%-13s %-14s rc=%-3s frames %s/%s %-34s %ss\n' "$1" "$2" "$rc" \
		"$(( $(stat -c%s hw.yuv 2>/dev/null || echo 0) / fb ))" "$(( $(stat -c%s sw.yuv) / fb ))" \
		"$psnr" "$(( $(date +%s) - t0 ))"
done
rm -f sw.yuv hw.yuv hw.err
