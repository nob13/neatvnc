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

#include <stdio.h>
#include <stdbool.h>
#include <string.h>

// SPS of a 1920x1080 constrained baseline stream from VideoToolbox, no VUI
static const uint8_t sps_without_vui[] = {
	0x27, 0x42, 0xc0, 0x28, 0xab, 0x40, 0x3c, 0x01, 0x13, 0xf2, 0xa0,
};

static const uint8_t sps_with_low_delay_vui[] = {
	0x27, 0x42, 0xc0, 0x28, 0xab, 0x40, 0x3c, 0x01, 0x13, 0xf2, 0xc0,
	0x36, 0x82, 0x01, 0x0a, 0x80,
};

// SPS of a 1512x982 stream from VideoToolbox, VUI with colour info only
static const uint8_t sps_with_colour_vui[] = {
	0x27, 0x42, 0xc0, 0x28, 0xab, 0x40, 0x2f, 0x83, 0xef, 0x2c, 0xd3,
	0x50, 0x20, 0x20, 0x10, 0x20,
};

static const uint8_t sps_with_colour_and_low_delay_vui[] = {
	0x27, 0x42, 0xc0, 0x28, 0xab, 0x40, 0x2f, 0x83, 0xef, 0x2c, 0xd3,
	0x50, 0x20, 0x20, 0x10, 0x6d, 0x04, 0x02, 0x15,
};

// PPS of the same stream
static const uint8_t pps[] = { 0x28, 0xce, 0x3c, 0x80 };

static bool test_add_vui(void)
{
	uint8_t buf[H264_SPS_MAX_SIZE + H264_SPS_VUI_EXTRA_SIZE];
	int size = h264_sps_add_low_delay_vui(buf, sizeof(buf),
			sps_without_vui, sizeof(sps_without_vui));
	return size == sizeof(sps_with_low_delay_vui) &&
		memcmp(buf, sps_with_low_delay_vui, size) == 0;
}

static bool test_extend_existing_vui(void)
{
	uint8_t buf[H264_SPS_MAX_SIZE + H264_SPS_VUI_EXTRA_SIZE];
	int size = h264_sps_add_low_delay_vui(buf, sizeof(buf),
			sps_with_colour_vui, sizeof(sps_with_colour_vui));
	return size == sizeof(sps_with_colour_and_low_delay_vui) &&
		memcmp(buf, sps_with_colour_and_low_delay_vui, size) == 0;
}

static bool test_keep_existing_restriction(void)
{
	uint8_t buf[H264_SPS_MAX_SIZE + H264_SPS_VUI_EXTRA_SIZE];
	return h264_sps_add_low_delay_vui(buf, sizeof(buf),
			sps_with_low_delay_vui,
			sizeof(sps_with_low_delay_vui)) < 0;
}

static bool test_reject_pps(void)
{
	uint8_t buf[H264_SPS_MAX_SIZE + H264_SPS_VUI_EXTRA_SIZE];
	return h264_sps_add_low_delay_vui(buf, sizeof(buf), pps,
			sizeof(pps)) < 0;
}

static bool test_reject_truncated(void)
{
	uint8_t buf[H264_SPS_MAX_SIZE + H264_SPS_VUI_EXTRA_SIZE];
	return h264_sps_add_low_delay_vui(buf, sizeof(buf), sps_without_vui,
			4) < 0;
}

static bool test_reject_small_buffer(void)
{
	uint8_t buf[sizeof(sps_with_low_delay_vui) - 1];
	return h264_sps_add_low_delay_vui(buf, sizeof(buf), sps_without_vui,
			sizeof(sps_without_vui)) < 0;
}

#define XSTR(s) STR(s)
#define STR(s) #s

#define RUN_TEST(name) ({ \
	bool ok = test_ ## name(); \
	printf("[%s] %s\n", ok ? " OK " : "FAIL", XSTR(name)); \
	ok; \
})

int main()
{
	bool ok = true;

	ok &= RUN_TEST(add_vui);
	ok &= RUN_TEST(extend_existing_vui);
	ok &= RUN_TEST(keep_existing_restriction);
	ok &= RUN_TEST(reject_pps);
	ok &= RUN_TEST(reject_truncated);
	ok &= RUN_TEST(reject_small_buffer);

	return ok ? 0 : 1;
}
