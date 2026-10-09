# S905X3 (HK1 X3) hardware H.264 encoder on mainline Linux

Amlogic HCodec H.264 encoder for Armbian `6.18.38-ophub`, ported from the
vendor `media_modules` driver (GPL) onto mainline APIs.

## Pieces

| Path | What |
|---|---|
| `avc/encoder.c` | vendor driver, minimal edits (clock level, init/exit, `remove`, `sched_set_fifo`) |
| `avc/compat.{c,h}` | vendor API → mainline: DOS/AO/HHI regs, canvas, CMA, firmware, power, IRQ (SPI 45) |
| `avc/venc.c` | CLI: raw YUV on stdin → H.264 Annex-B on stdout |
| `meson-vdec-keep-hcodec-gclk.patch` | stops `meson_vdec` clearing HCodec clock gates (needed for decode + encode at once) |
| `install.sh` | installs modules to `updates/`, firmware, udev rule, autoload, `/usr/local/bin/venc` |
| `../aml_unpack.py`, `../amlfw_list.py` | unpack the Android USB-burn image / extract the encoder microcode |

Firmware `ga_h264_enc_cabac.bin` comes from the box's own Android image
(`vendor/lib/firmware/video/h264_enc.bin`); it is Amlogic-proprietary, so do not
redistribute it — extract it with `amlfw_list.py`.

## Building (on the box)

The kernel was built with gcc 15, the box has gcc 12, and the shipped headers'
`modpost` needs a newer glibc. `~/kbuild` is a private copy of
`/usr/src/linux-headers-6.18.38-ophub` with `scripts/mod/modpost` rebuilt and
`-fmin-function-alignment` swapped for `-falign-functions` in its Makefile.

```
cd ~/venc/avc && make && gcc -O2 -o venc venc.c && sync
```

## Usage

```
venc -s WxH [-f nv12|nv21|i420] [-q QP | -b KBPS -r FPS] [-g GOP] [-n FRAMES] [-S stride -U chroma_off -B frame_bytes]
```

* camera / file: `ffmpeg -i in.mp4 -f rawvideo -pix_fmt nv12 - | venc -s 1920x1080 -b 4000 -r 30 > out.h264`
* test pattern: `ffmpeg -f lavfi -i testsrc2=s=1280x720:r=30 -t 10 -f rawvideo -pix_fmt nv12 - | venc -s 1280x720 -q 26 > t.h264`

Output is raw H.264; wrap with `ffmpeg -r 30 -i out.h264 -c copy out.mp4`.

## V4L2 encoders (FFmpeg / GStreamer)

Both modules also register standard V4L2 mem2mem encoders: `meson-venc-h264`
(`avc/venc_v4l2.c`, HCodec, up to 1920x1088) and `meson-venc-hevc`
(`hevc/venc_v4l2.c` + `hevc/w4enc.c`, WAVE420L, 256x128 up to 4096x2304,
firmware `meson/venc/monet.bin` from `get-firmware.py`). NV12/NV21, one or two
planes; bitrate, GOP, I/P/min/max QP and force-key-frame controls.

```
ffmpeg -i in.mkv -c:v hevc_v4l2m2m -b:v 6M -g 60 out.mp4
gst-launch-1.0 filesrc location=in.mkv ! matroskademux ! h264parse ! v4l2h264dec ! v4l2h265enc extra-controls=controls,video_bitrate=6000000 ! h265parse ! matroskamux ! filesink location=out.mkv
```

* `w4enc.c` replays the Chips&Media vpuapi command sequence (traced from the
  sample) in the kernel: IPPP, firmware rate control, IDRs forced by the driver
  and preceded by VPS/SPS/PPS.
* `/dev/HevcEnc` (`henc`) and the V4L2 HEVC encoder exclude each other (`EBUSY`).
* FFmpeg 5.1 `*_v4l2m2m` cannot write MKV directly (no extradata): write
  `.mp4`/raw and remux. `rawvideoparse` needs `colorimetry=bt709` for GStreamer.
* 4K HEVC with FFmpeg needs `-num_output_buffers 6` (16 x 12 MB OUTPUT buffers
  exhaust the 256 MB CMA); 4K zero-copy decode + encode needs a larger CMA.

## Measured

* H.264 Main, CABAC, I/P only, up to 1920x1088
* 1080p ≈ 58 fps, 720p ≈ 106 fps (incl. feeding raw frames); 3 concurrent sessions ≈ 150 fps total at 720p
* QP 26 on 720p testsrc2: 4.1 Mbit/s, PSNR 43.6 dB; `-b` converges within a few % above the QP-51 floor
* NV12 / NV21 / I420 verified by PSNR; 4th concurrent open returns `EBUSY`

## Known issues

* The vendor ucode runs in CBR mode on SM1 and takes per-MB QP from the CBR
  table in the shared buffer; `venc` fills it with the frame QP. Rate control
  is frame-level only.
* The ucode reports `IDR_DONE` (9) for P frames too.
* **Hardware H.264 decode (`meson_vdec`, ophub/unifreq) returns a corrupt luma
  plane** on this box (chroma is fine) with stock or patched module and any
  stream. Use software decode in front of `venc` until that is fixed.
* ffmpeg 5.1 `h264_v4l2m2m` hangs ~50% at start; GStreamer `v4l2h264dec` does not.
* Kernel update → rebuild both modules against the new headers and re-apply the vdec patch.
