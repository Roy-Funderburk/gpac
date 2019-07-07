/*
 *			GPAC - Multimedia Framework C SDK
 *
 *			Authors: Jean Le Feuvre
 *					 Yacine Mathurin Boubacar Aziakou
 *					 Samir Mustapha
 *			Copyright (c) Telecom ParisTech 2019
 *					All rights reserved
 *
 *  This file is part of GPAC / HEVC tile split and rewrite filter
 *
 *  GPAC is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU Lesser General Public License as published by
 *  the Free Software Foundation; either version 2, or (at your option)
 *  any later version.
 *
 *  GPAC is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public
 *  License along with this library; see the file COPYING.  If not, write to
 *  the Free Software Foundation, 675 Mass Ave, Cambridge, MA 02139, USA.
 *
 */

#include <gpac/bitstream.h>
#include <gpac/filters.h>
#include <gpac/avparse.h>
#include <gpac/constants.h>
#include <gpac/internal/media_dev.h>

typedef struct
{
	GF_FilterPid *opid;
	u32 width, height, orig_x, orig_y;
	GF_FilterPacket *cur_pck;
	s32 did_it;
	u8 pid_idx;
} HEVCTilePid;

typedef struct
{
	GF_FilterPid *ipid;
	GF_List *tpidstack;
	u32 num_tiles, got_p, did_it;
	HEVCState hevc_state;
	u32 hevc_nalu_size_length, cfg_crc, nb_pck;
	char *buffer_nal; // writes some comments

	u32 buffer_nal_alloc;
	GF_BitStream *bs_nal_in;
} GF_HEVCSplitCtx;

//Transform slice 1d address into 2D address
static u32 get_new_slice_address_and_tiles_coordinates(u32 *x, u32 *y, HEVCState *hevc)
{
	HEVCSliceInfo si = hevc->s_info;
	u32 i, tbX = 0, tbY = 0, slX, slY, PicWidthInCtbsY, PicHeightInCtbsX, valX = 0, valY = 0;

	PicWidthInCtbsY = (si.sps->width + si.sps->max_CU_width - 1) / si.sps->max_CU_width;
	PicHeightInCtbsX = (si.sps->height + si.sps->max_CU_height - 1) / si.sps->max_CU_height;

	slY = si.slice_segment_address % PicWidthInCtbsY;
	slX = si.slice_segment_address / PicWidthInCtbsY;
	*x = 0;
	*y = 0;
	if (si.pps->tiles_enabled_flag)
	{
		if (si.pps->uniform_spacing_flag)
		{
			for (i = 0; i < si.pps->num_tile_rows; i++)
			{
				if (i < si.pps->num_tile_rows - 1)
					valX = (i + 1) * PicHeightInCtbsX / si.pps->num_tile_rows - i * PicHeightInCtbsX / si.pps->num_tile_rows;
				else
					valX = PicHeightInCtbsX - tbX;
				if (slX < tbX + valX)
				{
					*x = i;
					break;
				}
				else
					tbX += valX;
			}
			for (i = 0; i < si.pps->num_tile_columns; i++)
			{
				if (i < si.pps->num_tile_columns - 1)
					valY = (i + 1) * PicWidthInCtbsY / si.pps->num_tile_columns - i * PicWidthInCtbsY / si.pps->num_tile_columns;
				else
					valY = PicWidthInCtbsY - tbY;
				if (slY < tbY + valY)
				{
					*y = i;
					break;
				}
				else
					tbY += valY;
			}
		}
		else
		{
			for (i = 0; i < si.pps->num_tile_rows; i++)
			{
				if (i < si.pps->num_tile_rows - 1)
					valX = si.pps->row_height[i];
				else
					valX = PicHeightInCtbsX - tbX;
				if (slX < tbX + valX)
				{
					*x = i;
					break;
				}
				else
					tbX += valX;
			}
			for (i = 0; i < si.pps->num_tile_columns; i++)
			{
				if (i < si.pps->num_tile_columns - 1)
					valY = si.pps->column_width[i];
				else
					valY = PicWidthInCtbsY - tbY;
				if (slY < tbY + valY)
				{
					*y = i;
					break;
				}
				else
					tbY += valY;
			}
		}
	}

	return (slX - tbX)*valX + slY - tbY;
}
//get the width and the height of the tile in pixel
static void get_size_of_tile(HEVCState *hevc, u32 index_row, u32 index_col, u32 pps_id, u32 *width, u32 *height, u32 *tx, u32 *ty)
{
	u32 i, sps_id, tbX = 0, tbY = 0, PicWidthInCtbsY, PicHeightInCtbsX, max_CU_width, max_CU_height;

	sps_id = hevc->pps[pps_id].sps_id;
	max_CU_width = hevc->sps[sps_id].max_CU_width;
	max_CU_height = hevc->sps[sps_id].max_CU_height;
	PicWidthInCtbsY = (hevc->sps[sps_id].width + max_CU_width - 1) / max_CU_width;
	PicHeightInCtbsX = (hevc->sps[sps_id].height + max_CU_height - 1) / max_CU_height;

	if (tx) *tx = 0;
	if (ty) *ty = 0;

	if (hevc->pps[pps_id].tiles_enabled_flag)
	{
		if (hevc->pps[pps_id].uniform_spacing_flag)
		{
			if (index_row < hevc->pps[pps_id].num_tile_rows - 1) {
				for (i = 0; i < index_row; i++)
					tbX += ((i + 1) * PicHeightInCtbsX / hevc->pps[pps_id].num_tile_rows - i * PicHeightInCtbsX / hevc->pps[pps_id].num_tile_rows);

				*height = (index_row + 1) * PicHeightInCtbsX / hevc->pps[pps_id].num_tile_rows - index_row * PicHeightInCtbsX / hevc->pps[pps_id].num_tile_rows;
			}
			else {
				for (i = 0; i < hevc->pps[pps_id].num_tile_rows - 1; i++)
					tbX += ((i + 1) * PicHeightInCtbsX / hevc->pps[pps_id].num_tile_rows - i * PicHeightInCtbsX / hevc->pps[pps_id].num_tile_rows);
				*height = PicHeightInCtbsX - tbX;
			}

			if (index_col < hevc->pps[pps_id].num_tile_columns - 1) {
				for (i = 0; i < index_col; i++)
					tbY += ((i + 1) * PicWidthInCtbsY / hevc->pps[pps_id].num_tile_columns - i * PicWidthInCtbsY / hevc->pps[pps_id].num_tile_columns);
				*width = (index_col + 1) * PicWidthInCtbsY / hevc->pps[pps_id].num_tile_columns - index_col * PicWidthInCtbsY / hevc->pps[pps_id].num_tile_columns;
			}
			else {
				for (i = 0; i < hevc->pps[pps_id].num_tile_columns - 1; i++)
					tbY += ((i + 1) * PicWidthInCtbsY / hevc->pps[pps_id].num_tile_columns - i * PicWidthInCtbsY / hevc->pps[pps_id].num_tile_columns);
				*width = PicWidthInCtbsY - tbY;
			}
		}
		else
		{
			if (index_row < hevc->pps[pps_id].num_tile_rows - 1)
				*height = hevc->pps[pps_id].row_height[index_row];
			else {
				for (i = 0; i < hevc->pps[pps_id].num_tile_rows - 1; i++)
					tbX += hevc->pps[pps_id].row_height[i];
				*height = PicHeightInCtbsX - tbX;
			}
			if (index_col < hevc->pps[pps_id].num_tile_columns - 1)
				*width = hevc->pps[pps_id].column_width[index_col];
			else
			{
				for (i = 0; i < hevc->pps[pps_id].num_tile_columns - 1; i++)
					tbY += hevc->pps[pps_id].column_width[i];
				*width = PicWidthInCtbsY - tbY;
			}

		}
		*width = *width * max_CU_width;
		*height = *height * max_CU_height;
		if (ty) *ty = tbX * max_CU_height;
		if (tx) *tx = tbY * max_CU_width;

		if (*tx + *width > hevc->sps[sps_id].width)
			*width = hevc->sps[sps_id].width - *tx;
		if (*ty + *height > hevc->sps[sps_id].height)
			*height = hevc->sps[sps_id].height - *ty;

	}
}
//rewrite the profile and level
static void write_profile_tier_level(GF_BitStream *bs_in, GF_BitStream *bs_out, Bool ProfilePresentFlag, u8 MaxNumSubLayersMinus1)
{
	u8 j;
	Bool sub_layer_profile_present_flag[8], sub_layer_level_present_flag[8];
	if (ProfilePresentFlag) {
		gf_bs_write_int(bs_out, gf_bs_read_int(bs_in, 8), 8);
		gf_bs_write_long_int(bs_out, gf_bs_read_long_int(bs_in, 32), 32);
		gf_bs_write_int(bs_out, gf_bs_read_int(bs_in, 4), 4);
		gf_bs_write_long_int(bs_out, gf_bs_read_long_int(bs_in, 44), 44);
	}
	gf_bs_write_int(bs_out, gf_bs_read_int(bs_in, 8), 8);

	for (j = 0; j < MaxNumSubLayersMinus1; j++) {
		sub_layer_profile_present_flag[j] = gf_bs_read_int(bs_in, 1);
		gf_bs_write_int(bs_out, sub_layer_profile_present_flag[j], 1);
		sub_layer_level_present_flag[j] = gf_bs_read_int(bs_in, 1);
		gf_bs_write_int(bs_out, sub_layer_level_present_flag[j], 1);
	}
	if (MaxNumSubLayersMinus1 > 0)
		for (j = MaxNumSubLayersMinus1; j < 8; j++)
			gf_bs_write_int(bs_out, gf_bs_read_int(bs_in, 2), 2);


	for (j = 0; j < MaxNumSubLayersMinus1; j++) {
		if (sub_layer_profile_present_flag[j]) {
			gf_bs_write_int(bs_out, gf_bs_read_int(bs_in, 8), 8);
			gf_bs_write_int(bs_out, gf_bs_read_int(bs_in, 32), 32);
			gf_bs_write_int(bs_out, gf_bs_read_int(bs_in, 4), 4);
			gf_bs_write_long_int(bs_out, gf_bs_read_long_int(bs_in, 44), 44);
		}
		if (sub_layer_level_present_flag[j])
			gf_bs_write_int(bs_out, gf_bs_read_int(bs_in, 8), 8);
	}
}

void hevc_rewrite_sps(char *in_SPS, u32 in_SPS_length, u32 width, u32 height, char **out_SPS, u32 *out_SPS_length)
{
	GF_BitStream *bs_in, *bs_out;
	u64 length_no_use = 4096;
	u8 *data_without_emulation_bytes = NULL;
	u32 data_without_emulation_bytes_size = 0, sps_ext_or_max_sub_layers_minus1;
	u8 max_sub_layers_minus1 = 0, layer_id;
	Bool conformance_window_flag, multiLayerExtSpsFlag;
	u32 chroma_format_idc;

	bs_in = gf_bs_new(in_SPS, in_SPS_length, GF_BITSTREAM_READ);
	gf_bs_enable_emulation_byte_removal(bs_in, GF_TRUE);
	bs_out = gf_bs_new(NULL, length_no_use, GF_BITSTREAM_WRITE);
	/*dst_buffer_size =*/ gf_bs_get_size(bs_out);
	if (!bs_in) goto exit;

	//copy NAL Header
	gf_bs_write_int(bs_out, gf_bs_read_int(bs_in, 7), 7);
	layer_id = gf_bs_read_int(bs_in, 6);
	gf_bs_write_int(bs_out, layer_id, 6);
	gf_bs_write_int(bs_out, gf_bs_read_int(bs_in, 3), 3);

	gf_bs_write_int(bs_out, gf_bs_read_int(bs_in, 4), 4); //copy vps_id

	if (layer_id == 0)
	{
		max_sub_layers_minus1 = gf_bs_read_int(bs_in, 3);
		gf_bs_write_int(bs_out, max_sub_layers_minus1, 3);
	}
	else
	{
		sps_ext_or_max_sub_layers_minus1 = gf_bs_read_int(bs_in, 3);
		gf_bs_write_int(bs_out, sps_ext_or_max_sub_layers_minus1, 3);
	}
	multiLayerExtSpsFlag = (layer_id != 0) && (sps_ext_or_max_sub_layers_minus1 == 7);
	if (!multiLayerExtSpsFlag) {
		gf_bs_write_int(bs_out, gf_bs_read_int(bs_in, 1), 1);
		write_profile_tier_level(bs_in, bs_out, 1, max_sub_layers_minus1);
	}

	gf_bs_set_ue(bs_out, gf_bs_get_ue(bs_in)); //copy sps_id

	if (multiLayerExtSpsFlag) {
		u8 update_rep_format_flag = gf_bs_read_int(bs_in, 1);
		gf_bs_write_int(bs_out, update_rep_format_flag, 1);
		if (update_rep_format_flag) {
			gf_bs_write_int(bs_out, gf_bs_read_int(bs_in, 8), 8);
		}
	}
	else {
		chroma_format_idc = gf_bs_get_ue(bs_in);
		gf_bs_set_ue(bs_out, chroma_format_idc);
		if (chroma_format_idc == 3)
			gf_bs_write_int(bs_out, gf_bs_read_int(bs_in, 1), 1); // copy separate_colour_plane_flag
		/*w =*/ gf_bs_get_ue(bs_in); //skip width bits in input bitstream
		/*h =*/ gf_bs_get_ue(bs_in); //skip height bits in input bitstream

		//Copy the new width and height in output bitstream
		gf_bs_set_ue(bs_out, width);
		gf_bs_set_ue(bs_out, height);

		//Get rid of the bit conformance_window_flag
		conformance_window_flag = gf_bs_read_int(bs_in, 1);
		//put the new conformance flag to zero
		gf_bs_write_int(bs_out, 0, 1);

		//Skip the bits related to conformance_window_offset
		if (conformance_window_flag)
		{
			gf_bs_get_ue(bs_in);
			gf_bs_get_ue(bs_in);
			gf_bs_get_ue(bs_in);
			gf_bs_get_ue(bs_in);
		}
	}

	//copy and write the rest of the bits in the byte
	while (gf_bs_get_bit_position(bs_in) != 8) {
		gf_bs_write_int(bs_out, gf_bs_read_int(bs_in, 1), 1);
	}

	//copy and write the rest of the bytes
	while (gf_bs_get_size(bs_in) != gf_bs_get_position(bs_in)) {
		gf_bs_write_int(bs_out, gf_bs_read_u8(bs_in), 8); //watchout, not aligned in destination bitstream
	}


	gf_bs_align(bs_out);						//align

	gf_free(data_without_emulation_bytes);
	data_without_emulation_bytes = NULL;
	data_without_emulation_bytes_size = 0;

	gf_bs_get_content(bs_out, &data_without_emulation_bytes, &data_without_emulation_bytes_size);

	*out_SPS_length = data_without_emulation_bytes_size + gf_media_nalu_emulation_bytes_add_count(data_without_emulation_bytes, data_without_emulation_bytes_size);
	*out_SPS = gf_malloc(*out_SPS_length);
	*out_SPS_length = gf_media_nalu_add_emulation_bytes(data_without_emulation_bytes, *out_SPS, data_without_emulation_bytes_size);

	exit:
	gf_bs_del(bs_in);
	gf_bs_del(bs_out);
	gf_free(data_without_emulation_bytes);
}

static void rewrite_pps_no_grid(char *in_PPS, u32 in_PPS_length, char **out_PPS, u32 *out_PPS_length)
{
	u64 length_no_use = 0;
	u8 cu_qp_delta_enabled_flag, tiles_enabled_flag, loop_filter_across_slices_enabled_flag;

	GF_BitStream *bs_in;
	GF_BitStream *bs_out;

	u8 *data_without_emulation_bytes = NULL;
	u32 data_without_emulation_bytes_size = 0;

	bs_in = gf_bs_new(in_PPS, in_PPS_length, GF_BITSTREAM_READ);
	gf_bs_enable_emulation_byte_removal(bs_in, GF_TRUE);
	bs_out = gf_bs_new(NULL, length_no_use, GF_BITSTREAM_WRITE);
	if (!bs_in) goto exit;

	//Read and write NAL header bits
	gf_bs_write_int(bs_out, gf_bs_read_int(bs_in, 16), 16);
	gf_bs_set_ue(bs_out, gf_bs_get_ue(bs_in)); //pps_pic_parameter_set_id
	gf_bs_set_ue(bs_out, gf_bs_get_ue(bs_in)); //pps_seq_parameter_set_id
	gf_bs_write_int(bs_out, gf_bs_read_int(bs_in, 7), 7); //from dependent_slice_segments_enabled_flag to cabac_init_present_flag
	gf_bs_set_ue(bs_out, gf_bs_get_ue(bs_in)); //num_ref_idx_l0_default_active_minus1
	gf_bs_set_ue(bs_out, gf_bs_get_ue(bs_in)); //num_ref_idx_l1_default_active_minus1
	gf_bs_set_se(bs_out, gf_bs_get_se(bs_in)); //init_qp_minus26
	gf_bs_write_int(bs_out, gf_bs_read_int(bs_in, 2), 2); //from constrained_intra_pred_flag to transform_skip_enabled_flag
	cu_qp_delta_enabled_flag = gf_bs_read_int(bs_in, 1); //cu_qp_delta_enabled_flag
	gf_bs_write_int(bs_out, cu_qp_delta_enabled_flag, 1); //
	if (cu_qp_delta_enabled_flag)
		gf_bs_set_ue(bs_out, gf_bs_get_ue(bs_in)); // diff_cu_qp_delta_depth
	gf_bs_set_se(bs_out, gf_bs_get_se(bs_in)); // pps_cb_qp_offset
	gf_bs_set_se(bs_out, gf_bs_get_se(bs_in)); // pps_cr_qp_offset
	gf_bs_write_int(bs_out, gf_bs_read_int(bs_in, 4), 4); // from pps_slice_chroma_qp_offsets_present_flag to transquant_bypass_enabled_flag

	tiles_enabled_flag = gf_bs_read_int(bs_in, 1); // tiles_enabled_flag
	gf_bs_write_int(bs_out, 0, 1);
	gf_bs_write_int(bs_out, gf_bs_read_int(bs_in, 1), 1); // entropy_coding_sync_enabled_flag
	if (tiles_enabled_flag)
	{
		u32 num_tile_columns_minus1 = gf_bs_get_ue(bs_in);
		u32 num_tile_rows_minus1 = gf_bs_get_ue(bs_in);
		u8 uniform_spacing_flag = gf_bs_read_int(bs_in, 1);
		if (!uniform_spacing_flag)
		{
			u32 i;
			for (i = 0; i < num_tile_columns_minus1; i++)
				gf_bs_get_ue(bs_in);
			for (i = 0; i < num_tile_rows_minus1; i++)
				gf_bs_get_ue(bs_in);
		}
		gf_bs_read_int(bs_in, 1);
	}

	loop_filter_across_slices_enabled_flag = gf_bs_read_int(bs_in, 1);
	gf_bs_write_int(bs_out, loop_filter_across_slices_enabled_flag, 1);

	//copy and write the rest of the bits in the byte
	while (gf_bs_get_bit_position(bs_in) != 8) {
		gf_bs_write_int(bs_out, gf_bs_read_int(bs_in, 1), 1);
	}

	//copy and write the rest of the bytes
	while (gf_bs_get_size(bs_in) != gf_bs_get_position(bs_in)) {
		gf_bs_write_int(bs_out, gf_bs_read_u8(bs_in), 8); //watchout, not aligned in destination bitstream
	}

	gf_bs_align(bs_out);						//align
	gf_free(data_without_emulation_bytes);
	data_without_emulation_bytes = NULL;
	data_without_emulation_bytes_size = 0;

	gf_bs_get_content(bs_out, &data_without_emulation_bytes, &data_without_emulation_bytes_size);

	*out_PPS_length = data_without_emulation_bytes_size + gf_media_nalu_emulation_bytes_add_count(data_without_emulation_bytes, data_without_emulation_bytes_size);
	*out_PPS = gf_malloc(*out_PPS_length);
	gf_media_nalu_add_emulation_bytes(data_without_emulation_bytes, *out_PPS, data_without_emulation_bytes_size);

	exit:
	gf_bs_del(bs_in);
	gf_bs_del(bs_out);
	gf_free(data_without_emulation_bytes);
}

static void rewrite_slice_address(GF_HEVCSplitCtx *ctx, s32 new_address, char *in_slice, u32 in_slice_length, char **out_slice, u32 *out_slice_length, u32 final_width, u32 final_height, u32 crop_origin_x, u32 crop_origin_y)
{
	char *data_without_emulation_bytes = NULL;
	u32 data_without_emulation_bytes_size = 0;
	u64 header_end;
	u32 dst_buf_size = 0;
	u8 *dst_buf = NULL;
	u32 num_entry_point_start;
	u32 pps_id;
	GF_BitStream *bs_ori, *bs_rw;
	Bool RapPicFlag = GF_FALSE;
	u32 nb_bits_per_adress_dst = 0;
	HEVC_PPS *pps;
	HEVC_SPS *sps;
	u32 al, slice_size, slice_offset_orig, slice_offset_dst;
	u32 first_slice_segment_in_pic_flag;
	u32 dependent_slice_segment_flag;
	u8 nal_unit_type;
	HEVCState *hevc = &ctx->hevc_state;

	data_without_emulation_bytes = gf_malloc( in_slice_length * sizeof(char) ); // In slice content shall be written as a character, sizeof(char) is the length on which each character is coded.
	data_without_emulation_bytes_size = gf_media_nalu_remove_emulation_bytes(in_slice, data_without_emulation_bytes, in_slice_length);
	bs_ori = gf_bs_new(data_without_emulation_bytes, data_without_emulation_bytes_size, GF_BITSTREAM_READ);

	gf_bs_get_refreshed_size(bs_ori);
	bs_rw = gf_bs_new(NULL, 0, GF_BITSTREAM_WRITE);

	assert(hevc->s_info.header_size_bits >= 0);
	assert(hevc->s_info.entry_point_start_bits >= 0);
	header_end = (u64) hevc->s_info.header_size_bits;

	num_entry_point_start = (u32) hevc->s_info.entry_point_start_bits;

	// nal_unit_header			 
	gf_bs_write_int(bs_rw, gf_bs_read_int(bs_ori, 1), 1);
	nal_unit_type = gf_bs_read_int(bs_ori, 6);
	gf_bs_write_int(bs_rw, nal_unit_type, 6);
	gf_bs_write_int(bs_rw, gf_bs_read_int(bs_ori, 9), 9);

	first_slice_segment_in_pic_flag = gf_bs_read_int(bs_ori, 1);    //first_slice_segment_in_pic_flag
	if (new_address <= 0)
		gf_bs_write_int(bs_rw, 1, 1);
	else
		gf_bs_write_int(bs_rw, 0, 1);

	switch (nal_unit_type) {
	case GF_HEVC_NALU_SLICE_IDR_W_DLP:
	case GF_HEVC_NALU_SLICE_IDR_N_LP:
		RapPicFlag = GF_TRUE;
		break;
	case GF_HEVC_NALU_SLICE_BLA_W_LP:
	case GF_HEVC_NALU_SLICE_BLA_W_DLP:
	case GF_HEVC_NALU_SLICE_BLA_N_LP:
	case GF_HEVC_NALU_SLICE_CRA:
		RapPicFlag = GF_TRUE;
		break;
	}

	if (RapPicFlag) {
		//no_output_of_prior_pics_flag
		gf_bs_write_int(bs_rw, gf_bs_read_int(bs_ori, 1), 1);
	}

	pps_id = gf_bs_get_ue(bs_ori);					 //pps_id
	gf_bs_set_ue(bs_rw, pps_id);

	pps = &hevc->pps[pps_id];
	sps = &hevc->sps[pps->sps_id];

	dependent_slice_segment_flag = 0;
	if (!first_slice_segment_in_pic_flag && pps->dependent_slice_segments_enabled_flag) //dependent_slice_segment_flag READ
	{
		dependent_slice_segment_flag = gf_bs_read_int(bs_ori, 1);
	}
	else
	{
		dependent_slice_segment_flag = GF_FALSE;
	}
	if (!first_slice_segment_in_pic_flag) 						    //slice_segment_address READ
	{
		gf_bs_read_int(bs_ori, sps->bitsSliceSegmentAddress);
	}
	else {} 	//original slice segment address = 0

	if (new_address > 0) //new address != 0
	{
		if (pps->dependent_slice_segments_enabled_flag)				    //dependent_slice_segment_flag WRITE
		{
			gf_bs_write_int(bs_rw, dependent_slice_segment_flag, 1);
		}

		if (final_width && final_height) {
			u32 nb_CTUs = ((final_width + sps->max_CU_width - 1) / sps->max_CU_width) * ((final_height + sps->max_CU_height - 1) / sps->max_CU_height);
			nb_bits_per_adress_dst = 0;
			while (nb_CTUs > (u32)(1 << nb_bits_per_adress_dst)) {
				nb_bits_per_adress_dst++;
			}
		}
		else {
			nb_bits_per_adress_dst = sps->bitsSliceSegmentAddress;
		}

		gf_bs_write_int(bs_rw, new_address, nb_bits_per_adress_dst);	    //slice_segment_address WRITE
	}
	else {} //new_address = 0

	//copy over until num_entry_points
	while (num_entry_point_start != (gf_bs_get_position(bs_ori) - 1) * 8 + gf_bs_get_bit_position(bs_ori)) //Copy till the end of the header
	{
		gf_bs_write_int(bs_rw, gf_bs_read_int(bs_ori, 1), 1);
	}
	//write num_entry_points
	if (new_address >= 0) {
		gf_bs_set_ue(bs_rw, 0);
	}

	//write slice extension to 0
	if (pps->slice_segment_header_extension_present_flag)
		gf_bs_write_int(bs_rw, 0, 1);

	//we may have unparsed data in the source bitstream (slice header) due to entry points or slice segment extensions
	//TODO: we might want to copy over the slice extension header bits
	while (header_end != (gf_bs_get_position(bs_ori) - 1) * 8 + gf_bs_get_bit_position(bs_ori))
		gf_bs_read_int(bs_ori, 1);

	//read byte_alignment() is bit=1 + x bit=0
	al = gf_bs_read_int(bs_ori, 1);
	if (al != 1) {
		GF_LOG(GF_LOG_ERROR, GF_LOG_CODEC, ("[HEVCTileSplit] source slice header not properly aligned\n"));
	}

	//write byte_alignment() is bit=1 + x bit=0
	gf_bs_write_int(bs_rw, 1, 1);
	gf_bs_align(bs_rw);					//align

	gf_bs_get_content(bs_rw, &dst_buf, &dst_buf_size); /* bs_rw: header.*/
	slice_size = (u32) gf_bs_available(bs_ori);/* Slice_size: the rest of the slice after the header (no_emulation_bytes in it).*/
	slice_offset_orig = (u32) gf_bs_get_position(bs_ori); /* Slice_offset_orig: Immediate next byte after header_end (start of the slice_payload) */
	slice_offset_dst = dst_buf_size;/* Slice_offset_dst: bs_rw_size (size of our new header). */
	dst_buf_size += slice_size;/* Size of our new header plus the payload itself.*/
	dst_buf = gf_realloc(dst_buf, sizeof(char)*dst_buf_size);/* A buffer for our new header plus the payload. */
	memcpy(dst_buf + slice_offset_dst, data_without_emulation_bytes + slice_offset_orig, sizeof(char) * slice_size);

	gf_free(data_without_emulation_bytes);
	data_without_emulation_bytes = dst_buf;
	data_without_emulation_bytes_size = dst_buf_size;

	u32 slice_length = data_without_emulation_bytes_size + gf_media_nalu_emulation_bytes_add_count(data_without_emulation_bytes, data_without_emulation_bytes_size);
	if (*out_slice_length < slice_length)
		*out_slice = gf_realloc(*out_slice, slice_length);

	*out_slice_length = slice_length;
	gf_media_nalu_add_emulation_bytes(data_without_emulation_bytes, *out_slice, data_without_emulation_bytes_size);

	gf_bs_del(bs_ori);
	gf_bs_del(bs_rw);
	gf_free(data_without_emulation_bytes);
}
static char* extract(GF_Filter *filter, GF_HEVCSplitCtx *ctx, char *buffer, u32 buffer_length, u32 *original_coord, u32 *out_size)
{
	u8 nal_unit_type, temporal_id, layer_id;
	u32 buf_size,i,j;
	HEVCState *hevc = &ctx->hevc_state; // ctx->hevc_state already set.

	gf_media_hevc_parse_nalu(buffer, buffer_length, hevc, &nal_unit_type, &temporal_id, &layer_id);
	switch (nal_unit_type) {
	// For all those cases
	case GF_HEVC_NALU_SLICE_TRAIL_N:
	case GF_HEVC_NALU_SLICE_TRAIL_R:
	case GF_HEVC_NALU_SLICE_TSA_N:
	case GF_HEVC_NALU_SLICE_TSA_R:
	case GF_HEVC_NALU_SLICE_STSA_N:
	case GF_HEVC_NALU_SLICE_STSA_R:
	case GF_HEVC_NALU_SLICE_RADL_N:
	case GF_HEVC_NALU_SLICE_RADL_R:
	case GF_HEVC_NALU_SLICE_RASL_N:
	case GF_HEVC_NALU_SLICE_RASL_R:
	case GF_HEVC_NALU_SLICE_BLA_W_LP:
	case GF_HEVC_NALU_SLICE_BLA_W_DLP:
	case GF_HEVC_NALU_SLICE_BLA_N_LP:
	case GF_HEVC_NALU_SLICE_IDR_W_DLP:
	case GF_HEVC_NALU_SLICE_IDR_N_LP:
	case GF_HEVC_NALU_SLICE_CRA:
		i = j = 0;
		/*newAddress =*/ get_new_slice_address_and_tiles_coordinates(&i, &j, hevc);
		// matching_opid (or *original_coord = newAddress)
		*original_coord = i * hevc->s_info.pps->num_tile_columns + j; 

		GF_FilterPid * opid = gf_filter_get_opid(filter, *original_coord);
		HEVCTilePid * tpid = gf_filter_pid_get_udta(opid);

		//rewrite and copy the Slice
		buf_size = ctx->buffer_nal_alloc;
		// &ctx->buffer_nal to not have to declare (char *) always at the start of the function.
		rewrite_slice_address(ctx, -1, buffer, buffer_length, &ctx->buffer_nal, &buf_size, 0, 0,tpid->orig_x,tpid->orig_y); 
		ctx->did_it++;
		if (buf_size > ctx->buffer_nal_alloc)
			ctx->buffer_nal_alloc = buf_size;

		*out_size = buf_size;
		return ctx->buffer_nal;

	default:
		*out_size = buffer_length;
		return buffer;
	}
	return NULL;
}

static GF_Err rewrite_hevc_dsi(GF_HEVCSplitCtx *ctx , GF_FilterPid *opid, char *data, u32 size, u32 new_width, u32 new_height)
{
	u32 i, j;
	u8 *new_dsi;
	u32 new_size;
	GF_HEVCConfig *hvcc = NULL;
	// Profile, tier and level syntax ( nal class: Reserved and unspecified)
	hvcc = gf_odf_hevc_cfg_read(data, size, GF_FALSE); 
	if (!hvcc) return GF_NON_COMPLIANT_BITSTREAM;

	// for all the list objects in param_array
	for (i = 0; i < gf_list_count(hvcc->param_array); i++) { // hvcc->param_array:list object
		GF_HEVCParamArray *ar = (GF_HEVCParamArray *)gf_list_get(hvcc->param_array, i); // ar contains the i-th item in param_array
		for (j = 0; j < gf_list_count(ar->nalus); j++) { // for all the nalus the i-th param got
			/*! used for storing AVC sequenceParameterSetNALUnit and pictureParameterSetNALUnit*/
			GF_AVCConfigSlot *sl = (GF_AVCConfigSlot *)gf_list_get(ar->nalus, j); // store j-th nalus in *sl

			if (ar->type == GF_HEVC_NALU_SEQ_PARAM) {
				char *outSPS=NULL;
				u32 outSize=0;
				hevc_rewrite_sps(sl->data, sl->size, new_width, new_height, &outSPS, &outSize);
				gf_free(sl->data);
				sl->data = outSPS;
				sl->size= outSize;
			}
			else if (ar->type == GF_HEVC_NALU_VID_PARAM) {
			}
			else if (ar->type == GF_HEVC_NALU_PIC_PARAM) {
				char *outPPS=NULL;
				u32 outSize=0;
				rewrite_pps_no_grid(sl->data, sl->size, &outPPS, &outSize);
				gf_free(sl->data);
				sl->data = outPPS;
				sl->size = outSize;
			}
		}
	}
	gf_odf_hevc_cfg_write(hvcc, &new_dsi, &new_size);
	gf_odf_hevc_cfg_del(hvcc);

	gf_filter_pid_set_property(opid, GF_PROP_PID_DECODER_CONFIG, &PROP_DATA_NO_COPY(new_dsi, new_size));
	return GF_OK;
}

static void bsnal(char *output_nal, char *rewritten_nal, u32 out_nal_size, u32 hevc_nalu_size_length)
{
	GF_BitStream *bsnal = gf_bs_new(output_nal, hevc_nalu_size_length, GF_BITSTREAM_WRITE);
	gf_bs_write_int(bsnal, out_nal_size, 8 * hevc_nalu_size_length); // bsnal is full up there !! and output_nal holds out_nal_size written as bitstream on 8 * hevc_nalu_size_length
	// At output_nal, skeep ctx->hevc_nalu_size_length then write "rewritten_nal" on out_nal_size Bytes
	memcpy(output_nal + hevc_nalu_size_length, rewritten_nal, out_nal_size);
	gf_bs_del(bsnal);
}

static GF_Err hevcsplit_configure_pid(GF_Filter *filter, GF_FilterPid *pid, Bool is_remove)
{
	u32 cfg_crc = 0, codecid, o_width, o_height;
	s32 pps_id = -1, sps_id = -1;
	const GF_PropertyValue *p, *dsi;
	GF_Err e;
	// private stack of the filter returned as an object GF_HEVCSplitCtx.
	GF_HEVCSplitCtx *ctx = (GF_HEVCSplitCtx*)gf_filter_get_udta(filter); 

	if (is_remove) {
		GF_FilterPid *opid;
		HEVCTilePid  *tpid;
		u32 count;
		count = gf_filter_get_opid_count(filter);
		for (u32 i=0; i<count; i++) {
			opid = gf_filter_get_opid(filter, i);
			tpid = gf_filter_pid_get_udta(opid);

			if (tpid->opid) {
				gf_filter_pid_remove(tpid->opid);
				tpid->opid = NULL;
			}
			if (tpid) gf_free(tpid);
			if(opid){
				gf_filter_pid_set_udta(opid, NULL);
				gf_filter_pid_remove(opid);
			}
		}
		ctx->ipid = NULL;
		return GF_OK;
	}
	// checks if input pid matchs its destination filter.
	if (!gf_filter_pid_check_caps(pid))
		return GF_NOT_SUPPORTED;

	// Gets the build in property of the pid: here the Id of the pid.
	p = gf_filter_pid_get_property(pid, GF_PROP_PID_ID);
	// If null (no Id), Gets the build in property of the pid: here ESID Id of the pid.
	if (!p) p = gf_filter_pid_get_property(pid, GF_PROP_PID_ESID);
	// 'p' is an object fulfilled above, retrieve from it the variable value
	//as an unsigned int. Q: '.uint' not in the doc, otherwise where

	p = gf_filter_pid_get_property(pid, GF_PROP_PID_CODECID);
	codecid = p ? p->value.uint : 0; // If p isn't null, get the codecid else codecid = 0;
	if (!codecid) return GF_NOT_SUPPORTED; // if codecid = 0;
	if (codecid != GF_CODECID_HEVC) {
		return GF_NOT_SUPPORTED; // codec must be HEVC
	}

	dsi = gf_filter_pid_get_property(pid, GF_PROP_PID_DECODER_CONFIG);
	cfg_crc = 0;
	if (dsi && dsi->value.data.ptr && dsi->value.data.size) {
		// Takes buffer and buffer size to return an u32
		cfg_crc = gf_crc_32(dsi->value.data.ptr, dsi->value.data.size);
	}

	if (cfg_crc == ctx->cfg_crc) return GF_OK;
	ctx->cfg_crc = cfg_crc;
	ctx->ipid = pid;

	// parse otherwise they should refer to something else
	u32 i, j;
	GF_HEVCConfig *hvcc = NULL;

	memset(&ctx->hevc_state, 0, sizeof(HEVCState));
	ctx->hevc_state.full_slice_header_parse = GF_TRUE;

	// Profile, tier and level syntax ( nal class: Reserved and unspecified)
	hvcc = gf_odf_hevc_cfg_read(dsi->value.data.ptr, dsi->value.data.size, GF_FALSE);
	if (!hvcc) return GF_NON_COMPLIANT_BITSTREAM;
	ctx->hevc_nalu_size_length = hvcc->nal_unit_size;
	// for all the list objects in param_array
	for (i = 0; i < gf_list_count(hvcc->param_array); i++) { // hvcc->param_array:list object
		// ar contains the i-th item in param_array
		GF_HEVCParamArray *ar = (GF_HEVCParamArray *)gf_list_get(hvcc->param_array, i);
		for (j = 0; j < gf_list_count(ar->nalus); j++) { // for all the nalus the i-th param got
			/*! used for storing AVC sequenceParameterSetNALUnit and pictureParameterSetNALUnit*/
			GF_AVCConfigSlot *sl = (GF_AVCConfigSlot *)gf_list_get(ar->nalus, j); // store j-th nalus in *sl
			s32 idx;

			if (ar->type == GF_HEVC_NALU_SEQ_PARAM) {
				idx = gf_media_hevc_read_sps(sl->data, sl->size, &ctx->hevc_state);
				if ((idx >= 0) && sps_id<0)
					sps_id = idx;
			}
			else if (ar->type == GF_HEVC_NALU_PIC_PARAM) {
				s32 id = gf_media_hevc_read_pps(sl->data, sl->size, &ctx->hevc_state);
				if ((id >= 0) && pps_id<0)
					pps_id = id;
			}
		}
	}
	gf_odf_hevc_cfg_del(hvcc);

	if (pps_id < 0) return GF_NON_COMPLIANT_BITSTREAM;
	if (sps_id < 0) return GF_NON_COMPLIANT_BITSTREAM;
	// ORIGINAL_SIZE
	o_width = ctx->hevc_state.sps[sps_id].width;
	o_height = ctx->hevc_state.sps[sps_id].height;
	ctx->num_tiles = ctx->hevc_state.pps[pps_id].num_tile_rows * ctx->hevc_state.pps[pps_id].num_tile_columns;

	GF_LOG(GF_LOG_INFO, GF_LOG_CODEC, ("[HEVCTileSplit] Input stream %dx%d with %d tiles\n", o_width, o_height, ctx->num_tiles));

	// Done with parsing SPS/pps/vps
	u32 rows = ctx->hevc_state.pps[pps_id].num_tile_rows;
	u32 cols = ctx->hevc_state.pps[pps_id].num_tile_columns;

	for (i = 0; i < rows; i++) {
		for (j = 0; j < cols; j++) {
			HEVCTilePid *tpid ;
			GF_SAFEALLOC(tpid, HEVCTilePid);
			gf_list_add(ctx->tpidstack, tpid);
			tpid->opid = gf_filter_pid_new(filter);

			gf_filter_pid_set_udta(tpid->opid, tpid);
			assert(tpid);//TODO IS IT USEFUL ?

			get_size_of_tile(&ctx->hevc_state, i, j, pps_id, &tpid->width, &tpid->height, &tpid->orig_x, &tpid->orig_y);
			gf_filter_pid_copy_properties(tpid->opid, ctx->ipid);
			// for each output pid, set decoder_config, width and height.
			gf_filter_pid_set_property(tpid->opid, GF_PROP_PID_WIDTH, &PROP_UINT(tpid->width));
			gf_filter_pid_set_property(tpid->opid, GF_PROP_PID_HEIGHT, &PROP_UINT(tpid->height));
			gf_filter_pid_set_property(tpid->opid, GF_PROP_PID_CROP_POS, &PROP_VEC2I_INT(tpid->orig_x, tpid->orig_y));
			gf_filter_pid_set_property(tpid->opid, GF_PROP_PID_ORIG_SIZE, &PROP_VEC2I_INT(o_width, o_height));
			// rewrite the decoder config
			e = rewrite_hevc_dsi(ctx, tpid->opid, dsi->value.data.ptr, dsi->value.data.size, tpid->width, tpid->height);
			if (e) return e;
		}
	}
	gf_filter_pid_set_framing_mode(pid, GF_TRUE); // reaggregate to form complete frame.
	return GF_OK;
}

static GF_Err hevcsplit_process(GF_Filter *filter)
{
	u32 nb_eos = 0, hevc_nalu_size_length;
	u32 data_size, nal_length, opid_idx = 0;
	u8 temporal_id, layer_id, nal_unit_type, i; // On default we have a nalu
	char *data;
	GF_HEVCSplitCtx *ctx = (GF_HEVCSplitCtx*)gf_filter_get_udta(filter);
	// Gets the first packet in the input pid buffer
	GF_FilterPacket *pck_src = gf_filter_pid_get_packet(ctx->ipid);
	if (!pck_src) {
		if (gf_filter_pid_is_eos(ctx->ipid)) { // if end of stream
			nb_eos++;
			return GF_EOS;
		}
		return GF_OK;
	}
	data = (char *) gf_filter_pck_get_data(pck_src, &data_size); //  a packet contains only data
	//TODO: this is a clock signaling, for now just trash ..
	if (!data) {
		gf_filter_pid_drop_packet(ctx->ipid);
		return GF_OK;
	}
	/*create a bitstream reader from data
	 read from there.
	 data is the whole image cut out into "slice_in_pic"
	 */
	gf_bs_reassign_buffer(ctx->bs_nal_in, data, data_size);
	ctx->nb_pck++;
	GF_LOG(GF_LOG_DEBUG, GF_LOG_CODEC, ("[HEVCTileSplit] splitting frame %d DTS "LLU" CTS "LLU"\n", ctx->nb_pck, gf_filter_pck_get_dts(pck_src), gf_filter_pck_get_cts(pck_src)));

	u8 *output_nal, *rewritten_nal;
	u32 out_nal_size;
	//gf_filter_get_opid_count
	GF_FilterPid * opid;
	HEVCTilePid * tpid;
	while (gf_bs_available(ctx->bs_nal_in))
	{
		// ctx->hevc_nalu_size_length filled using hvcc
		nal_length = gf_bs_read_int(ctx->bs_nal_in, ctx->hevc_nalu_size_length * 8);
		u32 pos = (u32) gf_bs_get_position(ctx->bs_nal_in);
		// skip the content of the nal from bs to buffer (useful for the next nal)
		gf_bs_skip_bytes(ctx->bs_nal_in, nal_length);
		// data+pos is an address
		gf_media_hevc_parse_nalu(data+pos, nal_length, &ctx->hevc_state, &nal_unit_type, &temporal_id, &layer_id);
		/* todo: copy source NAL to ALL destination pids if type > 34
		   arrange the size of the tiles
		 */
		rewritten_nal = extract(filter, ctx, data+pos, nal_length, &opid_idx, &out_nal_size);
		if (!rewritten_nal) continue;

		hevc_nalu_size_length = ctx->hevc_nalu_size_length;
		if (nal_unit_type > 34)
		{
			for(i = 0; i < ctx->num_tiles; i++)
			{
				//gf_filter_get_opid_count
				opid = gf_filter_get_opid(filter, i);
				tpid = gf_filter_pid_get_udta(opid);
				if (!tpid->opid) {
					continue;
				}
				if (!tpid->cur_pck) {
					// ctx->hevc_nalu_size_length (field used to indicate the nal_length) + out_nal_size: (the nal itself)
					tpid->cur_pck = gf_filter_pck_new_alloc(tpid->opid, ctx->hevc_nalu_size_length + out_nal_size, &output_nal);
					// todo: might need to rewrite crypto info
					gf_filter_pck_merge_properties(pck_src, tpid->cur_pck);
				}
				else { // to be reexamined !
					u8 *data_start;
					u32 new_size;
					gf_filter_pck_expand(tpid->cur_pck, ctx->hevc_nalu_size_length + out_nal_size, &data_start, &output_nal, &new_size);
				}
				bsnal(output_nal, rewritten_nal, out_nal_size, hevc_nalu_size_length);
			}
		}
		else
		{
			opid = gf_filter_get_opid(filter, opid_idx);
			tpid = gf_filter_pid_get_udta(opid);
			if (!tpid->opid) {
				continue;
			}
			if (!tpid->cur_pck) {
				// ctx->hevc_nalu_size_length (field used to indicate the nal_length) + out_nal_size: (the nal itself)
				tpid->cur_pck = gf_filter_pck_new_alloc(tpid->opid, ctx->hevc_nalu_size_length + out_nal_size, &output_nal);
				gf_filter_pck_merge_properties(pck_src, tpid->cur_pck);
				// TODO: might need to rewrite crypto info
			}
			else { // TODO to be reexamined !
				u8 *data_start;
				u32 new_size;
				gf_filter_pck_expand(tpid->cur_pck, ctx->hevc_nalu_size_length + out_nal_size, &data_start, &output_nal, &new_size); // just the alloc
			}
			bsnal(output_nal, rewritten_nal, out_nal_size, hevc_nalu_size_length);	// Then the data itself
		}
	}
	gf_filter_pid_drop_packet(ctx->ipid);
	// done rewriting all nals from input, send all output
	for (u32 idx = 0; idx < ctx->num_tiles; idx++) {
		opid = gf_filter_get_opid(filter, idx);
		tpid = gf_filter_pid_get_udta(opid);

		if (!tpid->opid) continue;
		if (tpid->cur_pck) {
			gf_filter_pck_send(tpid->cur_pck);
			tpid->cur_pck = NULL;
		}
	}
	return GF_OK;
}

static GF_Err hevcsplit_initialize(GF_Filter *filter)
{
	GF_LOG(GF_LOG_DEBUG, GF_LOG_CODEC, ("[HEVCTileSplit] hevcsplit_initialize started.\n"));
	GF_HEVCSplitCtx *ctx = (GF_HEVCSplitCtx *) gf_filter_get_udta(filter);
	ctx->bs_nal_in = gf_bs_new((char *)ctx, 1, GF_BITSTREAM_READ);
	ctx->tpidstack = gf_list_new();
	return GF_OK;
}

static void hevcsplit_finalize(GF_Filter *filter)
{
	GF_LOG(GF_LOG_DEBUG, GF_LOG_CODEC, ("[HEVCTileSplit] hevcsplit_finalize started.\n"));
	GF_HEVCSplitCtx *ctx = (GF_HEVCSplitCtx *) gf_filter_get_udta(filter);
	if (ctx->buffer_nal) gf_free(ctx->buffer_nal);
	gf_bs_del(ctx->bs_nal_in);
	u32 i, count;
	HEVCTilePid *tpid;
	count = gf_list_count(ctx->tpidstack);
	for (i=0; i<count; i++) {
		tpid = gf_list_get(ctx->tpidstack, i);
		gf_free(tpid);
	}
	gf_list_del(ctx->tpidstack);
}

static const GF_FilterCapability HEVCSplitCaps[] =
{
		CAP_UINT(GF_CAPS_INPUT,GF_PROP_PID_STREAM_TYPE, GF_STREAM_VISUAL),
		CAP_UINT(GF_CAPS_INPUT,GF_PROP_PID_CODECID, GF_CODECID_HEVC),
		CAP_BOOL(GF_CAPS_INPUT_EXCLUDED, GF_PROP_PID_UNFRAMED, GF_TRUE),
		CAP_UINT(GF_CAPS_OUTPUT, GF_PROP_PID_STREAM_TYPE, GF_STREAM_VISUAL),
		CAP_UINT(GF_CAPS_OUTPUT, GF_PROP_PID_CODECID, GF_CODECID_HEVC)
};

#define OFFS(_n)	#_n, offsetof(GF_HEVCSplitCtx, _n)

static const GF_FilterArgs HEVCSplitArgs[] =
{
		{0}
};

GF_FilterRegister HEVCSplitRegister = {
		.name = "hevcsplit",
		GF_FS_SET_DESCRIPTION("HEVC tile spliter")
		GF_FS_SET_HELP("This filter splits a motion-constrained tiled HEVC PID into N independent HEVC PIDs.")
		.private_size = sizeof(GF_HEVCSplitCtx),
		SETCAPS(HEVCSplitCaps),
		//hevc split shall be explicitly loaded
		.flags = GF_FS_REG_EXPLICIT_ONLY,
		.initialize = hevcsplit_initialize,
		.finalize = hevcsplit_finalize,
		.args = HEVCSplitArgs,
		.configure_pid = hevcsplit_configure_pid,
		.process = hevcsplit_process,
};

const GF_FilterRegister* hevcsplit_register(GF_FilterSession *session)
{
	GF_LOG(GF_LOG_DEBUG, GF_LOG_CODEC, ("[HEVCTileSplit] hevcsplit_register started.\n"));
	return &HEVCSplitRegister;
}