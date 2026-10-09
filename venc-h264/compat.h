/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Minimal stand-ins for the Amlogic vendor-kernel APIs used by encoder.c,
 * implemented on mainline (6.18) for SM1 / S905X3 only.
 */
#ifndef __VENC_COMPAT_H__
#define __VENC_COMPAT_H__

#include <linux/clk.h>
#include <linux/vmalloc.h>
#include <linux/dma-mapping.h>
#include <linux/io.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/reset.h>
#include <linux/soc/amlogic/meson-canvas.h>

#include "hcodec_regs.h"

struct venc_hw {
	void __iomem *dos;
	struct regmap *ao, *hhi;
	struct clk *dos_clk;
	struct meson_canvas *canvas;
	u8 canvas_base;
	struct platform_device *pdev;
};
extern struct venc_hw venc_hw;

int venc_hw_init(void);
void venc_hw_exit(void);

/* ---- registers: DOS/HCODEC use word indices, HHI/AO use byte offsets ---- */
#define READ_VREG(r)		readl(venc_hw.dos + ((r) << 2))
#define WRITE_VREG(r, v)	writel((v), venc_hw.dos + ((r) << 2))
#define READ_HREG(r)		READ_VREG(r)
#define WRITE_HREG(r, v)	WRITE_VREG(r, v)
#define WRITE_VREG_BITS(r, val, start, len) \
	WRITE_VREG(r, (READ_VREG(r) & ~(((1U << (len)) - 1) << (start))) | \
		      (((u32)(val) & ((1U << (len)) - 1)) << (start)))

static inline u32 venc_regmap_read(struct regmap *map, u32 off)
{
	u32 v = 0;

	regmap_read(map, off, &v);
	return v;
}

#define READ_HHI_REG(r)		venc_regmap_read(venc_hw.hhi, r)
#define WRITE_HHI_REG(r, v)	regmap_write(venc_hw.hhi, r, v)
#define WRITE_HHI_REG_BITS(r, val, start, len) \
	regmap_update_bits(venc_hw.hhi, r, ((1U << (len)) - 1) << (start), \
			   ((u32)(val) & ((1U << (len)) - 1)) << (start))
#define HHI_VDEC_CLK_CNTL	0x1e0
#define AO_RTI_GEN_PWR_SLEEP0	0xe8
#define AO_RTI_GEN_PWR_ISO0	0xec
/*
 * ponytail: vendor clears bits [4:3] of this on SM1; mainline never touches
 * it and M1 powered HCodec fine without, so accesses to it are dropped.
 * Revisit if the encoder stalls.
 */
#define AO_RTI_PWR_CNTL_REG0	0xffff

static inline u32 venc_ao_read(u32 r)
{
	return r == AO_RTI_PWR_CNTL_REG0 ? 0 : venc_regmap_read(venc_hw.ao, r);
}

static inline void venc_ao_write(u32 r, u32 v)
{
	if (r != AO_RTI_PWR_CNTL_REG0)
		regmap_write(venc_hw.ao, r, v);
}

#define READ_AOREG(r)		venc_ao_read(r)
#define WRITE_AOREG(r, v)	venc_ao_write(r, v)

#define DOS_GCLK_EN0		0x3f01
#define DOS_GEN_CTRL0		0x3f02
#define DOS_SW_RESET1		0x3f07
#define DOS_MEM_PD_HCODEC	0x3f32
/* only used on chips with the in-DOS canvas LUT, never on SM1 */
#ifndef HCODEC_MDEC_CAV_CFG0
#define HCODEC_MDEC_CAV_CFG0	0
#define HCODEC_MDEC_CAV_LUT_DATAL 0
#define HCODEC_MDEC_CAV_LUT_DATAH 0
#define HCODEC_MDEC_CAV_LUT_ADDR 0
#endif

/* ---- chip ids: only the ordering matters, the running chip is SM1 ---- */
#define MESON_CPU_MAJOR_ID_MG9TV	0x1c
#define MESON_CPU_MAJOR_ID_GXBB		0x1f
#define MESON_CPU_MAJOR_ID_GXTVBB	0x20
#define MESON_CPU_MAJOR_ID_GXL		0x21
#define MESON_CPU_MAJOR_ID_TXL		0x23
#define MESON_CPU_MAJOR_ID_G12A		0x28
#define MESON_CPU_MAJOR_ID_SM1		0x2b
#define MESON_CPU_MAJOR_ID_TL1		0x2e
#define MESON_CPU_MAJOR_ID_TM2		0x2f
#define MESON_CPU_MAJOR_ID_SC2		0x32
#define MESON_CPU_MAJOR_ID_T7		0x36
#define MESON_CPU_MAJOR_ID_T3		0x38
#define MESON_CPU_MAJOR_ID_T5M		0x41
#define MESON_CPU_MAJOR_ID_T3X		0x42
#define MESON_CPU_MAJOR_ID_TXHD2	0x43
#define MESON_CPU_MAJOR_ID_S7D		0x48
#define AM_MESON_CPU_MAJOR_ID_SM1	MESON_CPU_MAJOR_ID_SM1
#define AM_MESON_CPU_MAJOR_ID_TL1	MESON_CPU_MAJOR_ID_TL1
#define AM_MESON_CPU_MAJOR_ID_SC2	MESON_CPU_MAJOR_ID_SC2
#define AM_MESON_CPU_MAJOR_ID_T7	MESON_CPU_MAJOR_ID_T7
#define AM_MESON_CPU_MAJOR_ID_T3	MESON_CPU_MAJOR_ID_T3
#define AM_MESON_CPU_MAJOR_ID_T5M	MESON_CPU_MAJOR_ID_T5M
#define AM_MESON_CPU_MAJOR_ID_T3X	MESON_CPU_MAJOR_ID_T3X
#define AM_MESON_CPU_MAJOR_ID_TXHD2	MESON_CPU_MAJOR_ID_TXHD2
#define AM_MESON_CPU_MAJOR_ID_S7D	MESON_CPU_MAJOR_ID_S7D
static inline int get_cpu_type(void) { return MESON_CPU_MAJOR_ID_SM1; }
static inline int get_cpu_major_id(void) { return MESON_CPU_MAJOR_ID_SM1; }

/* ---- canvas ---- */
#define CANVAS_ADDR_NOWRAP	MESON_CANVAS_WRAP_NONE
#define CANVAS_BLKMODE_LINEAR	MESON_CANVAS_BLKMODE_LINEAR
struct canvas_s {
	ulong addr;
	u32 width, height;
	u8 wrap, blkmode, endian;
};
static inline bool is_support_vdec_canvas(void) { return false; }
static inline void canvas_config(u32 index, ulong addr, u32 width, u32 height,
				 u32 wrap, u32 blkmode)
{
	meson_canvas_config(venc_hw.canvas, index, addr, width, height,
			    wrap, blkmode, 0);
}
/* Mainline cannot read back canvases owned by others: CANVAS_BUFF input is unsupported. */
void canvas_read(u32 index, struct canvas_s *p);

/* ---- memory (CMA through the DMA API; dma address == phys on SM1) ---- */
#define CODEC_MM_FLAGS_CPU	0
unsigned long codec_mm_alloc_for_dma(const char *owner, int pages, int align, int flags);
int codec_mm_free_for_dma(const char *owner, unsigned long phys);
static inline unsigned long codec_mm_get_total_size(void) { return SZ_256M; }
static inline void *codec_mm_vmap(ulong phys, u32 size) { return phys_to_virt(phys); }
static inline void codec_mm_unmap_phyaddr(u8 *vaddr) { }
static inline void codec_mm_dma_flush(void *vaddr, int size, enum dma_data_direction dir)
{
	dma_sync_single_for_device(&venc_hw.pdev->dev, virt_to_phys(vaddr), size, dir);
}

/* ---- power / clocks ---- */
enum vdec_type_e { VDEC_1 = 0, VDEC_HCODEC, VDEC_2, VDEC_HEVC, VDEC_MAX };
static inline void vdec_poweron(enum vdec_type_e core) { }
static inline void vdec_poweroff(enum vdec_type_e core) { }
static inline bool vdec_on(enum vdec_type_e core) { return false; }
static inline u32 vdec_get_debug(void) { return 0; }
void amports_switch_gate(const char *name, int enable);
#define PDID_T3_DOS_HCODEC	0
#define PWR_ON			1
#define PWR_OFF			0
static inline int pwr_ctrl_psci_smc(int id, bool on) { return 0; }
static inline int pwr_ctrl_status_psci_smc(int id) { return 0; }

/* ---- firmware ---- */
#define VIDEO_ENC_H264		0
#define VFORMAT_H264_ENC	0
static inline bool fw_tee_enabled(void) { return false; }
static inline bool tee_enabled(void) { return false; }
static inline int get_firmware_data(int type, char *buf) { return -ENOENT; }
static inline int amvdec_loadmc_ex(int fmt, const char *name, char *def) { return -ENOENT; }
int get_data_from_name(const char *name, u8 *buf);

/* ---- misc ---- */
#define KV_CLASS_CONST		const
#define KV_CLASS_ATTR_CONST	const
#define DEBUG_AMVENC_264	"amvenc_264"
typedef void (*enc_set_debug_level_func)(const char *module, int level);
static inline int enc_register_set_debug_level_func(const char *m, enc_set_debug_level_func f)
{
	return 0;
}

#endif
