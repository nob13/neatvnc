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

#pragma once

#include <stddef.h>
#include <stdint.h>

/* Largest SPS that h264_sps_add_low_delay_vui() accepts. */
#define H264_SPS_MAX_SIZE 1024

/* Room that h264_sps_add_low_delay_vui() needs on top of the input size. */
#define H264_SPS_VUI_EXTRA_SIZE 16

/* Adds a bitstream restriction to the VUI of an SPS NAL unit (without start
 * code), declaring that frames are never reordered. The VUI is created if the
 * SPS has none. Without this, hardware decoders may hold back several frames
 * before they output the first one.
 *
 * Returns the size of the new NAL unit in dst, or -1 if the SPS already has a
 * bitstream restriction, cannot be parsed or dst is too small.
 */
int h264_sps_add_low_delay_vui(uint8_t* dst, size_t dst_size,
		const uint8_t* sps, size_t sps_size);
