/* SPDX-License-Identifier: GPL-2.0+ */

#ifndef __MESON_VDEC_CODEC_H264_MULTI_LMEM_H_
#define __MESON_VDEC_CODEC_H264_MULTI_LMEM_H_

#include <linux/build_bug.h>
#include <linux/types.h>

#define H264_MULTI_LMEM_PARAM_WORDS	0x100
#define H264_MULTI_LMEM_DPB_WORDS	0x100
#define H264_MULTI_LMEM_MMCO_WORDS	0x200
#define H264_MULTI_LMEM_DATA_WORDS	0x400

#define H264_MULTI_LMEM_MAX_DPB_FRAMES	24
#define H264_MULTI_LMEM_REF_WORDS	128
#define H264_MULTI_LMEM_REORDER_WORDS	66
#define H264_MULTI_LMEM_MMCO_CMD_WORDS	44
#define H264_MULTI_LMEM_LIST_WORDS	40

enum h264_multi_lmem_param {
	H264_MULTI_PARAM_SPS_FLAGS_2 = 0x6c,
	H264_MULTI_PARAM_NUM_REORDER_FRAMES = 0x6d,
	H264_MULTI_PARAM_MAX_BUFFER_FRAME = 0x6e,
	H264_MULTI_PARAM_NEW_PICTURE_STRUCTURE = 0x7c,
	H264_MULTI_PARAM_SLICE_TYPE = 0x82,
	H264_MULTI_PARAM_LOG2_MAX_FRAME_NUM = 0x83,
	H264_MULTI_PARAM_FRAME_MBS_ONLY = 0x84,
	H264_MULTI_PARAM_PIC_ORDER_CNT_TYPE = 0x85,
	H264_MULTI_PARAM_LOG2_MAX_PIC_ORDER_CNT_LSB = 0x86,
	H264_MULTI_PARAM_PIC_ORDER_PRESENT = 0x87,
	H264_MULTI_PARAM_MODE_8X8_FLAGS = 0x8c,
	H264_MULTI_PARAM_OFFSET_FOR_NON_REF_PIC = 0xe0,
	H264_MULTI_PARAM_OFFSET_FOR_TOP_TO_BOTTOM_FIELD = 0xe2,
	H264_MULTI_PARAM_MAX_REFERENCE_FRAME_NUM = 0xe4,
	H264_MULTI_PARAM_FRAME_NUM_GAP_ALLOWED = 0xe5,
	H264_MULTI_PARAM_NUM_REF_FRAMES_IN_POC_CYCLE = 0xe6,
	H264_MULTI_PARAM_PROFILE_IDC_MMCO = 0xe7,
	H264_MULTI_PARAM_LEVEL_IDC_MMCO = 0xe8,
	H264_MULTI_PARAM_DELTA_POC_ALWAYS_ZERO = 0xea,
	H264_MULTI_PARAM_FIRST_MB_IN_SLICE = 0xf0,
	H264_MULTI_PARAM_VUI_STATUS = 0xf4,
};

struct h264_multi_lmem_dpb {
	u16 entries[H264_MULTI_LMEM_MAX_DPB_FRAMES * 8];
	u16 max_buffer_frames;
	u16 size;
	u16 colocated_buf_status;
	u16 num_forward_short_term_refs;
	u16 num_short_term_refs;
	u16 num_refs;
	u16 current_index;
	u16 current_decoded_frame_num;
	u16 current_reference_frame_num;
	u16 l0_size;
	u16 l1_size;
	u16 nal_info;
	u16 picture_structure;
	u16 frame_num;
	u16 pic_order_cnt_lsb;
	u16 num_ref_idx_l0_active_minus1;
	u16 num_ref_idx_l1_active_minus1;
	u16 prev_pic_order_cnt_lsb;
	u16 previous_frame_num;
	u16 delta_pic_order_cnt_bottom[2];
	u16 delta_pic_order_cnt[2][2];
	u16 prev_pic_order_cnt_msb[2];
	u16 prev_frame_num_offset[2];
	u16 frame_pic_order_cnt[2];
	u16 top_field_pic_order_cnt[2];
	u16 bottom_field_pic_order_cnt[2];
	u16 colocated_mv_addr_start[2];
	u16 colocated_mv_addr_end[2];
	u16 colocated_mv_write_addr[2];
	u16 reserved[23];
};

struct h264_multi_lmem_mmco {
	u16 offset_for_ref_frame[H264_MULTI_LMEM_REF_WORDS];
	u16 references[H264_MULTI_LMEM_REF_WORDS];
	u16 l0_reorder[H264_MULTI_LMEM_REORDER_WORDS];
	u16 l1_reorder[H264_MULTI_LMEM_REORDER_WORDS];
	u16 commands[H264_MULTI_LMEM_MMCO_CMD_WORDS];
	u16 l0[H264_MULTI_LMEM_LIST_WORDS];
	u16 l1[H264_MULTI_LMEM_LIST_WORDS];
};

struct h264_multi_lmem {
	u16 params[H264_MULTI_LMEM_PARAM_WORDS];
	struct h264_multi_lmem_dpb dpb;
	struct h264_multi_lmem_mmco mmco;
};

static_assert(sizeof(struct h264_multi_lmem_dpb) ==
	      H264_MULTI_LMEM_DPB_WORDS * sizeof(u16));
static_assert(sizeof(struct h264_multi_lmem_mmco) ==
	      H264_MULTI_LMEM_MMCO_WORDS * sizeof(u16));
static_assert(sizeof(struct h264_multi_lmem) ==
	      H264_MULTI_LMEM_DATA_WORDS * sizeof(u16));
static_assert(offsetof(struct h264_multi_lmem, dpb) == 0x200);
static_assert(offsetof(struct h264_multi_lmem, mmco) == 0x400);

#endif
