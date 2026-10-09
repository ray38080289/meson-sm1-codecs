---
name: meson-sm1-codecs
description: Build, load, test and debug the Amlogic SM1 (S905X3/S905D3) V4L2 codec drivers in this repository (meson-vdec decoder, amvenc_avc H.264 and amvenc_hevc HEVC encoders). Use when changing the drivers, running the decode/encode regression, handling codec firmware, or chasing a hang, frame drop or quality problem.
---

# meson-sm1-codecs development

Three out-of-tree modules plus a CLI, all built on the SM1 board itself:

| Dir | Module | Device |
|---|---|---|
| `vdec/` | `meson_vdec` (`meson-vdec.ko`) | V4L2 stateful decoder: MPEG-1/2, H.264, HEVC, VP9 |
| `venc-h264/` | `amvenc_avc` | V4L2 encoder `meson-venc-h264`, `/dev/amvenc_avc`, `venc` CLI |
| `venc-hevc/` | `amvenc_hevc` | V4L2 encoder `meson-venc-hevc`, `/dev/HevcEnc` |

## Rules

* Never commit firmware (`*.bin`) or anything extracted from vendor/Android
  images. Firmware comes only from `get-firmware.py` (pinned public sources,
  md5-checked).
* Do not swap ucode versions. Newer `video_ucode.bin` releases speak a
  different protocol and hard-hang the decoder. A firmware change needs
  per-codec testing with netconsole running.
* Loading modules and `install.sh` need root. If you can't run sudo, give
  the user the exact commands.
* Commit messages follow the kernel style used in the history
  (`media: meson: vdec: <codec>: ...`, `venc/avc: ...`, `venc/hevc: ...`).

## Build and load

```sh
make                      # or make KDIR=<headers>; see README for ophub kernels
sync                      # a power loss after building can leave zeroed .ko files
sudo rmmod meson_vdec; sudo insmod vdec/meson-vdec.ko
sudo rmmod amvenc_avc; sudo insmod venc-h264/amvenc_avc.ko
sudo rmmod amvenc_hevc; sudo insmod venc-hevc/amvenc_hevc.ko
```

Stop every process that uses a device before `rmmod`.

## Test

* `sh tests/vdectest.sh [pattern]` (GStreamer) and `sh tests/fftest.sh [pattern]`
  (FFmpeg) compare hardware against software decode. Every line must PASS
  with matching frame counts; `inf` PSNR = bit-exact. Streams are generated
  once into `$VDECTEST_DIR` (default `~/vdectest`). Run both after any
  decoder change.
* Encoders: encode, decode with software, then compare PSNR against the
  source. Retime both sides first with `settb=1/30,setpts=N`, or container
  timestamps mis-pair frames and give bogus low PSNR. Expect ~43 dB at
  6 Mbit/s 1080p.
* FFmpeg over ssh needs `-nostdin`.
* FFmpeg 5.1 `*_v4l2m2m` encoders produce no extradata: write mp4 or raw, not mkv.
* GStreamer `rawvideoparse` needs `colorimetry=bt709`.
* `/tmp` is often a small tmpfs. Hash frames from a pipe (`-f framemd5 -`)
  instead of dumping raw video.
* H.264 hardware decode is single-instance. Make sure nothing else
  (a streaming pipeline, a player) holds it before calling a failure a
  driver bug.
* Validate the input before suspecting the driver. A file with broken
  timestamps (e.g. pts = dts with B-frames) looks like a decoder bug.
* Stop processes with `pkill -x <name>` or by PID. `pkill -f` matches the
  ssh command line that runs it and kills your own session.

## Debug

* Driver bugs often hard-hang the board. Before risky tests, stream the
  kernel log to another machine:
  `modprobe netconsole netconsole=@/<iface>,6666@<host-ip>/`
  and on `<host-ip>` run `nc -klu 6666`.
* Debug prints use `pr_debug`. Enable them with dynamic debug
  (`echo 'module meson_vdec +p' > /sys/kernel/debug/dynamic_debug/control`)
  instead of raising log levels.
* When a vendor userspace library drives the hardware (WAVE420L vpuapi),
  trace its register sequence and replay it in the kernel
  (`venc-hevc/w4enc.c`) rather than porting the library.
* For decoder stalls, compare against the vendor 4.9 `media_modules`
  driver: LMEM/RPM layout, swap context, DECODE_SIZE and the stream fetch
  behaviour must match what the ucode expects.

## Hardware and kernel pitfalls (each cost real debugging)

* `DOS_GCLK_EN0`: VDEC1 power-up used to write 0x3ff and clobbered the
  HCodec gates [26:12]. Keep them enabled or the H.264 encoder dies.
* HCodec `clock_level` 5 (fclk_div2p5, 800 MHz) hangs on G12A/SM1. Use 6
  (fclk_div3, 666 MHz).
* WAVE420L: `clk_disable_unused` gates fclk_div3/fclk_div5 (its bclk/cclk
  source), so hold them enabled. Map its RAM write-combine: Device
  mappings SIGBUS on arm64 `memset`/`memcpy`.
* A waiter in `wait_event_timeout()` must be woken with `wake_up()`;
  `wake_up_interruptible()` leaves it sleeping until the timeout.
* NV12/NV21 two-plane input: the UV plane starts at
  `y + bytesperline * height` with the real height, not the 16-aligned one.
* Linux 6.18: ioctl handlers get their fh with `file_to_v4l2_fh(file)`.
* Stateful m2m drain (CMD_STOP): always finish with an empty CAPTURE
  buffer flagged `V4L2_BUF_FLAG_LAST` plus the EOS event, even when no
  frame is left. Otherwise FFmpeg/GStreamer wait forever at EOS. Guard
  the drain against the job finishing concurrently.
* The stream fetch engine reads ahead past the ES write pointer. If the
  ucode decodes right up to the write frontier it consumes stale bytes
  (the VP9 frame drop): keep a packet of margin or defer the decode until
  the next packet or EOS arrives.
* HEVC frame-based input: streams with a small DPB (e.g. from the WAVE420L
  encoder) stall if the parser waits for more free output buffers than
  the stream needs.
