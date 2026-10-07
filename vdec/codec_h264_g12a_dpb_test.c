// SPDX-License-Identifier: GPL-2.0+

#include <kunit/test.h>

#include "codec_h264_g12a_dpb.h"

static struct h264_multi_config h264_multi_test_config(u8 poc_type)
{
	struct h264_multi_config config = {
		.pic_order_cnt_type = poc_type,
		.max_frame_num = 16,
		.max_pic_order_cnt_lsb = 16,
	};

	return config;
}

static void h264_multi_poc_type0_wrap_test(struct kunit *test)
{
	struct h264_multi_config config = h264_multi_test_config(0);
	struct h264_multi_poc_state state = {
		.prev_pic_order_cnt_lsb = 14,
	};
	struct h264_multi_picture picture = {
		.nal_ref_idc = 1,
		.pic_order_cnt_lsb = 1,
	};
	struct h264_multi_poc poc;

	KUNIT_ASSERT_EQ(test, h264_multi_poc_derive(&state, &config,
						    &picture, &poc), 0);
	KUNIT_EXPECT_EQ(test, poc.top, 17);
	KUNIT_EXPECT_EQ(test, poc.bottom, 17);
}

static void h264_multi_poc_non_ref_commit_test(struct kunit *test)
{
	struct h264_multi_config config = h264_multi_test_config(0);
	struct h264_multi_poc_state state = {
		.prev_pic_order_cnt_msb = 16,
		.prev_pic_order_cnt_lsb = 4,
		.prev_frame_num = 3,
	};
	struct h264_multi_picture picture = {
		.pic_order_cnt_lsb = 2,
		.frame_num = 4,
	};
	struct h264_multi_poc poc;

	KUNIT_ASSERT_EQ(test, h264_multi_poc_derive(&state, &config,
						    &picture, &poc), 0);
	h264_multi_poc_commit(&state, &config, &picture, &poc, false);
	KUNIT_EXPECT_EQ(test, state.prev_pic_order_cnt_msb, 16);
	KUNIT_EXPECT_EQ(test, state.prev_pic_order_cnt_lsb, 4);
	KUNIT_EXPECT_EQ(test, state.prev_frame_num, 3);
}

static void h264_multi_poc_mmco5_test(struct kunit *test)
{
	struct h264_multi_config config = h264_multi_test_config(0);
	struct h264_multi_poc_state state = {};
	struct h264_multi_picture picture = {
		.nal_ref_idc = 1,
		.pic_order_cnt_lsb = 12,
		.delta_pic_order_cnt_bottom = -10,
	};
	struct h264_multi_picture next = {
		.nal_ref_idc = 1,
		.pic_order_cnt_lsb = 1,
	};
	struct h264_multi_poc poc;

	KUNIT_ASSERT_EQ(test, h264_multi_poc_derive(&state, &config,
						    &picture, &poc), 0);
	h264_multi_poc_commit(&state, &config, &picture, &poc, true);
	KUNIT_EXPECT_EQ(test, state.prev_top_field_order_cnt, 10);
	KUNIT_ASSERT_EQ(test, h264_multi_poc_derive(&state, &config,
						    &next, &poc), 0);
	KUNIT_EXPECT_EQ(test, poc.top, 17);
}

static void h264_multi_poc_type1_cycle_test(struct kunit *test)
{
	struct h264_multi_config config = h264_multi_test_config(1);
	struct h264_multi_poc_state state = {};
	struct h264_multi_picture picture = {
		.nal_ref_idc = 1,
		.frame_num = 2,
	};
	struct h264_multi_poc poc;

	config.num_ref_frames_in_poc_cycle = 2;
	config.offset_for_ref_frame[0] = 2;
	config.offset_for_ref_frame[1] = 2;
	KUNIT_ASSERT_EQ(test, h264_multi_poc_derive(&state, &config,
						    &picture, &poc), 0);
	KUNIT_EXPECT_EQ(test, poc.top, 4);
	KUNIT_EXPECT_EQ(test, poc.bottom, 4);
}

static void h264_multi_poc_type2_frame_wrap_test(struct kunit *test)
{
	struct h264_multi_config config = h264_multi_test_config(2);
	struct h264_multi_poc_state state = {
		.prev_frame_num = 14,
	};
	struct h264_multi_picture picture = {
		.nal_ref_idc = 1,
		.frame_num = 1,
	};
	struct h264_multi_poc poc;

	KUNIT_ASSERT_EQ(test, h264_multi_poc_derive(&state, &config,
						    &picture, &poc), 0);
	KUNIT_EXPECT_EQ(test, poc.top, 34);
	KUNIT_EXPECT_EQ(test, poc.bottom, 34);
}

static void h264_multi_dpb_level_limit_test(struct kunit *test)
{
	struct h264_multi_config config = {
		.coded_width = 1920,
		.coded_height = 1088,
		.level_idc = 40,
		.max_refs = 4,
	};
	unsigned int count;

	KUNIT_ASSERT_EQ(test, h264_multi_dpb_buf_count(&config, &count), 0);
	KUNIT_EXPECT_EQ(test, count, 5U);
}

static void h264_multi_dpb_vui_limit_test(struct kunit *test)
{
	struct h264_multi_config config = {
		.coded_width = 1280,
		.coded_height = 720,
		.level_idc = 31,
		.max_refs = 2,
		.num_reorder_frames = 2,
		.max_dec_frame_buffering = 3,
		.bitstream_restriction = true,
	};
	unsigned int count;

	KUNIT_ASSERT_EQ(test, h264_multi_dpb_buf_count(&config, &count), 0);
	KUNIT_EXPECT_EQ(test, count, 4U);

	config.max_dec_frame_buffering = 1;
	KUNIT_EXPECT_EQ(test, h264_multi_dpb_buf_count(&config, &count),
			-EINVAL);
}

static unsigned int h264_multi_dpb_active_slots(struct h264_multi_dpb *dpb)
{
	unsigned int active = 0;
	unsigned int i;

	for (i = 0; i < H264_MULTI_DPB_SIZE; i++)
		active += dpb->slots[i].active;

	return active;
}

static struct h264_multi_dpb_slot *
h264_multi_dpb_find_ts(struct h264_multi_dpb *dpb, u64 reference_ts)
{
	unsigned int i;

	for (i = 0; i < H264_MULTI_DPB_SIZE; i++) {
		if (dpb->slots[i].active &&
		    dpb->slots[i].reference_ts == reference_ts)
			return &dpb->slots[i];
	}

	return NULL;
}

static void h264_multi_dpb_decode_ref(struct kunit *test,
				      struct h264_multi_dpb *dpb,
				      const struct h264_multi_config *config,
				      u16 frame_num, u64 reference_ts,
				      const struct h264_multi_marking *marking)
{
	struct h264_multi_picture picture = {
		.nal_ref_idc = 1,
		.frame_num = frame_num,
	};
	struct h264_multi_poc poc;

	KUNIT_ASSERT_EQ(test, h264_multi_dpb_begin(dpb, config, &picture,
						   &poc), 0);
	KUNIT_ASSERT_EQ(test, h264_multi_dpb_finish(dpb, config, &picture,
						    marking, &poc,
						     reference_ts,
						     reference_ts), 0);
}

static void h264_multi_dpb_sliding_window_test(struct kunit *test)
{
	struct h264_multi_config config = h264_multi_test_config(2);
	struct h264_multi_marking marking = {};
	struct h264_multi_dpb dpb;

	config.max_refs = 2;
	h264_multi_dpb_reset(&dpb);
	h264_multi_dpb_decode_ref(test, &dpb, &config, 0, 100, &marking);
	h264_multi_dpb_decode_ref(test, &dpb, &config, 1, 101, &marking);
	h264_multi_dpb_decode_ref(test, &dpb, &config, 2, 102, &marking);

	KUNIT_EXPECT_EQ(test, h264_multi_dpb_active_slots(&dpb), 2U);
	KUNIT_EXPECT_PTR_EQ(test, h264_multi_dpb_find_ts(&dpb, 100), NULL);
	KUNIT_EXPECT_NOT_NULL(test, h264_multi_dpb_find_ts(&dpb, 101));
	KUNIT_EXPECT_NOT_NULL(test, h264_multi_dpb_find_ts(&dpb, 102));
}

static void h264_multi_dpb_mmco1_test(struct kunit *test)
{
	struct h264_multi_config config = h264_multi_test_config(2);
	struct h264_multi_marking sliding = {};
	struct h264_multi_marking marking = {
		.adaptive = true,
		.count = 1,
		.ops[0] = {
			.opcode = 1,
			.difference_of_pic_nums_minus1 = 0,
		},
	};
	struct h264_multi_dpb dpb;

	config.max_refs = 4;
	h264_multi_dpb_reset(&dpb);
	h264_multi_dpb_decode_ref(test, &dpb, &config, 0, 100, &sliding);
	h264_multi_dpb_decode_ref(test, &dpb, &config, 1, 101, &sliding);
	h264_multi_dpb_decode_ref(test, &dpb, &config, 2, 102, &marking);

	KUNIT_EXPECT_EQ(test, h264_multi_dpb_active_slots(&dpb), 2U);
	KUNIT_EXPECT_NOT_NULL(test, h264_multi_dpb_find_ts(&dpb, 100));
	KUNIT_EXPECT_PTR_EQ(test, h264_multi_dpb_find_ts(&dpb, 101), NULL);
	KUNIT_EXPECT_NOT_NULL(test, h264_multi_dpb_find_ts(&dpb, 102));
}

static void h264_multi_dpb_long_term_test(struct kunit *test)
{
	struct h264_multi_config config = h264_multi_test_config(2);
	struct h264_multi_marking *sliding;
	struct h264_multi_marking *convert;
	struct h264_multi_marking *trim;
	struct v4l2_h264_dpb_entry *entries;
	struct h264_multi_picture current_pic = {
		.frame_num = 2,
	};
	struct h264_multi_dpb_slot *slot;
	struct h264_multi_dpb *dpb;
	unsigned int i;

	dpb = kunit_kzalloc(test, sizeof(*dpb), GFP_KERNEL);
	entries = kunit_kcalloc(test, H264_MULTI_DPB_SIZE, sizeof(*entries),
				GFP_KERNEL);
	sliding = kunit_kzalloc(test, sizeof(*sliding), GFP_KERNEL);
	convert = kunit_kzalloc(test, sizeof(*convert), GFP_KERNEL);
	trim = kunit_kzalloc(test, sizeof(*trim), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, dpb);
	KUNIT_ASSERT_NOT_NULL(test, entries);
	KUNIT_ASSERT_NOT_NULL(test, sliding);
	KUNIT_ASSERT_NOT_NULL(test, convert);
	KUNIT_ASSERT_NOT_NULL(test, trim);
	convert->adaptive = true;
	convert->count = 1;
	convert->ops[0].opcode = 3;
	convert->ops[0].long_term_frame_idx = 3;
	trim->adaptive = true;
	trim->count = 1;
	trim->ops[0].opcode = 4;
	trim->ops[0].max_long_term_frame_idx_plus1 = 3;

	config.max_refs = 4;
	h264_multi_dpb_reset(dpb);
	h264_multi_dpb_decode_ref(test, dpb, &config, 0, 100, sliding);
	h264_multi_dpb_decode_ref(test, dpb, &config, 1, 101, convert);
	slot = h264_multi_dpb_find_ts(dpb, 100);
	KUNIT_ASSERT_NOT_NULL(test, slot);
	KUNIT_EXPECT_TRUE(test, slot->long_term);
	KUNIT_EXPECT_EQ(test, slot->long_term_frame_idx, 3);

	h264_multi_dpb_to_v4l2(dpb, &config, &current_pic, entries);
	for (i = 0; i < H264_MULTI_DPB_SIZE; i++) {
		if (entries[i].reference_ts != 100)
			continue;
		KUNIT_EXPECT_EQ(test, entries[i].frame_num, 3);
		KUNIT_EXPECT_TRUE(test, entries[i].flags &
				  V4L2_H264_DPB_ENTRY_FLAG_VALID);
		KUNIT_EXPECT_TRUE(test, entries[i].flags &
				  V4L2_H264_DPB_ENTRY_FLAG_ACTIVE);
		KUNIT_EXPECT_TRUE(test, entries[i].flags &
				  V4L2_H264_DPB_ENTRY_FLAG_LONG_TERM);
		break;
	}
	KUNIT_EXPECT_LT(test, i, (unsigned int)H264_MULTI_DPB_SIZE);

	h264_multi_dpb_decode_ref(test, dpb, &config, 2, 102, trim);
	KUNIT_EXPECT_PTR_EQ(test, h264_multi_dpb_find_ts(dpb, 100), NULL);
}

static void h264_multi_dpb_multislice_picture_test(struct kunit *test)
{
	struct h264_multi_config config = h264_multi_test_config(2);
	struct h264_multi_picture first = {
		.nal_ref_idc = 1,
		.frame_num = 3,
	};
	struct h264_multi_picture next = first;
	struct h264_multi_dpb_picture pic_state;
	struct h264_multi_marking marking = {};
	struct h264_multi_dpb dpb;
	enum h264_multi_slice_action action;
	int ret;

	config.max_refs = 2;
	next.first_mb_in_slice = 120;
	h264_multi_dpb_reset(&dpb);
	h264_multi_dpb_picture_reset(&pic_state);

	ret = h264_multi_dpb_picture_begin(&dpb, &config, &pic_state, &first, 7,
					   &action);
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, action, H264_MULTI_SLICE_NEW_PICTURE);
	KUNIT_EXPECT_TRUE(test, pic_state.active);
	KUNIT_EXPECT_EQ(test, pic_state.buffer_index, 7U);

	ret = h264_multi_dpb_picture_begin(&dpb, &config, &pic_state, &next, 7,
					   &action);
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, action, H264_MULTI_SLICE_CONTINUE);
	KUNIT_EXPECT_EQ(test, h264_multi_dpb_active_slots(&dpb), 0U);

	ret = h264_multi_dpb_picture_finish(&dpb, &config, &pic_state, &marking,
					    103, 7);
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_EXPECT_FALSE(test, pic_state.active);
	KUNIT_EXPECT_NOT_NULL(test, h264_multi_dpb_find_ts(&dpb, 103));
	KUNIT_EXPECT_EQ(test, h264_multi_dpb_find_ts(&dpb, 103)->buffer_index,
			7U);
}

static void h264_multi_dpb_picture_sequence_error_test(struct kunit *test)
{
	struct h264_multi_config config = h264_multi_test_config(2);
	struct h264_multi_picture first = {
		.nal_ref_idc = 1,
		.frame_num = 3,
	};
	struct h264_multi_picture broken = first;
	struct h264_multi_dpb_picture pic_state;
	struct h264_multi_dpb dpb;
	enum h264_multi_slice_action action;
	int ret;

	broken.first_mb_in_slice = 20;
	h264_multi_dpb_reset(&dpb);
	h264_multi_dpb_picture_reset(&pic_state);
	ret = h264_multi_dpb_picture_begin(&dpb, &config, &pic_state, &broken, 1,
					   &action);
	KUNIT_EXPECT_EQ(test, ret, -EINVAL);

	ret = h264_multi_dpb_picture_begin(&dpb, &config, &pic_state, &first, 1,
					   &action);
	KUNIT_ASSERT_EQ(test, ret, 0);
	broken.frame_num++;
	ret = h264_multi_dpb_picture_begin(&dpb, &config, &pic_state, &broken, 2,
					   &action);
	KUNIT_EXPECT_EQ(test, ret, -EPIPE);
	KUNIT_EXPECT_TRUE(test, pic_state.active);
}

static void h264_multi_reorder_short_term_test(struct kunit *test)
{
	struct h264_multi_config config = h264_multi_test_config(2);
	struct h264_multi_picture picture = { .frame_num = 4 };
	struct v4l2_h264_reference refs[] = {
		{ .index = 0, .fields = V4L2_H264_FRAME_REF },
		{ .index = 1, .fields = V4L2_H264_FRAME_REF },
		{ .index = 2, .fields = V4L2_H264_FRAME_REF },
	};
	const u16 commands[] = { 0, 1, 3 };
	struct h264_multi_dpb dpb = {};
	int ret;

	dpb.slots[0] = (struct h264_multi_dpb_slot) {
		.active = true, .frame_num = 3,
	};
	dpb.slots[1] = (struct h264_multi_dpb_slot) {
		.active = true, .frame_num = 2,
	};
	dpb.slots[2] = (struct h264_multi_dpb_slot) {
		.active = true, .frame_num = 1,
	};
	ret = h264_multi_dpb_reorder_reflist(&dpb, &config, &picture, refs,
					     ARRAY_SIZE(refs), ARRAY_SIZE(refs),
					     commands,
					     ARRAY_SIZE(commands));
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, refs[0].index, 1);
	KUNIT_EXPECT_EQ(test, refs[1].index, 0);
	KUNIT_EXPECT_EQ(test, refs[2].index, 2);
}

static void h264_multi_reorder_wrap_test(struct kunit *test)
{
	struct h264_multi_config config = h264_multi_test_config(2);
	struct h264_multi_picture picture = { .frame_num = 1 };
	struct v4l2_h264_reference refs[] = {
		{ .index = 0, .fields = V4L2_H264_FRAME_REF },
		{ .index = 1, .fields = V4L2_H264_FRAME_REF },
	};
	const u16 commands[] = { 0, 1, 3 };
	struct h264_multi_dpb dpb = {};
	int ret;

	dpb.slots[0] = (struct h264_multi_dpb_slot) {
		.active = true, .frame_num = 0,
	};
	dpb.slots[1] = (struct h264_multi_dpb_slot) {
		.active = true, .frame_num = 15,
	};
	ret = h264_multi_dpb_reorder_reflist(&dpb, &config, &picture, refs,
					     ARRAY_SIZE(refs), ARRAY_SIZE(refs),
					     commands,
					     ARRAY_SIZE(commands));
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, refs[0].index, 1);
	KUNIT_EXPECT_EQ(test, refs[1].index, 0);
}

static void h264_multi_reorder_long_term_test(struct kunit *test)
{
	struct h264_multi_config config = h264_multi_test_config(2);
	struct h264_multi_picture picture = { .frame_num = 4 };
	struct v4l2_h264_reference refs[] = {
		{ .index = 0, .fields = V4L2_H264_FRAME_REF },
		{ .index = 1, .fields = V4L2_H264_FRAME_REF },
		{ .index = 2, .fields = V4L2_H264_FRAME_REF },
	};
	const u16 commands[] = { 2, 5, 3 };
	struct h264_multi_dpb dpb = {};
	int ret;

	dpb.slots[0] = (struct h264_multi_dpb_slot) {
		.active = true, .frame_num = 3,
	};
	dpb.slots[1] = (struct h264_multi_dpb_slot) {
		.active = true, .frame_num = 2,
	};
	dpb.slots[2] = (struct h264_multi_dpb_slot) {
		.active = true, .long_term = true, .long_term_frame_idx = 5,
	};
	ret = h264_multi_dpb_reorder_reflist(&dpb, &config, &picture, refs,
					     ARRAY_SIZE(refs), ARRAY_SIZE(refs),
					     commands,
					     ARRAY_SIZE(commands));
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, refs[0].index, 2);
	KUNIT_EXPECT_EQ(test, refs[1].index, 0);
	KUNIT_EXPECT_EQ(test, refs[2].index, 1);
}

static void h264_multi_reorder_invalid_test(struct kunit *test)
{
	struct h264_multi_config config = h264_multi_test_config(2);
	struct h264_multi_picture picture = { .frame_num = 4 };
	struct v4l2_h264_reference ref = {
		.index = 0, .fields = V4L2_H264_FRAME_REF,
	};
	const u16 commands[] = { 0, 0 };
	struct h264_multi_dpb dpb = {};
	int ret;

	dpb.slots[0] = (struct h264_multi_dpb_slot) {
		.active = true, .frame_num = 3,
	};
	ret = h264_multi_dpb_reorder_reflist(&dpb, &config, &picture, &ref, 1,
					     1, commands, ARRAY_SIZE(commands));
	KUNIT_EXPECT_EQ(test, ret, -EINVAL);
}

static void h264_multi_reorder_repeated_ref_test(struct kunit *test)
{
	struct h264_multi_config config = h264_multi_test_config(2);
	struct h264_multi_picture picture = { .frame_num = 2 };
	struct v4l2_h264_reference refs[V4L2_H264_REF_LIST_LEN] = {
		{ .index = 1, .fields = V4L2_H264_FRAME_REF },
		{ .index = 0, .fields = V4L2_H264_FRAME_REF },
	};
	const u16 commands[] = { 0, 0, 0, 0, 1, 0, 3 };
	struct h264_multi_dpb dpb = {};
	int ret;

	dpb.slots[0] = (struct h264_multi_dpb_slot) {
		.active = true, .frame_num = 0,
	};
	dpb.slots[1] = (struct h264_multi_dpb_slot) {
		.active = true, .frame_num = 1,
	};
	ret = h264_multi_dpb_reorder_reflist(&dpb, &config, &picture, refs,
					     2, 3, commands,
					     ARRAY_SIZE(commands));
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, refs[0].index, 1);
	KUNIT_EXPECT_EQ(test, refs[1].index, 0);
	KUNIT_EXPECT_EQ(test, refs[2].index, 1);
}

static struct kunit_case h264_multi_poc_test_cases[] = {
	KUNIT_CASE(h264_multi_poc_type0_wrap_test),
	KUNIT_CASE(h264_multi_poc_non_ref_commit_test),
	KUNIT_CASE(h264_multi_poc_mmco5_test),
	KUNIT_CASE(h264_multi_poc_type1_cycle_test),
	KUNIT_CASE(h264_multi_poc_type2_frame_wrap_test),
	KUNIT_CASE(h264_multi_dpb_level_limit_test),
	KUNIT_CASE(h264_multi_dpb_vui_limit_test),
	KUNIT_CASE(h264_multi_dpb_sliding_window_test),
	KUNIT_CASE(h264_multi_dpb_mmco1_test),
	KUNIT_CASE(h264_multi_dpb_long_term_test),
	KUNIT_CASE(h264_multi_dpb_multislice_picture_test),
	KUNIT_CASE(h264_multi_dpb_picture_sequence_error_test),
	KUNIT_CASE(h264_multi_reorder_short_term_test),
	KUNIT_CASE(h264_multi_reorder_wrap_test),
	KUNIT_CASE(h264_multi_reorder_long_term_test),
	KUNIT_CASE(h264_multi_reorder_repeated_ref_test),
	KUNIT_CASE(h264_multi_reorder_invalid_test),
	{}
};

static struct kunit_suite h264_multi_poc_test_suite = {
	.name = "meson-vdec-h264-multi-poc",
	.test_cases = h264_multi_poc_test_cases,
};

kunit_test_suite(h264_multi_poc_test_suite);

MODULE_LICENSE("GPL");
