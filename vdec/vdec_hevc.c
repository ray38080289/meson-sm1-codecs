// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (C) 2018 Maxime Jourdan <maxi.jourdan@wanadoo.fr>
 *
 * VDEC_HEVC is a video decoding block that allows decoding of
 * HEVC, VP9
 */

#include <linux/firmware.h>
#include <linux/clk.h>
#include <linux/iopoll.h>

#include "vdec_1.h"
#include "vdec_helpers.h"
#include "vdec_hevc.h"
#include "hevc_regs.h"
#include "dos_regs.h"

/* AO Registers */
#define AO_RTI_GEN_PWR_SLEEP0	0xe8
#define AO_RTI_GEN_PWR_ISO0	0xec
	#define GEN_PWR_VDEC_HEVC (BIT(7) | BIT(6))
	#define GEN_PWR_VDEC_HEVC_SM1 (BIT(2))

#define MC_SIZE	(4096 * 4)

/* HEVC reset domains used by the vendor frame-based decoder. */
#define VDEC_HEVC_RESET_MASK	(BIT(3) | BIT(4) | BIT(8) | BIT(10) | \
				 BIT(11) | BIT(12) | BIT(13) | BIT(14) | \
				 BIT(15) | BIT(17) | BIT(18) | BIT(19) | \
				 BIT(24) | BIT(26))

static int vdec_hevc_load_firmware(struct amvdec_session *sess,
				   const char *fwname, bool load_hardware)
{
	struct amvdec_core *core = sess->core;
	struct device *dev = core->dev_dec;
	struct amvdec_codec_ops *codec_ops = sess->fmt_out->codec_ops;
	const struct firmware *fw;
	dma_addr_t mc_addr_map;
	void *mc_addr;
	bool prepare_context;
	int ret;
	u32 val;

	prepare_context = codec_ops->prepare_firmware && !sess->priv;
	if (!load_hardware && !prepare_context)
		return 0;

	ret = request_firmware(&fw, fwname, dev);
	if (ret < 0)  {
		dev_err(dev, "Unable to request firmware %s\n", fwname);
		return ret;
	}

	if (fw->size < MC_SIZE) {
		dev_err(dev, "Firmware size %zu is too small. Expected %u.\n",
			fw->size, MC_SIZE);
		ret = -EINVAL;
		goto release_firmware;
	}

	if (load_hardware) {
		mc_addr = dma_alloc_coherent(core->dev, MC_SIZE, &mc_addr_map,
					     GFP_KERNEL);
		if (!mc_addr) {
			ret = -ENOMEM;
			goto release_firmware;
		}

		memcpy(mc_addr, fw->data, MC_SIZE);

		amvdec_write_dos(core, HEVC_MPSR, 0);
		amvdec_write_dos(core, HEVC_CPSR, 0);
		amvdec_read_dos(core, HEVC_MPSR);
		amvdec_read_dos(core, HEVC_MPSR);

		amvdec_write_dos(core, HEVC_IMEM_DMA_ADR, mc_addr_map);
		amvdec_write_dos(core, HEVC_IMEM_DMA_COUNT, MC_SIZE / 4);
		amvdec_write_dos(core, HEVC_IMEM_DMA_CTRL,
				 (0x8000 | (7 << 16)));

		ret = readl_poll_timeout(core->dos_base + HEVC_IMEM_DMA_CTRL,
					 val, !(val & BIT(15)), 10,
					 USEC_PER_SEC);
		if (ret)
			dev_err(dev, "Firmware load fail (DMA hang?)\n");
		dma_free_coherent(core->dev, MC_SIZE, mc_addr, mc_addr_map);
		if (ret)
			goto release_firmware;
	}

	if (prepare_context)
		ret = codec_ops->prepare_firmware(sess, fw->data, fw->size);

release_firmware:
	release_firmware(fw);
	return ret;
}

static void vdec_hevc_stbuf_init(struct amvdec_session *sess)
{
	struct amvdec_core *core = sess->core;

	amvdec_write_dos(core, HEVC_STREAM_CONTROL,
			 amvdec_read_dos(core, HEVC_STREAM_CONTROL) & ~1);
	amvdec_write_dos(core, HEVC_STREAM_START_ADDR, sess->vififo_paddr);
	amvdec_write_dos(core, HEVC_STREAM_END_ADDR,
			 sess->vififo_paddr + sess->vififo_size);
	amvdec_write_dos(core, HEVC_STREAM_RD_PTR, sess->vififo_paddr);
	amvdec_write_dos(core, HEVC_STREAM_WR_PTR, sess->vififo_paddr);
}

/* VDEC_HEVC specific ESPARSER configuration */
static void vdec_hevc_conf_esparser(struct amvdec_session *sess)
{
	struct amvdec_core *core = sess->core;

	/* set vififo_vbuf_rp_sel=>vdec_hevc */
	amvdec_write_dos(core, DOS_GEN_CTRL0, 3 << 1);
	amvdec_write_dos(core, HEVC_STREAM_CONTROL,
			 amvdec_read_dos(core, HEVC_STREAM_CONTROL) | BIT(3));
	amvdec_write_dos(core, HEVC_STREAM_CONTROL,
			 amvdec_read_dos(core, HEVC_STREAM_CONTROL) | 1);
	amvdec_write_dos(core, HEVC_STREAM_FIFO_CTL,
			 amvdec_read_dos(core, HEVC_STREAM_FIFO_CTL) | BIT(29));
}

static u32 vdec_hevc_vififo_level(struct amvdec_session *sess)
{
	return readl_relaxed(sess->core->dos_base + HEVC_STREAM_LEVEL);
}

static void vdec_hevc_wait_dma_idle(struct amvdec_core *core)
{
	struct device *dev = core->dev_dec;
	u32 val;
	int ret;

	ret = readl_poll_timeout(core->dos_base + HEVC_IMEM_DMA_CTRL, val,
				 !(val & BIT(15)), 10, 100000);
	if (ret)
		dev_warn(dev, "IMEM DMA did not become idle\n");

	ret = readl_poll_timeout(core->dos_base + HEVC_LMEM_DMA_CTRL, val,
				 !(val & BIT(15)), 10, 100000);
	if (ret)
		dev_warn(dev, "LMEM DMA did not become idle\n");

	ret = readl_poll_timeout(core->dos_base + HEVC_WRRSP_LMEM, val,
				 !(val & GENMASK(11, 0)), 10, 50000);
	if (ret)
		dev_warn(dev, "LMEM write response did not become idle\n");
}

static void vdec_hevc_wait_search_idle(struct amvdec_core *core)
{
	struct device *dev = core->dev_dec;
	u32 val;
	int ret;

	amvdec_write_dos(core, HEVC_SHIFT_STATUS, 0);
	ret = readl_poll_timeout(core->dos_base + HEVC_STREAM_CONTROL, val,
				 !(val & BIT(1)), 20000, 2000000);
	if (ret)
		dev_warn(dev, "HEVC stream search did not become idle\n");
}

void vdec_hevc_quiesce(struct amvdec_session *sess)
{
	struct amvdec_core *core = sess->core;

	/* Match vendor amhevc_stop(): stop the firmware before input release. */
	amvdec_write_dos(core, HEVC_MPSR, 0);
	amvdec_write_dos(core, HEVC_CPSR, 0);
	if (!READ_ONCE(sess->hardware_stalled))
		vdec_hevc_wait_dma_idle(core);

	/* Vendor uses three reset-register reads as the stop completion barrier. */
	amvdec_read_dos(core, DOS_SW_RESET3);
	amvdec_read_dos(core, DOS_SW_RESET3);
	amvdec_read_dos(core, DOS_SW_RESET3);
}

static void vdec_hevc_suspend(struct amvdec_session *sess)
{
	struct amvdec_core *core = sess->core;
	struct amvdec_codec_ops *codec_ops = sess->fmt_out->codec_ops;

	/* Disable interrupt */
	if (codec_ops->irq == AMVDEC_IRQ_MBOX0)
		amvdec_write_dos(core, HEVC_ASSIST_MBOX0_MASK, 0);
	else
		amvdec_write_dos(core, HEVC_ASSIST_MBOX1_MASK, 0);

	/*
	 * A context-switching codec may poll live HEVC registers from delayed
	 * work.  Stop that owner before quiescing or powering down the block.
	 * Legacy .stop frees DMA state, so it must remain after quiesce.
	 */
	if (codec_ops->context_switching && sess->priv)
		codec_ops->stop(sess);

	vdec_hevc_quiesce(sess);
	if (!READ_ONCE(sess->hardware_stalled))
		vdec_hevc_wait_search_idle(core);

	if (!codec_ops->context_switching &&
	    !READ_ONCE(sess->hardware_stalled) && sess->priv)
		codec_ops->stop(sess);

	/* Vendor dos_gclk_en_set(false) gates the inactive HEVC VCPU. */
	amvdec_clear_dos_bits(core, DOS_GCLK_EN3, BIT(5));
}

static void vdec_hevc_power_off(struct amvdec_session *sess)
{
	struct amvdec_core *core = sess->core;

	/* Enable VDEC_HEVC Isolation */
	if (core->platform->revision == VDEC_REVISION_SM1)
		regmap_update_bits(core->regmap_ao, AO_RTI_GEN_PWR_ISO0,
				   GEN_PWR_VDEC_HEVC_SM1,
				   GEN_PWR_VDEC_HEVC_SM1);
	else
		regmap_update_bits(core->regmap_ao, AO_RTI_GEN_PWR_ISO0,
				   0xc00, 0xc00);

	/* VDEC_HEVC Memories */
	amvdec_write_dos(core, DOS_MEM_PD_HEVC, 0xffffffffUL);

	if (core->platform->revision == VDEC_REVISION_SM1)
		regmap_update_bits(core->regmap_ao, AO_RTI_GEN_PWR_SLEEP0,
				   GEN_PWR_VDEC_HEVC_SM1,
				   GEN_PWR_VDEC_HEVC_SM1);
	else
		regmap_update_bits(core->regmap_ao, AO_RTI_GEN_PWR_SLEEP0,
				   GEN_PWR_VDEC_HEVC, GEN_PWR_VDEC_HEVC);
}

static int vdec_hevc_stop_suspended(struct amvdec_session *sess)
{
	struct amvdec_core *core = sess->core;

	vdec_hevc_power_off(sess);

	clk_disable_unprepare(core->vdec_hevc_clk);
	if (core->platform->revision >= VDEC_REVISION_G12A)
		clk_disable_unprepare(core->vdec_hevcf_clk);

	return 0;
}

static int vdec_hevc_stop(struct amvdec_session *sess)
{
	vdec_hevc_suspend(sess);
	return vdec_hevc_stop_suspended(sess);
}

static int vdec_hevc_init(struct amvdec_session *sess, bool load_firmware)
{
	struct amvdec_core *core = sess->core;
	struct amvdec_codec_ops *codec_ops = sess->fmt_out->codec_ops;
	int ret;

	/* Select HEVC just as vendor dos_gclk_en_set(VDEC_HEVC, true) does. */
	amvdec_clear_dos_bits(core, DOS_GCLK_EN0, 0x3ff);

	/* A powered-off block also needs its firmware DMA state reset. */
	amvdec_write_dos(core, DOS_SW_RESET3,
			 load_firmware ? 0xffffffff : VDEC_HEVC_RESET_MASK);
	amvdec_write_dos(core, DOS_SW_RESET3, 0);
	amvdec_write_dos(core, DOS_GCLK_EN3, 0xffffffff);

	vdec_hevc_stbuf_init(sess);

	ret = vdec_hevc_load_firmware(sess, sess->fmt_out->firmware_path,
				      load_firmware);
	if (ret)
		goto stop;

	ret = codec_ops->start(sess);
	if (ret)
		goto stop;

	amvdec_write_dos(core, DOS_SW_RESET3, BIT(12) | BIT(11));
	amvdec_write_dos(core, DOS_SW_RESET3, 0);
	amvdec_read_dos(core, DOS_SW_RESET3);

	if (!codec_ops->context_switching)
		amvdec_write_dos(core, HEVC_MPSR, 1);
	/* Let the firmware settle */
	usleep_range(10, 20);

	return 0;

stop:
	vdec_hevc_suspend(sess);
	return ret;
}

static int vdec_hevc_resume(struct amvdec_session *sess,
			    bool reload_firmware)
{
	return vdec_hevc_init(sess, reload_firmware);
}

static int __vdec_hevc_start(struct amvdec_session *sess)
{
	struct amvdec_core *core = sess->core;
	int ret;

	ret = clk_set_rate(core->vdec_hevc_clk, 666666666);
	if (ret)
		return ret;

	ret = clk_prepare_enable(core->vdec_hevc_clk);
	if (ret)
		return ret;

	if (core->platform->revision == VDEC_REVISION_SM1)
		regmap_update_bits(core->regmap_ao, AO_RTI_GEN_PWR_SLEEP0,
				   GEN_PWR_VDEC_HEVC_SM1, 0);
	else
		regmap_update_bits(core->regmap_ao, AO_RTI_GEN_PWR_SLEEP0,
				   GEN_PWR_VDEC_HEVC, 0);
	usleep_range(10, 20);

	/* VDEC_HEVC Memories */
	amvdec_write_dos(core, DOS_MEM_PD_HEVC, 0);

	/* Remove VDEC_HEVC Isolation */
	if (core->platform->revision == VDEC_REVISION_SM1)
		regmap_update_bits(core->regmap_ao, AO_RTI_GEN_PWR_ISO0,
				   GEN_PWR_VDEC_HEVC_SM1, 0);
	else
		regmap_update_bits(core->regmap_ao, AO_RTI_GEN_PWR_ISO0,
				   0xc00, 0);

	ret = vdec_hevc_init(sess, true);
	if (!ret)
		return 0;

	vdec_hevc_power_off(sess);
	clk_disable_unprepare(core->vdec_hevc_clk);
	return ret;
}

static int vdec_hevc_start(struct amvdec_session *sess)
{
	struct amvdec_core *core = sess->core;
	int ret;

	if (core->platform->revision >= VDEC_REVISION_G12A) {
		clk_set_rate(core->vdec_hevcf_clk, 666666666);
		ret = clk_prepare_enable(core->vdec_hevcf_clk);
		if (ret)
			return ret;

		ret = __vdec_hevc_start(sess);
		if (ret)
			clk_disable_unprepare(core->vdec_hevcf_clk);
		return ret;
	}

	return __vdec_hevc_start(sess);
}

struct amvdec_ops vdec_hevc_ops = {
	.start = vdec_hevc_start,
	.stop = vdec_hevc_stop,
	.stop_suspended = vdec_hevc_stop_suspended,
	.resume = vdec_hevc_resume,
	.suspend = vdec_hevc_suspend,
	.hw = AMVDEC_HW_VDEC_HEVC,
	.conf_esparser = vdec_hevc_conf_esparser,
	.vififo_level = vdec_hevc_vififo_level,
};
