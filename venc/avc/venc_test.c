// SPDX-License-Identifier: GPL-2.0
/*
 * Drive /dev/amvenc_avc (vendor ioctl API) to encode synthetic NV12 frames.
 * Usage: venc_test [width height frames out.h264]
 */
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

enum { SEQUENCE = 1, IDR = 3, NON_IDR = 4, SEQUENCE_DONE = 7, PICTURE_DONE = 8,
       IDR_DONE = 9, NON_IDR_DONE = 10 };
#define LOCAL_BUFF	0
#define FMT_NV12	3
#define FLUSH_INPUT	0x1
#define FLUSH_OUTPUT	0x2
#define QP		28
#define ALIGN(x, a)	(((x) + (a) - 1) / (a) * (a))

static int fd;

static uint32_t wait_done(void)
{
	struct pollfd p = { .fd = fd, .events = POLLIN };
	uint32_t stage = 0;

	if (poll(&p, 1, 3000) <= 0)
		fprintf(stderr, "poll timeout\n");
	ioctl(fd, IOC_GET_STAGE, &stage);
	return stage;
}

static uint32_t out_size(void)
{
	uint32_t o[4] = { 0 };

	ioctl(fd, IOC_GET_OUTPUT_SIZE, o);
	return o[0];
}

int main(int argc, char **argv)
{
	uint32_t w = argc > 2 ? atoi(argv[1]) : 640, h = argc > 2 ? atoi(argv[2]) : 480;
	int frames = argc > 3 ? atoi(argv[3]) : 30;
	const char *path = argc > 4 ? argv[4] : "out.h264";
	uint32_t pitch = ALIGN(w, 32), vh = ALIGN(h, 16), fsize = pitch * vh * 3 / 2;
	uint32_t a[56] = { 0 }, bufsize = 0, dct_off, bs_off, stage, n, i;
	uint8_t *mem, *y, *uv;
	FILE *out;

	fd = open("/dev/amvenc_avc", O_RDWR);
	if (fd < 0)
		return perror("open"), 1;

	a[0] = 0;		/* UCODE_MODE_FULL -> ga_h264_enc_cabac */
	a[1] = vh / 16;		/* rows per slice = whole picture */
	a[2] = w;
	a[3] = h;
	if (ioctl(fd, IOC_CONFIG_INIT, a))
		return perror("CONFIG_INIT"), 1;
	dct_off = a[1];
	bs_off = a[3];
	ioctl(fd, IOC_GET_BUFFINFO, &bufsize);
	printf("buf %u KiB, input @0x%x, bitstream @0x%x (%u KiB)\n",
	       bufsize >> 10, dct_off, bs_off, a[4] >> 10);
	mem = mmap(NULL, bufsize, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (mem == MAP_FAILED)
		return perror("mmap"), 1;
	out = fopen(path, "wb");

	memset(a, 0, sizeof(a));
	a[0] = SEQUENCE;
	a[2] = QP;
	a[3] = FLUSH_OUTPUT;
	ioctl(fd, IOC_NEW_CMD, a);
	stage = wait_done();
	n = out_size();
	printf("headers: stage %u, sps %u pps %u bytes\n", stage, n >> 16, n & 0xffff);
	if (stage != PICTURE_DONE)
		return 1;
	fwrite(mem + bs_off, 1, (n >> 16) + (n & 0xffff), out);

	for (i = 0; i < (uint32_t)frames; i++) {
		uint32_t cmd = i ? NON_IDR : IDR, r, c;

		y = mem + dct_off;
		uv = y + pitch * vh;
		for (r = 0; r < vh; r++)	/* diagonal gradient moving 4 px/frame */
			for (c = 0; c < pitch; c++)
				y[r * pitch + c] = (uint8_t)((r + c + 4 * i) & 0xff);
		for (r = 0; r < vh / 2; r++)
			for (c = 0; c < pitch; c += 2) {
				uv[r * pitch + c] = (uint8_t)(64 + r);
				uv[r * pitch + c + 1] = (uint8_t)(192 - c / 4);
			}

		memset(a, 0, sizeof(a));
		a[0] = cmd;
		a[2] = LOCAL_BUFF;
		a[3] = FMT_NV12;
		a[5] = fsize;
		a[6] = QP;
		a[7] = FLUSH_INPUT | FLUSH_OUTPUT;
		a[8] = 3000;		/* timeout ms */
		a[13] = w;
		a[14] = h;
		ioctl(fd, IOC_NEW_CMD, a);
		stage = wait_done();
		n = out_size();
		printf("frame %2u %s: stage %u, %u bytes\n", i, i ? "P  " : "IDR", stage, n);
		/* the cabac ucode reports IDR_DONE for P frames too */
		if ((stage != IDR_DONE && stage != NON_IDR_DONE) || !n || n > bufsize - bs_off)
			break;
		fwrite(mem + bs_off, 1, n, out);
		ioctl(fd, IOC_SUBMIT, &cmd);
	}
	fclose(out);
	close(fd);
	return 0;
}
