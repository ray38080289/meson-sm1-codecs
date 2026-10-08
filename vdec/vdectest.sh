#!/bin/sh
# Hardware decode regression: decode each stream through GStreamer V4L2 and
# compare with ffmpeg software decode (PSNR; "inf" = bit-exact).
# Streams are generated once into $D. Usage: sh vdectest.sh [pattern]
D=${VDECTEST_DIR:-$HOME/vdectest}
F="ffmpeg -nostdin -hide_banner -loglevel error -y"
SRC="-f lavfi -i testsrc2=size"
mkdir -p "$D"
cd "$D" || exit 1

gen() {	# file, encoder args...
	f=$1; shift
	[ -s "$f" ] || $F "$@" "$f"
}
gen m2.m2v      $SRC=1920x1080:rate=30 -frames:v 60 -c:v mpeg2video -g 15 -q:v 4
gen m2b.m2v     $SRC=1920x1080:rate=30 -frames:v 90 -c:v mpeg2video -bf 2 -g 15 -q:v 4
gen m2sd.m2v    $SRC=720x576:rate=25 -frames:v 75 -c:v mpeg2video -bf 2 -g 12 -q:v 3
gen m2i.m2v     $SRC=1920x1080:rate=25 -frames:v 50 -c:v mpeg2video -flags +ilme+ildct -top 1 -bf 2 -g 12 -q:v 4
gen m1.m1v      $SRC=1280x720:rate=30 -frames:v 60 -c:v mpeg1video -bf 2 -g 15 -q:v 4 -f mpeg1video
gen m2long.m2v  $SRC=1920x1080:rate=30 -frames:v 600 -c:v mpeg2video -bf 2 -g 15 -b:v 15M
gen h1080.hevc  $SRC=1920x1080:rate=30 -frames:v 30 -c:v libx265 -preset fast -x265-params log-level=error:keyint=15 -f hevc
gen hsd.hevc    $SRC=720x576:rate=25 -frames:v 20 -c:v libx265 -x265-params log-level=error -f hevc
gen hlong.hevc  $SRC=1920x1080:rate=30 -frames:v 300 -c:v libx265 -preset fast -x265-params log-level=error:keyint=60:bframes=3 -b:v 8M -f hevc
gen h10.hevc    $SRC=1920x1080:rate=30 -frames:v 30 -c:v libx265 -preset fast -pix_fmt yuv420p10le -x265-params log-level=error -f hevc
gen vp9.webm    $SRC=1920x1080:rate=30 -frames:v 30 -c:v libvpx-vp9 -deadline realtime -cpu-used 8 -b:v 4M -g 15
gen vp9sd.webm  $SRC=720x576:rate=25 -frames:v 50 -c:v libvpx-vp9 -deadline realtime -cpu-used 8 -b:v 2M
gen vp9long.webm $SRC=1920x1080:rate=30 -frames:v 300 -c:v libvpx-vp9 -deadline realtime -cpu-used 8 -b:v 6M -g 60
gen vp910.webm  $SRC=1920x1080:rate=30 -frames:v 30 -c:v libvpx-vp9 -deadline realtime -cpu-used 8 -pix_fmt yuv420p10le -b:v 4M

for f in m2.m2v m2b.m2v m2sd.m2v m2i.m2v m1.m1v m2long.m2v \
	 h1080.hevc hsd.hevc hlong.hevc h10.hevc \
	 vp9.webm vp9sd.webm vp9long.webm vp910.webm; do
	case $f in *${1:-}*) ;; *) continue ;; esac
	case $f in
	*.m2v|*.m1v) dec="mpegvideoparse ! v4l2mpeg4dec" ;;
	*.hevc) dec="h265parse ! v4l2h265dec" ;;
	*.webm) dec="matroskademux ! v4l2vp9dec" ;;
	esac
	size=$(ffprobe -v error -select_streams v:0 -show_entries stream=width,height -of csv=s=x:p=0 "$f" | head -1)
	size=${size%x}	# some containers add a trailing separator
	$F -i "$f" -vsync passthrough -f rawvideo -pix_fmt yuv420p sw.yuv
	rm -f hw.yuv
	t0=$(date +%s)
	timeout -s KILL 300 gst-launch-1.0 -q filesrc location="$f" ! $dec ! \
		videoconvert ! video/x-raw,format=I420 ! filesink location=hw.yuv > gst.log 2>&1
	rc=$?
	psnr=$(ffmpeg -nostdin -hide_banner -f rawvideo -pix_fmt yuv420p -s "$size" -i sw.yuv \
		-f rawvideo -pix_fmt yuv420p -s "$size" -i hw.yuv -lavfi "[0][1]psnr" -f null - 2>&1 |
		grep -o "average:[^ ]* min:[^ ]*")
	n_hw=$(( $(stat -c%s hw.yuv 2>/dev/null || echo 0) ))
	n_sw=$(( $(stat -c%s sw.yuv) ))
	warn=$(grep -c "undrained\|ERROR\|錯誤" gst.log)
	printf '%-14s rc=%-3s frames %s/%s %-34s %ss%s\n' "$f" "$rc" \
		"$((n_hw * 2 / 3 / ${size%x*} / ${size#*x}))" "$((n_sw * 2 / 3 / ${size%x*} / ${size#*x}))" \
		"$psnr" "$(( $(date +%s) - t0 ))" "$([ "$warn" -gt 0 ] && echo " gst-warn")"
done
rm -f sw.yuv hw.yuv
