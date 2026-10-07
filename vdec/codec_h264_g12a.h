/* SPDX-License-Identifier: GPL-2.0+ */

#ifndef __MESON_VDEC_CODEC_H264_MULTI_H_
#define __MESON_VDEC_CODEC_H264_MULTI_H_

#include "vdec.h"

#define H264_MULTI_MAX_POC_CYCLE	128

enum h264_multi_event {
	H264_MULTI_SLICE_HEAD_DONE = 0x01,
	H264_MULTI_PIC_DATA_DONE = 0x02,
	H264_MULTI_CONFIG_REQUEST = 0x11,
	H264_MULTI_DATA_REQUEST = 0x12,
	H264_MULTI_WRRSP_REQUEST = 0x13,
	H264_MULTI_WRRSP_DONE = 0x14,
	H264_MULTI_DECODE_BUFEMPTY = 0x20,
	H264_MULTI_DECODE_TIMEOUT = 0x21,
	H264_MULTI_SEARCH_BUFEMPTY = 0x22,
	H264_MULTI_DECODE_OVER_SIZE = 0x23,
	H264_MULTI_DECODE_ERROR_RESET = 0x24,
	H264_MULTI_DECODE_INIT_RESET = 0x25,
	H264_MULTI_FIND_NEXT_PIC_NAL = 0x50,
	H264_MULTI_FIND_NEXT_DVEL_NAL = 0x51,
	H264_MULTI_AUX_DATA_READY = 0x52,
	H264_MULTI_SEI_DATA_READY = 0x53,
	H264_MULTI_SEI_DATA_DONE = 0x54,
};

enum h264_multi_action {
	H264_MULTI_ACTION_SEARCH_HEAD = 0xf0,
	H264_MULTI_ACTION_DECODE_SLICE = 0xf1,
	H264_MULTI_ACTION_CONFIG_DONE = 0xf2,
	H264_MULTI_ACTION_DECODE_NEWPIC = 0xf3,
	H264_MULTI_ACTION_DECODE_START = 0xff,
};

struct h264_multi_config {
	u32 coded_width;
	u32 coded_height;
	u32 width;
	u32 height;
	u8 profile_idc;
	u8 level_idc;
	u8 chroma_format_idc;
	u8 max_refs;
	u8 num_reorder_frames;
	u8 max_dec_frame_buffering;
	u8 pic_order_cnt_type;
	u8 num_ref_frames_in_poc_cycle;
	u32 max_frame_num;
	u32 max_pic_order_cnt_lsb;
	s16 offset_for_non_ref_pic;
	s16 offset_for_top_to_bottom_field;
	s16 offset_for_ref_frame[H264_MULTI_MAX_POC_CYCLE];
	bool frame_mbs_only;
	bool bitstream_restriction;
	bool delta_pic_order_always_zero;
	bool frame_num_gap_allowed;
};

#define H264_MULTI_MAX_MMCO_OPS	43

struct h264_multi_mmco {
	u8 opcode;
	u16 difference_of_pic_nums_minus1;
	u16 long_term_pic_num;
	u16 long_term_frame_idx;
	u16 max_long_term_frame_idx_plus1;
};

struct h264_multi_marking {
	bool no_output_of_prior_pics;
	bool long_term_reference;
	bool adaptive;
	u8 count;
	struct h264_multi_mmco ops[H264_MULTI_MAX_MMCO_OPS];
};

struct h264_multi_picture {
	u16 frame_num;
	u16 pic_order_cnt_lsb;
	s32 delta_pic_order_cnt_bottom;
	s32 delta_pic_order_cnt[2];
	u16 first_mb_in_slice;
	u8 nal_unit_type;
	u8 nal_ref_idc;
	u8 slice_type;
	u8 num_ref_idx_l0_active;
	u8 num_ref_idx_l1_active;
	bool field_pic;
	bool bottom_field;
};

int codec_h264_multi_prepare_firmware(struct amvdec_session *sess,
				      const u8 *data, u32 len);
void codec_h264_multi_release_firmware(struct amvdec_session *sess);
int codec_h264_multi_read_lmem(struct amvdec_session *sess);
u16 codec_h264_multi_lmem_word(struct amvdec_session *sess,
			       unsigned int index);
int codec_h264_multi_parse_config(struct amvdec_session *sess,
				  u32 seq_info2, u32 seq_info, u32 crop_info,
				  u32 param4,
				  struct h264_multi_config *config);
int codec_h264_multi_parse_marking(struct amvdec_session *sess,
				   struct h264_multi_marking *marking);
int codec_h264_multi_parse_picture(struct amvdec_session *sess,
				   struct h264_multi_picture *picture);

extern struct amvdec_codec_ops codec_h264_g12a_ops;

#endif
