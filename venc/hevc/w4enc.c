// SPDX-License-Identifier: GPL-2.0
/*
 * WAVE420L HEVC encoder host interface for in-kernel users.
 *
 * Replays the command sequence of the Chips&Media vpuapi (wave4.c and
 * common.c, LGPL-2.1 OR BSD-3-Clause) as traced from the working sample
 * w4_enc_test, for the one configuration we need: 8-bit 4:2:0 NV12/NV21
 * source, Main profile, IPPP (custom GOP of a single P picture), compressed
 * reconstruction buffers, no secondary AXI, no SEI/VUI.
 */
#include <linux/bitmap.h>
#include <linux/dma-mapping.h>
#include <linux/firmware.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/sizes.h>
#include <linux/slab.h>

#include "w4enc.h"

#define W4_FW			"meson/venc/monet.bin"
MODULE_FIRMWARE(W4_FW);

/* host interface registers (vpuapi common_regdefine.h, wave4_regdefine.h) */
#define W4_PO_CONF		0x000
#define W4_VINT_REASON_USR	0x030
#define W4_HOST_INT_REQ		0x038
#define W4_VINT_ENABLE		0x048
#define W4_RESET_REQ		0x050
#define W4_RESET_STATUS		0x054
#define W4_REMAP_CTRL		0x060
#define W4_REMAP_VADDR		0x064
#define W4_REMAP_PADDR		0x068
#define W4_REMAP_CORE_START	0x06c
#define W4_BUSY_STATUS		0x070
#define W4_COMMAND		0x100
#define W4_CORE_INDEX		0x104
#define W4_INST_INDEX		0x108
#define W4_OPTION		0x10c	/* SET_PARAM / SET_FRAMEBUF option */
#define W4_RET_SUCCESS		0x110
#define W4_RET_FAIL_REASON	0x114
#define W4_ADDR_CODE_BASE	0x118
#define W4_CODE_SIZE		0x11c
#define W4_CODE_PARAM		0x120
#define W4_HW_OPTION		0x124
#define W4_TIMEOUT_CNT		0x134
#define W4_BS_START_ADDR	0x120
#define W4_BS_SIZE		0x124
#define W4_BS_PARAM		0x128
#define W4_BS_RD_PTR		0x130
#define W4_BS_WR_PTR		0x134
#define W4_ADDR_WORK_BASE	0x138
#define W4_WORK_SIZE		0x13c
#define W4_WORK_PARAM		0x140
#define W4_ADDR_TEMP_BASE	0x144
#define W4_TEMP_SIZE		0x148
#define W4_TEMP_PARAM		0x14c
#define W4_ADDR_SEC_AXI		0x150
#define W4_SEC_AXI_SIZE		0x154
#define W4_USE_SEC_AXI		0x158
/* SET_FRAMEBUF */
#define W4_COMMON_PIC_INFO	0x120
#define W4_PIC_SIZE		0x124
#define W4_SET_FB_NUM		0x128
#define W4_FBC_STRIDE		0x154
#define W4_ADDR_SUB_SAMPLED	0x158
#define W4_SUB_SAMPLED_SIZE	0x15c
#define W4_ADDR_LUMA_BASE0	0x160	/* + 16 * i: luma, cb, fbc y, fbc c */
#define W4_ADDR_MV_COL0		0x1e0	/* + 4 * i */
/* SET_PARAM (OPT_COMMON) */
#define W4_SET_PARAM_ENABLE	0x15c
#define W4_SEQ_SRC_SIZE		0x160
#define W4_SEQ_PIC_PARAM	0x16c
#define W4_SEQ_GOP_PARAM	0x170
#define W4_SEQ_INTRA_PARAM	0x174
#define W4_SEQ_CONF_WIN_TB	0x178
#define W4_SEQ_CONF_WIN_LR	0x17c
#define W4_SEQ_FRAME_RATE	0x180
#define W4_SEQ_INDEP_SLICE	0x184
#define W4_SEQ_DEP_SLICE	0x188
#define W4_SEQ_INTRA_REFRESH	0x18c
#define W4_ENC_PARAM		0x190
#define W4_SEQ_LAYER_PERIOD	0x194
#define W4_RC_PARAM		0x198
#define W4_RC_MIN_MAX_QP	0x19c
#define W4_RC_BIT_RATIO_0_3	0x1a0
#define W4_RC_BIT_RATIO_4_7	0x1a4
#define W4_NR_PARAM		0x1a8
#define W4_NR_WEIGHT		0x1ac
#define W4_NUM_UNITS_IN_TICK	0x1b0
#define W4_TIME_SCALE		0x1b4
#define W4_TICKS_POC_DIFF_ONE	0x1b8
#define W4_RC_TRANS_RATE	0x1bc
#define W4_RC_TARGET_RATE	0x1c0
#define W4_ROT_PARAM		0x1c4
#define W4_RET_ENC_MIN_FB_NUM	0x1cc
/* SET_PARAM (OPT_CUSTOM_GOP) */
#define W4_CUSTOM_GOP_PARAM	0x160
#define W4_CUSTOM_GOP_PIC0	0x164	/* + 4 * i */
#define W4_CUSTOM_GOP_LAMBDA0	0x188	/* + 4 * i */
/* ENC_PIC */
#define W4_ENC_REPORT_BASE	0x15c
#define W4_ENC_REPORT_SIZE	0x160
#define W4_ENC_REPORT_PARAM	0x164
#define W4_ENC_CODE_OPTION	0x168
#define W4_ENC_PIC_PARAM	0x16c
#define W4_ENC_SRC_PIC_IDX	0x170
#define W4_ENC_SRC_ADDR_Y	0x174
#define W4_ENC_SRC_ADDR_U	0x178
#define W4_ENC_SRC_ADDR_V	0x17c
#define W4_ENC_SRC_STRIDE	0x180
#define W4_ENC_SRC_FORMAT	0x184
#define W4_ENC_PREFIX_SEI_ADDR	0x188
#define W4_ENC_PREFIX_SEI_INFO	0x18c
#define W4_ENC_SUFFIX_SEI_ADDR	0x190
#define W4_ENC_SUFFIX_SEI_INFO	0x194
#define W4_ENC_LONGTERM_PIC	0x198
#define W4_ENC_SUB_FRAME_SYNC	0x19c
#define W4_ENC_CTU_OPT_PARAM	0x1a0
#define W4_ENC_ROI_ADDR		0x1a4
#define W4_ENC_CTU_QP_MAP_ADDR	0x1ac
#define W4_ENC_TIMESTAMP_LOW	0x1b0
#define W4_ENC_TIMESTAMP_HIGH	0x1b4
#define W4_RET_ENC_PIC_IDX	0x1a8	/* recon index, < 0: no picture */
#define W4_RET_ENC_PIC_BYTE	0x1c8
#define W4_RET_ENC_PIC_TYPE	0x1e0	/* 0 = I */

#define CMD_INIT_VPU		0x0001
#define CMD_SET_PARAM		0x0002
#define CMD_FINI_SEQ		0x0004
#define CMD_ENC_PIC		0x0008
#define CMD_SET_FRAMEBUF	0x0010
#define CMD_CREATE_INSTANCE	0x4000
#define HEVC_ENC		1	/* codec mode */
#define INT_SET_PARAM		BIT(1)
#define INT_ENC_PIC		BIT(3)

#define CODE_SIZE		SZ_1M
#define TEMP_SIZE		SZ_1M
#define WORK_SIZE		SZ_128K
#define SETUP_BS_SIZE		SZ_64K	/* SET_PARAM wants a bitstream buffer */
#define MAX_INST		4
#define MAX_FB			8

struct w4_enc {
	struct w4_enc_cfg cfg;
	u32 inst;
	void *base;			/* work + setup bitstream */
	dma_addr_t base_dma;
	void *fbs;			/* recon frames + their side buffers */
	dma_addr_t fbs_dma;
	size_t fbs_size;
};

static DEFINE_MUTEX(w4_lock);		/* one command at a time */
static DECLARE_BITMAP(w4_insts, MAX_INST);
static unsigned int w4_users;
static void __iomem *w4_regs;
static void *w4_common;			/* firmware code + temp buffer */
static dma_addr_t w4_common_dma;

static void w4_wr(u32 reg, u32 val)
{
	writel(val, w4_regs + reg);
}

static u32 w4_rd(u32 reg)
{
	return readl(w4_regs + reg);
}

static int w4_reset(u32 blocks)
{
	u32 v;
	int ret;

	w4_wr(W4_RESET_REQ, blocks);
	ret = readl_poll_timeout(w4_regs + W4_RESET_STATUS, v, !v, 10, 500000);
	w4_wr(W4_RESET_REQ, 0);
	return ret;
}

static void w4_issue(u32 inst, u32 codec, u32 cmd)
{
	w4_wr(W4_BUSY_STATUS, 1);
	w4_wr(W4_RET_SUCCESS, 0);
	w4_wr(W4_CORE_INDEX, 0);
	w4_wr(W4_INST_INDEX, (inst & 0xffff) | codec << 16);
	w4_wr(W4_COMMAND, cmd);
	if (cmd != CMD_INIT_VPU)
		w4_wr(W4_HOST_INT_REQ, 1);
}

/* irq: the command completes with that interrupt, else when BUSY clears */
static int w4_cmd(u32 inst, u32 cmd, u32 irq)
{
	u32 v;
	int ret;

	henc_k_clear_irq();
	w4_issue(inst, HEVC_ENC, cmd);
	if (irq) {
		v = henc_k_wait_irq(3000);
		w4_wr(W4_VINT_REASON_USR, 0);
		ret = v & irq ? 0 : -ETIMEDOUT;
	} else {
		ret = readl_poll_timeout(w4_regs + W4_BUSY_STATUS, v, !v,
					 10, 1000000);
	}
	if (ret) {
		pr_err("HevcEnc: command %#x timed out\n", cmd);
		return ret;
	}
	/* the success flag can trail the interrupt */
	if (readl_poll_timeout(w4_regs + W4_RET_SUCCESS, v, v, 10, 10000)) {
		pr_err("HevcEnc: command %#x failed, reason %#x\n", cmd,
		       w4_rd(W4_RET_FAIL_REASON));
		return -EIO;
	}
	return 0;
}

static void w4_set_work(struct w4_enc *e)
{
	w4_wr(W4_ADDR_WORK_BASE, e->base_dma);
	w4_wr(W4_WORK_SIZE, WORK_SIZE);
	w4_wr(W4_WORK_PARAM, 0);
}

static void w4_set_bufs(struct w4_enc *e, dma_addr_t bs, u32 size)
{
	w4_wr(W4_BS_START_ADDR, bs);
	w4_wr(W4_BS_SIZE, size);
	w4_wr(W4_BS_RD_PTR, bs);
	w4_wr(W4_BS_WR_PTR, bs);
	w4_wr(W4_BS_PARAM, 0xf);	/* little endian stream */
	w4_wr(W4_ADDR_SEC_AXI, 0);
	w4_wr(W4_SEC_AXI_SIZE, 0);
	w4_wr(W4_USE_SEC_AXI, 0);
	w4_set_work(e);
	w4_wr(W4_ADDR_TEMP_BASE, w4_common_dma + CODE_SIZE);
	w4_wr(W4_TEMP_SIZE, TEMP_SIZE);
	w4_wr(W4_TEMP_PARAM, 0);
}

/* power up, load the firmware and boot the core (w4_lock held) */
static int w4_boot(void)
{
	struct device *dev = henc_k_dev();
	const struct firmware *fw;
	u32 v;
	int ret, i;

	ret = request_firmware(&fw, W4_FW, dev);
	if (ret)
		return ret;
	w4_common = dma_alloc_coherent(dev, CODE_SIZE + TEMP_SIZE,
				       &w4_common_dma, GFP_KERNEL);
	if (!w4_common || fw->size > CODE_SIZE) {
		ret = -ENOMEM;
		goto free;
	}
	memcpy(w4_common, fw->data, fw->size);

	ret = henc_k_get();
	if (ret)
		goto free;
	w4_regs = henc_k_regs();

	w4_wr(W4_BUSY_STATUS, 0);
	ret = w4_reset(0x0fffffff);
	w4_wr(W4_PO_CONF, 0);
	ret = ret ?: w4_reset(0x07ffffff);
	if (ret)
		goto put;
	for (i = W4_COMMAND; i < 0x200; i += 4)
		w4_wr(i, 0);
	w4_wr(W4_REMAP_CTRL, 0x80000800 | ((CODE_SIZE >> 12) & 0x1ff));
	w4_wr(W4_REMAP_VADDR, 0);
	w4_wr(W4_REMAP_PADDR, w4_common_dma);
	w4_wr(W4_ADDR_CODE_BASE, w4_common_dma);
	w4_wr(W4_CODE_SIZE, CODE_SIZE);
	w4_wr(W4_CODE_PARAM, 0);
	w4_wr(W4_TIMEOUT_CNT, 0xffffffff);
	w4_wr(W4_HW_OPTION, 0);
	w4_wr(W4_VINT_ENABLE, 0x860a);
	w4_issue(0, 0, CMD_INIT_VPU);
	w4_wr(W4_REMAP_CORE_START, 1);
	ret = readl_poll_timeout(w4_regs + W4_BUSY_STATUS, v, !v, 10, 1000000);
	if (ret || !w4_rd(W4_RET_SUCCESS)) {
		pr_err("HevcEnc: firmware boot failed: %d, reason %#x\n", ret,
		       w4_rd(W4_RET_FAIL_REASON));
		ret = -EIO;
		goto put;
	}
	release_firmware(fw);
	return 0;

put:
	henc_k_put();
free:
	if (w4_common)
		dma_free_coherent(dev, CODE_SIZE + TEMP_SIZE, w4_common,
				  w4_common_dma);
	w4_common = NULL;
	release_firmware(fw);
	return ret;
}

static void w4_shutdown(void)
{
	henc_k_put();
	dma_free_coherent(henc_k_dev(), CODE_SIZE + TEMP_SIZE, w4_common,
			  w4_common_dma);
	w4_common = NULL;
}

static int w4_set_param(struct w4_enc *e, u32 *min_fb)
{
	const struct w4_enc_cfg *c = &e->cfg;
	int i, ret;

	w4_set_bufs(e, e->base_dma + WORK_SIZE, SETUP_BS_SIZE);
	w4_wr(W4_OPTION, 0);			/* OPT_COMMON */
	w4_wr(W4_SET_PARAM_ENABLE, ~0);
	/* sizes must be multiples of 8: encode padded, crop in the SPS */
	w4_wr(W4_SEQ_SRC_SIZE, ALIGN(c->height, 8) << 16 | ALIGN(c->width, 8));
	w4_wr(W4_SEQ_PIC_PARAM, 1 | 8 << 14);	/* Main, level auto, 8 bit */
	/* custom GOP; the sample's temporal layer QPs/period (unused here) */
	w4_wr(W4_SEQ_GOP_PARAM, 30 << 9 | 33 << 15 | 36 << 21);
	w4_wr(W4_SEQ_LAYER_PERIOD, 60 << 16);
	/*
	 * RC on/off, CU-level RC, HVS QP with scale 2, initial buffer level 1,
	 * initial QP 63 (= firmware's choice), VBV 3000 ms
	 */
	w4_wr(W4_RC_PARAM, (c->bitrate ? 1 : 0) | 1 << 1 | 1 << 2 | 1 << 3 | 2 << 4 |
	      1 << 9 | 63 << 14 | 3000 << 20);
	/*
	 * IDR refresh (1 = CRA), intra QP, intra period 0: the caller forces
	 * IDRs, the 420L firmware ignores "VPS/SPS/PPS before every IDR"
	 */
	w4_wr(W4_SEQ_INTRA_PARAM, 2 | c->qp_i << 3);
	w4_wr(W4_SEQ_CONF_WIN_TB, (ALIGN(c->height, 8) - c->height) << 16);
	w4_wr(W4_SEQ_CONF_WIN_LR, (ALIGN(c->width, 8) - c->width) << 16);
	w4_wr(W4_SEQ_FRAME_RATE, c->fps);
	w4_wr(W4_SEQ_INDEP_SLICE, 0);
	w4_wr(W4_SEQ_DEP_SLICE, 0);
	w4_wr(W4_SEQ_INTRA_REFRESH, 0);
	/*
	 * the sample's tool set: all CU sizes, TMVP, 2 merge candidates,
	 * dynamic merge, deblocking across slices, transform skip, SAO,
	 * intra in inter slices, intra NxN
	 */
	w4_wr(W4_ENC_PARAM, 0x1e0174f0);
	w4_wr(W4_RC_MIN_MAX_QP, c->qp_min | c->qp_max << 6 | 10 << 12);
	w4_wr(W4_RC_BIT_RATIO_0_3, 0x01010101);
	w4_wr(W4_RC_BIT_RATIO_4_7, 0x01010101);
	w4_wr(W4_NR_PARAM, 0);
	w4_wr(W4_NR_WEIGHT, 0x08421ce7);
	w4_wr(W4_RC_TARGET_RATE, c->bitrate);
	w4_wr(W4_RC_TRANS_RATE, 0);
	w4_wr(W4_ROT_PARAM, 0);
	w4_wr(W4_NUM_UNITS_IN_TICK, 1000);
	w4_wr(W4_TIME_SCALE, c->fps * 1000);
	w4_wr(W4_TICKS_POC_DIFF_ONE, 0);
	ret = w4_cmd(e->inst, CMD_SET_PARAM, INT_SET_PARAM);
	if (ret)
		return ret;

	w4_wr(W4_OPTION, 1);			/* OPT_CUSTOM_GOP */
	w4_wr(W4_SET_PARAM_ENABLE, ~0);
	w4_wr(W4_CUSTOM_GOP_PARAM, 1);		/* GOP of one picture: */
	w4_wr(W4_CUSTOM_GOP_PIC0, 1 | 1 << 2 | c->qp_p << 6); /* P, POC+1 */
	for (i = 1; i < 8; i++)
		w4_wr(W4_CUSTOM_GOP_PIC0 + 4 * i, 0);
	for (i = 0; i < 8; i++)
		w4_wr(W4_CUSTOM_GOP_LAMBDA0 + 4 * i, 0);
	ret = w4_cmd(e->inst, CMD_SET_PARAM, INT_SET_PARAM);
	/* only known once the GOP structure is (0 after OPT_COMMON) */
	*min_fb = w4_rd(W4_RET_ENC_MIN_FB_NUM);
	return ret;
}

/* compressed recon frames plus MV, FBC offset tables and 1/4 subsampled */
static int w4_set_framebuf(struct w4_enc *e, u32 n)
{
	struct device *dev = henc_k_dev();
	u32 bw = ALIGN(e->cfg.width, 8), bh = ALIGN(e->cfg.height, 8);
	u32 stride = ALIGN(bw, 32);
	u32 luma = stride * bh, fb = luma * 3 / 2;
	u32 mv = ALIGN(DIV_ROUND_UP(bw, 64) * DIV_ROUND_UP(bh, 64) * 128, 16);
	u32 fbc_y = ALIGN(DIV_ROUND_UP(bh, 16) * DIV_ROUND_UP(bw, 256) * 128, 16);
	u32 fbc_c = ALIGN(DIV_ROUND_UP(bh, 16) *
			  DIV_ROUND_UP(bw / 2, 256) * 128, 16);
	u32 sub = ALIGN(bw / 4, 16) * ALIGN(bh / 4, 8);
	/* region offsets, each region 4 KiB aligned with a 4 KiB margin */
	size_t o_mv = ALIGN((size_t)fb * n, SZ_4K);
	size_t o_fy = o_mv + ALIGN(mv * n, SZ_4K) + SZ_4K;
	size_t o_fc = o_fy + ALIGN(fbc_y * n, SZ_4K) + SZ_4K;
	size_t o_sub = o_fc + ALIGN(fbc_c * n, SZ_4K) + SZ_4K;
	dma_addr_t d;
	u32 i;

	e->fbs_size = o_sub + ALIGN(sub * n, SZ_4K) + SZ_4K;
	e->fbs = dma_alloc_coherent(dev, e->fbs_size, &e->fbs_dma, GFP_KERNEL);
	if (!e->fbs)
		return -ENOMEM;
	d = e->fbs_dma;

	w4_wr(W4_ADDR_SUB_SAMPLED, d + o_sub);
	w4_wr(W4_SUB_SAMPLED_SIZE, sub);
	w4_wr(W4_PIC_SIZE, bw << 16 | bh);
	w4_wr(W4_FBC_STRIDE, ALIGN(ALIGN(bw, 16) * 4, 32) << 16 |
	      ALIGN(ALIGN(bw / 2, 16) * 4, 32));
	w4_wr(W4_COMMON_PIC_INFO, stride);
	w4_wr(W4_OPTION, 1 << 4 | 1 << 3);	/* first and last batch */
	w4_wr(W4_SET_FB_NUM, n - 1);
	for (i = 0; i < n; i++) {
		w4_wr(W4_ADDR_LUMA_BASE0 + 16 * i, d + fb * i);
		w4_wr(W4_ADDR_LUMA_BASE0 + 16 * i + 4, d + fb * i + luma);
		w4_wr(W4_ADDR_LUMA_BASE0 + 16 * i + 8, d + o_fy + fbc_y * i);
		w4_wr(W4_ADDR_LUMA_BASE0 + 16 * i + 12, d + o_fc + fbc_c * i);
		w4_wr(W4_ADDR_MV_COL0 + 4 * i, d + o_mv + mv * i);
	}
	w4_set_work(e);
	return w4_cmd(e->inst, CMD_SET_FRAMEBUF, 0);
}

static void w4_free(struct w4_enc *e)
{
	struct device *dev = henc_k_dev();

	if (e->fbs)
		dma_free_coherent(dev, e->fbs_size, e->fbs, e->fbs_dma);
	if (e->base)
		dma_free_coherent(dev, WORK_SIZE + SETUP_BS_SIZE, e->base,
				  e->base_dma);
	kfree(e);
}

struct w4_enc *w4_enc_open(const struct w4_enc_cfg *cfg)
{
	struct w4_enc *e;
	u32 min_fb;
	int ret;

	e = kzalloc(sizeof(*e), GFP_KERNEL);
	if (!e)
		return ERR_PTR(-ENOMEM);
	e->cfg = *cfg;

	mutex_lock(&w4_lock);
	e->inst = find_first_zero_bit(w4_insts, MAX_INST);
	if (e->inst >= MAX_INST) {
		ret = -EBUSY;
		goto unlock;
	}
	if (!w4_users) {
		ret = w4_boot();
		if (ret)
			goto unlock;
	}
	w4_users++;
	set_bit(e->inst, w4_insts);

	e->base = dma_alloc_coherent(henc_k_dev(), WORK_SIZE + SETUP_BS_SIZE,
				     &e->base_dma, GFP_KERNEL);
	if (!e->base) {
		ret = -ENOMEM;
		goto release;
	}
	w4_set_work(e);
	ret = w4_cmd(e->inst, CMD_CREATE_INSTANCE, 0);
	if (ret)
		goto release;
	ret = w4_set_param(e, &min_fb);
	if (!ret && (!min_fb || min_fb > MAX_FB))
		ret = -EINVAL;
	ret = ret ?: w4_set_framebuf(e, min_fb);
	if (ret) {
		w4_set_work(e);
		w4_cmd(e->inst, CMD_FINI_SEQ, 0);
		goto release;
	}
	mutex_unlock(&w4_lock);
	return e;

release:
	clear_bit(e->inst, w4_insts);
	if (!--w4_users)
		w4_shutdown();
unlock:
	mutex_unlock(&w4_lock);
	w4_free(e);
	return ERR_PTR(ret);
}

void w4_enc_close(struct w4_enc *e)
{
	mutex_lock(&w4_lock);
	/* end of source, as the sample does before FINI_SEQ */
	w4_set_bufs(e, e->base_dma + WORK_SIZE, SETUP_BS_SIZE);
	w4_wr(W4_ENC_CODE_OPTION, 3);
	w4_wr(W4_ENC_PIC_PARAM, 0);
	w4_wr(W4_ENC_SRC_PIC_IDX, ~0);
	w4_cmd(e->inst, CMD_ENC_PIC, INT_ENC_PIC);
	w4_set_work(e);
	w4_cmd(e->inst, CMD_FINI_SEQ, 0);
	clear_bit(e->inst, w4_insts);
	if (!--w4_users)
		w4_shutdown();
	mutex_unlock(&w4_lock);
	w4_free(e);
}

int w4_enc_frame(struct w4_enc *e, dma_addr_t y, dma_addr_t uv, u32 stride,
		 bool nv21, bool idr, dma_addr_t bs, u32 size, u32 *len,
		 bool *key)
{
	int ret;

	mutex_lock(&w4_lock);
	w4_set_bufs(e, bs, size);
	w4_wr(W4_ENC_REPORT_BASE, 0);
	w4_wr(W4_ENC_REPORT_SIZE, 0);
	w4_wr(W4_ENC_REPORT_PARAM, 0);
	/* IDR: VPS + SPS + PPS + VCL, forced picture type 3 = IDR */
	w4_wr(W4_ENC_CODE_OPTION, idr ? 1 << 1 | 1 << 2 | 1 << 3 | 1 << 4 : 1 << 1);
	w4_wr(W4_ENC_PIC_PARAM, idr ? 1 << 20 | 3 << 21 : 0);
	w4_wr(W4_ENC_SRC_PIC_IDX, 0);
	w4_wr(W4_ENC_SRC_ADDR_Y, y);
	w4_wr(W4_ENC_SRC_ADDR_U, uv);
	w4_wr(W4_ENC_SRC_ADDR_V, ~0);
	w4_wr(W4_ENC_SRC_STRIDE, stride << 16 | stride);
	/* CbCr interleaved (+ NV21), 8 bit left justified, little endian */
	w4_wr(W4_ENC_SRC_FORMAT, (2 | nv21) | 4 << 3 | 0xf << 6);
	w4_wr(W4_ENC_PREFIX_SEI_ADDR, 0);
	w4_wr(W4_ENC_PREFIX_SEI_INFO, 0);
	w4_wr(W4_ENC_SUFFIX_SEI_ADDR, 0);
	w4_wr(W4_ENC_SUFFIX_SEI_INFO, 0);
	w4_wr(W4_ENC_ROI_ADDR, 0);
	w4_wr(W4_ENC_CTU_QP_MAP_ADDR, 0);
	w4_wr(W4_ENC_CTU_OPT_PARAM, 0);
	w4_wr(W4_ENC_TIMESTAMP_LOW, 0);
	w4_wr(W4_ENC_TIMESTAMP_HIGH, 0);
	w4_wr(W4_ENC_LONGTERM_PIC, 0);
	w4_wr(W4_ENC_SUB_FRAME_SYNC, 0);
	ret = w4_cmd(e->inst, CMD_ENC_PIC, INT_ENC_PIC);
	if (!ret && (s32)w4_rd(W4_RET_ENC_PIC_IDX) < 0)
		ret = -EIO;		/* no delay with IPPP: always a picture */
	*len = ret ? 0 : w4_rd(W4_RET_ENC_PIC_BYTE);
	*key = !ret && w4_rd(W4_RET_ENC_PIC_TYPE) == 0;
	mutex_unlock(&w4_lock);
	return ret;
}
