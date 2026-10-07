/*
 * regprobe: step through the userspace register access the WAVE420L sample
 * does first, printing (and flushing) before every access, so a bus hang
 * shows exactly which access killed the box.
 *   ./regprobe [step]   step 1: mmap only, 2: +read 0x1044, 3: +read CUR_PC,
 *                       4: +write 0x100
 */
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#define W4_VCPU_CUR_PC	0x0004

int main(int argc, char **argv)
{
	int step = argc > 1 ? atoi(argv[1]) : 4;
	volatile uint32_t *r;
	int fd;

	setvbuf(stdout, NULL, _IONBF, 0);
	fd = open("/dev/HevcEnc", O_RDWR);
	if (fd < 0)
		return perror("open"), 1;
	printf("open ok\n");
	r = mmap(NULL, 0x4000, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (r == MAP_FAILED)
		return perror("mmap"), 1;
	printf("mmap ok %p\n", (void *)r);
	if (step >= 2) {
		printf("reading 0x1044...\n");
		printf("product 0x1044 = %#x\n", r[0x1044 / 4]);
	}
	if (step >= 3) {
		printf("reading CUR_PC...\n");
		printf("cur_pc = %#x\n", r[W4_VCPU_CUR_PC / 4]);
	}
	if (step >= 4) {
		printf("writing 0x100...\n");
		r[0x100 / 4] = 0;
		printf("write ok\n");
	}
	munmap((void *)r, 0x4000);
	close(fd);
	printf("done\n");
	return 0;
}
