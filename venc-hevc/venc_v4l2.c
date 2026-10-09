// SPDX-License-Identifier: GPL-2.0
/*
 * V4L2 mem2mem stateful HEVC encoder on the Amlogic WAVE420L (S905X3/SM1),
 * on top of the w4_enc instance API: FFmpeg hevc_v4l2m2m and GStreamer
 * v4l2h265enc work on it.
 *
 * OUTPUT: NV12/NV21, single or two planes, Y pitch ALIGN(w, 32), read by the
 * encoder in place. CAPTURE: HEVC Annex-B written by the encoder in place,
 * VPS/SPS/PPS before every IDR. Rate control by the firmware.
 */
#include <linux/module.h>
#include <linux/platform_device.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-device.h>
#include <media/v4l2-event.h>
#include <media/v4l2-ioctl.h>
#include <media/v4l2-mem2mem.h>
#include <media/videobuf2-dma-contig.h>

#include "w4enc.h"

#define VENC_NAME	"meson-venc-hevc"
#define MIN_W		256
#define MIN_H		128
#define MAX_W		4096
#define MAX_H		2304
/* a coded frame: half the raw size, at least 2 MiB */
#define CAP_SIZE(w, h)	max_t(u32, SZ_2M, ALIGN((w) * (h) * 3 / 4, SZ_4K))

struct venc_fmt {
	u32 fourcc;
	u32 planes;
	bool nv21;
};

static const struct venc_fmt venc_formats[] = {
	{ V4L2_PIX_FMT_NV12M, 2, false },
	{ V4L2_PIX_FMT_NV12, 1, false },
	{ V4L2_PIX_FMT_NV21M, 2, true },
	{ V4L2_PIX_FMT_NV21, 1, true },
};

struct venc_dev {
	struct v4l2_device v4l2;
	struct video_device vfd;
	struct v4l2_m2m_dev *m2m;
	struct mutex lock;		/* vfd and queue ioctls */
	struct device *dev;
};

struct venc_ctx {
	struct v4l2_fh fh;
	struct venc_dev *vdev;
	struct v4l2_ctrl_handler hdl;
	struct v4l2_ctrl *bitrate, *rc_enable, *gop, *qp_i, *qp_p;
	struct v4l2_ctrl *qp_min, *qp_max;
	struct work_struct work;
	struct mutex done_lock;		/* job completion vs CMD_STOP */

	const struct venc_fmt *fmt;
	u32 width, height, bpl;
	u32 fps_num, fps_den;
	u32 cap_seq, out_seq;

	struct w4_enc *enc;		/* while streaming */
	u32 frame;			/* since the last IDR */
	bool force_idr;
};

static struct venc_dev *venc;

static inline struct venc_ctx *fh_to_ctx(struct v4l2_fh *fh)
{
	return container_of(fh, struct venc_ctx, fh);
}

static const struct venc_fmt *venc_find_fmt(u32 fourcc)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(venc_formats); i++)
		if (venc_formats[i].fourcc == fourcc)
			return &venc_formats[i];
	return NULL;
}

/* Y plane rows the encoder reads: it pads the height to 8 */
static u32 venc_y_size(u32 bpl, u32 h)
{
	return bpl * ALIGN(h, 8);
}

/* ---- encoding ---- */

static void venc_send_eos(struct venc_ctx *ctx)
{
	static const struct v4l2_event ev = { .type = V4L2_EVENT_EOS };
	struct v4l2_m2m_ctx *m2m = ctx->fh.m2m_ctx;
	struct vb2_v4l2_buffer *last = v4l2_m2m_dst_buf_remove(m2m);

	/*
	 * End with an empty LAST buffer: FFmpeg drops encoded data carrying
	 * LAST. Without a free one, the next queued CAPTURE buffer gets it.
	 */
	if (last) {
		vb2_set_plane_payload(&last->vb2_buf, 0, 0);
		last->sequence = ctx->cap_seq++;
		v4l2_m2m_last_buffer_done(m2m, last);
	} else {
		m2m->next_buf_last = true;
	}
	v4l2_event_queue_fh(&ctx->fh, &ev);
}

static int venc_start_enc(struct venc_ctx *ctx)
{
	struct w4_enc_cfg cfg = {
		.width = ctx->width,
		.height = ctx->height,
		.fps = max(1U, DIV_ROUND_CLOSEST(ctx->fps_num, ctx->fps_den)),
		.bitrate = ctx->rc_enable->val ? ctx->bitrate->val : 0,
		.qp_i = ctx->qp_i->val,
		.qp_p = ctx->qp_p->val,
		.qp_min = ctx->qp_min->val,
		.qp_max = ctx->qp_max->val,
	};

	ctx->enc = w4_enc_open(&cfg);
	if (IS_ERR(ctx->enc)) {
		int ret = PTR_ERR(ctx->enc);

		ctx->enc = NULL;
		return ret;
	}
	ctx->frame = 0;
	return 0;
}

static void venc_work(struct work_struct *work)
{
	struct venc_ctx *ctx = container_of(work, struct venc_ctx, work);
	struct v4l2_m2m_ctx *m2m = ctx->fh.m2m_ctx;
	struct vb2_v4l2_buffer *src, *dst;
	enum vb2_buffer_state state = VB2_BUF_STATE_ERROR;
	bool key = false, idr, last;
	u32 size = 0;
	dma_addr_t y, uv;
	int ret;

	src = v4l2_m2m_next_src_buf(m2m);
	dst = v4l2_m2m_next_dst_buf(m2m);

	if (!ctx->enc) {
		ret = venc_start_enc(ctx);
		if (ret) {
			dev_err(ctx->vdev->dev, "encoder instance: %d\n", ret);
			goto done;
		}
	}

	idr = !ctx->frame || READ_ONCE(ctx->force_idr) ||
	      (ctx->gop->val && ctx->frame >= ctx->gop->val);
	if (idr) {
		ctx->frame = 0;
		WRITE_ONCE(ctx->force_idr, false);
	}
	ctx->frame++;
	y = vb2_dma_contig_plane_dma_addr(&src->vb2_buf, 0);
	/* single plane: UV follows height (not padded) rows per V4L2 spec */
	uv = ctx->fmt->planes == 2 ?
	     vb2_dma_contig_plane_dma_addr(&src->vb2_buf, 1) :
	     y + ctx->bpl * ctx->height;
	ret = w4_enc_frame(ctx->enc, y, uv, ctx->bpl, ctx->fmt->nv21, idr,
			   vb2_dma_contig_plane_dma_addr(&dst->vb2_buf, 0),
			   vb2_plane_size(&dst->vb2_buf, 0), &size, &key);
	if (ret)
		dev_err(ctx->vdev->dev, "frame %u: %d\n", ctx->out_seq, ret);
	else
		state = VB2_BUF_STATE_DONE;

done:
	/*
	 * CMD_STOP may arrive mid-encode: with src still queued it makes src
	 * the last buffer, after removal the core ends the stream itself.
	 */
	mutex_lock(&ctx->done_lock);
	src = v4l2_m2m_src_buf_remove(m2m);
	dst = v4l2_m2m_dst_buf_remove(m2m);
	last = v4l2_m2m_is_last_draining_src_buf(m2m, src);
	src->sequence = ctx->out_seq++;
	v4l2_m2m_buf_copy_metadata(src, dst);
	dst->flags &= ~(V4L2_BUF_FLAG_KEYFRAME | V4L2_BUF_FLAG_PFRAME);
	dst->flags |= key ? V4L2_BUF_FLAG_KEYFRAME : V4L2_BUF_FLAG_PFRAME;
	dst->sequence = ctx->cap_seq++;
	vb2_set_plane_payload(&dst->vb2_buf, 0, size);
	v4l2_m2m_buf_done(src, state);
	v4l2_m2m_buf_done(dst, state);
	if (last)
		venc_send_eos(ctx);
	mutex_unlock(&ctx->done_lock);
	v4l2_m2m_job_finish(ctx->vdev->m2m, m2m);
}

static void venc_device_run(void *priv)
{
	struct venc_ctx *ctx = priv;

	schedule_work(&ctx->work);	/* encoding sleeps */
}

static const struct v4l2_m2m_ops venc_m2m_ops = {
	.device_run = venc_device_run,
};

/* ---- vb2 ---- */

static void venc_plane_sizes(struct venc_ctx *ctx, u32 sizes[2])
{
	u32 ysize = venc_y_size(ctx->bpl, ctx->height);

	if (ctx->fmt->planes == 2) {
		sizes[0] = ysize;
		sizes[1] = ysize / 2;
	} else {
		sizes[0] = ysize * 3 / 2;
	}
}

static int venc_queue_setup(struct vb2_queue *vq, unsigned int *nbuffers,
			    unsigned int *nplanes, unsigned int sizes[],
			    struct device *alloc_devs[])
{
	struct venc_ctx *ctx = vb2_get_drv_priv(vq);
	unsigned int i, n;
	u32 want[2];

	if (V4L2_TYPE_IS_OUTPUT(vq->type)) {
		n = ctx->fmt->planes;
		venc_plane_sizes(ctx, want);
	} else {
		n = 1;
		want[0] = CAP_SIZE(ctx->width, ctx->height);
	}
	if (*nplanes) {
		if (*nplanes != n)
			return -EINVAL;
		for (i = 0; i < n; i++)
			if (sizes[i] < want[i])
				return -EINVAL;
		return 0;
	}
	*nplanes = n;
	for (i = 0; i < n; i++)
		sizes[i] = want[i];
	return 0;
}

static int venc_buf_prepare(struct vb2_buffer *vb)
{
	struct venc_ctx *ctx = vb2_get_drv_priv(vb->vb2_queue);
	unsigned int i;
	u32 need[2];

	if (V4L2_TYPE_IS_OUTPUT(vb->type))
		venc_plane_sizes(ctx, need);
	else
		need[0] = 0;
	for (i = 0; i < vb->num_planes; i++) {
		if (vb2_plane_size(vb, i) < need[i])
			return -EINVAL;
		/* the encoder takes 32-bit physical addresses */
		if (vb2_dma_contig_plane_dma_addr(vb, i) +
		    vb2_plane_size(vb, i) > SZ_4G)
			return -EINVAL;
	}
	return 0;
}

static void venc_buf_queue(struct vb2_buffer *vb)
{
	struct venc_ctx *ctx = vb2_get_drv_priv(vb->vb2_queue);
	struct vb2_v4l2_buffer *vbuf = to_vb2_v4l2_buffer(vb);

	if (V4L2_TYPE_IS_CAPTURE(vb->type) && vb2_is_streaming(vb->vb2_queue) &&
	    v4l2_m2m_dst_buf_is_last(ctx->fh.m2m_ctx)) {
		vb2_set_plane_payload(vb, 0, 0);
		vbuf->sequence = ctx->cap_seq++;
		v4l2_m2m_last_buffer_done(ctx->fh.m2m_ctx, vbuf);
		return;
	}
	v4l2_m2m_buf_queue(ctx->fh.m2m_ctx, vbuf);
}

static int venc_start_streaming(struct vb2_queue *vq, unsigned int count)
{
	struct venc_ctx *ctx = vb2_get_drv_priv(vq);

	v4l2_m2m_update_start_streaming_state(ctx->fh.m2m_ctx, vq);
	if (V4L2_TYPE_IS_OUTPUT(vq->type))
		ctx->out_seq = 0;
	else
		ctx->cap_seq = 0;
	return 0;
}

static void venc_stop_streaming(struct vb2_queue *vq)
{
	struct venc_ctx *ctx = vb2_get_drv_priv(vq);
	struct vb2_v4l2_buffer *vbuf;

	cancel_work_sync(&ctx->work);
	for (;;) {
		vbuf = V4L2_TYPE_IS_OUTPUT(vq->type) ?
		       v4l2_m2m_src_buf_remove(ctx->fh.m2m_ctx) :
		       v4l2_m2m_dst_buf_remove(ctx->fh.m2m_ctx);
		if (!vbuf)
			break;
		v4l2_m2m_buf_done(vbuf, VB2_BUF_STATE_ERROR);
	}
	v4l2_m2m_update_stop_streaming_state(ctx->fh.m2m_ctx, vq);
	if (V4L2_TYPE_IS_OUTPUT(vq->type) &&
	    v4l2_m2m_has_stopped(ctx->fh.m2m_ctx)) {
		static const struct v4l2_event ev = { .type = V4L2_EVENT_EOS };

		v4l2_event_queue_fh(&ctx->fh, &ev);
	}
	/* a new sequence (maybe a new size) starts with headers and an IDR */
	if (V4L2_TYPE_IS_OUTPUT(vq->type) && ctx->enc) {
		w4_enc_close(ctx->enc);
		ctx->enc = NULL;
	}
}

static const struct vb2_ops venc_vb2_ops = {
	.queue_setup = venc_queue_setup,
	.buf_prepare = venc_buf_prepare,
	.buf_queue = venc_buf_queue,
	.start_streaming = venc_start_streaming,
	.stop_streaming = venc_stop_streaming,
};

static int venc_queue_init(void *priv, struct vb2_queue *src,
			   struct vb2_queue *dst)
{
	struct venc_ctx *ctx = priv;
	int ret;

	src->type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
	src->io_modes = VB2_MMAP | VB2_DMABUF;
	src->drv_priv = ctx;
	src->buf_struct_size = sizeof(struct v4l2_m2m_buffer);
	src->ops = &venc_vb2_ops;
	src->mem_ops = &vb2_dma_contig_memops;
	src->timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_COPY;
	src->lock = &ctx->vdev->lock;
	src->dev = ctx->vdev->dev;
	ret = vb2_queue_init(src);
	if (ret)
		return ret;

	dst->type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	dst->io_modes = VB2_MMAP | VB2_DMABUF;
	dst->drv_priv = ctx;
	dst->buf_struct_size = sizeof(struct v4l2_m2m_buffer);
	dst->ops = &venc_vb2_ops;
	dst->mem_ops = &vb2_dma_contig_memops;
	dst->timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_COPY;
	dst->lock = &ctx->vdev->lock;
	dst->dev = ctx->vdev->dev;
	return vb2_queue_init(dst);
}

/* ---- ioctls ---- */

static int venc_querycap(struct file *file, void *fh,
			 struct v4l2_capability *cap)
{
	strscpy(cap->driver, VENC_NAME, sizeof(cap->driver));
	strscpy(cap->card, "Amlogic WAVE420L HEVC Encoder", sizeof(cap->card));
	strscpy(cap->bus_info, "platform:" VENC_NAME, sizeof(cap->bus_info));
	return 0;
}

static int venc_enum_fmt_out(struct file *file, void *fh,
			     struct v4l2_fmtdesc *f)
{
	if (f->index >= ARRAY_SIZE(venc_formats))
		return -EINVAL;
	f->pixelformat = venc_formats[f->index].fourcc;
	return 0;
}

static int venc_enum_fmt_cap(struct file *file, void *fh,
			     struct v4l2_fmtdesc *f)
{
	if (f->index)
		return -EINVAL;
	f->pixelformat = V4L2_PIX_FMT_HEVC;
	f->flags = V4L2_FMT_FLAG_COMPRESSED;
	return 0;
}

static int venc_enum_framesizes(struct file *file, void *fh,
				struct v4l2_frmsizeenum *fs)
{
	if (fs->index || (!venc_find_fmt(fs->pixel_format) &&
			  fs->pixel_format != V4L2_PIX_FMT_HEVC))
		return -EINVAL;
	fs->type = V4L2_FRMSIZE_TYPE_STEPWISE;
	fs->stepwise.min_width = MIN_W;
	fs->stepwise.max_width = MAX_W;
	fs->stepwise.step_width = 2;
	fs->stepwise.min_height = MIN_H;
	fs->stepwise.max_height = MAX_H;
	fs->stepwise.step_height = 2;
	return 0;
}

/* fill a raw format for fmt/size; also the CAPTURE one when !fmt */
static void venc_fill_fmt(struct v4l2_pix_format_mplane *pix,
			  const struct venc_fmt *fmt, u32 w, u32 h)
{
	u32 bpl = ALIGN(w, 32), ysize = venc_y_size(bpl, h);

	pix->width = w;
	pix->height = h;
	pix->field = V4L2_FIELD_NONE;
	memset(pix->plane_fmt, 0, sizeof(pix->plane_fmt));
	memset(pix->reserved, 0, sizeof(pix->reserved));
	if (!fmt) {
		pix->pixelformat = V4L2_PIX_FMT_HEVC;
		pix->num_planes = 1;
		pix->plane_fmt[0].sizeimage = CAP_SIZE(w, h);
		return;
	}
	pix->pixelformat = fmt->fourcc;
	pix->num_planes = fmt->planes;
	pix->plane_fmt[0].bytesperline = bpl;
	if (fmt->planes == 2) {
		pix->plane_fmt[0].sizeimage = ysize;
		pix->plane_fmt[1].bytesperline = bpl;
		pix->plane_fmt[1].sizeimage = ysize / 2;
	} else {
		pix->plane_fmt[0].sizeimage = ysize * 3 / 2;
	}
	if (!pix->colorspace)
		pix->colorspace = V4L2_COLORSPACE_REC709;
}

static int venc_try_fmt_out(struct file *file, void *fh, struct v4l2_format *f)
{
	struct v4l2_pix_format_mplane *pix = &f->fmt.pix_mp;
	const struct venc_fmt *fmt = venc_find_fmt(pix->pixelformat);

	if (!fmt)
		fmt = &venc_formats[0];
	venc_fill_fmt(pix, fmt, clamp(ALIGN(pix->width, 2), MIN_W, MAX_W),
		      clamp(ALIGN(pix->height, 2), MIN_H, MAX_H));
	return 0;
}

static int venc_g_fmt_out(struct file *file, void *fh, struct v4l2_format *f)
{
	struct venc_ctx *ctx = fh_to_ctx(file_to_v4l2_fh(file));

	venc_fill_fmt(&f->fmt.pix_mp, ctx->fmt, ctx->width, ctx->height);
	return 0;
}

static int venc_s_fmt_out(struct file *file, void *fh, struct v4l2_format *f)
{
	struct venc_ctx *ctx = fh_to_ctx(file_to_v4l2_fh(file));
	struct vb2_queue *vq = v4l2_m2m_get_vq(ctx->fh.m2m_ctx, f->type);

	if (vb2_is_busy(vq))
		return -EBUSY;
	venc_try_fmt_out(file, fh, f);
	ctx->fmt = venc_find_fmt(f->fmt.pix_mp.pixelformat);
	ctx->width = f->fmt.pix_mp.width;
	ctx->height = f->fmt.pix_mp.height;
	ctx->bpl = f->fmt.pix_mp.plane_fmt[0].bytesperline;
	return 0;
}

static int venc_g_fmt_cap(struct file *file, void *fh, struct v4l2_format *f)
{
	struct venc_ctx *ctx = fh_to_ctx(file_to_v4l2_fh(file));

	venc_fill_fmt(&f->fmt.pix_mp, NULL, ctx->width, ctx->height);
	return 0;
}

static int venc_s_fmt_cap(struct file *file, void *fh, struct v4l2_format *f)
{
	struct venc_ctx *ctx = fh_to_ctx(file_to_v4l2_fh(file));
	struct vb2_queue *vq = v4l2_m2m_get_vq(ctx->fh.m2m_ctx, f->type);

	if (vb2_is_busy(vq))
		return -EBUSY;
	/* the coded size follows the OUTPUT format */
	return venc_g_fmt_cap(file, fh, f);
}

static int venc_g_parm(struct file *file, void *fh, struct v4l2_streamparm *a)
{
	struct venc_ctx *ctx = fh_to_ctx(file_to_v4l2_fh(file));

	if (!V4L2_TYPE_IS_OUTPUT(a->type))
		return -EINVAL;
	a->parm.output.capability = V4L2_CAP_TIMEPERFRAME;
	a->parm.output.timeperframe.numerator = ctx->fps_den;
	a->parm.output.timeperframe.denominator = ctx->fps_num;
	return 0;
}

static int venc_s_parm(struct file *file, void *fh, struct v4l2_streamparm *a)
{
	struct venc_ctx *ctx = fh_to_ctx(file_to_v4l2_fh(file));
	struct v4l2_fract *t = &a->parm.output.timeperframe;

	if (!V4L2_TYPE_IS_OUTPUT(a->type))
		return -EINVAL;
	if (t->numerator && t->denominator) {
		ctx->fps_den = t->numerator;
		ctx->fps_num = t->denominator;
	}
	return venc_g_parm(file, fh, a);
}

static int venc_encoder_cmd(struct file *file, void *fh,
			    struct v4l2_encoder_cmd *ec)
{
	struct venc_ctx *ctx = fh_to_ctx(file_to_v4l2_fh(file));
	int ret;

	mutex_lock(&ctx->done_lock);
	ret = v4l2_m2m_ioctl_encoder_cmd(file, fh, ec);
	/* nothing left to encode: the core ends the stream but sends no event */
	if (!ret && ec->cmd == V4L2_ENC_CMD_STOP &&
	    !ctx->fh.m2m_ctx->last_src_buf) {
		static const struct v4l2_event ev = { .type = V4L2_EVENT_EOS };

		v4l2_event_queue_fh(&ctx->fh, &ev);
	}
	mutex_unlock(&ctx->done_lock);
	return ret;
}

static int venc_subscribe_event(struct v4l2_fh *fh,
				const struct v4l2_event_subscription *sub)
{
	if (sub->type == V4L2_EVENT_EOS)
		return v4l2_event_subscribe(fh, sub, 0, NULL);
	return v4l2_ctrl_subscribe_event(fh, sub);
}

static const struct v4l2_ioctl_ops venc_ioctl_ops = {
	.vidioc_querycap = venc_querycap,
	.vidioc_enum_fmt_vid_cap = venc_enum_fmt_cap,
	.vidioc_enum_fmt_vid_out = venc_enum_fmt_out,
	.vidioc_enum_framesizes = venc_enum_framesizes,
	.vidioc_g_fmt_vid_cap_mplane = venc_g_fmt_cap,
	.vidioc_try_fmt_vid_cap_mplane = venc_g_fmt_cap,
	.vidioc_s_fmt_vid_cap_mplane = venc_s_fmt_cap,
	.vidioc_g_fmt_vid_out_mplane = venc_g_fmt_out,
	.vidioc_try_fmt_vid_out_mplane = venc_try_fmt_out,
	.vidioc_s_fmt_vid_out_mplane = venc_s_fmt_out,
	.vidioc_g_parm = venc_g_parm,
	.vidioc_s_parm = venc_s_parm,
	.vidioc_reqbufs = v4l2_m2m_ioctl_reqbufs,
	.vidioc_querybuf = v4l2_m2m_ioctl_querybuf,
	.vidioc_qbuf = v4l2_m2m_ioctl_qbuf,
	.vidioc_dqbuf = v4l2_m2m_ioctl_dqbuf,
	.vidioc_prepare_buf = v4l2_m2m_ioctl_prepare_buf,
	.vidioc_create_bufs = v4l2_m2m_ioctl_create_bufs,
	.vidioc_expbuf = v4l2_m2m_ioctl_expbuf,
	.vidioc_streamon = v4l2_m2m_ioctl_streamon,
	.vidioc_streamoff = v4l2_m2m_ioctl_streamoff,
	.vidioc_try_encoder_cmd = v4l2_m2m_ioctl_try_encoder_cmd,
	.vidioc_encoder_cmd = venc_encoder_cmd,
	.vidioc_subscribe_event = venc_subscribe_event,
	.vidioc_unsubscribe_event = v4l2_event_unsubscribe,
};

/* ---- controls ---- */

/* values are read when a stream starts; only the key frame button acts now */
static int venc_s_ctrl(struct v4l2_ctrl *ctrl)
{
	struct venc_ctx *ctx = container_of(ctrl->handler, struct venc_ctx,
					    hdl);

	if (ctrl->id == V4L2_CID_MPEG_VIDEO_FORCE_KEY_FRAME)
		WRITE_ONCE(ctx->force_idr, true);
	return 0;
}

static const struct v4l2_ctrl_ops venc_ctrl_ops = {
	.s_ctrl = venc_s_ctrl,
};

static int venc_init_ctrls(struct venc_ctx *ctx)
{
	struct v4l2_ctrl_handler *hdl = &ctx->hdl;
	const struct v4l2_ctrl_ops *ops = &venc_ctrl_ops;

	v4l2_ctrl_handler_init(hdl, 14);
	ctx->bitrate = v4l2_ctrl_new_std(hdl, ops, V4L2_CID_MPEG_VIDEO_BITRATE,
					 100000, 40000000, 1, 4000000);
	ctx->rc_enable = v4l2_ctrl_new_std(hdl, ops,
					   V4L2_CID_MPEG_VIDEO_FRAME_RC_ENABLE,
					   0, 1, 1, 1);
	v4l2_ctrl_new_std_menu(hdl, ops, V4L2_CID_MPEG_VIDEO_BITRATE_MODE,
			       V4L2_MPEG_VIDEO_BITRATE_MODE_VBR,
			       ~BIT(V4L2_MPEG_VIDEO_BITRATE_MODE_VBR),
			       V4L2_MPEG_VIDEO_BITRATE_MODE_VBR);
	ctx->gop = v4l2_ctrl_new_std(hdl, ops, V4L2_CID_MPEG_VIDEO_GOP_SIZE,
				     0, 65535, 1, 60);
	ctx->qp_i = v4l2_ctrl_new_std(hdl, ops,
				      V4L2_CID_MPEG_VIDEO_HEVC_I_FRAME_QP,
				      0, 51, 1, 30);
	ctx->qp_p = v4l2_ctrl_new_std(hdl, ops,
				      V4L2_CID_MPEG_VIDEO_HEVC_P_FRAME_QP,
				      0, 51, 1, 31);
	ctx->qp_min = v4l2_ctrl_new_std(hdl, ops,
					V4L2_CID_MPEG_VIDEO_HEVC_MIN_QP,
					0, 51, 1, 8);
	ctx->qp_max = v4l2_ctrl_new_std(hdl, ops,
					V4L2_CID_MPEG_VIDEO_HEVC_MAX_QP,
					0, 51, 1, 51);
	v4l2_ctrl_new_std(hdl, ops, V4L2_CID_MPEG_VIDEO_FORCE_KEY_FRAME,
			  0, 0, 0, 0);
	/* fixed: no B frames, Main profile */
	v4l2_ctrl_new_std(hdl, ops, V4L2_CID_MPEG_VIDEO_B_FRAMES, 0, 0, 1, 0);
	v4l2_ctrl_new_std_menu(hdl, ops, V4L2_CID_MPEG_VIDEO_HEVC_PROFILE,
			       V4L2_MPEG_VIDEO_HEVC_PROFILE_MAIN,
			       ~BIT(V4L2_MPEG_VIDEO_HEVC_PROFILE_MAIN),
			       V4L2_MPEG_VIDEO_HEVC_PROFILE_MAIN);
	/*
	 * ponytail: any level is accepted (GStreamer fixates the list to its
	 * first entry and fails if refused); the firmware picks the level for
	 * the size. Pass it to SEQ_PIC_PARAM if a declared level ever matters.
	 */
	v4l2_ctrl_new_std_menu(hdl, ops, V4L2_CID_MPEG_VIDEO_HEVC_LEVEL,
			       V4L2_MPEG_VIDEO_HEVC_LEVEL_6_2, 0,
			       V4L2_MPEG_VIDEO_HEVC_LEVEL_4_1);
	v4l2_ctrl_new_std_menu(hdl, ops, V4L2_CID_MPEG_VIDEO_HEADER_MODE,
			       V4L2_MPEG_VIDEO_HEADER_MODE_JOINED_WITH_1ST_FRAME,
			       ~BIT(V4L2_MPEG_VIDEO_HEADER_MODE_JOINED_WITH_1ST_FRAME),
			       V4L2_MPEG_VIDEO_HEADER_MODE_JOINED_WITH_1ST_FRAME);
	v4l2_ctrl_new_std(hdl, ops, V4L2_CID_MIN_BUFFERS_FOR_OUTPUT, 1, 1, 1, 1);
	if (hdl->error) {
		int ret = hdl->error;

		v4l2_ctrl_handler_free(hdl);
		return ret;
	}
	ctx->fh.ctrl_handler = hdl;
	return 0;
}

/* ---- file ops ---- */

static int venc_open(struct file *file)
{
	struct venc_ctx *ctx;
	int ret;

	ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;
	ctx->vdev = venc;
	ctx->fmt = &venc_formats[0];
	ctx->width = 1280;
	ctx->height = 720;
	ctx->bpl = ALIGN(ctx->width, 32);
	ctx->fps_num = 30;
	ctx->fps_den = 1;
	INIT_WORK(&ctx->work, venc_work);
	mutex_init(&ctx->done_lock);
	v4l2_fh_init(&ctx->fh, &venc->vfd);
	ret = venc_init_ctrls(ctx);
	if (ret)
		goto err_fh;
	ctx->fh.m2m_ctx = v4l2_m2m_ctx_init(venc->m2m, ctx, venc_queue_init);
	if (IS_ERR(ctx->fh.m2m_ctx)) {
		ret = PTR_ERR(ctx->fh.m2m_ctx);
		goto err_ctrls;
	}
	v4l2_fh_add(&ctx->fh, file);
	return 0;

err_ctrls:
	v4l2_ctrl_handler_free(&ctx->hdl);
err_fh:
	v4l2_fh_exit(&ctx->fh);
	kfree(ctx);
	return ret;
}

static int venc_release(struct file *file)
{
	struct venc_ctx *ctx = fh_to_ctx(file_to_v4l2_fh(file));

	mutex_lock(&venc->lock);
	v4l2_m2m_ctx_release(ctx->fh.m2m_ctx);
	mutex_unlock(&venc->lock);
	cancel_work_sync(&ctx->work);
	if (ctx->enc)
		w4_enc_close(ctx->enc);
	v4l2_ctrl_handler_free(&ctx->hdl);
	v4l2_fh_del(&ctx->fh, file);
	v4l2_fh_exit(&ctx->fh);
	kfree(ctx);
	return 0;
}

static const struct v4l2_file_operations venc_fops = {
	.owner = THIS_MODULE,
	.open = venc_open,
	.release = venc_release,
	.poll = v4l2_m2m_fop_poll,
	.unlocked_ioctl = video_ioctl2,
	.mmap = v4l2_m2m_fop_mmap,
};

int w4_v4l2_register(struct device *dev)
{
	struct video_device *vfd;
	int ret;

	venc = kzalloc(sizeof(*venc), GFP_KERNEL);
	if (!venc)
		return -ENOMEM;
	venc->dev = dev;
	mutex_init(&venc->lock);
	ret = v4l2_device_register(dev, &venc->v4l2);
	if (ret)
		goto err_free;
	venc->m2m = v4l2_m2m_init(&venc_m2m_ops);
	if (IS_ERR(venc->m2m)) {
		ret = PTR_ERR(venc->m2m);
		goto err_v4l2;
	}
	vfd = &venc->vfd;
	strscpy(vfd->name, VENC_NAME, sizeof(vfd->name));
	vfd->fops = &venc_fops;
	vfd->ioctl_ops = &venc_ioctl_ops;
	vfd->release = video_device_release_empty;
	vfd->lock = &venc->lock;
	vfd->v4l2_dev = &venc->v4l2;
	vfd->vfl_dir = VFL_DIR_M2M;
	vfd->device_caps = V4L2_CAP_VIDEO_M2M_MPLANE | V4L2_CAP_STREAMING;
	ret = video_register_device(vfd, VFL_TYPE_VIDEO, -1);
	if (ret)
		goto err_m2m;
	dev_info(dev, "V4L2 HEVC encoder at /dev/video%d\n", vfd->num);
	return 0;

err_m2m:
	v4l2_m2m_release(venc->m2m);
err_v4l2:
	v4l2_device_unregister(&venc->v4l2);
err_free:
	kfree(venc);
	venc = NULL;
	return ret;
}

void w4_v4l2_unregister(void)
{
	if (!venc)
		return;
	video_unregister_device(&venc->vfd);
	v4l2_m2m_release(venc->m2m);
	v4l2_device_unregister(&venc->v4l2);
	kfree(venc);
	venc = NULL;
}
