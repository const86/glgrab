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
	{NULL}
};

static const AVClass glgrab_class = {
	.class_name = "glgrab",
	.item_name = av_default_item_name,
	.option = options,
	.version = LIBAVUTIL_VERSION_INT
};

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
	codec->pix_fmt = AV_PIX_FMT_YUV420P;
	return 0;
}

static int read_header(struct AVFormatContext *avctx) {
	struct glgrab_priv *const g = avctx->priv_data;
	int rc = 0;

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

		int size = avpicture_get_size(AV_PIX_FMT_YUV420P, codec->width, codec->height);

		AVPacket pkt1 = {0};
		av_init_packet(&pkt1);
		err = av_new_packet(&pkt1, size);
		if (!err) {
			AVPicture src;
			avpicture_fill(&src, frame->data, AV_PIX_FMT_YUV420P,
				copy.padded_width, copy.padded_height);

			AVPicture pic;
			avpicture_fill(&pic, pkt1.data, AV_PIX_FMT_YUV420P, codec->width, codec->height);

			if (copy.width < codec->width || copy.height < codec->height)
				memset(pkt1.data, 0, pkt1.size);

			av_picture_copy(&pic, &src, AV_PIX_FMT_YUV420P, FFMIN(codec->width, copy.width),
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
