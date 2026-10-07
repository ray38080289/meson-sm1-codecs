// SPDX-License-Identifier: GPL-2.0+
/*
 * MPEG-1/2 decoding with the Amlogic "mpeg12_multi" firmware (G12A / SM1).
 *
 * The single-instance mpeg12 firmware lets the AMRISC track its own reference
 * canvases, which reads garbage references on SM1 (I pictures fine, every P/B
 * shifted). The vendor driver instead runs the multi firmware one picture per
 * run and tells it which canvases are the references and the reconstruction
 * target. This follows vmpeg12_multi.c (media_modules, 4.9 branch) for a
 * single stream-based session:
 *
 *   start:   canvases, workspace, first picture registers
 *   PIC_DONE: update references, output a picture, then per picture
 *            amvdec_stop -> VLD swap save -> resets -> VLD swap restore ->
 *            picture registers -> amvdec_start
 */
#include <linux/delay.h>
#include <linux/iopoll.h>
#include <media/v4l2-mem2mem.h>
#include <media/videobuf2-dma-contig.h>

#include "codec_mpeg12_multi.h"
#include "dos_regs.h"
#include "vdec_helpers.h"

/* protocol registers */
#define MREG_CC_ADDR		AV_SCRATCH_0
#define MREG_REF0		AV_SCRATCH_2
#define MREG_REF1		AV_SCRATCH_3
#define MREG_SEQ_INFO		AV_SCRATCH_4
#define MREG_PIC_INFO		AV_SCRATCH_5
#define MREG_PIC_WIDTH		AV_SCRATCH_6
#define MREG_PIC_HEIGHT		AV_SCRATCH_7
#define MREG_INPUT		AV_SCRATCH_8
#define MREG_BUFFEROUT		AV_SCRATCH_9
#define MREG_CMD		AV_SCRATCH_A
#define MREG_CO_MV_START	AV_SCRATCH_B
#define MREG_ERROR_COUNT	AV_SCRATCH_C
#define MREG_FRAME_OFFSET	AV_SCRATCH_D
#define MREG_WAIT_BUFFER	AV_SCRATCH_E
#define MREG_FATAL_ERROR	AV_SCRATCH_F
#define MREG_INFO_NOTIFY	0x2740		/* AV_SCRATCH_G */
#define MREG_SIGNAL_TYPE	0x2744		/* AV_SCRATCH_H */
#define MREG_USERDATA		0x274c		/* AV_SCRATCH_J */

#define F_CODE_REG		0x3008
#define SLICE_VER_POS_PIC_TYPE	0x3010
#define MB_INFO			0x30b0
#define REC_CANVAS_ADDR		0x26c8
#define ANC2_CANVAS_ADDR	0x2648
#define DMC_REQ_CTRL		0x00
#define DMC_CHAN_STS		0xc8
#define DMC_DOS_VDEC		BIT(21)

#define BUFFEROUT_PIC_DONE	1
#define BUFFEROUT_DATA_EMPTY	2
#define BUFFEROUT_SEQ_END	3
#define BUFFEROUT_DATA_REQUEST	4

#define PICINFO_ERROR		BIT(31)
#define PICINFO_TYPE(x)		(((x) >> 16) & 3)	/* 0 I, 1 P, 2 B */
#define PICINFO_PROG		BIT(15)
#define PICINFO_TOP_FIRST	BIT(13)
#define SEQINFO_EXT_AVAILABLE	BIT(31)
#define SEQINFO_PROG		BIT(16)
#define SEQ_DAR(x)		((x) & 0xf)

#define WORKSPACE_SIZE		(4 * SZ_64K)
#define CCBUF_SIZE		0x1400
#define SWAP_SIZE		PAGE_SIZE
#define MAX_BUFS		16

struct codec_mpeg12m {
	struct amvdec_session *sess;
	struct mutex lock;		/* refs, picture state, restart */
	struct work_struct restart_work;
	void __iomem *dmc;

	void *ws_vaddr, *cc_vaddr, *swap_vaddr;
	dma_addr_t ws_paddr, cc_paddr, swap_paddr;

	int refs[2];			/* [0] older, [1] newer; -1 = none */
	u8 ref_use[MAX_BUFS];
	int rec;			/* fw index being reconstructed */
	unsigned int dec_num;

	/* firmware state carried from one picture to the next */
	bool ctx_valid;
	u32 seq_info, pic_w, pic_h, mpeg1_2, pic_head, f_code;
	u32 slice_ver, vcop, mb_info, signal_type;
	u32 frame_w, frame_h;

	bool swap_valid;		/* VLD state saved in the swap page */
	u32 wrap_cookie;
	bool waiting_buffer;		/* no free CAPTURE buffer for the next run */
	bool waiting_data;		/* DATA_EMPTY: rerun once input arrives */
	bool running;
};

static u32 mpeg12m_spec(struct amvdec_session *sess, int idx)
{
	return sess->canvas_values[idx];
}

static int mpeg12m_spec_to_index(struct amvdec_session *sess, u32 spec)
{
	int i;

	for (i = 0; i < sess->canvas_reg_count; i++)
		if (sess->canvas_values[i] == spec)
			return i;
	return -1;
}

/* lowest fw index whose CAPTURE buffer is queued and is not a reference */
static int mpeg12m_find_free(struct codec_mpeg12m *m)
{
	struct amvdec_session *sess = m->sess;
	struct v4l2_m2m_buffer *buf;
	unsigned long flags;
	int best = -1, i;

	spin_lock_irqsave(&sess->m2m_ctx->cap_q_ctx.rdy_spinlock, flags);
	v4l2_m2m_for_each_dst_buf(sess->m2m_ctx, buf) {
		for (i = 0; i < sess->canvas_reg_count; i++) {
			if (sess->fw_idx_to_vb2_idx[i] != buf->vb.vb2_buf.index)
				continue;
			if (m->ref_use[i] || i == m->refs[0] || i == m->refs[1])
				break;
			if (best < 0 || i < best)
				best = i;
			break;
		}
	}
	spin_unlock_irqrestore(&sess->m2m_ctx->cap_q_ctx.rdy_spinlock, flags);
	return best;
}

/* vmpeg12_hw_ctx_restore() minus the canvas setup done once in start() */
static void mpeg12m_program_picture(struct codec_mpeg12m *m)
{
	struct amvdec_session *sess = m->sess;
	struct amvdec_core *core = sess->core;

	amvdec_write_dos(core, MREG_CO_MV_START, m->ws_paddr);
	amvdec_write_dos(core, MREG_CC_ADDR, m->cc_paddr);
	amvdec_write_dos(core, MREG_REF0, m->refs[0] < 0 ? 0xffffffff :
			 mpeg12m_spec(sess, m->refs[0]));
	amvdec_write_dos(core, MREG_REF1, m->refs[1] < 0 ? 0xffffffff :
			 mpeg12m_spec(sess, m->refs[1]));
	amvdec_write_dos(core, REC_CANVAS_ADDR, mpeg12m_spec(sess, m->rec));
	amvdec_write_dos(core, ANC2_CANVAS_ADDR, mpeg12m_spec(sess, m->rec));
	amvdec_write_dos(core, MPEG1_2_REG, m->ctx_valid ? m->mpeg1_2 : 0);
	amvdec_write_dos(core, PSCALE_CTRL, 0);
	amvdec_write_dos(core, PIC_HEAD_INFO, m->ctx_valid ? m->pic_head : 0x380);
	amvdec_write_dos(core, M4_CONTROL_REG, 0);
	amvdec_write_dos(core, ASSIST_MBOX1_CLR_REG, 1);
	amvdec_write_dos(core, MREG_BUFFEROUT, 0);
	amvdec_write_dos(core, ASSIST_MBOX1_MASK, 1);
	amvdec_write_dos(core, MREG_CMD, (m->frame_w && m->frame_h) ?
			 (m->frame_w << 16) | m->frame_h : 0);
	amvdec_write_dos(core, MREG_PIC_WIDTH, m->pic_w);
	amvdec_write_dos(core, MREG_PIC_HEIGHT, m->pic_h);
	amvdec_write_dos(core, MREG_SEQ_INFO, m->seq_info);
	amvdec_write_dos(core, F_CODE_REG, m->f_code);
	amvdec_write_dos(core, SLICE_VER_POS_PIC_TYPE, m->slice_ver);
	amvdec_write_dos(core, MB_INFO, m->mb_info);
	amvdec_write_dos(core, VCOP_CTRL_REG, m->vcop);
	amvdec_write_dos(core, MREG_SIGNAL_TYPE, m->signal_type);
	amvdec_write_dos(core, MREG_ERROR_COUNT, 0);
	/* bit0: CC data at MREG_CC_ADDR; bit1 off: no AV_SCRATCH_G info IRQ */
	amvdec_write_dos(core, MREG_FATAL_ERROR, 1);
	amvdec_write_dos(core, MREG_WAIT_BUFFER, 0);
	amvdec_write_dos_bits(core, MDEC_PIC_DC_CTRL, BIT(17));	/* NV12 */
	amvdec_clear_dos_bits(core, MDEC_PIC_DC_CTRL, BIT(16));
	if (!m->ctx_valid)
		amvdec_write_dos(core, MREG_USERDATA, 0);
	amvdec_write_dos(core, MREG_INPUT, m->ctx_valid << 6);
}

static void mpeg12m_save_context(struct codec_mpeg12m *m, u32 bufferout)
{
	struct amvdec_core *core = m->sess->core;

	if (bufferout == BUFFEROUT_SEQ_END) {
		m->ctx_valid = false;
		return;
	}
	m->seq_info = amvdec_read_dos(core, MREG_SEQ_INFO);
	m->pic_w = amvdec_read_dos(core, MREG_PIC_WIDTH);
	m->pic_h = amvdec_read_dos(core, MREG_PIC_HEIGHT);
	m->mpeg1_2 = amvdec_read_dos(core, MPEG1_2_REG);
	m->pic_head = amvdec_read_dos(core, PIC_HEAD_INFO);
	m->f_code = amvdec_read_dos(core, F_CODE_REG);
	m->slice_ver = amvdec_read_dos(core, SLICE_VER_POS_PIC_TYPE);
	m->vcop = amvdec_read_dos(core, VCOP_CTRL_REG);
	m->mb_info = amvdec_read_dos(core, MB_INFO);
	m->signal_type = amvdec_read_dos(core, MREG_SIGNAL_TYPE);
	m->ctx_valid = true;
}

/* amvdec_stop() */
static void mpeg12m_stop_cpu(struct amvdec_core *core)
{
	u32 v;

	amvdec_write_dos(core, MPSR, 0);
	amvdec_write_dos(core, CPSR, 0);
	readl_poll_timeout(core->dos_base + IMEM_DMA_CTRL, v, !(v & BIT(15)),
			   10, 100000);
	readl_poll_timeout(core->dos_base + LMEM_DMA_CTRL, v, !(v & BIT(15)),
			   10, 100000);
	amvdec_write_dos(core, DOS_SW_RESET0, BIT(12) | BIT(11));
	amvdec_write_dos(core, DOS_SW_RESET0, 0);
	amvdec_read_dos(core, DOS_SW_RESET0);
}

/* wait_vmmpeg12_search_done(): let the VIFIFO read pointer settle */
static void mpeg12m_wait_rp_stable(struct amvdec_core *core)
{
	u32 rp = amvdec_read_dos(core, VLD_MEM_VIFIFO_RP), prev;
	int i;

	for (i = 0; i < 1000; i++) {
		usleep_range(100, 500);
		prev = rp;
		rp = amvdec_read_dos(core, VLD_MEM_VIFIFO_RP);
		if (rp == prev)
			return;
	}
}

/* vdec_save_input_context(): VLD state to the swap page */
static void mpeg12m_swap_save(struct codec_mpeg12m *m)
{
	struct amvdec_core *core = m->sess->core;
	u32 v;

	amvdec_write_dos(core, VLD_MEM_VIFIFO_CONTROL, BIT(15));
	amvdec_write_dos(core, VLD_MEM_SWAP_ADDR, m->swap_paddr);
	amvdec_write_dos(core, VLD_MEM_SWAP_CTL, 3);
	readl_poll_timeout(core->dos_base + VLD_MEM_SWAP_CTL, v, !(v & BIT(7)),
			   10, 100000);
	amvdec_write_dos(core, VLD_MEM_SWAP_CTL, 0);
	m->wrap_cookie = amvdec_read_dos(core, VLD_MEM_VIFIFO_WRAP_COUNT);
	m->swap_valid = true;
}

/* vdec_reset_core(): VLD, VLD part, VIFIFO, MC, DBLK, PIC_DC behind the DMC */
static void mpeg12m_reset_core(struct codec_mpeg12m *m)
{
	struct amvdec_core *core = m->sess->core;
	u32 v;

	if (m->dmc) {
		writel(readl(m->dmc + DMC_REQ_CTRL) & ~DMC_DOS_VDEC,
		       m->dmc + DMC_REQ_CTRL);
		readl_poll_timeout(m->dmc + DMC_CHAN_STS, v, v & DMC_DOS_VDEC,
				   1, 1000);
	}
	amvdec_write_dos(core, DOS_SW_RESET0,
			 BIT(3) | BIT(4) | BIT(5) | BIT(7) | BIT(8) | BIT(9));
	amvdec_write_dos(core, DOS_SW_RESET0, 0);
	if (m->dmc)
		writel(readl(m->dmc + DMC_REQ_CTRL) | DMC_DOS_VDEC,
		       m->dmc + DMC_REQ_CTRL);
}

/* vdec_prepare_input() for a stream that already ran once */
static void mpeg12m_swap_restore(struct codec_mpeg12m *m)
{
	struct amvdec_core *core = m->sess->core;
	u32 v;

	amvdec_write_dos(core, VLD_MEM_VIFIFO_CONTROL, 0);
	amvdec_write_dos(core, DOS_SW_RESET0, BIT(5) | BIT(4) | BIT(3));
	amvdec_write_dos(core, DOS_SW_RESET0, 0);
	amvdec_write_dos(core, POWER_CTL_VLD, BIT(4));
	amvdec_write_dos(core, VLD_MEM_VIFIFO_CONTROL, 0);
	amvdec_write_dos(core, VLD_MEM_SWAP_ADDR, m->swap_paddr);
	amvdec_write_dos(core, VLD_MEM_SWAP_CTL, 1);
	readl_poll_timeout(core->dos_base + VLD_MEM_SWAP_CTL, v, !(v & BIT(7)),
			   10, 100000);
	amvdec_write_dos(core, VLD_MEM_SWAP_CTL, 0);
	amvdec_write_dos(core, VLD_MEM_VIFIFO_WRAP_COUNT, m->wrap_cookie);
	amvdec_write_dos(core, VLD_MEM_VIFIFO_CONTROL, (0x11 << 16) | BIT(10));
}

/*
 * One vendor run() for the next picture. commit: the previous picture was
 * consumed (DONE), so save the VLD state; otherwise (DATA_EMPTY) replay it.
 */
static void mpeg12m_next_run(struct codec_mpeg12m *m, bool commit)
{
	struct amvdec_session *sess = m->sess;
	struct amvdec_core *core = sess->core;
	u32 save;
	int idx;

	if (m->running) {
		mpeg12m_stop_cpu(core);
		amvdec_write_dos(core, ASSIST_MBOX1_MASK, 0);
		mpeg12m_wait_rp_stable(core);
		if (commit)
			mpeg12m_swap_save(m);
		m->running = false;
	}

	idx = mpeg12m_find_free(m);
	if (idx < 0) {
		m->waiting_buffer = true;
		return;
	}
	m->waiting_buffer = false;
	m->rec = idx;

	save = amvdec_read_dos(core, POWER_CTL_VLD);
	amvdec_write_dos(core, DOS_SW_RESET0, 0xfffffff0);
	amvdec_write_dos(core, DOS_SW_RESET0, 0);
	amvdec_write_dos(core, POWER_CTL_VLD, save);
	mpeg12m_reset_core(m);
	if (m->swap_valid)
		mpeg12m_swap_restore(m);

	mpeg12m_program_picture(m);
	amvdec_write_dos_bits(core, VLD_MEM_VIFIFO_CONTROL, BIT(2) | BIT(1));

	/* amvdec_start() */
	amvdec_read_dos(core, DOS_SW_RESET0);
	amvdec_write_dos(core, DOS_SW_RESET0, BIT(12) | BIT(11));
	amvdec_write_dos(core, DOS_SW_RESET0, 0);
	amvdec_read_dos(core, DOS_SW_RESET0);
	amvdec_write_dos(core, MPSR, 1);
	m->running = true;
}

static void mpeg12m_restart_work(struct work_struct *work)
{
	struct codec_mpeg12m *m = container_of(work, struct codec_mpeg12m,
					       restart_work);

	mutex_lock(&m->lock);
	if (m->sess->status == STATUS_RUNNING && !m->waiting_data)
		mpeg12m_next_run(m, true);
	mutex_unlock(&m->lock);
}

static int codec_mpeg12m_start(struct amvdec_session *sess)
{
	struct amvdec_core *core = sess->core;
	struct codec_mpeg12m *m;
	int ret;

	m = kzalloc(sizeof(*m), GFP_KERNEL);
	if (!m)
		return -ENOMEM;
	m->sess = sess;
	mutex_init(&m->lock);
	INIT_WORK(&m->restart_work, mpeg12m_restart_work);
	m->refs[0] = m->refs[1] = -1;

	m->ws_vaddr = dma_alloc_coherent(core->dev, WORKSPACE_SIZE,
					 &m->ws_paddr, GFP_KERNEL);
	m->cc_vaddr = dma_alloc_coherent(core->dev, CCBUF_SIZE,
					 &m->cc_paddr, GFP_KERNEL);
	m->swap_vaddr = dma_alloc_coherent(core->dev, SWAP_SIZE,
					   &m->swap_paddr, GFP_KERNEL);
	if (!m->ws_vaddr || !m->cc_vaddr || !m->swap_vaddr) {
		ret = -ENOMEM;
		goto free;
	}
	m->dmc = ioremap(0xff638000, 0x400);	/* G12A/SM1 DMC */

	ret = amvdec_set_canvases(sess, (u32[]){ ANC0_CANVAS_ADDR, 0 },
				  (u32[]){ MAX_BUFS, 0 });
	if (ret)
		goto free;

	sess->keyframe_found = 1;
	sess->priv = m;

	/* vdec_1_start() already reset, set up the VIFIFO and loaded the ucode */
	m->rec = mpeg12m_find_free(m);
	if (m->rec < 0)
		m->rec = 0;
	mpeg12m_program_picture(m);
	m->running = true;	/* vdec_1 sets MPSR right after this */
	return 0;

free:
	if (m->dmc)
		iounmap(m->dmc);
	if (m->swap_vaddr)
		dma_free_coherent(core->dev, SWAP_SIZE, m->swap_vaddr, m->swap_paddr);
	if (m->cc_vaddr)
		dma_free_coherent(core->dev, CCBUF_SIZE, m->cc_vaddr, m->cc_paddr);
	if (m->ws_vaddr)
		dma_free_coherent(core->dev, WORKSPACE_SIZE, m->ws_vaddr, m->ws_paddr);
	kfree(m);
	return ret;
}

static int codec_mpeg12m_stop(struct amvdec_session *sess)
{
	struct codec_mpeg12m *m = sess->priv;
	struct amvdec_core *core = sess->core;

	if (!m)
		return 0;
	cancel_work_sync(&m->restart_work);
	if (m->dmc)
		iounmap(m->dmc);
	dma_free_coherent(core->dev, SWAP_SIZE, m->swap_vaddr, m->swap_paddr);
	dma_free_coherent(core->dev, CCBUF_SIZE, m->cc_vaddr, m->cc_paddr);
	dma_free_coherent(core->dev, WORKSPACE_SIZE, m->ws_vaddr, m->ws_paddr);
	kfree(m);
	sess->priv = NULL;
	return 0;
}

static void mpeg12m_update_dar(struct amvdec_session *sess, u32 seq)
{
	switch (SEQ_DAR(seq)) {
	case 2:
		amvdec_set_par_from_dar(sess, 4, 3);
		break;
	case 3:
		amvdec_set_par_from_dar(sess, 16, 9);
		break;
	case 4:
		amvdec_set_par_from_dar(sess, 221, 100);
		break;
	default:
		sess->pixelaspect.numerator = 1;
		sess->pixelaspect.denominator = 1;
		break;
	}
}

/* update_reference(): returns the fw index to output now, or -1 */
static int mpeg12m_update_reference(struct codec_mpeg12m *m, int index)
{
	int out;

	m->ref_use[index]++;
	if (m->refs[1] < 0) {
		m->refs[1] = index;
		return index;
	}
	if (m->refs[0] < 0) {
		m->refs[0] = m->refs[1];
		m->refs[1] = index;
		return -1;
	}
	m->ref_use[m->refs[0]]--;
	out = m->refs[1];
	m->refs[0] = m->refs[1];
	m->refs[1] = index;
	return out;
}

static irqreturn_t codec_mpeg12m_threaded_isr(struct amvdec_session *sess)
{
	struct codec_mpeg12m *m = sess->priv;
	struct amvdec_core *core = sess->core;
	u32 reg, info, seq, offset, field = V4L2_FIELD_NONE;
	int index, out;

	if (!m)
		return IRQ_HANDLED;

	if (amvdec_read_dos(core, MREG_INFO_NOTIFY) == 1) {
		amvdec_write_dos(core, MREG_INFO_NOTIFY, 0);
		return IRQ_HANDLED;
	}
	if (amvdec_read_dos(core, MREG_USERDATA) & BIT(16)) {
		amvdec_write_dos(core, MREG_USERDATA, 0);	/* CC data: ack */
		return IRQ_HANDLED;
	}

	mutex_lock(&m->lock);
	reg = amvdec_read_dos(core, MREG_BUFFEROUT);
	if (reg == BUFFEROUT_DATA_REQUEST)
		goto unlock;		/* stream mode: the VIFIFO keeps filling */
	if (reg == BUFFEROUT_DATA_EMPTY) {
		/* replay this picture from the last committed state */
		mpeg12m_stop_cpu(core);
		m->running = false;
		m->waiting_data = true;
		goto unlock;
	}

	info = amvdec_read_dos(core, MREG_PIC_INFO);
	seq = amvdec_read_dos(core, MREG_SEQ_INFO);
	offset = amvdec_read_dos(core, MREG_FRAME_OFFSET);
	index = mpeg12m_spec_to_index(sess,
				      amvdec_read_dos(core, REC_CANVAS_ADDR));
	if (index < 0) {
		dev_err(core->dev, "MPEG1/2: unknown REC canvas\n");
		amvdec_abort(sess);
		goto unlock;
	}
	m->dec_num++;
	m->frame_w = amvdec_read_dos(core, MREG_PIC_WIDTH);
	m->frame_h = amvdec_read_dos(core, MREG_PIC_HEIGHT);
	if (!m->frame_w || m->frame_w > 1920)
		m->frame_w = 1920;
	if (!m->frame_h || m->frame_h > 1088)
		m->frame_h = 1088;

	if (!(info & PICINFO_PROG) ||
	    ((seq & SEQINFO_EXT_AVAILABLE) && !(seq & SEQINFO_PROG)))
		field = (info & PICINFO_TOP_FIRST) ? V4L2_FIELD_INTERLACED_TB :
						     V4L2_FIELD_INTERLACED_BT;
	mpeg12m_update_dar(sess, seq);

	if (PICINFO_TYPE(info) != 2)		/* I or P */
		out = mpeg12m_update_reference(m, index);
	else					/* B: no forward ref yet -> drop */
		out = m->refs[0] < 0 ? -1 : index;

	mpeg12m_save_context(m, reg);
	if (out >= 0)
		amvdec_dst_buf_done_idx(sess, out, offset, field, 0);

	schedule_work(&m->restart_work);
unlock:
	mutex_unlock(&m->lock);
	return IRQ_HANDLED;
}

static irqreturn_t codec_mpeg12m_isr(struct amvdec_session *sess)
{
	amvdec_write_dos(sess->core, ASSIST_MBOX1_CLR_REG, 1);
	return IRQ_WAKE_THREAD;
}

static void codec_mpeg12m_capture_queued(struct amvdec_session *sess)
{
	struct codec_mpeg12m *m = sess->priv;

	if (m && READ_ONCE(m->waiting_buffer))
		schedule_work(&m->restart_work);
}

static void codec_mpeg12m_input_queued(struct amvdec_session *sess,
				       u32 payload_size)
{
	struct codec_mpeg12m *m = sess->priv;

	if (!m || !READ_ONCE(m->waiting_data))
		return;
	mutex_lock(&m->lock);
	if (m->waiting_data) {
		m->waiting_data = false;
		mpeg12m_next_run(m, false);	/* replay, no commit */
	}
	mutex_unlock(&m->lock);
}

static const u8 mpeg12m_eos_sequence[SZ_1K] = { 0x00, 0x00, 0x01, 0xB7 };

static const u8 *codec_mpeg12m_eos_sequence(u32 *len)
{
	*len = ARRAY_SIZE(mpeg12m_eos_sequence);
	return mpeg12m_eos_sequence;
}

struct amvdec_codec_ops codec_mpeg12_multi_ops = {
	.start = codec_mpeg12m_start,
	.stop = codec_mpeg12m_stop,
	.isr = codec_mpeg12m_isr,
	.threaded_isr = codec_mpeg12m_threaded_isr,
	.capture_queued = codec_mpeg12m_capture_queued,
	.input_queued = codec_mpeg12m_input_queued,
	.eos_sequence = codec_mpeg12m_eos_sequence,
};
