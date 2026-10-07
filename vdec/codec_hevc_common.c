// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (C) 2018 Maxime Jourdan <mjourdan@baylibre.com>
 */

#include <media/v4l2-mem2mem.h>
#include <media/videobuf2-dma-contig.h>

#include <linux/slab.h>

#include "codec_hevc_common.h"
#include "vdec_helpers.h"
#include "hevc_regs.h"

#define MMU_COMPRESS_HEADER_SIZE 0x48000
#define MMU_MAP_SIZE 0x4800

const u16 vdec_hevc_parser_cmd[] = {
	0x0401,	0x8401,	0x0800,	0x0402,
	0x9002,	0x1423,	0x8CC3,	0x1423,
	0x8804,	0x9825,	0x0800,	0x04FE,
	0x8406,	0x8411,	0x1800,	0x8408,
	0x8409,	0x8C2A,	0x9C2B,	0x1C00,
	0x840F,	0x8407,	0x8000,	0x8408,
	0x2000,	0xA800,	0x8410,	0x04DE,
	0x840C,	0x840D,	0xAC00,	0xA000,
	0x08C0,	0x08E0,	0xA40E,	0xFC00,
	0x7C00
};

/* Configure decode head read mode */
void codec_hevc_setup_decode_head(struct amvdec_session *sess, int is_10bit)
{
	struct amvdec_core *core = sess->core;
	u32 use_mmu = codec_hevc_use_mmu(core->platform->revision,
					 sess->pixfmt_cap, is_10bit);
	u32 body_size = amvdec_amfbc_body_size(sess->width, sess->height,
					       is_10bit, use_mmu);
	u32 head_size = amvdec_amfbc_head_size(sess->width, sess->height);

	if (!codec_hevc_use_fbc(sess->pixfmt_cap, is_10bit)) {
		/* Enable 2-plane reference read mode */
		amvdec_write_dos(core, HEVCD_MPP_DECOMP_CTL1, BIT(31));
		return;
	}

	/* enable mem saving mode for 8-bit */
	if (!is_10bit)
		amvdec_write_dos_bits(core, HEVC_SAO_CTRL5, BIT(9));
	else
		amvdec_clear_dos_bits(core, HEVC_SAO_CTRL5, BIT(9));

	if (codec_hevc_use_mmu(core->platform->revision,
			       sess->pixfmt_cap, is_10bit))
		amvdec_write_dos(core, HEVCD_MPP_DECOMP_CTL1, BIT(4));
	else if (!is_10bit)
		amvdec_write_dos(core, HEVCD_MPP_DECOMP_CTL1, BIT(3));
	else
		amvdec_write_dos(core, HEVCD_MPP_DECOMP_CTL1, 0);

	if (core->platform->revision < VDEC_REVISION_SM1)
		amvdec_write_dos(core, HEVCD_MPP_DECOMP_CTL2, body_size / 32);
	amvdec_write_dos(core, HEVC_CM_BODY_LENGTH, body_size);
	amvdec_write_dos(core, HEVC_CM_HEADER_OFFSET, body_size);
	amvdec_write_dos(core, HEVC_CM_HEADER_LENGTH, head_size);
}
EXPORT_SYMBOL_GPL(codec_hevc_setup_decode_head);

static void codec_hevc_setup_buffers_gxbb(struct amvdec_session *sess,
					  struct codec_hevc_common *comm,
					  int is_10bit)
{
	struct amvdec_core *core = sess->core;
	struct v4l2_m2m_buffer *buf;
	u32 buf_num = v4l2_m2m_num_dst_bufs_ready(sess->m2m_ctx);
	dma_addr_t buf_y_paddr = 0;
	dma_addr_t buf_uv_paddr = 0;
	u32 idx = 0;
	u32 val;
	int i;

	amvdec_write_dos(core, HEVCD_MPP_ANC2AXI_TBL_CONF_ADDR, 0);

	v4l2_m2m_for_each_dst_buf(sess->m2m_ctx, buf) {
		struct vb2_buffer *vb = &buf->vb.vb2_buf;

		idx = vb->index;

		if (codec_hevc_use_fbc(sess->pixfmt_cap, is_10bit))
			buf_y_paddr = comm->fbc_buffer_paddr[idx];
		else
			buf_y_paddr = vb2_dma_contig_plane_dma_addr(vb, 0);

		if (codec_hevc_use_fbc(sess->pixfmt_cap, is_10bit)) {
			val = buf_y_paddr | (idx << 8) | 1;
			amvdec_write_dos(core, HEVCD_MPP_ANC2AXI_TBL_CMD_ADDR,
					 val);
		} else {
			buf_uv_paddr = vb2_dma_contig_plane_dma_addr(vb, 1);
			val = buf_y_paddr | ((idx * 2) << 8) | 1;
			amvdec_write_dos(core, HEVCD_MPP_ANC2AXI_TBL_CMD_ADDR,
					 val);
			val = buf_uv_paddr | ((idx * 2 + 1) << 8) | 1;
			amvdec_write_dos(core, HEVCD_MPP_ANC2AXI_TBL_CMD_ADDR,
					 val);
		}
	}

	if (codec_hevc_use_fbc(sess->pixfmt_cap, is_10bit))
		val = buf_y_paddr | (idx << 8) | 1;
	else
		val = buf_y_paddr | ((idx * 2) << 8) | 1;

	/* Fill the remaining unused slots with the last buffer's Y addr */
	for (i = buf_num; i < MAX_REF_PIC_NUM; ++i)
		amvdec_write_dos(core, HEVCD_MPP_ANC2AXI_TBL_CMD_ADDR, val);

	amvdec_write_dos(core, HEVCD_MPP_ANC2AXI_TBL_CONF_ADDR, 1);
	amvdec_write_dos(core, HEVCD_MPP_ANC_CANVAS_ACCCONFIG_ADDR, 1);
	for (i = 0; i < 32; ++i)
		amvdec_write_dos(core, HEVCD_MPP_ANC_CANVAS_DATA_ADDR, 0);
}

static void codec_hevc_setup_buffers_gxl(struct amvdec_session *sess,
					 struct codec_hevc_common *comm,
					 int is_10bit)
{
	struct amvdec_core *core = sess->core;
	struct v4l2_m2m_buffer *buf;
	u32 pixfmt_cap = sess->pixfmt_cap;
	const u32 revision = core->platform->revision;
	int i;

	amvdec_write_dos(core, HEVCD_MPP_ANC2AXI_TBL_CONF_ADDR,
			 BIT(2) | BIT(1));

	v4l2_m2m_for_each_dst_buf(sess->m2m_ctx, buf) {
		struct vb2_buffer *vb = &buf->vb.vb2_buf;
		dma_addr_t buf_y_paddr = 0;
		dma_addr_t buf_uv_paddr = 0;
		u32 idx = vb->index;

		if (codec_hevc_use_downsample(pixfmt_cap, is_10bit)) {
			if (codec_hevc_use_mmu(revision, pixfmt_cap, is_10bit))
				buf_y_paddr = comm->mmu_header_paddr[idx];
			else
				buf_y_paddr = comm->fbc_buffer_paddr[idx];
		} else {
			buf_y_paddr = vb2_dma_contig_plane_dma_addr(vb, 0);
		}
		comm->ref_buffer_paddr[idx][0] = buf_y_paddr;

		if (!codec_hevc_use_fbc(pixfmt_cap, is_10bit)) {
			buf_uv_paddr = vb2_dma_contig_plane_dma_addr(vb, 1);
			comm->ref_buffer_paddr[idx][1] = buf_uv_paddr;
		}
		comm->ref_buffer_count = max(comm->ref_buffer_count, idx + 1);
	}

	for (i = 0; i < comm->ref_buffer_count; i++) {
		amvdec_write_dos(core, HEVCD_MPP_ANC2AXI_TBL_DATA,
				 comm->ref_buffer_paddr[i][0] >> 5);
		if (!codec_hevc_use_fbc(pixfmt_cap, is_10bit))
			amvdec_write_dos(core, HEVCD_MPP_ANC2AXI_TBL_DATA,
					 comm->ref_buffer_paddr[i][1] >> 5);
	}

	amvdec_write_dos(core, HEVCD_MPP_ANC2AXI_TBL_CONF_ADDR, 1);
	amvdec_write_dos(core, HEVCD_MPP_ANC_CANVAS_ACCCONFIG_ADDR, 1);
	for (i = 0; i < 32; ++i)
		amvdec_write_dos(core, HEVCD_MPP_ANC_CANVAS_DATA_ADDR, 0);
}

void codec_hevc_restore_buffers(struct amvdec_session *sess,
				struct codec_hevc_common *comm,
				int is_10bit)
{
	if (sess->core->platform->revision == VDEC_REVISION_GXBB)
		codec_hevc_setup_buffers_gxbb(sess, comm, is_10bit);
	else
		codec_hevc_setup_buffers_gxl(sess, comm, is_10bit);
}
EXPORT_SYMBOL_GPL(codec_hevc_restore_buffers);

void codec_hevc_free_mmu_headers(struct amvdec_session *sess,
				 struct codec_hevc_common *comm)
{
	struct device *dev = sess->core->dev;
	int i;

	for (i = 0; i < MAX_REF_PIC_NUM; ++i) {
		if (comm->mmu_header_vaddr[i]) {
			dma_free_coherent(dev, MMU_COMPRESS_HEADER_SIZE,
					  comm->mmu_header_vaddr[i],
					  comm->mmu_header_paddr[i]);
			comm->mmu_header_vaddr[i] = NULL;
		}
	}
}
EXPORT_SYMBOL_GPL(codec_hevc_free_mmu_headers);

static int codec_hevc_alloc_mmu_headers(struct amvdec_session *sess,
					struct codec_hevc_common *comm)
{
	struct device *dev = sess->core->dev;
	struct v4l2_m2m_buffer *buf;

	v4l2_m2m_for_each_dst_buf(sess->m2m_ctx, buf) {
		u32 idx = buf->vb.vb2_buf.index;
		dma_addr_t paddr;
		void *vaddr;

		if (comm->mmu_header_vaddr[idx])
			continue;

		vaddr = dma_alloc_coherent(dev, MMU_COMPRESS_HEADER_SIZE,
					   &paddr, GFP_KERNEL);
		if (!vaddr) {
			codec_hevc_free_mmu_headers(sess, comm);
			return -ENOMEM;
		}

		comm->mmu_header_vaddr[idx] = vaddr;
		comm->mmu_header_paddr[idx] = paddr;
	}

	return 0;
}

static void codec_hevc_free_mmu_body(struct device *dev,
				     struct sg_table *sgt)
{
	struct sg_page_iter piter;

	dma_unmap_sgtable(dev, sgt, DMA_BIDIRECTIONAL, 0);
	for_each_sgtable_page(sgt, &piter, 0)
		__free_page(sg_page_iter_page(&piter));
	sg_free_table(sgt);
	kfree(sgt);
}

static struct sg_table *codec_hevc_alloc_mmu_body(struct device *dev,
						  size_t size)
{
	unsigned int num_pages = PAGE_ALIGN(size) >> PAGE_SHIFT;
	struct sg_table *sgt;
	struct page **pages;
	unsigned int i;
	int ret = -ENOMEM;

	pages = kvmalloc_array(num_pages, sizeof(*pages), GFP_KERNEL);
	if (!pages)
		return ERR_PTR(-ENOMEM);

	for (i = 0; i < num_pages; i++) {
		pages[i] = alloc_page(GFP_KERNEL | __GFP_NOWARN);
		if (!pages[i])
			goto free_pages;
	}

	sgt = kzalloc(sizeof(*sgt), GFP_KERNEL);
	if (!sgt)
		goto free_pages;

	ret = sg_alloc_table_from_pages(sgt, pages, num_pages, 0, size,
					GFP_KERNEL);
	if (ret)
		goto free_sgt;

	ret = dma_map_sgtable(dev, sgt, DMA_BIDIRECTIONAL, 0);
	if (ret)
		goto free_table;

	kvfree(pages);
	return sgt;

free_table:
	sg_free_table(sgt);
free_sgt:
	kfree(sgt);
free_pages:
	while (i)
		__free_page(pages[--i]);
	kvfree(pages);
	return ERR_PTR(ret);
}

void codec_hevc_free_fbc_buffers(struct amvdec_session *sess,
				 struct codec_hevc_common *comm)
{
	struct device *dev = sess->core->dev;
	int i;

	for (i = 0; i < MAX_REF_PIC_NUM; ++i) {
		if (comm->mmu_body_sgt[i]) {
			codec_hevc_free_mmu_body(dev, comm->mmu_body_sgt[i]);
			comm->mmu_body_sgt[i] = NULL;
		}
		if (comm->fbc_buffer_vaddr[i]) {
			dma_free_coherent(dev, comm->fbc_buffer_size,
					  comm->fbc_buffer_vaddr[i],
					  comm->fbc_buffer_paddr[i]);
			comm->fbc_buffer_vaddr[i] = NULL;
		}
		comm->fbc_buffer_paddr[i] = 0;
	}
	comm->fbc_buffer_size = 0;

	if (comm->mmu_map_vaddr) {
		dma_free_coherent(dev, MMU_MAP_SIZE,
				  comm->mmu_map_vaddr,
				  comm->mmu_map_paddr);
		comm->mmu_map_vaddr = NULL;
	}

	codec_hevc_free_mmu_headers(sess, comm);
	comm->ref_buffer_count = 0;
}
EXPORT_SYMBOL_GPL(codec_hevc_free_fbc_buffers);

static int codec_hevc_alloc_fbc_buffers(struct amvdec_session *sess,
					struct codec_hevc_common *comm,
					int is_10bit)
{
	struct device *dev = sess->core->dev;
	struct v4l2_m2m_buffer *buf;
	u32 use_mmu;
	u32 am21_size;
	const u32 revision = sess->core->platform->revision;
	int ret;

	use_mmu = codec_hevc_use_mmu(revision, sess->pixfmt_cap,
				     is_10bit);

	am21_size = amvdec_amfbc_size(sess->width, sess->height,
				      is_10bit, use_mmu);
	comm->fbc_buffer_size = am21_size;

	v4l2_m2m_for_each_dst_buf(sess->m2m_ctx, buf) {
		u32 idx = buf->vb.vb2_buf.index;
		struct sg_table *sgt;
		dma_addr_t paddr;
		void *vaddr;

		if (use_mmu && comm->mmu_body_sgt[idx])
			continue;
		if (!use_mmu && comm->fbc_buffer_vaddr[idx])
			continue;

		if (use_mmu) {
			sgt = codec_hevc_alloc_mmu_body(dev, am21_size);
			if (IS_ERR(sgt))
				return PTR_ERR(sgt);
			comm->mmu_body_sgt[idx] = sgt;
			continue;
		}

		vaddr = dma_alloc_coherent(dev, am21_size, &paddr, GFP_KERNEL);
		if (!vaddr)
			return -ENOMEM;

		comm->fbc_buffer_vaddr[idx] = vaddr;
		comm->fbc_buffer_paddr[idx] = paddr;
	}

	if (codec_hevc_use_mmu(revision, sess->pixfmt_cap, is_10bit) &&
	    codec_hevc_use_downsample(sess->pixfmt_cap, is_10bit)) {
		ret = codec_hevc_alloc_mmu_headers(sess, comm);
		if (ret) {
			codec_hevc_free_fbc_buffers(sess, comm);
			return ret;
		}
	}

	return 0;
}

int codec_hevc_setup_buffers(struct amvdec_session *sess,
			     struct codec_hevc_common *comm,
			     int is_10bit)
{
	struct amvdec_core *core = sess->core;
	struct device *dev = core->dev;
	u32 use_mmu;
	u32 fbc_size = 0;
	bool use_fbc;
	int ret;

	use_mmu = codec_hevc_use_mmu(core->platform->revision,
				     sess->pixfmt_cap, is_10bit);
	use_fbc = use_mmu ||
		  codec_hevc_use_downsample(sess->pixfmt_cap, is_10bit);
	if (use_fbc)
		fbc_size = amvdec_amfbc_size(sess->width, sess->height,
					     is_10bit, use_mmu);

	if (comm->fbc_buffer_size != fbc_size ||
	    !!comm->mmu_map_vaddr != !!use_mmu)
		codec_hevc_free_fbc_buffers(sess, comm);

	if (use_mmu && !comm->mmu_map_vaddr) {
		comm->mmu_map_vaddr = dma_alloc_coherent(dev, MMU_MAP_SIZE,
							 &comm->mmu_map_paddr,
							 GFP_KERNEL);
		if (!comm->mmu_map_vaddr)
			return -ENOMEM;
	}

	if (use_fbc) {
		ret = codec_hevc_alloc_fbc_buffers(sess, comm, is_10bit);
		if (ret)
			return ret;
	}

	codec_hevc_restore_buffers(sess, comm, is_10bit);

	return 0;
}
EXPORT_SYMBOL_GPL(codec_hevc_setup_buffers);

int codec_hevc_fill_mmu_map(struct amvdec_session *sess,
			    struct codec_hevc_common *comm,
			    struct vb2_buffer *vb,
			    u32 is_10bit)
{
	struct sg_dma_page_iter dma_iter;
	struct sg_table *sgt = comm->mmu_body_sgt[vb->index];
	u32 use_mmu;
	u32 size;
	u32 nb_pages;
	u32 *mmu_map = comm->mmu_map_vaddr;
	u32 i = 0;

	if (!sgt || !mmu_map)
		return -EINVAL;

	use_mmu = codec_hevc_use_mmu(sess->core->platform->revision,
				     sess->pixfmt_cap, is_10bit);

	size = amvdec_amfbc_size(sess->width, sess->height, is_10bit,
				 use_mmu);

	nb_pages = PAGE_ALIGN(size) >> PAGE_SHIFT;
	if (nb_pages > MMU_MAP_SIZE / sizeof(*mmu_map))
		return -E2BIG;

	for_each_sgtable_dma_page(sgt, &dma_iter, 0) {
		dma_addr_t addr;

		if (i == nb_pages)
			break;
		addr = sg_page_iter_dma_address(&dma_iter);
		if ((addr >> PAGE_SHIFT) > U32_MAX)
			return -ERANGE;
		mmu_map[i++] = addr >> PAGE_SHIFT;
	}

	if (i != nb_pages)
		return -EINVAL;

	dma_wmb();
	return 0;
}
EXPORT_SYMBOL_GPL(codec_hevc_fill_mmu_map);
