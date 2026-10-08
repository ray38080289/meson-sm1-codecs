#!/bin/sh
# FFmpeg *_v4l2m2m decode vs software, frame md5 by pts
cd ~/vdectest
[ -s m2.mkv ] || ffmpeg -nostdin -hide_banner -loglevel error -y -i m2b.m2v -c copy m2.mkv
[ -s m2sd.mkv ] || ffmpeg -nostdin -hide_banner -loglevel error -y -i m2sd.m2v -c copy m2sd.mkv
for t in "c264.mkv h264_v4l2m2m" "c265.mkv hevc_v4l2m2m" "vp9.webm vp9_v4l2m2m" "vp9sd.webm vp9_v4l2m2m" "vp9long.webm vp9_v4l2m2m" "m2.mkv mpeg2_v4l2m2m" "m2sd.mkv mpeg2_v4l2m2m"; do
	set -- $t
	ffmpeg -nostdin -hide_banner -loglevel error -y -i $1 -vsync passthrough -pix_fmt yuv420p -f framemd5 sw.md5
	timeout -s KILL 60 ffmpeg -nostdin -hide_banner -loglevel error -y -c:v $2 -i $1 -vsync passthrough -pix_fmt yuv420p -f framemd5 hw.md5 2>hw.err
	rc=$?
	python3 -c "
def rd(f):
    try: return [ (l.split(\",\")[-1].strip(), int(l.split(\",\")[2])) for l in open(f) if not l.startswith(\"#\")]
    except Exception: return []
A=rd(\"hw.md5\"); B=rd(\"sw.md5\"); pos={h:t for h,t in B}
bad=[t for h,t in A if pos.get(h)!=t]
print(\"%-14s %-15s rc=$rc frames %d/%d bad=%d first_bad=%s\" % (\"$1\", \"$2\", len(A), len(B), len(bad), bad[:1]))
"
	head -c 120 hw.err
done
echo DONE
