# meson-sm1-codecs

Hardware video decoding and encoding for Amlogic **SM1** (S905X3 / S905D3)
on mainline Linux, as standard **V4L2 mem2mem** devices: FFmpeg
(`*_v4l2m2m`) and GStreamer (`v4l2*dec` / `v4l2*enc`) use them directly.

Tested on an HK1 X3 (S905X3) TV box running Armbian with kernel
6.18.55-ophub. Other SM1 boards should work but are untested.

> **100% AI-generated:** everything here except the imported vendor and
> upstream code was written by Claude (Anthropic). See [AI-generated](#ai-generated).

| | Format | Max size | 1080p | 4K | Quality |
|---|---|---|---|---|---|
| Decode | MPEG-1/2 | 1920x1088 | 193 fps | — | IDCT rounding only (~68 dB vs FFmpeg) |
| Decode | H.264 | 4096x2304 | 113 fps | ~40 fps | bit-exact vs FFmpeg |
| Decode | HEVC 8/10-bit | 4096x2304 | 167 fps | ~55 fps | bit-exact vs FFmpeg (8-bit) |
| Decode | VP9 8/10-bit | 4096x2304 | 186 fps | ~73 fps | bit-exact vs FFmpeg (8-bit) |
| Encode | H.264 Main | 1920x1088 | ~58 fps | — | ~43 dB at 6 Mbit/s |
| Encode | HEVC Main | 4096x2304 | ~59 fps | ~11 fps (raw input bound) | ~43 dB at 6 Mbit/s |

## Contents

| Directory | Module | What |
|---|---|---|
| `vdec/` | `meson-vdec` | the mainline/staging meson video decoder with SM1 fixes (MPEG-1/2 multi-instance protocol, HEVC/VP9/H.264 fixes, drain/EOS, FFmpeg compatibility) |
| `venc-h264/` | `amvenc_avc` | Amlogic HCodec H.264 encoder: the vendor driver on a mainline compat layer, a V4L2 encoder (`meson-venc-h264`) and the `venc` CLI |
| `venc-hevc/` | `amvenc_hevc` | Chips&Media WAVE420L HEVC encoder: the vendor driver plus an in-kernel command layer (`w4enc.c`) and a V4L2 encoder (`meson-venc-hevc`) |
| `get-firmware.py` | | fetches the firmware from public repositories (none is included here) |
| `tests/` | | decode regression scripts (GStreamer and FFmpeg against software decode) |

## Build and install

Needs the headers of the running kernel, `gcc`, `make`, `python3`.

```sh
make
sudo ./install.sh
sudo reboot
```

`install.sh` puts the modules into `/lib/modules/$(uname -r)/updates/` (taking
precedence over the in-kernel `meson-vdec`), runs `get-firmware.py`, installs
`venc` to `/usr/local/bin` and loads the encoders at boot.
`sudo ./install.sh --uninstall` removes it again. Rebuild after every kernel
update.

## Firmware

`get-firmware.py` downloads every file the drivers need, pinned to a commit
and checked against the md5 they were tested with:

| File | Source |
|---|---|
| `meson/vdec/sm1_mpeg12_multi.bin`, `sm1_hevc_mmu_multi.bin`, `meson/venc/ga_h264_enc_cabac.bin` | [CoreELEC/media_modules-aml](https://github.com/CoreELEC/media_modules-aml) |
| `meson/venc/monet.bin` (+ Chips&Media licence) | [starfive-tech/soft_3rdpart](https://github.com/starfive-tech/soft_3rdpart) |
| `meson/vdec/g12a_h264.bin`, `sm1_vp9_mmu.bin` (only if missing) | [linux-firmware](https://gitlab.com/kernel-firmware/linux-firmware) |

The Amlogic ucode is under Amlogic's terms (see `LICENSE.amlogic_vdec` in
linux-firmware: use with Amlogic chips, not as part of the kernel). Other
ucode versions (newer `video_ucode.bin` releases) speak a different protocol
and can hang the decoder; the md5 check refuses them.

## Usage

```sh
# decode
ffmpeg -c:v hevc_v4l2m2m -i in.mkv -f null -
gst-launch-1.0 filesrc location=in.webm ! matroskademux ! v4l2vp9dec ! autovideosink

# encode
ffmpeg -i in.mkv -c:v h264_v4l2m2m -b:v 6M -g 60 out.mp4
ffmpeg -i in.mkv -c:v hevc_v4l2m2m -b:v 6M -g 60 out.mp4

# hardware transcode, no copies (decoder buffers go straight to the encoder)
gst-launch-1.0 filesrc location=in.mkv ! matroskademux ! h264parse ! v4l2h264dec ! \
    v4l2h265enc extra-controls=controls,video_bitrate=6000000 ! h265parse ! \
    matroskamux ! filesink location=out.mkv

# raw frames to H.264 without V4L2
ffmpeg -i in.mp4 -f rawvideo -pix_fmt nv12 - | venc -s 1920x1080 -b 4000 -r 30 > out.h264
```

Encoders: NV12/NV21 input (one or two planes); controls for bitrate (rate
control on/off), GOP size, I/P/min/max QP and force key frame.

## Known limitations

* H.264 decode: streams with the deblocking filter disabled get a few wrong
  rows at the bottom of each macroblock row (the hardware/ucode does not write
  them); normal streams are unaffected. One H.264 decode session at a time.
* FFmpeg 5.1 `*_v4l2m2m` encoders produce no extradata, so they cannot write
  MKV directly: write MP4 or raw and remux. GStreamer `rawvideoparse` needs
  `colorimetry=bt709`.
* 4K HEVC encoding with FFmpeg needs `-num_output_buffers 6` with the default
  256 MB CMA; 4K decode plus encode in one pipeline needs a larger CMA
  (e.g. `cma=512M`).
* The H.264 encoder rate control is frame-level; the HEVC encoder uses the
  firmware's. Changing controls other than force-key-frame takes effect at the
  next stream start.
* The decoder may log "decoder did not become inactive" when stopping; it is
  harmless.

## Building against ophub kernels

The ophub kernels are built with a newer gcc (15) than Armbian bookworm ships
(12), and their packaged `modpost` needs a newer glibc. Make a copy of
`/usr/src/linux-headers-$(uname -r)`, rebuild `scripts/mod/modpost` in it,
replace `-fmin-function-alignment` with `-falign-functions` in its Makefile,
and build with `make KDIR=<that copy>`.

## Tests

`tests/vdectest.sh` (GStreamer) and `tests/fftest.sh` (FFmpeg) generate test
streams once and compare hardware decoding against FFmpeg's software decoder
(frame counts and PSNR, `inf` = bit-exact).

## AI-generated

All work in this repository on top of the imported code is 100%
AI-generated by [Claude](https://claude.com) (Anthropic) in Claude Code:
the porting to mainline, every decoder fix, the V4L2 encoders and `w4enc.c`,
the build and install scripts, `get-firmware.py`, the tests and this README,
as well as the debugging behind them (register traces, firmware analysis).
The repository owner directed the work and ran everything on the hardware.

Not AI-generated, and imported unmodified in their own commits:

* `vdec/`: the meson video decoder from mainline Linux / the unifreq staging
  tree (commit "pristine unifreq/ophub staging driver")
* `venc-hevc/vpu.c` and friends: Amlogic's WAVE420L driver (commit "vendor
  WAVE420L HevcEnc driver ... unmodified")
* `venc-h264/encoder.c`: Amlogic's HCodec driver, imported together with
  its first port

Commits made with Claude carry a `Co-Authored-By: Claude` trailer.

## Licence

The kernel code is GPL-2.0: the meson decoder comes from mainline Linux /
the unifreq staging tree, the encoders from Amlogic's `media_modules` and
Chips&Media's driver; `venc-hevc/w4enc.c` follows the Chips&Media vpuapi
(LGPL-2.1 OR BSD-3-Clause, used under BSD-3-Clause). No firmware is
distributed here.
