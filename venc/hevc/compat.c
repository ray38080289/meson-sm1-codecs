// SPDX-License-Identifier: GPL-2.0
/*
 * Glue between the vendor WAVE420L HevcEnc driver and mainline: borrows the
 * mainline vdec node's DOS range, AO syscon and "dos" clock, maps the CBUS
 * reset register, creates the "HevcEnc" platform device (no DT node exists)
 * with its IRQ, and backs codec_mm with CMA.
 */
#include <linux/irq.h>
#include <linux/irqdomain.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_address.h>

#include "compat.h"

MODULE_IMPORT_NS("DMA_BUF");

#define WAVE420L_SPI		187	/* vendor DT: interrupts = <0 0xbb 1> */
#define CBUS_RESET_BASE		0xffd01000

struct henc_hw henc_hw;

void amports_switch_gate(const char *name, int enable)
{
	if (enable)
		WARN_ON(clk_prepare_enable(henc_hw.dos_clk));
	else
		clk_disable_unprepare(henc_hw.dos_clk);
}

/* codec_mm_free_for_dma() only gets the address back, so remember sizes */
static struct { unsigned long phys; size_t size; } henc_allocs[4];

unsigned long codec_mm_alloc_for_dma(const char *owner, int pages, int align, int flags)
{
	size_t size = (size_t)pages << PAGE_SHIFT;
	struct page *page;
	dma_addr_t dma;
	int i;

	for (i = 0; i < ARRAY_SIZE(henc_allocs); i++)
		if (!henc_allocs[i].phys)
			break;
	if (i == ARRAY_SIZE(henc_allocs))
		return 0;
	page = dma_alloc_pages(&henc_hw.pdev->dev, size, &dma, DMA_BIDIRECTIONAL, GFP_KERNEL);
	if (!page)
		return 0;
	henc_allocs[i].phys = dma;
	henc_allocs[i].size = size;
	return dma;
}

int codec_mm_free_for_dma(const char *owner, unsigned long phys)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(henc_allocs); i++) {
		if (henc_allocs[i].phys != phys)
			continue;
		dma_free_pages(&henc_hw.pdev->dev, henc_allocs[i].size,
			       phys_to_page(phys), phys, DMA_BIDIRECTIONAL);
		henc_allocs[i].phys = 0;
		return 0;
	}
	return -EINVAL;
}

static int henc_map_irq(void)
{
	struct device_node *gic;
	struct irq_fwspec spec = {
		.param_count = 3,
		.param = { 0 /* GIC_SPI */, WAVE420L_SPI, IRQ_TYPE_EDGE_RISING },
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

int henc_hw_init(void)
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
	henc_hw.ao = syscon_regmap_lookup_by_phandle(np, "amlogic,ao-sysctrl");
	hhi = of_find_compatible_node(NULL, NULL, "amlogic,meson-gx-hhi-sysctrl");
	henc_hw.hhi = hhi ? syscon_node_to_regmap(hhi) : ERR_PTR(-ENODEV);
	of_node_put(hhi);
	if (IS_ERR(henc_hw.ao) || IS_ERR(henc_hw.hhi)) {
		ret = -ENODEV;
		goto out;
	}
	henc_hw.dos_clk = of_clk_get_by_name(np, "dos");
	if (IS_ERR(henc_hw.dos_clk)) {
		ret = PTR_ERR(henc_hw.dos_clk);
		goto out;
	}
	/* shared with meson_vdec / the reset driver, so map without claiming */
	henc_hw.dos = ioremap(res.start, resource_size(&res));
	henc_hw.reset = ioremap(CBUS_RESET_BASE, 0x100);
	if (!henc_hw.dos || !henc_hw.reset) {
		ret = -ENOMEM;
		goto unmap;
	}
	irq = henc_map_irq();
	if (irq < 0) {
		ret = irq;
		goto unmap;
	}
	irq_res = (struct resource)DEFINE_RES_IRQ_NAMED(irq, "wave420l_irq");
	henc_hw.pdev = platform_device_register_simple("HevcEnc", -1, &irq_res, 1);
	if (IS_ERR(henc_hw.pdev)) {
		ret = PTR_ERR(henc_hw.pdev);
		goto unmap;
	}
	ret = dma_coerce_mask_and_coherent(&henc_hw.pdev->dev, DMA_BIT_MASK(32));
	if (ret)
		goto unreg;
	pr_info("HevcEnc: dos %pR irq %d\n", &res, irq);
	of_node_put(np);
	return 0;

unreg:
	platform_device_unregister(henc_hw.pdev);
unmap:
	if (henc_hw.reset)
		iounmap(henc_hw.reset);
	if (henc_hw.dos)
		iounmap(henc_hw.dos);
	clk_put(henc_hw.dos_clk);
out:
	of_node_put(np);
	return ret;
}

void henc_hw_exit(void)
{
	platform_device_unregister(henc_hw.pdev);
	iounmap(henc_hw.reset);
	iounmap(henc_hw.dos);
	clk_put(henc_hw.dos_clk);
}
