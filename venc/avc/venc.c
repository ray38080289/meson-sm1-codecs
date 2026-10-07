// SPDX-License-Identifier: GPL-2.0
/*
 * venc - raw YUV on stdin -> H.264 Annex-B on stdout, using the S905X3
 * HCodec through /dev/amvenc_avc.
 *
 *   venc -s 1280x720 [-f nv12|nv21|i420] [-q 28 | -b 4000 -r 30] [-g 60] < in.yuv > out.h264
 */
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#define IOC(n)			_IOW('E', n, uint32_t)
#define IOC_NEW_CMD		IOC(0x02)
#define IOC_GET_STAGE		IOC(0x03)
#define IOC_GET_OUTPUT_SIZE	IOC(0x04)
#define IOC_CONFIG_INIT		IOC(0x05)
#define IOC_GET_BUFFINFO	IOC(0x08)
#define IOC_SUBMIT		IOC(0x09)

enum { SEQUENCE = 1, IDR = 3, NON_IDR = 4, PICTURE_DONE = 8, IDR_DONE = 9, NON_IDR_DONE = 10 };
enum { FMT_NV21 = 2, FMT_NV12 = 3, FMT_YUV420 = 4 };	/* amvenc_frame_fmt_e */
#define LOCAL_BUFF	0
#define FLUSH_INPUT	0x1
#define FLUSH_OUTPUT	0x2
#define FLUSH_CBR	0x80
#define CBR_TABLE_SIZE	0x800	/* 16 tables x 128 B, the ucode picks per-MB QP from here */
#define ALIGN(x, a)	(((x) + (a) - 1) / (a) * (a))
#define MAX_W		1920
#define MAX_H		1088

static int fd;

static void die(const char *msg)
{
	fprintf(stderr, "venc: %s%s%s\n", msg, errno ? ": " : "", errno ? strerror(errno) : "");
	exit(1);
}

static void usage(void)
{
	fprintf(stderr,
		"usage: venc -s WxH [-f nv12|nv21|i420] [-q QP | -b KBPS -r FPS] [-g GOP] [-n FRAMES]\n"
		"            [-S STRIDE -U CHROMA_OFFSET -B FRAME_BYTES]\n"
		"  raw frames on stdin, H.264 Annex-B on stdout\n"
		"  -S/-U/-B describe padded input (default: tightly packed), e.g. GStreamer\n"
		"           v4l2h264dec 1080p NV12 output is -S 1920 -U 2097152 -B 3145728\n"
		"  -q  fixed QP 10..51 (default 26)\n"
		"  -b  target bitrate in kbit/s (frame-level rate control), -r frame rate (default 30)\n"
		"  -g  IDR interval in frames (default 60, 0 = only the first frame)\n");
	exit(2);
}

/* run one command and return the encoder stage it finished with */
static uint32_t run(uint32_t *a)
{
	struct pollfd p = { .fd = fd, .events = POLLIN };
	uint32_t stage = 0;

	if (ioctl(fd, IOC_NEW_CMD, a))
		die("NEW_CMD");
	if (poll(&p, 1, 3000) <= 0)
		die("encoder timeout");
	ioctl(fd, IOC_GET_STAGE, &stage);
	return stage;
}

static uint32_t out_size(void)
{
	uint32_t o[4] = { 0 };

	ioctl(fd, IOC_GET_OUTPUT_SIZE, o);
	return o[0];
}

static int read_full(void *buf, size_t n)
{
	size_t got = 0;

	while (got < n) {
		ssize_t r = read(0, (char *)buf + got, n - got);

		if (r <= 0)
			return 0;
		got += r;
	}
	return 1;
}

static void write_full(const void *buf, size_t n)
{
	while (n) {
		ssize_t r = write(1, buf, n);

		if (r <= 0)
			die("write");
		buf = (const char *)buf + r;
		n -= r;
	}
}

/* copy a w x h plane (source stride sstride) into the encoder buffer, repeating the last row */
static void put_plane(uint8_t *dst, uint32_t pitch, uint32_t rows, const uint8_t *src,
		      uint32_t w, uint32_t h, uint32_t sstride)
{
	uint32_t r;

	for (r = 0; r < rows; r++)
		memcpy(dst + r * pitch, src + (size_t)(r < h ? r : h - 1) * sstride, w);
}

int main(int argc, char **argv)
{
	uint32_t w = 0, h = 0, fmt = FMT_NV12, qp = 26, kbps = 0, fps = 30, gop = 60;
	uint32_t sstride = 0, uv_off = 0, frame_bytes = 0;
	long long limit = -1, n;
	int opt;

	while ((opt = getopt(argc, argv, "s:f:q:b:r:g:n:S:U:B:")) != -1) {
		switch (opt) {
		case 's': if (sscanf(optarg, "%ux%u", &w, &h) != 2) usage(); break;
		case 'f':
			fmt = !strcmp(optarg, "nv12") ? FMT_NV12 : !strcmp(optarg, "nv21") ? FMT_NV21 :
			      !strcmp(optarg, "i420") ? FMT_YUV420 : 0;
			if (!fmt) usage();
			break;
		case 'q': qp = atoi(optarg); break;
		case 'b': kbps = atoi(optarg); break;
		case 'r': fps = atoi(optarg); break;
		case 'g': gop = atoi(optarg); break;
		case 'n': limit = atoll(optarg); break;
		case 'S': sstride = atoi(optarg); break;
		case 'U': uv_off = atoi(optarg); break;
		case 'B': frame_bytes = atoi(optarg); break;
		default: usage();
		}
	}
	if (!w || !h || w > MAX_W || h > MAX_H || (w & 1) || (h & 1) || qp < 10 || qp > 51 || !fps)
		usage();

	/* encoder.c canvas layout: NV12/NV21 pitch is 32-aligned, I420 Y pitch 64-aligned */
	uint32_t pitch = ALIGN(w, fmt == FMT_YUV420 ? 64 : 32), vh = ALIGN(h, 16), cw = w / 2, ch = h / 2;
	if (!sstride)
		sstride = w;
	if (!uv_off)
		uv_off = sstride * h;
	if (!frame_bytes)
		frame_bytes = uv_off + sstride * h / 2;
	if (sstride < w || uv_off < sstride * h || frame_bytes < uv_off + sstride * h / 2)
		usage();
	size_t in_size = frame_bytes;
	uint32_t cstride = fmt == FMT_YUV420 ? sstride / 2 : sstride;
	uint32_t a[56] = { 0 }, bufsize = 0, dct_off, bs_off, bs_size, cbr_off;
	uint8_t *mem, *in, hdr[256];
	uint32_t hdr_len, frame_target = kbps ? kbps * 1000 / 8 / fps : 0;
	double qf = qp;		/* rate control state */
	unsigned long long total = 0;

	fd = open("/dev/amvenc_avc", O_RDWR);
	if (fd < 0)
		die("open /dev/amvenc_avc");
	a[0] = 0;		/* UCODE_MODE_FULL */
	a[1] = vh / 16;		/* one slice */
	a[2] = w;
	a[3] = h;
	if (ioctl(fd, IOC_CONFIG_INIT, a))
		die("CONFIG_INIT");
	dct_off = a[1];
	bs_off = a[3];
	bs_size = a[4];
	cbr_off = a[9];
	ioctl(fd, IOC_GET_BUFFINFO, &bufsize);
	mem = mmap(NULL, bufsize, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	in = malloc(in_size);
	if (mem == MAP_FAILED || !in)
		die("mmap/malloc");

	/* SPS + PPS, kept to repeat before every IDR */
	memset(a, 0, sizeof(a));
	a[0] = SEQUENCE;
	a[2] = qp;
	a[3] = FLUSH_OUTPUT;
	if (run(a) != PICTURE_DONE)
		die("SPS/PPS generation failed");
	n = out_size();
	hdr_len = (n >> 16) + (n & 0xffff);
	if (hdr_len > sizeof(hdr))
		die("headers too large");
	memcpy(hdr, mem + bs_off, hdr_len);

	for (n = 0; n != limit && read_full(in, in_size); n++) {
		int idr = n == 0 || (gop && n % gop == 0);
		uint32_t cmd = idr ? IDR : NON_IDR, stage, sz, fq;
		uint8_t *y = mem + dct_off, *c = y + pitch * vh;

		put_plane(y, pitch, vh, in, w, h, sstride);
		if (fmt == FMT_YUV420) {
			/* I420: U then V at half pitch */
			put_plane(c, pitch / 2, vh / 2, in + uv_off, cw, ch, cstride);
			put_plane(y + pitch * vh * 5 / 4, pitch / 2, vh / 2,
				  in + uv_off + (size_t)cstride * ch, cw, ch, cstride);
		} else {
			put_plane(c, pitch, vh / 2, in + uv_off, w, ch, cstride);
		}

		fq = (uint32_t)(qf + 0.5);
		/*
		 * On GXTVBB+ the kernel always runs the ucode in CBR mode and it
		 * takes QP from this table, not from the request: fill every
		 * table with the frame QP.
		 */
		memset(mem + cbr_off, fq, CBR_TABLE_SIZE);
		memset(a, 0, sizeof(a));
		a[0] = cmd;
		a[2] = LOCAL_BUFF;
		a[3] = fmt;
		a[5] = pitch * vh * 3 / 2;
		a[6] = fq;
		a[7] = FLUSH_INPUT | FLUSH_OUTPUT | FLUSH_CBR;
		a[8] = 2000;
		a[13] = w;
		a[14] = h;
		stage = run(a);
		sz = out_size();
		/* the cabac ucode reports IDR_DONE for P frames too */
		if ((stage != IDR_DONE && stage != NON_IDR_DONE) || !sz || sz > bs_size) {
			fprintf(stderr, "venc: frame %lld failed (stage %u, %u bytes)\n", n, stage, sz);
			return 1;
		}
		if (idr)
			write_full(hdr, hdr_len);
		write_full(mem + bs_off, sz);
		ioctl(fd, IOC_SUBMIT, &cmd);
		total += sz;

		if (frame_target) {
			/*
			 * ponytail: proportional frame-level RC on a log scale (6 QP
			 * = 2x bits); no VBV model, IDRs allowed 4x budget. The ucode's
			 * per-MB CBR tables could do finer control if ever needed.
			 */
			double ratio = (double)sz / (idr ? 4.0 * frame_target : frame_target);
			double step = ratio > 1.0 ? 1.0 : -1.0;

			if (ratio > 1.15 || ratio < 0.87)
				qf += step * (ratio > 2.0 || ratio < 0.5 ? 2.0 : 0.5);
			qf = qf < 10 ? 10 : qf > 51 ? 51 : qf;
		}
	}
	fprintf(stderr, "venc: %lld frames, %llu bytes", n, total);
	if (n)
		fprintf(stderr, ", %.0f kbit/s at %u fps, last qp %.0f", total * 8.0 * fps / n / 1000, fps, qf);
	fprintf(stderr, "\n");
	return 0;
}
