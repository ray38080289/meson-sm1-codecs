/* SPDX-License-Identifier: GPL-2.0+ */

#ifndef __MESON_VDEC_CODEC_H264_MULTI_DPB_H_
#define __MESON_VDEC_CODEC_H264_MULTI_DPB_H_

#include "codec_h264_g12a.h"

#define H264_MULTI_DPB_SIZE	V4L2_H264_NUM_DPB_ENTRIES

struct h264_multi_poc {
	s32 top;
	s32 bottom;
};

struct h264_multi_poc_state {
	s32 prev_pic_order_cnt_msb;
	s32 prev_pic_order_cnt_lsb;
	s32 prev_frame_num_offset;
	s32 current_pic_order_cnt_msb;
	s32 current_frame_num_offset;
	s32 prev_top_field_order_cnt;
	u16 prev_frame_num;
	bool prev_has_mmco5;
	bool prev_bottom_field;
};

struct h264_multi_dpb_slot {
	u64 reference_ts;
	u32 buffer_index;
	s32 top_field_order_cnt;
	s32 bottom_field_order_cnt;
	u16 frame_num;
	u16 long_term_frame_idx;
	u8 fields;
	bool active;
	bool long_term;
};

struct h264_multi_dpb {
	struct h264_multi_poc_state poc_state;
	struct h264_multi_dpb_slot slots[H264_MULTI_DPB_SIZE];
	s32 max_long_term_frame_idx;
	s32 current_long_term_frame_idx;
};

struct h264_multi_dpb_picture {
	struct h264_multi_picture picture;
	struct h264_multi_poc poc;
	struct vb2_v4l2_buffer *vbuf;
	u32 buffer_index;
	bool active;
};

enum h264_multi_slice_action {
	H264_MULTI_SLICE_NEW_PICTURE,
	H264_MULTI_SLICE_CONTINUE,
};

void h264_multi_poc_reset(struct h264_multi_poc_state *state);
int h264_multi_poc_derive(struct h264_multi_poc_state *state,
			  const struct h264_multi_config *config,
			  const struct h264_multi_picture *picture,
			  struct h264_multi_poc *poc);
void h264_multi_poc_commit(struct h264_multi_poc_state *state,
			   const struct h264_multi_config *config,
			   const struct h264_multi_picture *picture,
			   const struct h264_multi_poc *poc,
			   bool has_mmco5);
void h264_multi_dpb_reset(struct h264_multi_dpb *dpb);
int h264_multi_dpb_buf_count(const struct h264_multi_config *config,
			     unsigned int *buf_count);
int h264_multi_dpb_begin(struct h264_multi_dpb *dpb,
			 const struct h264_multi_config *config,
			 const struct h264_multi_picture *picture,
			 struct h264_multi_poc *poc);
int h264_multi_dpb_finish(struct h264_multi_dpb *dpb,
			  const struct h264_multi_config *config,
			  const struct h264_multi_picture *picture,
			  const struct h264_multi_marking *marking,
			  const struct h264_multi_poc *poc,
			  u64 reference_ts, u32 buffer_index);
void h264_multi_dpb_picture_reset(struct h264_multi_dpb_picture *pic_state);
int h264_multi_dpb_picture_begin(struct h264_multi_dpb *dpb,
				 const struct h264_multi_config *config,
				 struct h264_multi_dpb_picture *pic_state,
				 const struct h264_multi_picture *picture,
				 u32 buffer_index,
				 enum h264_multi_slice_action *action);
int h264_multi_dpb_picture_finish(struct h264_multi_dpb *dpb,
				  const struct h264_multi_config *config,
				  struct h264_multi_dpb_picture *pic_state,
				  const struct h264_multi_marking *marking,
				  u64 reference_ts, u32 buffer_index);
void h264_multi_dpb_to_v4l2(const struct h264_multi_dpb *dpb,
			    const struct h264_multi_config *config,
			    const struct h264_multi_picture *picture,
			    struct v4l2_h264_dpb_entry *entries);
int h264_multi_dpb_reorder_reflist(const struct h264_multi_dpb *dpb,
				   const struct h264_multi_config *config,
				   const struct h264_multi_picture *picture,
				   struct v4l2_h264_reference *refs,
				   unsigned int num_valid,
				   unsigned int num_active,
				   const u16 *commands,
				   unsigned int num_commands);

#endif
