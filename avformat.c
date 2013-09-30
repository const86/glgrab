/* GLGrab AVInputFormat
 *
 * Copyright 2013 Constantin Baranov
 *
 * This file is part of GLGrab.
 *
 * GLGrab is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * GLGrab is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with GLGrab.  If not, see <http://www.gnu.org/licenses/>.
 */

#define _XOPEN_SOURCE 700

#include "glgrab.h"
#include "mrb.h"
#include <float.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
#include <time.h>

#pragma GCC visibility push(default)

struct glgrab_priv {
	AVClass *class;
	struct mrb rb;
	struct AVStream *stream;

	AVRational framerate;
	int width, height;

	union {
		float s;
		struct timespec ts;
	} poll;

	enum AVPixelFormat pix_fmt;
	void (*convert_pix_fmt)(AVPicture *, const AVPicture *, int, int);

	int64_t last_pts;
	AVPacket pkt0;
	uint64_t ts0;
};

static const struct AVOption options[] = {
	{"framerate", NULL, offsetof(struct glgrab_priv, framerate), AV_OPT_TYPE_RATIONAL,
	 {.dbl = AV_TIME_BASE}, DBL_MIN, DBL_MAX, AV_OPT_FLAG_DECODING_PARAM},
	{"video_size", NULL, offsetof(struct glgrab_priv, width), AV_OPT_TYPE_IMAGE_SIZE,
	 {.str = NULL}, 0, 0, AV_OPT_FLAG_DECODING_PARAM},
	{"poll", "poll interval, in seconds", offsetof(struct glgrab_priv, poll), AV_OPT_TYPE_FLOAT,
	 {.dbl = 0}, 0, FLT_MAX, AV_OPT_FLAG_DECODING_PARAM},
	{"pixel_format", NULL, offsetof(struct glgrab_priv, pix_fmt), AV_OPT_TYPE_PIXEL_FMT,
	 {.i64 = AV_PIX_FMT_BGRA}, 0, 0, AV_OPT_FLAG_DECODING_PARAM},
	{NULL}
};

static const AVClass glgrab_class = {
	.class_name = "glgrab",
	.item_name = av_default_item_name,
	.option = options,
	.version = LIBAVUTIL_VERSION_INT
};

static void convert_bgra(AVPicture *dst, const AVPicture *src, int width, int height) {
	av_picture_copy(dst, src, AV_PIX_FMT_BGRA, width, height);
}

#define BT_709_KB 0.0722f
#define BT_709_KR 0.2126f

#define BT_601_KB 0.114f
#define BT_601_KR 0.299f

static void __attribute__((noinline)) convert_yuv420p_impl(const uint8_t *restrict BGRA, int BGRAS,
	uint8_t *restrict Y, int YS, uint8_t *restrict U, int US, uint8_t *restrict V, int VS,
	size_t width, size_t height) {
	const float KB = BT_709_KB;
	const float KR = BT_709_KR;

	typedef uint16_t word;

	const int SY = 8;
	const int SC = 8;

	const float KG = 1.f - KB - KR;
	const float KY = 220.f / 256.f;
	const float KC = 112.f / 256.f;

	const float KRY = KY * KR;
	const float KGY = KY * KG;
	const float KBY = KY * KB;
	const float KRUz = KC * KR / (1.f - KB);
	const float KGUz = KC * KG / (1.f - KB);
	const float KBU = KC;
	const float KRV = KC;
	const float KGVz = KC * KG / (1.f - KR);
	const float KBVz = KC * KB / (1.f - KR);

	const word Ybias = (16u << SY) + (1u << (SY - 1));
	const word KRYi = KRY * (1 << SY) + 0.5f;
	const word KGYi = KGY * (1 << SY) + 0.5f;
	const word KBYi = KBY * (1 << SY) + 0.5f;

	const word Cbias = (128u << SC) + (1u << (SC - 1));
	const word KRUzi = KRUz * (1 << SC) + 0.5f;
	const word KGUzi = KGUz * (1 << SC) + 0.5f;
	const word KBUi = KBU * (1 << SC) + 0.5f;
	const word KRVi = KRV * (1 << SC) + 0.5f;
	const word KGVzi = KGVz * (1 << SC) + 0.5f;
	const word KBVzi = KBVz * (1 << SC) + 0.5f;

	for (size_t i2 = 0; i2 < height / 2; i2++) {
		const uint8_t *row0 = BGRA + BGRAS * (i2 * 2);

		uint8_t *yrow0 = Y + YS * (i2 * 2);
		uint8_t *u = U + US * i2;
		uint8_t *v = V + VS * i2;

		for (size_t j2 = 0; j2 < width / 2; j2++) {
			const uint8_t *p0 = row0 + j2 * 2 * 4, *p1 = p0 + BGRAS;
			uint8_t *y0 = yrow0 + j2 * 2, *y1 = y0 + YS;

			const uint8_t b00 = p0[0], g00 = p0[1], r00 = p0[2], b01 = p0[4], g01 = p0[5], r01 = p0[6];
			const uint8_t b10 = p1[0], g10 = p1[1], r10 = p1[2], b11 = p1[4], g11 = p1[5], r11 = p1[6];

			y0[0] = (word)((word)(KRYi * r00 + KGYi * g00) + (word)(KBYi * b00 + Ybias)) >> SY;
			y0[1] = (word)((word)(KRYi * r01 + KGYi * g01) + (word)(KBYi * b01 + Ybias)) >> SY;
			y1[0] = (word)((word)(KRYi * r10 + KGYi * g10) + (word)(KBYi * b10 + Ybias)) >> SY;
			y1[1] = (word)((word)(KRYi * r11 + KGYi * g11) + (word)(KBYi * b11 + Ybias)) >> SY;

			const uint8_t r = (word)((word)(r00 + r01) + (word)(r10 + r11)) >> 2;
			const uint8_t g = (word)((word)(g00 + g01) + (word)(g10 + g11)) >> 2;
			const uint8_t b = (word)((word)(b00 + b01) + (word)(b10 + b11)) >> 2;

			u[j2] = (word)((word)(KBUi * b + Cbias) - (word)(KRUzi * r + KGUzi * g)) >> SC;
			v[j2] = (word)((word)(KRVi * r + Cbias) - (word)(KBVzi * b + KGVzi * g)) >> SC;
		}
	}
}

static void convert_yuv420p(AVPicture *restrict dst, const AVPicture *restrict src, int width, int height) {
	convert_yuv420p_impl(src->data[0], src->linesize[0], dst->data[0], dst->linesize[0],
		dst->data[1], dst->linesize[1], dst->data[2], dst->linesize[2], width, height);
}

static int setup_stream(struct AVFormatContext *avctx) {
	struct glgrab_priv *const g = avctx->priv_data;
	AVStream *s = g->stream = avformat_new_stream(avctx, NULL);
	if (s == NULL)
		return AVERROR(ENOMEM);

	s->r_frame_rate = g->framerate;

	AVCodecContext *codec = s->codec;
	codec->time_base = s->time_base = av_inv_q(g->framerate);
	codec->codec_type = AVMEDIA_TYPE_VIDEO;
	codec->codec_id = AV_CODEC_ID_RAWVIDEO;
	codec->width = FFALIGN(g->width, 2);
	codec->height = FFALIGN(g->height, 2);
	codec->pix_fmt = g->pix_fmt;
	return 0;
}

static int read_header(struct AVFormatContext *avctx) {
	struct glgrab_priv *const g = avctx->priv_data;
	int rc = 0;

	switch (g->pix_fmt) {

	case AV_PIX_FMT_BGRA:
		g->convert_pix_fmt = convert_bgra;
		break;

	case AV_PIX_FMT_YUV420P:
		g->convert_pix_fmt = convert_yuv420p;
		break;

	default:
		av_log(avctx, AV_LOG_ERROR, "Cannot output pixel format %s\n", av_get_pix_fmt_name(g->pix_fmt));
		return AVERROR(EINVAL);

	}

	if (g->poll.s > 0) {
		float poll = g->poll.s;
		g->poll.ts.tv_sec = poll;
		g->poll.ts.tv_nsec = FFMIN((poll - g->poll.ts.tv_sec) * 1e9, 999999999L);
	} else {
		g->poll.ts.tv_sec = g->poll.ts.tv_nsec = 0;
	}

	g->last_pts = -1;
	av_init_packet(&g->pkt0);

	if (g->width > 0 && g->height > 0) {
		rc = setup_stream(avctx);
	} else {
		avctx->ctx_flags = AVFMTCTX_NOHEADER;
	}

	avctx->duration = -1;
	return rc ? rc : AVERROR(mrb_open(&g->rb, avctx->filename));
}

static int read_packet(struct AVFormatContext *avctx, AVPacket *pkt) {
	struct glgrab_priv *const g = avctx->priv_data;
	int err = 0;

	for (bool retry = true; retry; mrb_release(&g->rb)) {
		const void *p;
		while (!mrb_reveal(&g->rb, &p)) {
			const struct timespec *ts = &g->poll.ts;
			if (ts->tv_sec || ts->tv_nsec)
				clock_nanosleep(CLOCK_MONOTONIC, 0, ts, NULL);
			else
				return AVERROR(EAGAIN);
		}

		if (!p) {
			if (g->pkt0.pts != AV_NOPTS_VALUE) {
				av_free_packet(pkt);
				*pkt = g->pkt0;
				g->pkt0 = (AVPacket){0};
				av_init_packet(&g->pkt0);
				return 0;
			}

			return AVERROR_EOF;
		}

		const struct glgrab_frame *frame = p, copy = *frame;
		if (!mrb_check(&g->rb))
			continue;

		if (!g->stream) {
			g->width = copy.width;
			g->height = copy.height;
			err = setup_stream(avctx);
			if (err) {
				retry = false;
				continue;
			}
		}

		AVStream *s = g->stream;
		AVCodecContext *codec = s->codec;

		const AVRational ns = {1, 1000000000};
		int64_t pts = av_rescale_q_rnd(copy.ns, ns, s->time_base, AV_ROUND_NEAR_INF);
		if (pts <= g->last_pts)
			continue;

		int size = avpicture_get_size(codec->pix_fmt, codec->width, codec->height);

		AVPacket pkt1 = {0};
		av_init_packet(&pkt1);
		err = av_new_packet(&pkt1, size);
		if (!err) {
			AVPicture src;
			avpicture_fill(&src, frame->data, AV_PIX_FMT_BGRA, FFALIGN(copy.width, 2), copy.height);
			src.data[0] += src.linesize[0] * (copy.height - 1);
			src.linesize[0] = -src.linesize[0];

			AVPicture pic;
			avpicture_fill(&pic, pkt1.data, codec->pix_fmt, codec->width, codec->height);

			if (copy.width < codec->width || copy.height < codec->height)
				memset(pkt1.data, 0, pkt1.size);

			g->convert_pix_fmt(&pic, &src, FFMIN(codec->width, copy.width),
				FFMIN(codec->height, copy.height));

			if (!mrb_check(&g->rb)) {
				av_free_packet(&pkt1);
				continue;
			}

			pkt1.pts = pts;
			pkt1.dts = pts;
			pkt1.stream_index = s->index;
			pkt1.flags = AV_PKT_FLAG_KEY;

			if (g->pkt0.pts == AV_NOPTS_VALUE) {
				if (av_compare_ts(copy.ns, ns, pkt1.pts, s->time_base) <= 0) {
					g->pkt0 = pkt1;
					g->ts0 = copy.ns;
					continue;
				} else {
					av_free_packet(pkt);
					*pkt = pkt1;
					g->last_pts = pkt1.pts;
				}
			} else if (pkt1.pts > g->pkt0.pts) {
				av_free_packet(pkt);
				*pkt = g->pkt0;
				g->last_pts = g->pkt0.pts;

				g->pkt0 = pkt1;
				g->ts0 = copy.ns;
			} else if (av_compare_ts(pkt1.pts, s->time_base, copy.ns, ns) < 0) {
				uint64_t ts = av_rescale_q_rnd(pkt1.pts, s->time_base, ns, AV_ROUND_NEAR_INF);

				g->last_pts = pkt1.pts;
				av_free_packet(pkt);

				if (ts - g->ts0 < copy.ns - ts) {
					*pkt = g->pkt0;
					av_free_packet(&pkt1);
				} else {
					*pkt = pkt1;
					av_free_packet(&g->pkt0);
				}

				g->pkt0 = (AVPacket){0};
				av_init_packet(&g->pkt0);
			} else {
				av_free_packet(&g->pkt0);
				g->pkt0 = pkt1;
				g->ts0 = copy.ns;
				continue;
			}
		}

		retry = false;
	}

	return err;
}

static int read_close(struct AVFormatContext *avctx) {
	struct glgrab_priv *const g = avctx->priv_data;
	av_free_packet(&g->pkt0);
	return AVERROR(mrb_close(&g->rb));
}

struct AVInputFormat glgrab_avformat = {
	.name = "glgrab",
	.long_name = "GLGrab",
	.flags = AVFMT_NOFILE | AVFMT_NOBINSEARCH | AVFMT_NOGENSEARCH | AVFMT_NO_BYTE_SEEK,
	.priv_class = &glgrab_class,
	.priv_data_size = sizeof(struct glgrab_priv),
	.read_header = read_header,
	.read_packet = read_packet,
	.read_close = read_close
};
