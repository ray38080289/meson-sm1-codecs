/* SPDX-License-Identifier: GPL-2.0 */
#ifndef W4ENC_H
#define W4ENC_H

#include <linux/types.h>

struct device;

/* vpu.c: the core for in-kernel use, exclusive with /dev/HevcEnc */
int henc_k_get(void);
void henc_k_put(void);
void __iomem *henc_k_regs(void);
struct device *henc_k_dev(void);
void henc_k_clear_irq(void);
u32 henc_k_wait_irq(unsigned int ms);

/* w4enc.c: WAVE420L HEVC encoder instances */
struct w4_enc_cfg {
	u32 width, height;
	u32 fps;
	u32 bitrate;		/* bit/s, 0 = fixed QP */
	u32 qp_i, qp_p, qp_min, qp_max;
};

struct w4_enc;
struct w4_enc *w4_enc_open(const struct w4_enc_cfg *cfg);
void w4_enc_close(struct w4_enc *e);
int w4_enc_frame(struct w4_enc *e, dma_addr_t y, dma_addr_t uv, u32 stride,
		 bool nv21, bool idr, dma_addr_t bs, u32 size, u32 *len,
		 bool *key);

/* venc_v4l2.c */
int w4_v4l2_register(struct device *dev);
void w4_v4l2_unregister(void);

#endif
