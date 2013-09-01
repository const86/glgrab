#define _XOPEN_SOURCE 700

#include "glgrab.h"
#include "mrb.h"
#include <libavformat/avformat.h>
#include <libavutil/opt.h>

struct glgrab_priv {
	struct mrb rb;
	struct AVStream *stream;
};

static const struct AVOption options[] = {
	{NULL}
};

static const AVClass glgrab_class = {
	.class_name = "glgrab",
	.item_name = av_default_item_name,
	.option = options,
	.version = LIBAVUTIL_VERSION_INT
};

static int read_header(struct AVFormatContext *avctx) {
	struct glgrab_priv *const g = avctx->priv_data;
	avctx->ctx_flags = AVFMTCTX_NOHEADER;
	return AVERROR(mrb_open(&g->rb, avctx->filename));
}

static int read_packet(struct AVFormatContext *avctx, AVPacket *pkt) {
	struct glgrab_priv *const g = avctx->priv_data;
	int err = 0;

	for (bool retry = true; retry; mrb_release(&g->rb)) {
		const void *p;
		while (!mrb_reveal(&g->rb, &p)) {
			const struct timespec ts = {0, 10000000};
			clock_nanosleep(CLOCK_MONOTONIC, 0, &ts, NULL);
		}

		if (!p)
			return AVERROR_EOF;

		const struct glgrab_frame *frame = p, copy = *frame;
		if (!mrb_check(&g->rb))
			continue;

		if (!g->stream) {
			AVStream *s = g->stream = avformat_new_stream(avctx, NULL);
			AVCodecContext *codec = s->codec;
			codec->time_base = s->time_base = AV_TIME_BASE_Q;
			codec->codec_type = AVMEDIA_TYPE_VIDEO;
			codec->codec_id = AV_CODEC_ID_RAWVIDEO;
			codec->width = copy.width;
			codec->height = copy.height;
			codec->pix_fmt = AV_PIX_FMT_BGRA;
		}

		AVStream *s = g->stream;
		AVCodecContext *codec = s->codec;
		if (copy.width != codec->width || copy.height != codec->height)
			continue;

		int size = avpicture_get_size(codec->pix_fmt, copy.width, copy.height);

		err = av_new_packet(pkt, size);
		if (!err) {
			int stride = (copy.width + 1 & ~1) * 4;
			AVPicture pic = {
				{(uint8_t *)frame->data + stride * (copy.height - 1)},
				{-stride}
			};

			avpicture_layout(&pic, codec->pix_fmt, copy.width, copy.height, pkt->data, size);
			if (!mrb_check(&g->rb))
				continue;

			const AVRational ns = {1, 1000000000};
			pkt->pts = av_rescale_q_rnd(copy.ns, ns, s->time_base, AV_ROUND_NEAR_INF);
			pkt->dts = AV_NOPTS_VALUE;
			pkt->stream_index = s->index;
			pkt->flags = AV_PKT_FLAG_KEY;
		}

		retry = false;
	}

	return err;
}

static int read_close(struct AVFormatContext *avctx) {
	struct glgrab_priv *const g = avctx->priv_data;
	return AVERROR(mrb_close(&g->rb));
}

struct AVInputFormat glgrab_avformat = {
	.name = "glgrab",
	.long_name = "GLGrab",
	.flags = AVFMT_NOFILE | AVFMT_NOBINSEARCH | AVFMT_NOGENSEARCH | AVFMT_NO_BYTE_SEEK,
	.priv_data_size = sizeof(struct glgrab_priv),
	.read_header = read_header,
	.read_packet = read_packet,
	.read_close = read_close
};
