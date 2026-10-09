#!/usr/bin/env python3
"""
Fetch the video firmware the S905X3/S905D3 (Amlogic SM1) codec drivers need
(meson-vdec, amvenc_avc, amvenc_hevc) from the public repositories that host
it, check it against the exact files the drivers were tested with, and
install it under /lib/firmware.

Nothing is redistributed by this project: each file comes from its upstream
at a pinned commit. Terms: Amlogic ucode, see LICENSE.amlogic_vdec in
linux-firmware (use with Amlogic chips, not as part of the kernel); WAVE420L
firmware, Chips&Media licence installed as meson/venc/LICENSE.monet.txt.

    sudo python3 get-firmware.py              # install into /lib/firmware
    python3 get-firmware.py --dest DIR        # or anywhere else
"""
import argparse
import hashlib
import os
import struct
import sys
import urllib.request

COREELEC = ('https://raw.githubusercontent.com/CoreELEC/media_modules-aml/'
            '201519678d35a9d7bf4dde1f3a328aa9d533589b/firmware/')
STARFIVE = ('https://raw.githubusercontent.com/starfive-tech/soft_3rdpart/'
            'b60da16be1b36453aa46599889c28310fd148fb8/wave420l/firmware/')
LINUX_FW = ('https://gitlab.com/kernel-firmware/linux-firmware/-/raw/'
            'fb505a691b2a37b9d1fc20617433dfd52fb6e27e/meson/vdec/')

# installed path, md5, source URL, ucode name inside a PACK (None: whole file),
# pad to size, only install when missing (distros ship linux-firmware)
FILES = [
    ('meson/vdec/sm1_mpeg12_multi.bin', 'b7f0239f9836eff40982c217ed51a149',
     COREELEC + 'video_ucode.bin', 'sm1_mpeg12_multi.bin', 16384, False),
    ('meson/vdec/sm1_hevc_mmu_multi.bin', '72c6e5b641c54362dc735a00fd1cad6e',
     COREELEC + 'video_ucode.bin', 'sm1_hevc_mmu.bin', 0, False),
    ('meson/venc/ga_h264_enc_cabac.bin', '54dd341ef2d06bdbce243837355e20fc',
     COREELEC + 'h264_enc.bin', 'ga_h264_enc_cabac.bin', 0, False),
    ('meson/venc/monet.bin', 'dbfaf48e103e1bf7f1101b49c5e3bbde',
     STARFIVE + 'monet.bin', None, 0, False),
    ('meson/venc/LICENSE.monet.txt', None,
     STARFIVE + 'LICENSE.txt', None, 0, False),
    ('meson/vdec/g12a_h264.bin', '36426a1997896b20167f924008347cc6',
     LINUX_FW + 'g12a_h264.bin', None, 0, True),
    ('meson/vdec/sm1_vp9_mmu.bin', 'c90ec8043cb0c95c247bd9ea4de6d4d8',
     LINUX_FW + 'sm1_vp9_mmu.bin', None, 0, True),
]


def pack_entries(buf):
    """name -> ucode of an Amlogic 'PACK' firmware (video_ucode.bin etc.)"""
    if buf[256:260] == b'KCAP':         # 256-byte signature in front
        buf = buf[256:]
    if buf[:4] != b'KCAP':
        raise ValueError('not an Amlogic PACK firmware')
    out, off = {}, 256                  # package header
    while off + 256 <= len(buf):
        info = buf[off:off + 256]       # package_info: name, format, cpu, len
        length = struct.unpack_from('<i', info, 96)[0]
        if not length:
            break
        fw = buf[off + 256:off + 256 + length]  # 512-byte head + data
        size = struct.unpack_from('<i', fw, 200)[0]
        out[info[:32].split(b'\0')[0].decode()] = fw[512:512 + size]
        off += 256 + length
    return out


def md5(data):
    return hashlib.md5(data).hexdigest()


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('--dest', default='/lib/firmware')
    dest = ap.parse_args().dest

    cache, failed = {}, 0
    for path, want, url, entry, pad, only_missing in FILES:
        target = os.path.join(dest, path)
        if os.path.exists(target):
            have = md5(open(target, 'rb').read())
            if only_missing or not want or have == want:
                print(f'kept       {path}')
                continue
        if url not in cache:
            print(f'download   {url}')
            with urllib.request.urlopen(url, timeout=60) as r:
                cache[url] = r.read()
        data = pack_entries(cache[url])[entry] if entry else cache[url]
        data = data.ljust(pad, b'\0')
        if want and md5(data) != want:
            print(f'MISMATCH   {path}: got {md5(data)}, want {want}',
                  file=sys.stderr)
            failed += 1
            continue
        os.makedirs(os.path.dirname(target), exist_ok=True)
        with open(target + '.tmp', 'wb') as f:
            f.write(data)
        os.chmod(target + '.tmp', 0o644)
        os.replace(target + '.tmp', target)
        print(f'installed  {path}')
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
