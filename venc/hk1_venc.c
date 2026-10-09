// SPDX-License-Identifier: GPL-2.0
/*
 * Milestone 1: bring up the S905X3 (SM1) HCodec H.264 encoder core on a
 * mainline kernel. Powers the core, loads the vendor microcode, runs the
 * AMRISC for a moment and reports whether its PC moves, then powers off.
 *
 * Sequence ported from Amlogic media_modules (encoder.c avc_poweron /
 * amvenc_loadmc / amvenc_start), register indices from hcodec_regs.h.
 *
 * ponytail: no DT node of its own yet; borrows the mainline vdec node's DOS
 * range, AO syscon and "dos" clock. Proper binding comes with the real driver.
 */
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/firmware.h>
#include <linux/io.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>

/* AO sysctrl */
#define AO_RTI_GEN_PWR_SLEEP0	0xe8
#define AO_RTI_GEN_PWR_ISO0	0xec
#define GEN_PWR_HCODEC_SM1	BIT(0)

/* HHI */
#define HHI_VDEC_CLK_CNTL	0x1e0
/* hcodec half [31:16]: gate bit24, src [27:25] (1 = fclk_div3, 666 MHz), div [22:16] */
#define HCODEC_CLK_MASK		GENMASK(31, 16)
#define HCODEC_CLK_667M		(BIT(24) | (1 << 25) | (0 << 16))

/* DOS registers are word indices; byte offset = idx * 4 */
#define R(idx)			((idx) * 4)
#define DOS_GCLK_EN0		R(0x3f01)
#define DOS_GEN_CTRL0		R(0x3f02)
#define DOS_SW_RESET1		R(0x3f07)
#define DOS_MEM_PD_HCODEC	R(0x3f32)
#define HCODEC_ASSIST_MMC_CTRL1	R(0x1002)
#define HCODEC_MPSR		R(0x1301)
#define HCODEC_MPC_P		R(0x1306)
#define HCODEC_MPC_E		R(0x1308)
#define HCODEC_CPSR		R(0x1321)
#define HCODEC_IMEM_DMA_CTRL	R(0x1340)
#define HCODEC_IMEM_DMA_ADR	R(0x1341)
#define HCODEC_IMEM_DMA_COUNT	R(0x1342)

#define FW_NAME			"meson/venc/ga_h264_enc_cabac.bin"
#define MC_SIZE			(4096 * 4)	/* IMEM DMA moves 0x1000 words */

static struct {
	struct platform_device *pdev;
	void __iomem *dos;
	struct regmap *ao, *hhi;
	struct clk *dos_clk;
} v;

static u32 rd(u32 off) { return readl_relaxed(v.dos + off); }
static void wr(u32 off, u32 val) { writel_relaxed(val, v.dos + off); }

static void hcodec_power(bool on)
{
	if (on) {
		regmap_update_bits(v.ao, AO_RTI_GEN_PWR_SLEEP0, GEN_PWR_HCODEC_SM1, 0);
		udelay(10);
		wr(DOS_SW_RESET1, 0xffffffff);
		wr(DOS_SW_RESET1, 0);
		regmap_update_bits(v.hhi, HHI_VDEC_CLK_CNTL, HCODEC_CLK_MASK, HCODEC_CLK_667M);
		wr(DOS_GCLK_EN0, rd(DOS_GCLK_EN0) | (0x7fff << 12));
		wr(DOS_MEM_PD_HCODEC, 0);
		regmap_update_bits(v.ao, AO_RTI_GEN_PWR_ISO0, GEN_PWR_HCODEC_SM1, 0);
		udelay(10);
		wr(DOS_GEN_CTRL0, rd(DOS_GEN_CTRL0) | 1);
		wr(DOS_GEN_CTRL0, rd(DOS_GEN_CTRL0) & ~1);
		mdelay(10);
	} else {
		regmap_update_bits(v.ao, AO_RTI_GEN_PWR_ISO0, GEN_PWR_HCODEC_SM1, GEN_PWR_HCODEC_SM1);
		wr(DOS_MEM_PD_HCODEC, 0xffffffff);
		wr(DOS_GCLK_EN0, rd(DOS_GCLK_EN0) & ~(0x7fff << 12));
		regmap_update_bits(v.hhi, HHI_VDEC_CLK_CNTL, BIT(24), 0);
		regmap_update_bits(v.ao, AO_RTI_GEN_PWR_SLEEP0, GEN_PWR_HCODEC_SM1, GEN_PWR_HCODEC_SM1);
	}
}

static int hcodec_load_mc(struct device *dev)
{
	const struct firmware *fw;
	dma_addr_t dma;
	void *buf;
	int ret, i;

	ret = request_firmware(&fw, FW_NAME, dev);
	if (ret)
		return ret;
	buf = dma_alloc_coherent(dev, MC_SIZE, &dma, GFP_KERNEL);
	if (!buf) {
		release_firmware(fw);
		return -ENOMEM;
	}
	memcpy(buf, fw->data, min_t(size_t, fw->size, MC_SIZE));
	dev_info(dev, "fw %zu bytes, first words %08x %08x\n",
		 fw->size, ((u32 *)buf)[0], ((u32 *)buf)[1]);
	release_firmware(fw);

	wr(HCODEC_MPSR, 0);
	wr(HCODEC_CPSR, 0);
	wr(HCODEC_IMEM_DMA_ADR, dma);
	wr(HCODEC_IMEM_DMA_COUNT, MC_SIZE / 4);
	wr(HCODEC_IMEM_DMA_CTRL, 0x8000 | (0xf << 16));
	ret = -ETIMEDOUT;
	for (i = 0; i < 1000; i++) {
		if (!(rd(HCODEC_IMEM_DMA_CTRL) & 0x8000)) {
			ret = 0;
			break;
		}
		usleep_range(100, 200);
	}
	dma_free_coherent(dev, MC_SIZE, buf, dma);
	return ret;
}

static int hk1_venc_test(void)
{
	struct device *dev = &v.pdev->dev;
	u32 pc[4];
	int ret, i;

	ret = clk_prepare_enable(v.dos_clk);
	if (ret)
		return ret;
	hcodec_power(true);
	dev_info(dev, "powered: SLEEP0/ISO0 bit0 cleared, MPSR=%08x\n", rd(HCODEC_MPSR));

	ret = hcodec_load_mc(dev);
	dev_info(dev, "microcode load: %d\n", ret);
	if (!ret) {
		wr(DOS_SW_RESET1, BIT(12) | BIT(11));
		wr(DOS_SW_RESET1, 0);
		wr(HCODEC_ASSIST_MMC_CTRL1, 0x32);
		wr(HCODEC_MPSR, 1);
		for (i = 0; i < 4; i++) {
			udelay(50);
			pc[i] = rd(HCODEC_MPC_E);
		}
		dev_info(dev, "running: MPSR=%08x MPC_E %04x %04x %04x %04x MPC_P %04x\n",
			 rd(HCODEC_MPSR), pc[0], pc[1], pc[2], pc[3], rd(HCODEC_MPC_P));
		wr(HCODEC_MPSR, 0);
		wr(HCODEC_CPSR, 0);
	}

	hcodec_power(false);
	clk_disable_unprepare(v.dos_clk);
	return ret;
}

static int __init hk1_venc_init(void)
{
	struct device_node *np, *hhi;
	struct resource res;
	int ret;

	np = of_find_compatible_node(NULL, NULL, "amlogic,sm1-vdec");
	if (!np)
		return -ENODEV;
	ret = of_address_to_resource(np, 0, &res);	/* "dos" */
	if (ret)
		goto put_np;
	v.ao = syscon_regmap_lookup_by_phandle(np, "amlogic,ao-sysctrl");
	hhi = of_find_compatible_node(NULL, NULL, "amlogic,meson-gx-hhi-sysctrl");
	v.hhi = hhi ? syscon_node_to_regmap(hhi) : ERR_PTR(-ENODEV);
	of_node_put(hhi);
	v.dos_clk = of_clk_get_by_name(np, "dos");
	if (IS_ERR(v.ao) || IS_ERR(v.hhi) || IS_ERR(v.dos_clk)) {
		pr_err("hk1_venc: missing ao/hhi/dos clk\n");
		ret = -ENODEV;
		goto put_clk;
	}
	/* shared with meson_vdec, so map without claiming the region */
	v.dos = ioremap(res.start, resource_size(&res));
	if (!v.dos) {
		ret = -ENOMEM;
		goto put_clk;
	}
	v.pdev = platform_device_register_simple("hk1_venc", -1, NULL, 0);
	if (IS_ERR(v.pdev)) {
		ret = PTR_ERR(v.pdev);
		goto unmap;
	}
	ret = dma_coerce_mask_and_coherent(&v.pdev->dev, DMA_BIT_MASK(32));
	if (!ret)
		ret = hk1_venc_test();
	if (!ret)
		goto put_np;

	platform_device_unregister(v.pdev);
unmap:
	iounmap(v.dos);
put_clk:
	if (!IS_ERR_OR_NULL(v.dos_clk))
		clk_put(v.dos_clk);
put_np:
	of_node_put(np);
	return ret;
}

static void __exit hk1_venc_exit(void)
{
	platform_device_unregister(v.pdev);
	iounmap(v.dos);
	clk_put(v.dos_clk);
}

module_init(hk1_venc_init);
module_exit(hk1_venc_exit);
MODULE_DESCRIPTION("S905X3 HCodec H.264 encoder bring-up test");
MODULE_LICENSE("GPL");
MODULE_FIRMWARE(FW_NAME);
