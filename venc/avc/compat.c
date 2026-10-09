// SPDX-License-Identifier: GPL-2.0
/*
 * Glue between the vendor HCodec encoder and mainline: borrows the mainline
 * vdec node's DOS range, AO syscon, "dos" clock and canvas provider, creates
 * the "amvenc_avc" platform device (no DT node exists), and backs codec_mm
 * with CMA.
 */
#include <linux/delay.h>
#include <linux/firmware.h>
#include <linux/irq.h>
#include <linux/irqdomain.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_platform.h>

#include "compat.h"

MODULE_IMPORT_NS("DMA_BUF");

#define VENC_NUM_CANVAS		12	/* encoder.c uses enc_canvas_offset + 0..11 */
#define HCODEC_MBOX2_SPI	45	/* "mailbox_2" in the vendor DT */

struct venc_hw venc_hw;

void canvas_read(u32 index, struct canvas_s *p)
{
	memset(p, 0, sizeof(*p));
	pr_err("amvenc: canvas input (index %u) is not supported on mainline\n", index);
}

void amports_switch_gate(const char *name, int enable)
{
	if (enable)
		WARN_ON(clk_prepare_enable(venc_hw.dos_clk));
	else
		clk_disable_unprepare(venc_hw.dos_clk);
}

int get_data_from_name(const char *name, u8 *buf)
{
	const struct firmware *fw;
	char path[64];
	int ret;

	snprintf(path, sizeof(path), "meson/venc/%s.bin", name);
	ret = request_firmware(&fw, path, &venc_hw.pdev->dev);
	if (ret)
		return ret;
	/* encoder.c's mc buffer is MC_SIZE = 32 KiB; the ucode is 24 KiB */
	ret = min_t(size_t, fw->size, 4096 * 8);
	memcpy(buf, fw->data, ret);
	release_firmware(fw);
	return ret;
}

/* codec_mm_free_for_dma() only gets the address back, so remember sizes */
static struct { unsigned long phys; size_t size; } venc_allocs[8];

unsigned long codec_mm_alloc_for_dma(const char *owner, int pages, int align, int flags)
{
	size_t size = (size_t)pages << PAGE_SHIFT;
	struct page *page;
	dma_addr_t dma;
	int i;

	for (i = 0; i < ARRAY_SIZE(venc_allocs); i++)
		if (!venc_allocs[i].phys)
			break;
	if (i == ARRAY_SIZE(venc_allocs))
		return 0;
	page = dma_alloc_pages(&venc_hw.pdev->dev, size, &dma, DMA_BIDIRECTIONAL, GFP_KERNEL);
	if (!page)
		return 0;
	venc_allocs[i].phys = dma;
	venc_allocs[i].size = size;
	return dma;
}

int codec_mm_free_for_dma(const char *owner, unsigned long phys)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(venc_allocs); i++) {
		if (venc_allocs[i].phys != phys)
			continue;
		dma_free_pages(&venc_hw.pdev->dev, venc_allocs[i].size,
			       phys_to_page(phys), phys, DMA_BIDIRECTIONAL);
		venc_allocs[i].phys = 0;
		return 0;
	}
	return -EINVAL;
}

static int venc_map_irq(void)
{
	struct device_node *gic;
	struct irq_fwspec spec = {
		.param_count = 3,
		.param = { 0 /* GIC_SPI */, HCODEC_MBOX2_SPI, IRQ_TYPE_EDGE_RISING },
	};
	int irq;

	gic = of_find_compatible_node(NULL, NULL, "arm,gic-400");
	if (!gic)
		return -ENODEV;
	spec.fwnode = of_fwnode_handle(gic);
	irq = irq_create_fwspec_mapping(&spec);
	of_node_put(gic);
	return irq ? irq : -EINVAL;
}

static int venc_alloc_canvases(struct device_node *vdec_np)
{
	struct platform_device *canvas_pdev;
	struct device_node *cnp;
	u8 idx, prev = 0;
	int i, ret;

	cnp = of_parse_phandle(vdec_np, "amlogic,canvas", 0);
	if (!cnp)
		return -ENODEV;
	canvas_pdev = of_find_device_by_node(cnp);
	of_node_put(cnp);
	if (!canvas_pdev)
		return -EPROBE_DEFER;
	venc_hw.canvas = platform_get_drvdata(canvas_pdev);
	put_device(&canvas_pdev->dev);
	if (!venc_hw.canvas)
		return -EPROBE_DEFER;

	/*
	 * ponytail: encoder.c addresses canvases as base + n and packs them into
	 * HW registers, so insist on a consecutive run from the first-free
	 * allocator rather than remapping every use.
	 */
	for (i = 0; i < VENC_NUM_CANVAS; i++) {
		ret = meson_canvas_alloc(venc_hw.canvas, &idx);
		if (ret || (i && idx != prev + 1)) {
			if (!ret)
				meson_canvas_free(venc_hw.canvas, idx);
			while (i--)
				meson_canvas_free(venc_hw.canvas, venc_hw.canvas_base + i);
			return ret ?: -EBUSY;
		}
		if (!i)
			venc_hw.canvas_base = idx;
		prev = idx;
	}
	return 0;
}

int venc_hw_init(void)
{
	struct device_node *np, *hhi;
	struct resource res, irq_res;
	int ret, irq;

	np = of_find_compatible_node(NULL, NULL, "amlogic,sm1-vdec");
	if (!np)
		return -ENODEV;
	ret = of_address_to_resource(np, 0, &res);	/* "dos" */
	if (ret)
		goto out;
	venc_hw.ao = syscon_regmap_lookup_by_phandle(np, "amlogic,ao-sysctrl");
	hhi = of_find_compatible_node(NULL, NULL, "amlogic,meson-gx-hhi-sysctrl");
	venc_hw.hhi = hhi ? syscon_node_to_regmap(hhi) : ERR_PTR(-ENODEV);
	of_node_put(hhi);
	if (IS_ERR(venc_hw.ao) || IS_ERR(venc_hw.hhi)) {
		ret = -ENODEV;
		goto out;
	}
	venc_hw.dos_clk = of_clk_get_by_name(np, "dos");
	if (IS_ERR(venc_hw.dos_clk)) {
		ret = PTR_ERR(venc_hw.dos_clk);
		goto out;
	}
	ret = venc_alloc_canvases(np);
	if (ret)
		goto put_clk;
	/* shared with meson_vdec, so map without claiming the region */
	venc_hw.dos = ioremap(res.start, resource_size(&res));
	if (!venc_hw.dos) {
		ret = -ENOMEM;
		goto free_canvas;
	}
	irq = venc_map_irq();
	if (irq < 0) {
		ret = irq;
		goto unmap;
	}
	irq_res = (struct resource)DEFINE_RES_IRQ(irq);
	venc_hw.pdev = platform_device_register_simple("amvenc_avc", -1, &irq_res, 1);
	if (IS_ERR(venc_hw.pdev)) {
		ret = PTR_ERR(venc_hw.pdev);
		goto unmap;
	}
	ret = dma_coerce_mask_and_coherent(&venc_hw.pdev->dev, DMA_BIT_MASK(32));
	if (ret)
		goto unreg;
	pr_info("amvenc: dos %pR irq %d canvas %u..%u\n", &res, irq,
		venc_hw.canvas_base, venc_hw.canvas_base + VENC_NUM_CANVAS - 1);
	of_node_put(np);
	return 0;

unreg:
	platform_device_unregister(venc_hw.pdev);
unmap:
	iounmap(venc_hw.dos);
free_canvas:
	for (irq = 0; irq < VENC_NUM_CANVAS; irq++)
		meson_canvas_free(venc_hw.canvas, venc_hw.canvas_base + irq);
put_clk:
	clk_put(venc_hw.dos_clk);
out:
	of_node_put(np);
	return ret;
}

void venc_hw_exit(void)
{
	int i;

	platform_device_unregister(venc_hw.pdev);
	iounmap(venc_hw.dos);
	for (i = 0; i < VENC_NUM_CANVAS; i++)
		meson_canvas_free(venc_hw.canvas, venc_hw.canvas_base + i);
	clk_put(venc_hw.dos_clk);
}
