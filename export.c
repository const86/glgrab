/* Fast realtime encoder
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

#include <libavcodec/avcodec.h>
#include <libavdevice/avdevice.h>
#include <libavformat/avformat.h>
#include <libavutil/opt.h>
#include <libavutil/timestamp.h>
#include <libswscale/swscale.h>
#include <pthread.h>
#include <stdbool.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>
#include "glgrab.h"

static void swarm_help(const char *name) {
	fprintf(stderr, "\n"
		"Usage: %s [OPTION...] INPUT OUTPUT\n"
		"Options:\n"
		" -G  general options\n"
		" -i  demuxer name (may be guessed)\n"
		" -I  demuxer options\n"
		" -S  scaler flags (like neighbor or area+print_info)\n"
		" -e  encoder name\n"
		" -E  encoder options\n"
		" -o  muxer name (may be guessed)\n"
		" -O  muxer options\n"
		"\n"
		"General options:\n"
		" threads    number of threads\n"
		" pix_fmt    encoded picture format\n"
		" log_level  verbosity (like debug or verbose)\n"
		"\n"
		"Encoder, (de)muxer, and general options are comma separated key=value pairs.\n"
		"\n",
		name);
}

struct swarm_item {
	struct swarm_item *next;
	struct swarm_item *next_out;
	AVPacket pkt;
	volatile bool ready;
};

struct swarm_thread {
	pthread_t tid;
	struct swarm *swarm;
	struct SwsContext *scaler;
	AVCodecContext *encoder;
	struct swarm_item **ptail;
	struct swarm_item *head;
};

struct swarm {
	const AVClass *class;

	pthread_mutex_t demuxer_lock;
	AVFormatContext *demuxer;
	AVStream *istream;
	struct swarm_item *tail;

	pthread_spinlock_t muxer_lock;
	AVFormatContext *muxer;
	AVStream *ostream;
	struct swarm_item *head;

	struct swarm_thread *threads;
	int log_level;
	int sws_flags;
	enum AVPixelFormat pix_fmt;
	int nb_threads;
};

static void (*orig_int_handler)(int);
static volatile bool interrupted = false;

static void int_handler(int sig) {
	__atomic_store_n(&interrupted, true, __ATOMIC_RELEASE);
	const char msg[] = "Interrupt caught! Second interrupt will corrupt output file.\n";
	write(STDERR_FILENO, msg, sizeof(msg) - 1);
	sigset(sig, orig_int_handler);
}

static int swarm_thread_init(struct swarm_thread *t, struct swarm *swarm,
			AVCodec *encoder, AVDictionary *encoder_opts_tmpl) {
	int rc = 0;
	AVDictionary *encoder_opts = NULL;
	av_dict_copy(&encoder_opts, encoder_opts_tmpl, 0);

	t->swarm = swarm;
	t->ptail = &t->head;

	t->encoder = avcodec_alloc_context3(NULL);
	if (t->encoder == NULL) {
		rc = AVERROR(ENOMEM);
		goto fail;
	}

	rc = avcodec_copy_context(t->encoder, swarm->ostream->codec);
	if (rc != 0)
		goto fail;

	rc = avcodec_open2(t->encoder, encoder, &encoder_opts);
	if (rc != 0)
		goto fail;

	av_dict_free(&encoder_opts);

	AVCodecContext *decoder = swarm->istream->codec;
	if (t->encoder->pix_fmt != decoder->pix_fmt) {
		t->scaler = sws_getCachedContext(NULL,
						decoder->width, decoder->height, decoder->pix_fmt,
						t->encoder->width, t->encoder->height, t->encoder->pix_fmt,
						swarm->sws_flags, NULL, NULL, NULL);
		if (t->scaler == NULL) {
			rc = AVERROR(ENOMEM);
			goto fail;
		}
	}

	return 0;

fail:
	avcodec_close(t->encoder);
	av_freep(&t->encoder);
	av_dict_free(&encoder_opts);
	return rc;
}

static void swarm_thread_destroy(struct swarm_thread *t) {
	sws_freeContext(t->scaler);
	avcodec_close(t->encoder);
	av_freep(&t->encoder);
}

static int swarm_init(struct swarm *swarm, int argc, char **argv) {
	const AVClass *sws_class = sws_get_class();
	const AVOption *sws_flags_opt = av_opt_find2(&sws_class, "sws_flags", NULL, 0, 0, NULL);

	int rc = 0;
	int nb_threads = 0;

	AVInputFormat *demuxer = NULL;
	AVDictionary *demuxer_opts = NULL;
	AVCodec *encoder = NULL;
	AVDictionary *encoder_opts = NULL;
	AVOutputFormat *muxer = NULL;
	AVDictionary *muxer_opts = NULL;

	av_opt_set_defaults(swarm);
	swarm->sws_flags = SWS_AREA;

	for (int c; (c = getopt(argc, argv, "G:i:I:S:e:E:o:O:")) != -1;) {
		switch (c) {
		case 'G':
			rc = av_opt_set_from_string(swarm, optarg, NULL, "=", ",");
			if (rc > 0)
				rc = 0;
			break;
		case 'i':
			demuxer = av_find_input_format(optarg);
			if (demuxer == NULL)
				rc = AVERROR_DEMUXER_NOT_FOUND;
			break;
		case 'I':
			rc = av_dict_parse_string(&demuxer_opts, optarg, "=", ",", 0);
			break;
		case 'S':
			rc = av_opt_eval_flags(&sws_class, sws_flags_opt, optarg, &swarm->sws_flags);
			break;
		case 'e':
			encoder = avcodec_find_encoder_by_name(optarg);
			if (encoder == NULL)
				rc = AVERROR_ENCODER_NOT_FOUND;
			break;
		case 'E':
			rc = av_dict_parse_string(&encoder_opts, optarg, "=", ",", 0);
			break;
		case 'o':
			muxer = av_guess_format(optarg, NULL, NULL);
			if (muxer == NULL)
				rc = AVERROR_MUXER_NOT_FOUND;
			break;
		case 'O':
			rc = av_dict_parse_string(&muxer_opts, optarg, "=", ",", 0);
			break;
		default:
			swarm_help(argv[0]);
			rc = AVERROR_OPTION_NOT_FOUND;
			break;
		}

		if (rc != 0)
			goto fail;
	}

	av_log_set_level(swarm->log_level);

	if (argc - optind != 2) {
		av_log(swarm, AV_LOG_FATAL, "No input and/or output file specified\n");
		rc = AVERROR(EINVAL);
		goto fail;
	}

	rc = avformat_open_input(&swarm->demuxer, argv[optind], demuxer, &demuxer_opts);
	av_dict_free(&demuxer_opts);
	if (rc != 0)
		goto fail;

	rc = avformat_find_stream_info(swarm->demuxer, NULL);
	if (rc < 0)
		goto fail;

	for (int i = 0; i < swarm->demuxer->nb_streams; i++) {
		AVStream *s = swarm->demuxer->streams[i];
		if (s->codec->codec_type == AVMEDIA_TYPE_VIDEO) {
			swarm->istream = s;
			break;
		}
	}

	if (swarm->istream == NULL) {
		rc = AVERROR(EINVAL);
		goto fail;
	}

	rc = avcodec_open2(swarm->istream->codec, avcodec_find_decoder(swarm->istream->codec->codec_id), NULL);
	if (rc != 0)
		goto fail;

	rc = avformat_alloc_output_context2(&swarm->muxer, muxer, NULL, argv[optind + 1]);
	if (rc != 0)
		goto fail;

	rc = avio_open2(&swarm->muxer->pb, argv[optind + 1], AVIO_FLAG_WRITE, NULL, &muxer_opts);
	if (rc != 0)
		goto fail;

	swarm->ostream = avformat_new_stream(swarm->muxer, encoder);
	if (swarm->ostream == NULL) {
		rc = AVERROR(ENOMEM);
		goto fail;
	}

	swarm->ostream->codec->thread_count = 1;
	swarm->ostream->codec->gop_size = 1;
	swarm->ostream->codec->time_base = swarm->istream->time_base;
	swarm->ostream->codec->width = swarm->istream->codec->width;
	swarm->ostream->codec->height = swarm->istream->codec->height;

	if (swarm->muxer->oformat->flags & AVFMT_GLOBALHEADER)
		swarm->ostream->codec->flags |= CODEC_FLAG_GLOBAL_HEADER;

	if (swarm->pix_fmt != AV_PIX_FMT_NONE)
		swarm->ostream->codec->pix_fmt = swarm->pix_fmt;
	else
		swarm->ostream->codec->pix_fmt = swarm->istream->codec->pix_fmt;

	swarm->threads = av_calloc(swarm->nb_threads, sizeof(struct swarm_thread));
	if (swarm->threads == NULL) {
		rc = AVERROR(ENOMEM);
		goto fail;
	}

	for (; nb_threads < swarm->nb_threads; nb_threads++) {
		rc = swarm_thread_init(&swarm->threads[nb_threads], swarm, encoder, encoder_opts);
		if (rc != 0)
			goto fail;
	}

	avcodec_close(swarm->ostream->codec);
	rc = avcodec_copy_context(swarm->ostream->codec, swarm->threads[0].encoder);
	if (rc != 0)
		goto fail;

	rc = avformat_write_header(swarm->muxer, &muxer_opts);
	av_dict_free(&muxer_opts);
	if (rc != 0)
		goto fail;

	swarm->head = swarm->tail = av_mallocz(sizeof(struct swarm_item));
	if (swarm->head == NULL) {
		rc = AVERROR(ENOMEM);
		goto fail;
	}

	swarm->demuxer_lock = (pthread_mutex_t)PTHREAD_MUTEX_INITIALIZER;

	rc = pthread_spin_init(&swarm->muxer_lock, 0);
	if (rc != 0) {
		rc = AVERROR(rc);
		goto fail;
	}

	return 0;

fail:
	av_log(swarm, AV_LOG_FATAL, "swarm_init: %s\n", av_err2str(rc));

	pthread_mutex_destroy(&swarm->demuxer_lock);

	for (int i = 0; i < nb_threads; i++) {
		swarm_thread_destroy(&swarm->threads[i]);
	}
	av_freep(&swarm->threads);

	if (swarm->muxer != NULL) {
		avio_closep(&swarm->muxer->pb);
		avformat_free_context(swarm->muxer);
	}

	if (swarm->istream != NULL)
		avcodec_close(swarm->istream->codec);

	if (swarm->demuxer)
		avformat_close_input(&swarm->demuxer);

	av_freep(&swarm->tail);

	av_dict_free(&demuxer_opts);
	av_dict_free(&encoder_opts);
	av_dict_free(&muxer_opts);
	return rc;
}

static int swarm_close(struct swarm *swarm) {
	int rc = av_write_trailer(swarm->muxer);
	if (rc != 0)
		av_log(swarm, AV_LOG_ERROR, "av_write_trailer: %s\n", av_err2str(rc));

	for (int i = 0; i < swarm->nb_threads; i++) {
		swarm_thread_destroy(&swarm->threads[i]);
	}

	av_freep(&swarm->tail);
	av_freep(&swarm->threads);

	pthread_mutex_destroy(&swarm->demuxer_lock);
	pthread_spin_destroy(&swarm->muxer_lock);

	avio_closep(&swarm->muxer->pb);
	avformat_free_context(swarm->muxer);

	avcodec_close(swarm->istream->codec);
	avformat_close_input(&swarm->demuxer);
	return rc;
}

static int swarm_encode(struct swarm_thread *t, struct swarm_item *item, AVFrame *frame) {
	AVPacket pkt = {0};
	av_init_packet(&pkt);

	int got = 0;
	int rc = avcodec_encode_video2(t->encoder, &pkt, frame, &got);
	if (rc != 0)
		return rc;

	if (item) {
		*t->ptail = item;
		t->ptail = &item->next_out;
	}

	if (got == 0)
		return 0;

	av_log(t->swarm, AV_LOG_DEBUG, "encoded frame %s\n", av_ts2str(pkt.pts));

	if ((pkt.flags & AV_PKT_FLAG_KEY) == 0)
		av_log(t->swarm, AV_LOG_ERROR, "encoder gives non-key frame, stream will be corrupted!\n");

	item = t->head;
	t->head = item->next_out;
	item->pkt = pkt;

	if (t->head == NULL)
		t->ptail = &t->head;

	__atomic_store_n(&item->ready, true, __ATOMIC_RELEASE);
	return 0;
}

static int swarm_process_frame(struct swarm_thread *t, struct swarm_item *item, AVFrame *frame) {
	frame->pict_type = AV_PICTURE_TYPE_I;

	if (t->scaler == NULL)
		return swarm_encode(t, item, frame);

	int rc = 0;
	AVFrame *frame2 = avcodec_alloc_frame();
	if (frame2 == NULL) {
		rc = AVERROR(ENOMEM);
		goto fail;
	}

	AVPicture *pic = (AVPicture *)frame2;
	rc = avpicture_alloc(pic, t->encoder->pix_fmt, frame->width, frame->height);
	if (rc != 0)
		goto fail;

	sws_scale(t->scaler, (void *)frame->data, frame->linesize,
		0, frame->height, frame2->data, frame2->linesize);

	frame2->pts = frame->pts;
	frame2->pict_type = frame->pict_type;
	rc = swarm_encode(t, item, frame2);
	avpicture_free(pic);

fail:
	if (rc != 0)
		__atomic_store_n(&item->ready, true, __ATOMIC_RELEASE);

	avcodec_free_frame(&frame2);
	return rc;
}

static int swarm_read_frame(struct swarm *swarm, AVPacket *pkt, struct swarm_item **pitem, AVFrame **pframe) {
	int rc = pthread_mutex_lock(&swarm->demuxer_lock);
	if (rc != 0)
		return AVERROR(rc);

	AVFrame *frame = NULL;
	AVPacket pkt0 = {0};
	av_init_packet(&pkt0);

	if (__atomic_load_n(&interrupted, __ATOMIC_ACQUIRE)) {
		rc = AVERROR_EOF;
	} else {
		rc = av_read_frame(swarm->demuxer, &pkt0);
		if (rc == 0 && pkt0.stream_index != swarm->istream->index) {
			goto fail;
		}
	}

	if (rc == AVERROR_EOF) {
		rc = 0;
	} else if (rc != 0) {
		goto fail;
	} else {
		av_free_packet(pkt);
		if (swarm->istream->codec->codec_id == AV_CODEC_ID_RAWVIDEO &&
			swarm->istream->codec->pix_fmt != AV_PIX_FMT_PAL8) {
			*pkt = pkt0;
			pkt0 = (AVPacket){0};
		} else {
			rc = av_copy_packet(pkt, &pkt0);
			av_free_packet(&pkt0);
		}
	}

	if (rc != 0)
		goto fail;

	frame = avcodec_alloc_frame();
	if (frame == NULL) {
		rc = AVERROR(ENOMEM);
		goto fail;
	}

	int got = 0;
	rc = avcodec_decode_video2(swarm->istream->codec, frame, &got, pkt);
	if (rc >= 0)
		rc = 0;

	if (!got) {
		if (pkt->size == 0)
			rc = AVERROR_EOF;

		goto fail;
	}

	if (rc != 0)
		goto fail;

	struct swarm_item *item = av_mallocz(sizeof(*item));
	if (item == NULL) {
		rc = AVERROR(ENOMEM);
		goto fail;
	}

	frame->pts = av_rescale_q_rnd(frame->pkt_pts, swarm->istream->time_base,
				swarm->ostream->codec->time_base, AV_ROUND_NEAR_INF);

	swarm->tail->next = item;
	item = swarm->tail;
	swarm->tail = item->next;

	*pitem = item;
	*pframe = frame;
	frame = NULL;

fail:
	if (rc != 0)
		av_log(swarm,
			AVUNERROR(rc) == EAGAIN ? AV_LOG_VERBOSE : rc == AVERROR_EOF ? AV_LOG_INFO : AV_LOG_WARNING,
			"failed to read frame: %s\n", av_err2str(rc));
	else if (*pframe == NULL)
		av_log(swarm, AV_LOG_VERBOSE, "read frame %s, decoded nothing\n", av_ts2str(pkt0.pts));
	else
		av_log(swarm, AV_LOG_DEBUG, "read frame %s, decoded %s\n",
			av_ts2str(pkt0.pts), av_ts2str((*pframe)->pts));

	avcodec_free_frame(&frame);
	av_free_packet(&pkt0);
	pthread_mutex_unlock(&swarm->demuxer_lock);
	return rc;
}

static int swarm_write_frames(struct swarm *swarm) {
	int rc = pthread_spin_trylock(&swarm->muxer_lock);
	if (rc == EBUSY)
		return 0;

	if (rc != 0)
		return AVERROR(rc);

	for (struct swarm_item *item = swarm->head, *next;
	     next = item->next, rc == 0 && __atomic_load_n(&item->ready, __ATOMIC_ACQUIRE);
	     swarm->head = item = next) {
		AVPacket *pkt = &item->pkt;

		if (pkt->size > 0) {
			pkt->dts = av_rescale_q_rnd(pkt->dts, swarm->ostream->codec->time_base,
						swarm->ostream->time_base, AV_ROUND_NEAR_INF);
			pkt->pts = av_rescale_q_rnd(pkt->pts, swarm->ostream->codec->time_base,
						swarm->ostream->time_base, AV_ROUND_NEAR_INF);

			av_log(swarm, AV_LOG_DEBUG, "writing size:%d pts:%s\n", pkt->size, av_ts2str(pkt->pts));
			rc = av_write_frame(swarm->muxer, pkt);
		}

		av_free_packet(pkt);
		av_free(item);
	}

	pthread_spin_unlock(&swarm->muxer_lock);

	if (rc != 0)
		av_log(swarm, AV_LOG_WARNING, "av_write_frame: %s\n", av_err2str(rc));

	return rc;
}

static void *swarm_thread_main(void *arg) {
	struct swarm_thread *t = arg;
	struct swarm *swarm = t->swarm;
	int rc = 0;

	for (;;) {
		AVPacket pkt = {0};
		av_init_packet(&pkt);

		struct swarm_item *item = NULL;
		AVFrame *frame = NULL;
		rc = swarm_read_frame(swarm, &pkt, &item, &frame);

		if (AVUNERROR(rc) == EAGAIN) {
			const struct timespec ts = {0, 10000000};
			clock_nanosleep(CLOCK_MONOTONIC, 0, &ts, NULL);
			continue;
		}

		if (rc == 0 && item != NULL) {
			rc = swarm_process_frame(t, item, frame);
		} else if (rc == AVERROR_EOF) {
			rc = 0;

			while (rc == 0 && t->head != NULL) {
				rc = swarm_encode(t, NULL, NULL);
			}

			swarm_write_frames(swarm);
			break;
		}

		avcodec_free_frame(&frame);
		av_free_packet(&pkt);

		if (swarm_write_frames(swarm) != 0 || rc != 0)
			break;

	}

	return NULL;
}

static void swarm_run(struct swarm *swarm) {
	int nb_threads;

	for (nb_threads = 1; nb_threads < swarm->nb_threads; nb_threads++) {
		struct swarm_thread *t = &swarm->threads[nb_threads];
		int rc = pthread_create(&t->tid, NULL, swarm_thread_main, t);
		if (rc != 0) {
			av_log(swarm, AV_LOG_ERROR, "pthread_create: %s\n", av_err2str(AVERROR(rc)));
			break;
		}
	}

	struct swarm_thread *t = &swarm->threads[0];
	t->tid = pthread_self();
	swarm_thread_main(t);

	for (int i = 1; i < nb_threads; i++) {
		int rc = pthread_join(swarm->threads[i].tid, NULL);
		if (rc != 0)
			av_log(swarm, AV_LOG_ERROR, "pthread_join: %s\n", av_err2str(AVERROR(rc)));
	}
}

int main(int argc, char **argv) {
	avcodec_register_all();
	av_register_all();
	avdevice_register_all();
	av_register_input_format(&glgrab_avformat);

	const struct AVOption swarm_options[] = {
		{"threads", NULL, offsetof(struct swarm, nb_threads), AV_OPT_TYPE_INT, {.i64 = 1}, 1, INT_MAX},
		{"pix_fmt", NULL, offsetof(struct swarm, pix_fmt), AV_OPT_TYPE_PIXEL_FMT, {.i64 = AV_PIX_FMT_NONE}},

		{"log_level", NULL, offsetof(struct swarm, log_level), AV_OPT_TYPE_INT,
		 {.i64 = AV_LOG_INFO}, AV_LOG_QUIET, AV_LOG_DEBUG, .unit = "log_level"},
		{"quiet", NULL, 0, AV_OPT_TYPE_CONST, {.i64 = AV_LOG_QUIET}, .unit = "log_level"},
		{"panic", NULL, 0, AV_OPT_TYPE_CONST, {.i64 = AV_LOG_PANIC}, .unit = "log_level"},
		{"fatal", NULL, 0, AV_OPT_TYPE_CONST, {.i64 = AV_LOG_FATAL}, .unit = "log_level"},
		{"error", NULL, 0, AV_OPT_TYPE_CONST, {.i64 = AV_LOG_ERROR}, .unit = "log_level"},
		{"warning", NULL, 0, AV_OPT_TYPE_CONST, {.i64 = AV_LOG_WARNING}, .unit = "log_level"},
		{"info", NULL, 0, AV_OPT_TYPE_CONST, {.i64 = AV_LOG_INFO}, .unit = "log_level"},
		{"verbose", NULL, 0, AV_OPT_TYPE_CONST, {.i64 = AV_LOG_VERBOSE}, .unit = "log_level"},
		{"debug", NULL, 0, AV_OPT_TYPE_CONST, {.i64 = AV_LOG_DEBUG}, .unit = "log_level"},
		{NULL}
	};

	const struct AVClass swarm_class = {
		.class_name = "swarm",
		.item_name = av_default_item_name,
		.option = swarm_options,
		.version = LIBAVUTIL_VERSION_INT
	};

	struct swarm swarm = {
		.class = &swarm_class
	};

	orig_int_handler = sigset(SIGINT, int_handler);
	int rc = swarm_init(&swarm, argc, argv);

	if (rc == 0) {
		swarm_run(&swarm);
		rc = swarm_close(&swarm);
	}

	av_opt_free(&swarm);
	return !!rc;
}
