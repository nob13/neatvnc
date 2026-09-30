/*
 * Copyright (c) 2026 Norbert Schultz
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES WITH
 * REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY
 * AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT,
 * INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM
 * LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE
 * OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
 * PERFORMANCE OF THIS SOFTWARE.
 */

#include "enc/h264-sps.h"

#include <stdbool.h>
#include <stdlib.h>

struct bit_reader {
	const uint8_t* data;
	size_t size;
	size_t pos;
	bool error;
};

struct bit_writer {
	uint8_t* data;
	size_t size;
	size_t pos;
	bool error;
};

static uint32_t read_bit(struct bit_reader* r)
{
	if (r->pos >= r->size * 8) {
		r->error = true;
		return 0;
	}
	uint32_t bit = (r->data[r->pos / 8] >> (7 - r->pos % 8)) & 1;
	r->pos++;
	return bit;
}

static uint32_t read_bits(struct bit_reader* r, int n)
{
	uint32_t value = 0;
	for (int i = 0; i < n; ++i)
		value = (value << 1) | read_bit(r);
	return value;
}

static uint32_t read_ue(struct bit_reader* r)
{
	int leading_zeros = 0;
	while (!read_bit(r)) {
		if (r->error || ++leading_zeros > 31) {
			r->error = true;
			return 0;
		}
	}
	return ((1u << leading_zeros) - 1) + read_bits(r, leading_zeros);
}

static void write_bit(struct bit_writer* w, uint32_t bit)
{
	if (w->pos >= w->size * 8) {
		w->error = true;
		return;
	}
	uint8_t mask = 1 << (7 - w->pos % 8);
	if (bit)
		w->data[w->pos / 8] |= mask;
	else
		w->data[w->pos / 8] &= ~mask;
	w->pos++;
}

static void write_ue(struct bit_writer* w, uint32_t value)
{
	uint64_t code = (uint64_t)value + 1;
	int n_bits = 0;
	while ((code >> n_bits) > 1)
		n_bits++;

	for (int i = 0; i < n_bits; ++i)
		write_bit(w, 0);
	for (int i = n_bits; i >= 0; --i)
		write_bit(w, (code >> i) & 1);
}

static bool has_chroma_format_info(uint32_t profile_idc)
{
	switch (profile_idc) {
	case 44:
	case 83:
	case 86:
	case 100:
	case 110:
	case 118:
	case 122:
	case 128:
	case 134:
	case 135:
	case 138:
	case 139:
	case 244:
		return true;
	}
	return false;
}

static void skip_hrd_parameters(struct bit_reader* r)
{
	uint32_t cpb_cnt_minus1 = read_ue(r);
	if (cpb_cnt_minus1 > 31) {
		r->error = true;
		return;
	}
	read_bits(r, 8); // bit_rate_scale, cpb_size_scale
	for (uint32_t i = 0; i <= cpb_cnt_minus1; ++i) {
		read_ue(r); // bit_rate_value_minus1
		read_ue(r); // cpb_size_value_minus1
		read_bit(r); // cbr_flag
	}
	read_bits(r, 20); // delay and time offset lengths
}

/* Finds the bit position of bitstream_restriction_flag in the RBSP. Returns -1
 * if the flag is already set.
 */
static int find_bitstream_restriction_flag(struct bit_reader* r,
		size_t* pos_out)
{
	if (read_bit(r)) { // aspect_ratio_info_present_flag
		if (read_bits(r, 8) == 255) // aspect_ratio_idc, Extended_SAR
			read_bits(r, 32); // sar_width, sar_height
	}
	if (read_bit(r)) // overscan_info_present_flag
		read_bit(r); // overscan_appropriate_flag
	if (read_bit(r)) { // video_signal_type_present_flag
		read_bits(r, 4); // video_format, video_full_range_flag
		if (read_bit(r)) // colour_description_present_flag
			read_bits(r, 24);
	}
	if (read_bit(r)) { // chroma_loc_info_present_flag
		read_ue(r);
		read_ue(r);
	}
	if (read_bit(r)) { // timing_info_present_flag
		read_bits(r, 32); // num_units_in_tick
		read_bits(r, 32); // time_scale
		read_bit(r); // fixed_frame_rate_flag
	}
	bool nal_hrd = read_bit(r);
	if (nal_hrd)
		skip_hrd_parameters(r);
	bool vcl_hrd = read_bit(r);
	if (vcl_hrd)
		skip_hrd_parameters(r);
	if (nal_hrd || vcl_hrd)
		read_bit(r); // low_delay_hrd_flag
	read_bit(r); // pic_struct_present_flag

	*pos_out = r->pos;
	bool restricted = read_bit(r);
	return r->error || restricted ? -1 : 0;
}

/* Finds the bit position where the low delay VUI data has to be inserted into
 * the RBSP. That is the position of vui_parameters_present_flag if the SPS has
 * no VUI, or that of bitstream_restriction_flag otherwise.
 */
static int find_insert_pos(struct bit_reader* r, size_t* pos_out,
		uint32_t* max_num_ref_frames, bool* has_vui)
{
	uint32_t profile_idc = read_bits(r, 8);
	read_bits(r, 16); // constraint flags, level_idc
	read_ue(r); // seq_parameter_set_id

	if (has_chroma_format_info(profile_idc)) {
		if (read_ue(r) == 3) // chroma_format_idc
			read_bit(r); // separate_colour_plane_flag
		read_ue(r); // bit_depth_luma_minus8
		read_ue(r); // bit_depth_chroma_minus8
		read_bit(r); // qpprime_y_zero_transform_bypass_flag
		if (read_bit(r)) // seq_scaling_matrix_present_flag
			return -1;
	}

	read_ue(r); // log2_max_frame_num_minus4
	uint32_t pic_order_cnt_type = read_ue(r);
	if (pic_order_cnt_type == 0) {
		read_ue(r); // log2_max_pic_order_cnt_lsb_minus4
	} else if (pic_order_cnt_type == 1) {
		read_bit(r); // delta_pic_order_always_zero_flag
		read_ue(r); // offset_for_non_ref_pic
		read_ue(r); // offset_for_top_to_bottom_field
		uint32_t n = read_ue(r);
		if (n > 255)
			return -1;
		for (uint32_t i = 0; i < n; ++i)
			read_ue(r); // offset_for_ref_frame
	}

	*max_num_ref_frames = read_ue(r);
	read_bit(r); // gaps_in_frame_num_value_allowed_flag
	read_ue(r); // pic_width_in_mbs_minus1
	read_ue(r); // pic_height_in_map_units_minus1
	if (!read_bit(r)) // frame_mbs_only_flag
		read_bit(r); // mb_adaptive_frame_field_flag
	read_bit(r); // direct_8x8_inference_flag
	if (read_bit(r)) { // frame_cropping_flag
		for (int i = 0; i < 4; ++i)
			read_ue(r);
	}

	*pos_out = r->pos;
	*has_vui = read_bit(r);
	if (r->error)
		return -1;
	return *has_vui ? find_bitstream_restriction_flag(r, pos_out) : 0;
}

static void write_low_delay_vui(struct bit_writer* w,
		uint32_t max_num_ref_frames, bool has_vui)
{
	if (!has_vui) {
		write_bit(w, 1); // vui_parameters_present_flag
		write_bit(w, 0); // aspect_ratio_info_present_flag
		write_bit(w, 0); // overscan_info_present_flag
		write_bit(w, 0); // video_signal_type_present_flag
		write_bit(w, 0); // chroma_loc_info_present_flag
		write_bit(w, 0); // timing_info_present_flag
		write_bit(w, 0); // nal_hrd_parameters_present_flag
		write_bit(w, 0); // vcl_hrd_parameters_present_flag
		write_bit(w, 0); // pic_struct_present_flag
	}
	write_bit(w, 1); // bitstream_restriction_flag
	write_bit(w, 1); // motion_vectors_over_pic_boundaries_flag
	write_ue(w, 2); // max_bytes_per_pic_denom
	write_ue(w, 1); // max_bits_per_mb_denom
	write_ue(w, 15); // log2_max_mv_length_horizontal
	write_ue(w, 15); // log2_max_mv_length_vertical
	write_ue(w, 0); // max_num_reorder_frames
	write_ue(w, max_num_ref_frames); // max_dec_frame_buffering
}

int h264_sps_add_low_delay_vui(uint8_t* dst, size_t dst_size,
		const uint8_t* sps, size_t sps_size)
{
	if (sps_size < 2 || sps_size > H264_SPS_MAX_SIZE ||
			(sps[0] & 0x1f) != 7)
		return -1;

	uint8_t rbsp[H264_SPS_MAX_SIZE];
	size_t rbsp_size = 0;
	int zeros = 0;
	for (size_t i = 1; i < sps_size; ++i) {
		if (zeros >= 2 && sps[i] == 3) {
			zeros = 0;
			continue;
		}
		zeros = sps[i] == 0 ? zeros + 1 : 0;
		rbsp[rbsp_size++] = sps[i];
	}

	struct bit_reader reader = { .data = rbsp, .size = rbsp_size };
	size_t insert_pos = 0;
	uint32_t max_num_ref_frames = 0;
	bool has_vui = false;
	if (find_insert_pos(&reader, &insert_pos, &max_num_ref_frames,
				&has_vui) < 0)
		return -1;

	/* The bitstream restriction is the last part of the VUI, so everything
	 * behind it is only the old trailing bits.
	 */
	uint8_t out[H264_SPS_MAX_SIZE + H264_SPS_VUI_EXTRA_SIZE];
	struct bit_writer writer = { .data = out, .size = sizeof(out) };
	reader.pos = 0;
	for (size_t i = 0; i < insert_pos; ++i)
		write_bit(&writer, read_bit(&reader));
	write_low_delay_vui(&writer, max_num_ref_frames, has_vui);

	// rbsp_trailing_bits
	write_bit(&writer, 1);
	while (writer.pos % 8)
		write_bit(&writer, 0);
	if (writer.error)
		return -1;

	size_t out_size = writer.pos / 8;
	size_t n = 0;
	if (dst_size < 1)
		return -1;
	dst[n++] = sps[0];

	zeros = 0;
	for (size_t i = 0; i < out_size; ++i) {
		if (zeros >= 2 && out[i] <= 3) {
			if (n >= dst_size)
				return -1;
			dst[n++] = 3;
			zeros = 0;
		}
		if (n >= dst_size)
			return -1;
		dst[n++] = out[i];
		zeros = out[i] == 0 ? zeros + 1 : 0;
	}

	return n;
}
