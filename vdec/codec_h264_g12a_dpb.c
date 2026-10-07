// SPDX-License-Identifier: GPL-2.0+

#include <linux/string.h>

#include <media/v4l2-h264.h>

#include "codec_h264_g12a_dpb.h"

void h264_multi_poc_reset(struct h264_multi_poc_state *state)
{
	memset(state, 0, sizeof(*state));
}

static void h264_multi_poc_type0(struct h264_multi_poc_state *state,
				 const struct h264_multi_config *config,
				 const struct h264_multi_picture *picture,
				 struct h264_multi_poc *poc)
{
	s32 prev_msb = state->prev_pic_order_cnt_msb;
	s32 prev_lsb = state->prev_pic_order_cnt_lsb;
	s32 msb;

	if (picture->nal_unit_type == 5) {
		prev_msb = 0;
		prev_lsb = 0;
	} else if (state->prev_has_mmco5) {
		prev_msb = 0;
		prev_lsb = state->prev_bottom_field ? 0 :
			state->prev_top_field_order_cnt;
	}

	if (picture->pic_order_cnt_lsb < prev_lsb &&
	    prev_lsb - picture->pic_order_cnt_lsb >=
	    config->max_pic_order_cnt_lsb / 2)
		msb = prev_msb + config->max_pic_order_cnt_lsb;
	else if (picture->pic_order_cnt_lsb > prev_lsb &&
		 picture->pic_order_cnt_lsb - prev_lsb >
		 config->max_pic_order_cnt_lsb / 2)
		msb = prev_msb - config->max_pic_order_cnt_lsb;
	else
		msb = prev_msb;

	state->current_pic_order_cnt_msb = msb;
	if (!picture->field_pic) {
		poc->top = msb + picture->pic_order_cnt_lsb;
		poc->bottom = poc->top + picture->delta_pic_order_cnt_bottom;
	} else if (picture->bottom_field) {
		poc->bottom = msb + picture->pic_order_cnt_lsb;
		poc->top = poc->bottom;
	} else {
		poc->top = msb + picture->pic_order_cnt_lsb;
		poc->bottom = poc->top;
	}
}

static s32 h264_multi_frame_num_offset(struct h264_multi_poc_state *state,
				       const struct h264_multi_config *config,
				       const struct h264_multi_picture *picture)
{
	if (picture->nal_unit_type == 5 || state->prev_has_mmco5)
		return 0;
	if (picture->frame_num < state->prev_frame_num)
		return state->prev_frame_num_offset + config->max_frame_num;

	return state->prev_frame_num_offset;
}

static void h264_multi_poc_type1(struct h264_multi_poc_state *state,
				 const struct h264_multi_config *config,
				 const struct h264_multi_picture *picture,
				 struct h264_multi_poc *poc)
{
	s32 expected_delta = 0;
	s32 expected_poc = 0;
	s32 abs_frame_num;
	s32 cycle;
	s32 cycle_frame;
	unsigned int i;

	state->current_frame_num_offset =
		h264_multi_frame_num_offset(state, config, picture);
	abs_frame_num = config->num_ref_frames_in_poc_cycle ?
		state->current_frame_num_offset + picture->frame_num : 0;
	if (!picture->nal_ref_idc && abs_frame_num > 0)
		abs_frame_num--;

	for (i = 0; i < config->num_ref_frames_in_poc_cycle; i++)
		expected_delta += config->offset_for_ref_frame[i];
	if (abs_frame_num > 0) {
		cycle = (abs_frame_num - 1) /
			config->num_ref_frames_in_poc_cycle;
		cycle_frame = (abs_frame_num - 1) %
			config->num_ref_frames_in_poc_cycle;
		expected_poc = cycle * expected_delta;
		for (i = 0; i <= cycle_frame; i++)
			expected_poc += config->offset_for_ref_frame[i];
	}
	if (!picture->nal_ref_idc)
		expected_poc += config->offset_for_non_ref_pic;

	if (!picture->field_pic) {
		poc->top = expected_poc + picture->delta_pic_order_cnt[0];
		poc->bottom = poc->top +
			config->offset_for_top_to_bottom_field +
			picture->delta_pic_order_cnt[1];
	} else if (picture->bottom_field) {
		poc->bottom = expected_poc +
			config->offset_for_top_to_bottom_field +
			picture->delta_pic_order_cnt[0];
		poc->top = poc->bottom;
	} else {
		poc->top = expected_poc + picture->delta_pic_order_cnt[0];
		poc->bottom = poc->top;
	}
}

static void h264_multi_poc_type2(struct h264_multi_poc_state *state,
				 const struct h264_multi_config *config,
				 const struct h264_multi_picture *picture,
				 struct h264_multi_poc *poc)
{
	s32 temp_poc;

	state->current_frame_num_offset =
		h264_multi_frame_num_offset(state, config, picture);
	if (picture->nal_unit_type == 5)
		temp_poc = 0;
	else if (!picture->nal_ref_idc)
		temp_poc = 2 * (state->current_frame_num_offset +
			picture->frame_num) - 1;
	else
		temp_poc = 2 * (state->current_frame_num_offset +
			picture->frame_num);
	poc->top = temp_poc;
	poc->bottom = temp_poc;
}

int h264_multi_poc_derive(struct h264_multi_poc_state *state,
			  const struct h264_multi_config *config,
			  const struct h264_multi_picture *picture,
			  struct h264_multi_poc *poc)
{
	if (!state || !config || !picture || !poc)
		return -EINVAL;

	switch (config->pic_order_cnt_type) {
	case 0:
		if (!config->max_pic_order_cnt_lsb)
			return -EINVAL;
		h264_multi_poc_type0(state, config, picture, poc);
		break;
	case 1:
		if (config->num_ref_frames_in_poc_cycle >
		    ARRAY_SIZE(config->offset_for_ref_frame))
			return -EINVAL;
		h264_multi_poc_type1(state, config, picture, poc);
		break;
	case 2:
		h264_multi_poc_type2(state, config, picture, poc);
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

void h264_multi_poc_commit(struct h264_multi_poc_state *state,
			   const struct h264_multi_config *config,
			   const struct h264_multi_picture *picture,
			   const struct h264_multi_poc *poc,
			   bool has_mmco5)
{
	if (!picture->nal_ref_idc)
		return;

	if (config->pic_order_cnt_type == 0 && !has_mmco5) {
		state->prev_pic_order_cnt_msb = state->current_pic_order_cnt_msb;
		state->prev_pic_order_cnt_lsb = picture->pic_order_cnt_lsb;
	}
	state->prev_frame_num_offset = has_mmco5 ? 0 :
		state->current_frame_num_offset;
	state->prev_frame_num = has_mmco5 ? 0 : picture->frame_num;
	state->prev_has_mmco5 = has_mmco5;
	state->prev_bottom_field = picture->bottom_field;
	if (has_mmco5) {
		if (picture->field_pic)
			state->prev_top_field_order_cnt = 0;
		else
			state->prev_top_field_order_cnt =
				poc->top - min(poc->top, poc->bottom);
	} else {
		state->prev_top_field_order_cnt = poc->top;
	}
}

void h264_multi_dpb_reset(struct h264_multi_dpb *dpb)
{
	memset(dpb, 0, sizeof(*dpb));
	h264_multi_poc_reset(&dpb->poc_state);
	dpb->max_long_term_frame_idx = -1;
	dpb->current_long_term_frame_idx = -1;
}

struct h264_level_limit {
	u8 level_idc;
	u32 max_dpb_mbs;
};

static const struct h264_level_limit h264_level_limits[] = {
	{ 9, 396 },
	{ 10, 396 },
	{ 11, 900 },
	{ 12, 2376 },
	{ 13, 2376 },
	{ 20, 2376 },
	{ 21, 4752 },
	{ 22, 8100 },
	{ 30, 8100 },
	{ 31, 18000 },
	{ 32, 20480 },
	{ 40, 32768 },
	{ 41, 32768 },
	{ 42, 34816 },
	{ 50, 110400 },
	{ 51, 184320 },
	{ 52, 184320 },
};

int h264_multi_dpb_buf_count(const struct h264_multi_config *config,
			     unsigned int *buf_count)
{
	u32 frame_mbs;
	u32 dpb_frames;
	unsigned int i;

	if (!config || !buf_count || !config->coded_width ||
	    !config->coded_height)
		return -EINVAL;

	if (config->bitstream_restriction) {
		if (config->max_dec_frame_buffering > H264_MULTI_DPB_SIZE ||
		    config->num_reorder_frames >
			config->max_dec_frame_buffering ||
		    config->max_dec_frame_buffering < config->max_refs)
			return -EINVAL;
		dpb_frames = max_t(u32, 1, config->max_dec_frame_buffering);
	} else {
		frame_mbs = DIV_ROUND_UP(config->coded_width, 16) *
			DIV_ROUND_UP(config->coded_height, 16);
		for (i = 0; i < ARRAY_SIZE(h264_level_limits); i++) {
			if (h264_level_limits[i].level_idc == config->level_idc)
				break;
		}
		if (i == ARRAY_SIZE(h264_level_limits))
			return -EINVAL;

		dpb_frames = clamp(h264_level_limits[i].max_dpb_mbs /
				   frame_mbs, 1U,
				   (u32)H264_MULTI_DPB_SIZE);
		dpb_frames = max_t(u32, dpb_frames, config->max_refs);
	}

	*buf_count = dpb_frames + 1;
	return 0;
}

static void h264_multi_dpb_clear_refs(struct h264_multi_dpb *dpb)
{
	memset(dpb->slots, 0, sizeof(dpb->slots));
}

static s32 h264_multi_dpb_pic_num(const struct h264_multi_dpb_slot *slot,
				  const struct h264_multi_config *config,
				  const struct h264_multi_picture *picture)
{
	if (slot->long_term)
		return slot->long_term_frame_idx;
	if (slot->frame_num > picture->frame_num)
		return (s32)slot->frame_num - config->max_frame_num;

	return slot->frame_num;
}

static int h264_multi_dpb_find_pic_num(struct h264_multi_dpb *dpb,
				       const struct h264_multi_config *config,
				       const struct h264_multi_picture *picture,
				       s32 pic_num)
{
	unsigned int i;

	for (i = 0; i < H264_MULTI_DPB_SIZE; i++) {
		if (dpb->slots[i].active && !dpb->slots[i].long_term &&
		    h264_multi_dpb_pic_num(&dpb->slots[i], config, picture) ==
		    pic_num)
			return i;
	}

	return -ENOENT;
}

static int h264_multi_dpb_find_long_term(struct h264_multi_dpb *dpb,
					 u16 long_term_frame_idx)
{
	unsigned int i;

	for (i = 0; i < H264_MULTI_DPB_SIZE; i++) {
		if (dpb->slots[i].active && dpb->slots[i].long_term &&
		    dpb->slots[i].long_term_frame_idx == long_term_frame_idx)
			return i;
	}

	return -ENOENT;
}

static void h264_multi_dpb_unmark_long_term(struct h264_multi_dpb *dpb,
					    u16 long_term_frame_idx)
{
	int slot = h264_multi_dpb_find_long_term(dpb, long_term_frame_idx);

	if (slot >= 0)
		memset(&dpb->slots[slot], 0, sizeof(dpb->slots[slot]));
}

static void
h264_multi_dpb_sliding_window(struct h264_multi_dpb *dpb,
			      const struct h264_multi_config *config,
			      const struct h264_multi_picture *picture)
{
	s32 lowest_pic_num = S32_MAX;
	unsigned int refs = 0;
	int oldest = -1;
	unsigned int i;

	for (i = 0; i < H264_MULTI_DPB_SIZE; i++) {
		s32 pic_num;

		if (!dpb->slots[i].active)
			continue;
		refs++;
		if (dpb->slots[i].long_term)
			continue;
		pic_num = h264_multi_dpb_pic_num(&dpb->slots[i], config,
						 picture);
		if (pic_num < lowest_pic_num) {
			lowest_pic_num = pic_num;
			oldest = i;
		}
	}

	if (refs >= config->max_refs && oldest >= 0)
		memset(&dpb->slots[oldest], 0, sizeof(dpb->slots[oldest]));
}

static int h264_multi_dpb_apply_mmco(struct h264_multi_dpb *dpb,
				     const struct h264_multi_config *config,
				     const struct h264_multi_picture *picture,
				     const struct h264_multi_marking *marking,
				     bool *has_mmco5)
{
	s32 current_pic_num = picture->frame_num;
	unsigned int i;

	for (i = 0; i < marking->count; i++) {
		const struct h264_multi_mmco *op = &marking->ops[i];
		int slot;

		switch (op->opcode) {
		case 1:
			slot = h264_multi_dpb_find_pic_num(dpb, config, picture,
							   current_pic_num -
					(op->difference_of_pic_nums_minus1 + 1));
			if (slot >= 0)
				memset(&dpb->slots[slot], 0,
				       sizeof(dpb->slots[slot]));
			break;
		case 2:
			h264_multi_dpb_unmark_long_term(dpb,
							op->long_term_pic_num);
			break;
		case 3:
			h264_multi_dpb_unmark_long_term(dpb,
							op->long_term_frame_idx);
			slot = h264_multi_dpb_find_pic_num(dpb, config, picture,
							   current_pic_num -
					(op->difference_of_pic_nums_minus1 + 1));
			if (slot >= 0) {
				dpb->slots[slot].long_term = true;
				dpb->slots[slot].long_term_frame_idx =
					op->long_term_frame_idx;
			}
			break;
		case 4:
			dpb->max_long_term_frame_idx =
				op->max_long_term_frame_idx_plus1 - 1;
			for (slot = 0; slot < H264_MULTI_DPB_SIZE; slot++) {
				if (dpb->slots[slot].active &&
				    dpb->slots[slot].long_term &&
				    dpb->slots[slot].long_term_frame_idx >
				    dpb->max_long_term_frame_idx)
					memset(&dpb->slots[slot], 0,
					       sizeof(dpb->slots[slot]));
			}
			break;
		case 5:
			h264_multi_dpb_clear_refs(dpb);
			dpb->max_long_term_frame_idx = -1;
			*has_mmco5 = true;
			break;
		case 6:
			h264_multi_dpb_unmark_long_term(dpb,
							op->long_term_frame_idx);
			dpb->current_long_term_frame_idx =
				op->long_term_frame_idx;
			break;
		default:
			return -EINVAL;
		}
	}

	return 0;
}

static int h264_multi_dpb_store_current(struct h264_multi_dpb *dpb,
					const struct h264_multi_picture *picture,
					const struct h264_multi_poc *poc,
					u64 reference_ts,
					u32 buffer_index,
					bool has_mmco5)
{
	struct h264_multi_dpb_slot *slot;
	unsigned int i;

	for (i = 0; i < H264_MULTI_DPB_SIZE; i++) {
		if (!dpb->slots[i].active)
			break;
	}
	if (i == H264_MULTI_DPB_SIZE)
		return -ENOSPC;

	slot = &dpb->slots[i];
	slot->active = true;
	slot->reference_ts = reference_ts;
	slot->buffer_index = buffer_index;
	slot->frame_num = has_mmco5 ? 0 : picture->frame_num;
	slot->top_field_order_cnt = poc->top;
	slot->bottom_field_order_cnt = poc->bottom;
	slot->fields = V4L2_H264_FRAME_REF;
	if (dpb->current_long_term_frame_idx >= 0) {
		slot->long_term = true;
		slot->long_term_frame_idx = dpb->current_long_term_frame_idx;
	}

	return 0;
}

int h264_multi_dpb_begin(struct h264_multi_dpb *dpb,
			 const struct h264_multi_config *config,
			 const struct h264_multi_picture *picture,
			 struct h264_multi_poc *poc)
{
	int ret;

	if (picture->field_pic)
		return -EOPNOTSUPP;
	ret = h264_multi_poc_derive(&dpb->poc_state, config, picture, poc);
	if (ret)
		return ret;
	if (picture->nal_unit_type == 5) {
		h264_multi_dpb_clear_refs(dpb);
		dpb->max_long_term_frame_idx = -1;
	}
	dpb->current_long_term_frame_idx = -1;

	return 0;
}

int h264_multi_dpb_finish(struct h264_multi_dpb *dpb,
			  const struct h264_multi_config *config,
			  const struct h264_multi_picture *picture,
			  const struct h264_multi_marking *marking,
			  const struct h264_multi_poc *poc,
			  u64 reference_ts, u32 buffer_index)
{
	bool has_mmco5 = false;
	int ret = 0;

	if (!picture->nal_ref_idc)
		goto commit;

	if (picture->nal_unit_type == 5) {
		if (marking->long_term_reference) {
			dpb->max_long_term_frame_idx = 0;
			dpb->current_long_term_frame_idx = 0;
		}
	} else if (marking->adaptive) {
		ret = h264_multi_dpb_apply_mmco(dpb, config, picture, marking,
						&has_mmco5);
		if (ret)
			return ret;
	} else if (!config->max_refs) {
		goto commit;
	} else {
		h264_multi_dpb_sliding_window(dpb, config, picture);
	}

	ret = h264_multi_dpb_store_current(dpb, picture, poc, reference_ts,
					   buffer_index, has_mmco5);
	if (ret)
		return ret;
commit:
	h264_multi_poc_commit(&dpb->poc_state, config, picture, poc,
			      has_mmco5);
	return ret;
}

void h264_multi_dpb_picture_reset(struct h264_multi_dpb_picture *pic_state)
{
	memset(pic_state, 0, sizeof(*pic_state));
}

static bool
h264_multi_same_picture(const struct h264_multi_picture *pic_state,
			const struct h264_multi_picture *next)
{
	return pic_state->frame_num == next->frame_num &&
		pic_state->nal_unit_type == next->nal_unit_type &&
		!!pic_state->nal_ref_idc == !!next->nal_ref_idc &&
		pic_state->field_pic == next->field_pic &&
		pic_state->bottom_field == next->bottom_field &&
		pic_state->pic_order_cnt_lsb == next->pic_order_cnt_lsb &&
		pic_state->delta_pic_order_cnt_bottom ==
			next->delta_pic_order_cnt_bottom &&
		pic_state->delta_pic_order_cnt[0] == next->delta_pic_order_cnt[0] &&
		pic_state->delta_pic_order_cnt[1] == next->delta_pic_order_cnt[1];
}

int h264_multi_dpb_picture_begin(struct h264_multi_dpb *dpb,
				 const struct h264_multi_config *config,
				 struct h264_multi_dpb_picture *pic_state,
				 const struct h264_multi_picture *picture,
				 u32 buffer_index,
				 enum h264_multi_slice_action *action)
{
	int ret;

	if (!dpb || !config || !pic_state || !picture || !action)
		return -EINVAL;

	if (!pic_state->active) {
		if (picture->first_mb_in_slice)
			return -EINVAL;

		ret = h264_multi_dpb_begin(dpb, config, picture,
					   &pic_state->poc);
		if (ret)
			return ret;

		pic_state->picture = *picture;
		pic_state->buffer_index = buffer_index;
		pic_state->active = true;
		*action = H264_MULTI_SLICE_NEW_PICTURE;
		return 0;
	}

	if (!picture->first_mb_in_slice ||
	    !h264_multi_same_picture(&pic_state->picture, picture))
		return -EPIPE;

	pic_state->picture = *picture;
	*action = H264_MULTI_SLICE_CONTINUE;
	return 0;
}

int h264_multi_dpb_picture_finish(struct h264_multi_dpb *dpb,
				  const struct h264_multi_config *config,
				  struct h264_multi_dpb_picture *pic_state,
				  const struct h264_multi_marking *marking,
				  u64 reference_ts, u32 buffer_index)
{
	int ret;

	if (!dpb || !config || !pic_state || !marking)
		return -EINVAL;
	if (!pic_state->active)
		return -EPIPE;

	ret = h264_multi_dpb_finish(dpb, config, &pic_state->picture, marking,
				    &pic_state->poc, reference_ts, buffer_index);
	if (!ret)
		h264_multi_dpb_picture_reset(pic_state);

	return ret;
}

void h264_multi_dpb_to_v4l2(const struct h264_multi_dpb *dpb,
			    const struct h264_multi_config *config,
			    const struct h264_multi_picture *picture,
			    struct v4l2_h264_dpb_entry *entries)
{
	unsigned int i;

	memset(entries, 0, sizeof(*entries) * H264_MULTI_DPB_SIZE);
	for (i = 0; i < H264_MULTI_DPB_SIZE; i++) {
		const struct h264_multi_dpb_slot *slot = &dpb->slots[i];

		if (!slot->active)
			continue;
		entries[i].reference_ts = slot->reference_ts;
		entries[i].frame_num = slot->long_term ?
			slot->long_term_frame_idx : slot->frame_num;
		entries[i].pic_num = h264_multi_dpb_pic_num(slot, config,
							    picture);
		entries[i].top_field_order_cnt = slot->top_field_order_cnt;
		entries[i].bottom_field_order_cnt =
			slot->bottom_field_order_cnt;
		entries[i].fields = slot->fields;
		entries[i].flags = V4L2_H264_DPB_ENTRY_FLAG_VALID |
			V4L2_H264_DPB_ENTRY_FLAG_ACTIVE;
		if (slot->long_term)
			entries[i].flags |= V4L2_H264_DPB_ENTRY_FLAG_LONG_TERM;
	}
}

static int
h264_multi_dpb_reorder_target(const struct h264_multi_dpb *dpb,
			      const struct h264_multi_config *config,
			      const struct h264_multi_picture *picture,
			      unsigned int idc, unsigned int value,
			      u32 *pic_num_pred,
			      struct v4l2_h264_reference *target)
{
	bool long_term = idc == 2;
	s32 pic_num;
	unsigned int i;

	if (idc < 2) {
		u32 difference = value + 1;
		u32 pic_num_no_wrap;

		if (!config->max_frame_num || difference > config->max_frame_num)
			return -EINVAL;
		if (idc == 0)
			pic_num_no_wrap = (*pic_num_pred + config->max_frame_num -
					   difference) % config->max_frame_num;
		else
			pic_num_no_wrap = (*pic_num_pred + difference) %
					  config->max_frame_num;
		*pic_num_pred = pic_num_no_wrap;
		pic_num = pic_num_no_wrap > picture->frame_num ?
			(s32)pic_num_no_wrap - config->max_frame_num :
			pic_num_no_wrap;
	} else {
		pic_num = value;
	}

	for (i = 0; i < H264_MULTI_DPB_SIZE; i++) {
		const struct h264_multi_dpb_slot *slot = &dpb->slots[i];

		if (!slot->active || slot->long_term != long_term)
			continue;
		if (h264_multi_dpb_pic_num(slot, config, picture) != pic_num)
			continue;
		target->index = i;
		target->fields = V4L2_H264_FRAME_REF;
		return 0;
	}

	return -ENOENT;
}

int h264_multi_dpb_reorder_reflist(const struct h264_multi_dpb *dpb,
				   const struct h264_multi_config *config,
				   const struct h264_multi_picture *picture,
				   struct v4l2_h264_reference *refs,
				   unsigned int num_valid,
				   unsigned int num_active,
				   const u16 *commands,
				   unsigned int num_commands)
{
	struct v4l2_h264_reference reordered[V4L2_H264_REF_LIST_LEN];
	u32 pic_num_pred;
	unsigned int ref_idx = 0;
	unsigned int list_len;
	unsigned int pos = 0;

	if (!dpb || !config || !picture || !refs || !commands || !num_commands ||
	    num_valid > ARRAY_SIZE(reordered) ||
	    num_active > ARRAY_SIZE(reordered))
		return -EINVAL;
	if (!num_valid)
		return !num_active && commands[0] == 3 ? 0 : -EINVAL;

	pic_num_pred = picture->frame_num;
	list_len = num_valid;
	while (pos < num_commands) {
		struct v4l2_h264_reference target;
		unsigned int out;
		unsigned int i;
		u16 idc = commands[pos++];
		int ret;

		if (idc == 3)
			return list_len >= num_active ? 0 : -EINVAL;
		if (idc > 2 || pos >= num_commands ||
		    ref_idx >= ARRAY_SIZE(reordered))
			return -EINVAL;
		ret = h264_multi_dpb_reorder_target(dpb, config, picture, idc,
						    commands[pos++],
						    &pic_num_pred, &target);
		if (ret)
			return ret;

		/* A reference may be inserted again after the selected prefix. */
		memcpy(reordered, refs, ref_idx * sizeof(*refs));
		reordered[ref_idx] = target;
		out = ref_idx + 1;
		for (i = ref_idx; i < list_len &&
		     out < ARRAY_SIZE(reordered); i++) {
			if (refs[i].index == target.index)
				continue;
			reordered[out++] = refs[i];
		}
		memcpy(refs, reordered, out * sizeof(*refs));
		list_len = out;
		ref_idx++;
	}

	return -EINVAL;
}
