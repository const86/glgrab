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

#include <float.h>
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
		" -S  scaler options\n"
		" -e  encoder name\n"
		" -E  encoder options\n"
		" -o  muxer name (may be guessed)\n"
		" -O  muxer options\n"
		"\n"
		"Uppercased options are comma separated key=value pairs.\n"
		"\n"
		"General options:\n"
		" threads    number of threads\n"
		" log_level  verbosity (like debug or verbose)\n"
		" progress   progress report interval in seconds, disable if negative\n"
		"\n"
		"Scaler options:\n"
		" pix_fmt    target picture format\n"
		" size       target picture size\n"
		" sws_flags  tune scaler (like area or neighbor+print_info)\n"
		"\n",
		name);
}

struct swarm_item {
	struct swarm_item *next;
	struct swarm_item *next_out;
	AVPacket pkt;
	volatile bool ready;
};

struct swarm_scaler {
	const AVClass *class;
	struct SwsContext *ctx;
	enum AVPixelFormat pix_fmt;
	int width, height;
};

struct swarm_thread {
	pthread_t tid;
	struct swarm *swarm;
	struct swarm_scaler scaler;
	AVCodecContext *encoder;
	struct swarm_item **ptail;
	struct swarm_item *head;
};

struct swarm_progress {
	float interval;
	timer_t timer;
	AVRational *time_base;

	int64_t ts;
	unsigned long decoded;
	unsigned long encoded;
	unsigned long written;
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
	struct swarm_progress progress;
	int log_level;
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

static void swarm_progress_report(int sig, siginfo_t *si, void *ctx) {
	struct swarm_progress *p = si->si_value.sival_ptr;
	char buf[100];
	int n = snprintf(buf, sizeof(buf), "time: %s  frames: %lu >= %lu >= %lu\x1b[K\r",
		av_ts2timestr(__atomic_load_n(&p->ts, __ATOMIC_RELAXED), p->time_base),
		__atomic_load_n(&p->decoded, __ATOMIC_RELAXED),
		__atomic_load_n(&p->encoded, __ATOMIC_RELAXED),
		__atomic_load_n(&p->written, __ATOMIC_RELAXED));
	write(STDERR_FILENO, buf, FFMIN(n, sizeof(buf) - 1));
}

static void *swarm_scaler_child_next(void *obj, void *prev) {
	struct swarm_scaler *scaler = obj;
	return prev == NULL ? scaler->ctx : NULL;
}

static const struct AVOption swarm_scaler_options[] = {
	{"pix_fmt", NULL, offsetof(struct swarm_scaler, pix_fmt), AV_OPT_TYPE_PIXEL_FMT, {.i64 = AV_PIX_FMT_NONE}},
	{"size", NULL, offsetof(struct swarm_scaler, width), AV_OPT_TYPE_IMAGE_SIZE, {.str = NULL}},
	{NULL}
};

static const struct AVClass swarm_scaler_class = {
	.class_name = "swarm_scaler",
	.item_name = av_default_item_name,
	.option = swarm_scaler_options,
	.version = LIBAVUTIL_VERSION_INT,
	.child_next = swarm_scaler_child_next
};

static int swarm_thread_init(struct swarm_thread *t, struct swarm *swarm,
	AVDictionary *sws_opts_tmpl, AVCodec *encoder, AVDictionary *encoder_opts_tmpl) {
	int rc = 0;
	AVCodecContext *decoder = swarm->istream->codec;

	AVDictionary *encoder_opts = NULL;
	av_dict_copy(&encoder_opts, encoder_opts_tmpl, 0);

	AVDictionary *sws_opts = NULL;
	av_dict_copy(&sws_opts, sws_opts_tmpl, 0);

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

	t->scaler.class = &swarm_scaler_class;
	av_opt_set_defaults(&t->scaler);

	t->scaler.ctx = sws_alloc_context();
	if (t->scaler.ctx == NULL) {
		rc = AVERROR(ENOMEM);
		goto fail;
	}

	if (encoder->pix_fmts != NULL) {
		t->scaler.pix_fmt = encoder->pix_fmts[0];

		for (const enum AVPixelFormat *p = encoder->pix_fmts; *p != AV_PIX_FMT_NONE; p++) {
			if (*p == decoder->pix_fmt) {
				t->scaler.pix_fmt = *p;
				break;
			}
		}
	}

	t->scaler.width = decoder->width;
	t->scaler.height = decoder->height;
	av_opt_set_dict(&t->scaler, &sws_opts);

	av_opt_set_int(t->scaler.ctx, "sws_flags", SWS_AREA, 0);
	av_opt_set_int(t->scaler.ctx, "dstw", t->scaler.width, 0);
	av_opt_set_int(t->scaler.ctx, "dsth", t->scaler.height, 0);
	av_opt_set_int(t->scaler.ctx, "dst_format", t->scaler.pix_fmt, 0);
	av_opt_set_dict(t->scaler.ctx, &sws_opts);
	av_dict_free(&sws_opts);

	av_opt_set_int(t->scaler.ctx, "srcw", decoder->width, 0);
	av_opt_set_int(t->scaler.ctx, "srch", decoder->height, 0);
	av_opt_set_int(t->scaler.ctx, "src_format", decoder->pix_fmt, 0);

	rc = sws_init_context(t->scaler.ctx, NULL, NULL);
	if (rc != 0)
		goto fail;

	int64_t dstw = t->scaler.width, dsth = t->scaler.height, dst_format = t->scaler.pix_fmt;
	av_opt_get_int(t->scaler.ctx, "dstw", 0, &dstw);
	av_opt_get_int(t->scaler.ctx, "dsth", 0, &dsth);
	av_opt_get_int(t->scaler.ctx, "dst_format", 0, &dst_format);

	t->encoder->width = dstw;
	t->encoder->height = dsth;
	t->encoder->pix_fmt = dst_format;

	rc = avcodec_open2(t->encoder, encoder, &encoder_opts);
	if (rc != 0)
		goto fail;

	av_dict_free(&encoder_opts);
	return 0;

fail:
	sws_freeContext(t->scaler.ctx);
	avcodec_close(t->encoder);
	av_freep(&t->encoder);
	av_dict_free(&sws_opts);
	av_dict_free(&encoder_opts);
	return rc;
}

static void swarm_thread_destroy(struct swarm_thread *t) {
	sws_freeContext(t->scaler.ctx);
	av_opt_free(&t->scaler);

	avcodec_close(t->encoder);
	av_freep(&t->encoder);
}

static int swarm_init(struct swarm *swarm, int argc, char **argv) {
	int rc = 0;
	int nb_threads = 0;

	AVInputFormat *demuxer = NULL;
	AVDictionary *demuxer_opts = NULL;
	AVDictionary *sws_opts = NULL;
	AVCodec *encoder = NULL;
	AVDictionary *encoder_opts = NULL;
	AVOutputFormat *muxer = NULL;
	AVDictionary *muxer_opts = NULL;

	av_opt_set_defaults(swarm);

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
			rc = av_dict_parse_string(&sws_opts, optarg, "=", ",", 0);
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

	if (swarm->muxer->oformat->flags & AVFMT_GLOBALHEADER)
		swarm->ostream->codec->flags |= CODEC_FLAG_GLOBAL_HEADER;

	swarm->threads = av_calloc(swarm->nb_threads, sizeof(struct swarm_thread));
	if (swarm->threads == NULL) {
		rc = AVERROR(ENOMEM);
		goto fail;
	}

	for (; nb_threads < swarm->nb_threads; nb_threads++) {
		rc = swarm_thread_init(&swarm->threads[nb_threads], swarm, sws_opts, encoder, encoder_opts);
		if (rc != 0)
			goto fail;
	}

	av_dict_free(&sws_opts);
	av_dict_free(&encoder_opts);

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

	if (swarm->progress.interval > 0) {
		struct sigevent sev = {
			.sigev_notify = SIGEV_SIGNAL,
			.sigev_signo = SIGALRM,
			.sigev_value = {
				.sival_ptr = &swarm->progress
			}
		};

		rc = timer_create(CLOCK_MONOTONIC, &sev, &swarm->progress.timer);
		if (rc != 0) {
			rc = AVERROR(rc);
			goto fail;
		}

		swarm->progress.time_base = &swarm->istream->time_base;
	}

	rc = pthread_spin_init(&swarm->muxer_lock, 0);
	if (rc != 0) {
		rc = AVERROR(rc);
		goto fail_timer;
	}

	swarm->demuxer_lock = (pthread_mutex_t)PTHREAD_MUTEX_INITIALIZER;
	return 0;

fail_timer:
	timer_delete(swarm->progress.timer);

fail:
	av_log(swarm, AV_LOG_FATAL, "swarm_init: %s\n", av_err2str(rc));

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

	if (swarm->progress.interval > 0)
		timer_delete(swarm->progress.timer);

	while (swarm->head != NULL) {
		struct swarm_item *item = swarm->head;
		swarm->head = item->next;

		av_free_packet(&item->pkt);
		av_free(item);
	}

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

	__atomic_fetch_add(&t->swarm->progress.encoded, 1, __ATOMIC_RELAXED);
	__atomic_store_n(&item->ready, true, __ATOMIC_RELEASE);
	return 0;
}

static int swarm_process_frame(struct swarm_thread *t, struct swarm_item *item, AVFrame *frame) {
	int rc = 0;
	AVFrame *frame2 = avcodec_alloc_frame();
	if (frame2 == NULL) {
		rc = AVERROR(ENOMEM);
		goto fail;
	}

	AVPicture *pic = (AVPicture *)frame2;
	rc = avpicture_alloc(pic, t->encoder->pix_fmt, t->encoder->width, t->encoder->height);
	if (rc != 0)
		goto fail;

	sws_scale(t->scaler.ctx, (void *)frame->data, frame->linesize,
		0, frame->height, frame2->data, frame2->linesize);

	frame2->pts = frame->pts;
	frame2->pict_type = AV_PICTURE_TYPE_I;
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
		__atomic_store_n(&swarm->progress.ts, pkt0.pts, __ATOMIC_RELAXED);
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
	__atomic_fetch_add(&swarm->progress.decoded, 1, __ATOMIC_RELAXED);

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
			if (rc != 0)
				break;

			__atomic_fetch_add(&swarm->progress.written, 1, __ATOMIC_RELAXED);
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
	struct swarm_progress *progress = &swarm->progress;
	if (progress->interval > 0) {
		if (progress->interval < 1e-6)
			progress->interval = 1e-6;

		struct timespec ts = {progress->interval};
		ts.tv_nsec = FFMIN((progress->interval - ts.tv_sec) * 1e9, 999999999);

		struct itimerspec val = {ts, ts};
		if (timer_settime(progress->timer, 0, &val, NULL) != 0)
			av_log(swarm, AV_LOG_WARNING, "timer_settime(%lu.%09ld): %s\n",
				ts.tv_sec, ts.tv_nsec, av_err2str(AVERROR(errno)));
	} else {
		progress = 0;
	}

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

	if (progress != NULL) {
		struct itimerspec val = {{0, 0}};
		if (timer_settime(progress->timer, 0, &val, NULL) != 0)
			av_log(swarm, AV_LOG_WARNING, "timer_settime(0): %s\n", av_err2str(AVERROR(errno)));
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
		{"progress", NULL, offsetof(struct swarm, progress) + offsetof(struct swarm_progress, interval),
		 AV_OPT_TYPE_FLOAT, {.dbl = 1}, -FLT_MAX, FLT_MAX},

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

	struct sigaction sa = {
		.sa_sigaction = &swarm_progress_report,
		.sa_flags = SA_RESTART | SA_SIGINFO
	};
	sigemptyset(&sa.sa_mask);
	sigaction(SIGALRM, &sa, NULL);

	orig_int_handler = sigset(SIGINT, int_handler);
	int rc = swarm_init(&swarm, argc, argv);

	if (rc == 0) {
		swarm_run(&swarm);
		rc = swarm_close(&swarm);
	}

	av_opt_free(&swarm);
	return !!rc;
}
