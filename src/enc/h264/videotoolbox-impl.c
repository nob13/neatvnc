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

#include "enc/h264-encoder.h"
#include "neatvnc.h"
#include "frame.h"
#include "sys/queue.h"
#include "vec.h"
#include "usdt.h"

#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <time.h>
#include <assert.h>
#include <aml.h>

#include <TargetConditionals.h>
#include <CoreFoundation/CoreFoundation.h>
#include <CoreMedia/CoreMedia.h>
#include <CoreVideo/CoreVideo.h>
#include <VideoToolbox/VideoToolbox.h>

struct fb_queue_entry {
	struct nvnc_frame* fb;
	TAILQ_ENTRY(fb_queue_entry) link;
};

TAILQ_HEAD(fb_queue, fb_queue_entry);

struct h264_encoder_videotoolbox {
	struct h264_encoder base;

	uint32_t width;
	uint32_t height;
	int quality;

	VTCompressionSessionRef session;

	struct fb_queue fb_queue;

	struct aml_work* work;
	struct nvnc_frame* current_fb;
	struct vec current_packet;
	bool current_frame_is_keyframe;

	bool please_destroy;
};

struct h264_encoder_impl h264_encoder_videotoolbox_impl;

static const uint8_t start_code[] = { 0, 0, 0, 1 };

static uint64_t gettime_us(void)
{
	struct timespec ts = { 0 };
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000000ULL + ts.tv_nsec / 1000ULL;
}

static struct nvnc_frame* fb_queue_dequeue(struct fb_queue* queue)
{
	if (TAILQ_EMPTY(queue))
		return NULL;

	struct fb_queue_entry* entry = TAILQ_FIRST(queue);
	TAILQ_REMOVE(queue, entry, link);
	struct nvnc_frame* fb = entry->fb;
	free(entry);

	return fb;
}

static int fb_queue_enqueue(struct fb_queue* queue, struct nvnc_frame* fb)
{
	struct fb_queue_entry* entry = calloc(1, sizeof(*entry));
	if (!entry)
		return -1;

	entry->fb = fb;
	nvnc_frame_ref(fb);
	TAILQ_INSERT_TAIL(queue, entry, link);

	return 0;
}

static int set_property_int(VTCompressionSessionRef session, CFStringRef key,
		int value)
{
	CFNumberRef number = CFNumberCreate(NULL, kCFNumberIntType, &value);
	if (!number)
		return -1;

	OSStatus status = VTSessionSetProperty(session, key, number);
	CFRelease(number);
	return status == noErr ? 0 : -1;
}

static int set_property_float(VTCompressionSessionRef session, CFStringRef key,
		float value)
{
	CFNumberRef number = CFNumberCreate(NULL, kCFNumberFloatType, &value);
	if (!number)
		return -1;

	OSStatus status = VTSessionSetProperty(session, key, number);
	CFRelease(number);
	return status == noErr ? 0 : -1;
}

static bool sample_is_keyframe(CMSampleBufferRef sample)
{
	CFArrayRef attachments =
		CMSampleBufferGetSampleAttachmentsArray(sample, false);
	if (!attachments || CFArrayGetCount(attachments) == 0)
		return true;

	CFDictionaryRef attachment = CFArrayGetValueAtIndex(attachments, 0);
	return !CFDictionaryContainsKey(attachment,
			kCMSampleAttachmentKey_NotSync);
}

static int h264_encoder__append_parameter_sets(
		struct h264_encoder_videotoolbox* self,
		CMFormatDescriptionRef desc)
{
	size_t count = 0;
	OSStatus status = CMVideoFormatDescriptionGetH264ParameterSetAtIndex(
			desc, 0, NULL, NULL, &count, NULL);
	if (status != noErr)
		return -1;

	for (size_t i = 0; i < count; ++i) {
		const uint8_t* data = NULL;
		size_t size = 0;

		status = CMVideoFormatDescriptionGetH264ParameterSetAtIndex(
				desc, i, &data, &size, NULL, NULL);
		if (status != noErr)
			return -1;

		vec_append(&self->current_packet, start_code,
				sizeof(start_code));
		vec_append(&self->current_packet, data, size);
	}

	return 0;
}

/* VideoToolbox produces length prefixed NAL units (AVCC), but open-h264
 * requires an Annex B byte stream, so the length prefixes are replaced with
 * start codes.
 */
static int h264_encoder__append_nal_units(
		struct h264_encoder_videotoolbox* self,
		CMFormatDescriptionRef desc, CMBlockBufferRef block)
{
	int length_size = 0;
	OSStatus status = CMVideoFormatDescriptionGetH264ParameterSetAtIndex(
			desc, 0, NULL, NULL, NULL, &length_size);
	if (status != noErr || length_size < 1 || length_size > 4)
		return -1;

	CMBlockBufferRef contiguous = NULL;
	status = CMBlockBufferCreateContiguous(NULL, block, NULL, NULL, 0, 0,
			0, &contiguous);
	if (status != kCMBlockBufferNoErr)
		return -1;

	char* data = NULL;
	size_t size = 0;
	status = CMBlockBufferGetDataPointer(contiguous, 0, NULL, &size,
			&data);
	if (status != kCMBlockBufferNoErr) {
		CFRelease(contiguous);
		return -1;
	}

	int rc = 0;
	size_t pos = 0;
	while (pos + length_size <= size) {
		uint32_t nal_size = 0;
		for (int i = 0; i < length_size; ++i)
			nal_size = (nal_size << 8) | (uint8_t)data[pos + i];
		pos += length_size;

		if (nal_size > size - pos) {
			rc = -1;
			break;
		}

		vec_append(&self->current_packet, start_code,
				sizeof(start_code));
		vec_append(&self->current_packet, data + pos, nal_size);
		pos += nal_size;
	}

	CFRelease(contiguous);
	return rc;
}

static void h264_encoder__on_output(void* userdata, void* frame_userdata,
		OSStatus status, VTEncodeInfoFlags flags,
		CMSampleBufferRef sample)
{
	struct h264_encoder_videotoolbox* self = userdata;

	if (status != noErr) {
		nvnc_log(NVNC_LOG_ERROR, "Failed to encode frame: %d",
				(int)status);
		return;
	}

	if (!sample || (flags & kVTEncodeInfo_FrameDropped)) {
		nvnc_log(NVNC_LOG_WARNING, "Encoder dropped a frame");
		return;
	}

	CMFormatDescriptionRef desc =
		CMSampleBufferGetFormatDescription(sample);
	CMBlockBufferRef block = CMSampleBufferGetDataBuffer(sample);
	if (!desc || !block) {
		nvnc_log(NVNC_LOG_ERROR, "Encoded frame is incomplete");
		return;
	}

	if (sample_is_keyframe(sample) &&
			h264_encoder__append_parameter_sets(self, desc) < 0)
		goto failure;

	if (h264_encoder__append_nal_units(self, desc, block) < 0)
		goto failure;

	return;

failure:
	nvnc_log(NVNC_LOG_ERROR, "Failed to convert encoded frame");
	vec_clear(&self->current_packet);
}

static void h264_encoder__set_quality(struct h264_encoder_videotoolbox* self)
{
	/* The quality value is a QP value between 1 and 51. */
	if (__builtin_available(macOS 12.0, iOS 15.0, *)) {
		if (set_property_int(self->session,
				kVTCompressionPropertyKey_MaxAllowedFrameQP,
				self->quality) == 0)
			return;
	}

	float quality = 1.0f - (self->quality - 1) / 50.0f;
	set_property_float(self->session, kVTCompressionPropertyKey_Quality,
			quality);
}

static void h264_encoder__set_profile(struct h264_encoder_videotoolbox* self)
{
	/* open-h264 requires baseline profile, so we use constrained
	 * baseline if it is available.
	 */
	if (__builtin_available(macOS 12.0, iOS 15.0, *)) {
		CFStringRef level =
			kVTProfileLevel_H264_ConstrainedBaseline_AutoLevel;
		if (VTSessionSetProperty(self->session,
				kVTCompressionPropertyKey_ProfileLevel,
				level) == noErr)
			return;
	}

	VTSessionSetProperty(self->session,
			kVTCompressionPropertyKey_ProfileLevel,
			kVTProfileLevel_H264_Baseline_AutoLevel);
}

static int h264_encoder__init_session(struct h264_encoder_videotoolbox* self)
{
	CFMutableDictionaryRef spec = CFDictionaryCreateMutable(NULL, 0,
			&kCFTypeDictionaryKeyCallBacks,
			&kCFTypeDictionaryValueCallBacks);
	if (!spec)
		return -1;

#if TARGET_OS_OSX
	CFStringRef require_hw =
		kVTVideoEncoderSpecification_RequireHardwareAcceleratedVideoEncoder;
	CFDictionarySetValue(spec, require_hw, kCFBooleanTrue);
#endif

	/* Low latency rate control is not used because it drops frames, and
	 * open-h264 needs one packet for every frame.
	 */
	OSStatus status = VTCompressionSessionCreate(NULL, self->width,
			self->height, kCMVideoCodecType_H264, spec, NULL, NULL,
			h264_encoder__on_output, self, &self->session);
	CFRelease(spec);
	if (status != noErr) {
		nvnc_log(NVNC_LOG_DEBUG,
				"Failed to create compression session: %d",
				(int)status);
		return -1;
	}

	VTSessionSetProperty(self->session, kVTCompressionPropertyKey_RealTime,
			kCFBooleanTrue);

	/* B-frames are bad for latency */
	VTSessionSetProperty(self->session,
			kVTCompressionPropertyKey_AllowFrameReordering,
			kCFBooleanFalse);

	/* Zero means no limit. We'll select key frames manually. */
	set_property_int(self->session,
			kVTCompressionPropertyKey_MaxKeyFrameInterval, 0);

	h264_encoder__set_profile(self);
	h264_encoder__set_quality(self);

	// Encode BT.709 into the bitstream:
	VTSessionSetProperty(self->session,
			kVTCompressionPropertyKey_ColorPrimaries,
			kCVImageBufferColorPrimaries_ITU_R_709_2);
	VTSessionSetProperty(self->session,
			kVTCompressionPropertyKey_TransferFunction,
			kCVImageBufferTransferFunction_ITU_R_709_2);
	VTSessionSetProperty(self->session,
			kVTCompressionPropertyKey_YCbCrMatrix,
			kCVImageBufferYCbCrMatrix_ITU_R_709_2);

	status = VTCompressionSessionPrepareToEncodeFrames(self->session);
	if (status != noErr) {
		nvnc_log(NVNC_LOG_DEBUG,
				"Failed to prepare compression session: %d",
				(int)status);
		VTCompressionSessionInvalidate(self->session);
		CFRelease(self->session);
		self->session = NULL;
		return -1;
	}

	return 0;
}

static int h264_encoder__schedule_work(struct h264_encoder_videotoolbox* self)
{
	if (self->current_fb)
		return 0;

	self->current_fb = fb_queue_dequeue(&self->fb_queue);
	if (!self->current_fb)
		return 0;

	DTRACE_PROBE1(neatvnc, h264_encode_frame_begin, self->current_fb->pts);

	self->current_frame_is_keyframe =
		self->base.next_frame_should_be_keyframe;
	self->base.next_frame_should_be_keyframe = false;

	return aml_start(aml_get_default(), self->work);
}

static void h264_encoder__do_work(struct aml_work* work)
{
	struct h264_encoder_videotoolbox* self = aml_get_userdata(work);
	struct nvnc_frame* fb = self->current_fb;

	CVPixelBufferRef pixbuf = NULL;
	CVReturn cv_rc = CVPixelBufferCreateWithIOSurface(NULL,
			fb->buffer->iosurface, NULL, &pixbuf);
	if (cv_rc != kCVReturnSuccess) {
		nvnc_log(NVNC_LOG_ERROR, "Failed to wrap IOSurface: %d",
				(int)cv_rc);
		return;
	}

	CFDictionaryRef frame_props = NULL;
	if (self->current_frame_is_keyframe) {
		const void* keys[] = { kVTEncodeFrameOptionKey_ForceKeyFrame };
		const void* values[] = { kCFBooleanTrue };
		frame_props = CFDictionaryCreate(NULL, keys, values, 1,
				&kCFTypeDictionaryKeyCallBacks,
				&kCFTypeDictionaryValueCallBacks);
	}

	uint64_t pts = fb->pts != NVNC_NO_PTS ? fb->pts : gettime_us();

	// The output callback is called before CompleteFrames returns
	OSStatus status = VTCompressionSessionEncodeFrame(self->session,
			pixbuf, CMTimeMake(pts, 1000000), kCMTimeInvalid,
			frame_props, NULL, NULL);
	if (status == noErr)
		status = VTCompressionSessionCompleteFrames(self->session,
				kCMTimeInvalid);
	if (status != noErr)
		nvnc_log(NVNC_LOG_ERROR, "Failed to encode frame: %d",
				(int)status);

	if (frame_props)
		CFRelease(frame_props);
	CVPixelBufferRelease(pixbuf);
}

static void h264_encoder__on_work_done(struct aml_work* work)
{
	struct h264_encoder_videotoolbox* self = aml_get_userdata(work);

	uint64_t pts = nvnc_frame_get_pts(self->current_fb);
	nvnc_frame_unref(self->current_fb);
	self->current_fb = NULL;

	DTRACE_PROBE1(neatvnc, h264_encode_frame_end, pts);

	if (self->please_destroy) {
		h264_encoder_destroy(&self->base);
		return;
	}

	/* An empty packet is still passed on, so that open-h264 can finish
	 * the frame. The client has missed a frame, so it needs a key frame.
	 */
	if (self->current_packet.len == 0) {
		nvnc_log(NVNC_LOG_WARNING,
				"Whoops, encoded packet length is 0");
		self->base.next_frame_should_be_keyframe = true;
	}

	void* userdata = self->base.userdata;

	// Must make a copy of packet because the callback might destroy the
	// encoder object.
	struct vec packet;
	vec_init(&packet, self->current_packet.len);
	vec_append(&packet, self->current_packet.data,
			self->current_packet.len);

	vec_clear(&self->current_packet);
	h264_encoder__schedule_work(self);

	self->base.on_packet_ready(packet.data, packet.len, pts, userdata);
	vec_destroy(&packet);
}

static struct h264_encoder* h264_encoder_videotoolbox_create(uint32_t width,
		uint32_t height, uint32_t format, int quality)
{
	struct h264_encoder_videotoolbox* self = calloc(1, sizeof(*self));
	if (!self)
		return NULL;

	self->base.impl = &h264_encoder_videotoolbox_impl;

	if (vec_init(&self->current_packet, 65536) < 0)
		goto packet_failure;

	self->work = aml_work_new(h264_encoder__do_work,
			h264_encoder__on_work_done, self, NULL);
	if (!self->work)
		goto worker_failure;

	self->base.next_frame_should_be_keyframe = true;
	TAILQ_INIT(&self->fb_queue);

	/* VideoToolbox converts the pixel format of the input, so the format
	 * need not be checked here.
	 */
	self->width = width;
	self->height = height;
	self->quality = quality;

	if (h264_encoder__init_session(self) < 0)
		goto session_failure;

	return &self->base;

session_failure:
	aml_unref(self->work);
worker_failure:
	vec_destroy(&self->current_packet);
packet_failure:
	free(self);
	return NULL;
}

static void h264_encoder_videotoolbox_destroy(struct h264_encoder* base)
{
	struct h264_encoder_videotoolbox* self =
		(struct h264_encoder_videotoolbox*)base;

	if (self->current_fb) {
		self->please_destroy = true;
		return;
	}

	struct nvnc_frame* fb;
	while ((fb = fb_queue_dequeue(&self->fb_queue)))
		nvnc_frame_unref(fb);

	VTCompressionSessionInvalidate(self->session);
	CFRelease(self->session);
	vec_destroy(&self->current_packet);
	aml_unref(self->work);
	free(self);
}

static void h264_encoder_videotoolbox_feed(struct h264_encoder* base,
		struct nvnc_frame* fb)
{
	struct h264_encoder_videotoolbox* self =
		(struct h264_encoder_videotoolbox*)base;
	assert(fb->buffer->type == NVNC_BUFFER_IOSURFACE);

	// TODO: Add transform support
	assert(fb->transform == NVNC_TRANSFORM_NORMAL);

	int rc __attribute__((unused)) = fb_queue_enqueue(&self->fb_queue, fb);
	assert(rc == 0); // TODO

	rc = h264_encoder__schedule_work(self);
	assert(rc == 0); // TODO
}

struct h264_encoder_impl h264_encoder_videotoolbox_impl = {
	.create = h264_encoder_videotoolbox_create,
	.destroy = h264_encoder_videotoolbox_destroy,
	.feed = h264_encoder_videotoolbox_feed,
};
