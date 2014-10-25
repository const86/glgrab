/* GL grabbing itself
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
#define GL_GLEXT_PROTOTYPES

#include "glgrab.h"

#include <GL/gl.h>
#include <errno.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#pragma GCC visibility push(default)

#define DEFAULT_MRB_SIZE (256ULL << 20)

enum state {
	virgin,
	initializing,
	ready,
	using,
	failed
};

static uint64_t now(void) {
	struct timespec ts = {0, 0};
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * UINT64_C(1000000000) + ts.tv_nsec;
}

static void release(struct glgrab *g, enum state state) {
	__atomic_store_n(&g->state, state, __ATOMIC_RELEASE);
}

int glgrab_init(struct glgrab *g, const char *path, uint64_t bufsize, uint64_t max_frame_size) {
	int state = virgin;
	if (!__atomic_compare_exchange_n(&g->state, &state, initializing,
			false, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED)) {
		return state == failed ? EINVAL : 0;
	}

	int err = mrb_create(&g->rb, path, bufsize, max_frame_size);

	if (err == 0) {
		g->start_time = now();
		state = ready;
	} else {
		fprintf(stderr, "glgrab: Failed to create buffer \"%s\" size %" PRIu64 ": %s\n",
			path, bufsize, strerror(err));
		state = failed;
	}

	release(g, state);
	return err;
}

static unsigned long long str2int(const char *s, unsigned long long def) {
	if (!s)
		return def;

	char *end;
	errno = 0;

	long long int x = strtoll(s, &end, 0);
	return errno == 0 && *end == '\0' && x >= 0 ? x : def;
}

int glgrab_init_from_env(struct glgrab *g) {
	const char *path = getenv("GLGRAB_MRB");
	if (path == NULL)
		return EINVAL;

	uint64_t bufsize = str2int(getenv("GLGRAB_BUFSIZE"), DEFAULT_MRB_SIZE);
	return glgrab_init(g, path, bufsize, str2int(getenv("GLGRAB_MAXFRAME"), bufsize));
}

static bool try_lock(struct glgrab *g) {
	enum state state = ready;
	return __atomic_compare_exchange_n(&g->state, &state, using, false, __ATOMIC_ACQUIRE, __ATOMIC_ACQUIRE);
}

bool glgrab_destroy(struct glgrab *g) {
	if (!try_lock(g))
		return false;

	mrb_shutdown(&g->rb);
	release(g, virgin);
	return true;
}

bool glgrab_reset(struct glgrab *g) {
	if (!try_lock(g))
		return false;

	glGenFramebuffers(1, &g->fbo);
	glGenRenderbuffers(1, &g->rbo);
	glGenBuffers(1, &g->pbo);

	g->frame = NULL;
	release(g, ready);
	return true;
}

static unsigned linewidth(unsigned w) {
	return (w * 4 + 7) & ~7U;
}

static bool check_error(struct glgrab *g, const char msg[]) {
	GLenum error = glGetError();
	if (error == GL_NO_ERROR)
		return false;

	if (error != g->last_error) {
		g->last_error = error;
		fprintf(stderr, "glgrab: error %s: 0x%x\n", msg, error);
	}

	return true;
}

bool glgrab_take_frame(struct glgrab *g, uint32_t width, uint32_t height) {
	if (!try_lock(g))
		return false;

	bool res = false;
	bool resize = true;

	check_error(g, "before grabbing");

	GLint pixel_pack_buffer;
	glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &pixel_pack_buffer);
	glBindBuffer(GL_PIXEL_PACK_BUFFER, g->pbo);

	if (g->frame != NULL) {
		size_t size = linewidth(g->frame->width) * g->frame->height;
		GLvoid *data = glMapBufferRange(GL_PIXEL_PACK_BUFFER, 0, size, GL_MAP_READ_BIT);
		if (data != NULL) {
			memcpy(g->frame->data, data, size);
			glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
		}

		resize = g->frame->width != width || g->frame->height != height;

		if (!check_error(g, "reading PBO")) {
			mrb_commit(&g->rb);
		}
	}

	size_t size = linewidth(width) * height;
	g->frame = mrb_reserve(&g->rb, sizeof(struct glgrab_frame) + size);
	if (g->frame != NULL) {
		GLint draw_buffers_n;
		glGetIntegerv(GL_MAX_DRAW_BUFFERS, &draw_buffers_n);

		GLenum draw_buffers[draw_buffers_n];
		for (int i = 0; i < draw_buffers_n; i++) {
			GLint buf;
			glGetIntegerv(GL_DRAW_BUFFER0 + i, &buf);
			draw_buffers[i] = buf;
		}

		GLint read_buffer, pack_alignment;
		glGetIntegerv(GL_READ_BUFFER, &read_buffer);
		glGetIntegerv(GL_PACK_ALIGNMENT, &pack_alignment);

		GLint read_fbo, draw_fbo;
		glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &read_fbo);
		glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &draw_fbo);

		glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g->fbo);
		glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);

		glDrawBuffers(1, &(GLenum){GL_COLOR_ATTACHMENT0});
		glReadBuffer(GL_BACK);

		if (resize) {
			GLint rbo;
			glGetIntegerv(GL_RENDERBUFFER_BINDING, &rbo);
			glBindRenderbuffer(GL_RENDERBUFFER, g->rbo);
			glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA, width, height);
			glBindRenderbuffer(GL_RENDERBUFFER, rbo);

			glFramebufferRenderbuffer(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
				GL_RENDERBUFFER, g->rbo);

			glBufferData(GL_PIXEL_PACK_BUFFER, size, NULL, GL_STREAM_READ);
		}

		glBlitFramebuffer(0, 0, width, height, 0, 0, width, height, GL_COLOR_BUFFER_BIT, GL_NEAREST);

		glBindFramebuffer(GL_READ_FRAMEBUFFER, g->fbo);
		glReadBuffer(GL_COLOR_ATTACHMENT0);
		glPixelStorei(GL_PACK_ALIGNMENT, 8);

		glReadPixels(0, 0, width, height, GL_BGRA, GL_UNSIGNED_BYTE, NULL);

		glBindFramebuffer(GL_READ_FRAMEBUFFER, read_fbo);
		glBindFramebuffer(GL_DRAW_FRAMEBUFFER, draw_fbo);

		glDrawBuffers(draw_buffers_n, draw_buffers);
		glReadBuffer(read_buffer);
		glPixelStorei(GL_PACK_ALIGNMENT, pack_alignment);

		if (check_error(g, "filling PBO")) {
			g->frame = NULL;
		} else {
			g->frame->width = width;
			g->frame->height = height;
			g->frame->ns = now() - g->start_time;
			res = true;
		}
	} else {
		fprintf(stderr, "glgrab: Failed to allocate frame %ux%u in buffer\n", width, height);
	}

	glBindBuffer(GL_PIXEL_PACK_BUFFER, pixel_pack_buffer);

	release(g, ready);
	return res;
}
