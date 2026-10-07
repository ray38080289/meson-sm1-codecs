/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Minimal stand-ins for the Amlogic vendor-kernel APIs used by the WAVE420L
 * HevcEnc driver (vpu.c), implemented on mainline (6.18) for SM1 / S905X3.
 */
#ifndef __HENC_COMPAT_H__
#define __HENC_COMPAT_H__

#include <linux/clk.h>
#include <linux/dma-buf.h>
#include <linux/dma-mapping.h>
#include <linux/io.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/sizes.h>

struct henc_hw {
	void __iomem *dos;	/* DOS block, shared with meson_vdec */
	void __iomem *reset;	/* CBUS reset controller, RESET0 at +0x4 */
	struct regmap *ao, *hhi;
	struct clk *dos_clk;
	/*
	 * WAVE420L muxes source fclk_div3 (aclk) and fclk_div5 (bclk/cclk);
	 * mainline has no other user of fclk_div5, so clk_disable_unused()
	 * gates it and any access to the bclk/cclk register domain hangs.
	 */
	struct clk *fclk_div3, *fclk_div5;
	struct platform_device *pdev;
};
extern struct henc_hw henc_hw;

/* ---- registers: DOS uses word indices, HHI/AO use byte offsets ---- */
#define READ_VREG(r)		readl(henc_hw.dos + ((r) << 2))
#define WRITE_VREG(r, v)	writel((v), henc_hw.dos + ((r) << 2))

static inline u32 henc_regmap_read(struct regmap *map, u32 off)
{
	u32 v = 0;

	regmap_read(map, off, &v);
	return v;
}

#define READ_HHI_REG(r)		henc_regmap_read(henc_hw.hhi, r)
#define WRITE_HHI_REG(r, v)	regmap_write(henc_hw.hhi, r, v)
#define READ_AOREG(r)		henc_regmap_read(henc_hw.ao, r)
#define WRITE_AOREG(r, v)	regmap_write(henc_hw.ao, r, v)

#define HHI_WAVE420L_CLK_CNTL	0x268	/* g12a/sm1: (0x9a << 2) */
#define HHI_WAVE420L_CLK_CNTL2	0x26c
#define AO_RTI_GEN_PWR_SLEEP0	0xe8
#define AO_RTI_GEN_PWR_ISO0	0xec

#define DOS_SW_RESET4		0x3f37
#define DOS_MEM_PD_WAVE420L	0x3f39
#define DOS_WAVE420L_CNTL_STAT	0x3f3a

/* CBUS RESET0 (write 1 to pulse); only bit 21 = DOS_CAPB3 is ever used */
#define RESET0_REGISTER		0x401
#define READ_MPEG_REG(r)	readl(henc_hw.reset + (((r) - 0x400) << 2))
#define WRITE_MPEG_REG(r, v)	writel((v), henc_hw.reset + (((r) - 0x400) << 2))

/* ---- chip ids: only the ordering matters, the running chip is SM1 ---- */
#define MESON_CPU_MAJOR_ID_GXBB		0x1f
#define MESON_CPU_MAJOR_ID_GXL		0x21
#define MESON_CPU_MAJOR_ID_GXM		0x22
#define MESON_CPU_MAJOR_ID_TXL		0x23
#define MESON_CPU_MAJOR_ID_TXLX		0x24
#define MESON_CPU_MAJOR_ID_GXLX		0x26
#define MESON_CPU_MAJOR_ID_G12A		0x28
#define MESON_CPU_MAJOR_ID_G12B		0x29
#define MESON_CPU_MAJOR_ID_SM1		0x2b
#define MESON_CPU_MAJOR_ID_TL1		0x2e
#define MESON_CPU_MAJOR_ID_TM2		0x2f
#define MESON_CPU_MAJOR_ID_SC2		0x32
static inline int get_cpu_type(void) { return MESON_CPU_MAJOR_ID_SM1; }

/* G12A-only efuse check, never reached on SM1 */
#define EFUSE_LIC2		0
#define READ_EFUSE_REG(r)	0

/* ---- memory: CMA through the DMA API (dma address == phys on SM1) ---- */
unsigned long codec_mm_alloc_for_dma(const char *owner, int pages, int align, int flags);
int codec_mm_free_for_dma(const char *owner, unsigned long phys);
static inline unsigned long codec_mm_get_total_size(void) { return SZ_256M; }

/* ---- power / clocks ---- */
void amports_switch_gate(const char *name, int enable);
#define PDID_DOS_WAVE		0
#define PWR_ON			1
#define PWR_OFF			0
static inline int pwr_ctrl_psci_smc(int id, bool on) { return 0; }

/* mainline vm_flags are read-only; vpu.c ORs these in through vm_flags_set() */
#define VM_RESERVED		(VM_DONTEXPAND | VM_DONTDUMP)

int henc_hw_init(void);
void henc_hw_exit(void);

#endif
