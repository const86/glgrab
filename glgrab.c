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
#include "rgba2yuv420p.h"
#include "cuda.h"

#include <GL/gl.h>
#include <errno.h>
#include <inttypes.h>
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
	return errno == 0 && *end == '\0' && x >= 0 ? (unsigned long long)x : def;
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
	g->tex = 0;

	g->engine_shoot = NULL;
	g->engine_copy = NULL;
	g->engine_cleanup = NULL;

	g->frame = NULL;
	release(g, ready);
	return true;
}

static bool readpixels_shoot(struct glgrab *g, size_t width, size_t height, size_t pitch) {
	GLint pixel_pack_buffer = 0;
	glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &pixel_pack_buffer);
	glBindBuffer(GL_PIXEL_PACK_BUFFER, g->readpixels.pbo);

	GLint pack_row_length = 0;
	glGetIntegerv(GL_PACK_ROW_LENGTH, &pack_row_length);

	glPixelStorei(GL_PACK_ROW_LENGTH, pitch);
	glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, NULL);

	glPixelStorei(GL_PACK_ROW_LENGTH, pack_row_length);
	glBindBuffer(GL_PIXEL_PACK_BUFFER, pixel_pack_buffer);
	return true;
}

static bool readpixels_copy(struct glgrab *g) {
	GLint pixel_pack_buffer = 0;
	glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &pixel_pack_buffer);
	glBindBuffer(GL_PIXEL_PACK_BUFFER, g->readpixels.pbo);

	size_t size = g->frame->padded_width * g->frame->padded_height * 4;
	GLvoid *data = glMapBufferRange(GL_PIXEL_PACK_BUFFER, 0, size, GL_MAP_READ_BIT);

	if (data == NULL) {
		return false;
	}

	rgba2yuv420p(data, g->frame->data,
		g->frame->padded_width >> width_align,
		g->frame->padded_height >> height_align);

	glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
	glBindBuffer(GL_PIXEL_PACK_BUFFER, pixel_pack_buffer);
	return true;
}

static void readpixels_cleanup(struct glgrab *g) {
	glDeleteBuffers(1, &g->readpixels.pbo);
	g->readpixels.pbo = 0;
}

static void readpixels_init(struct glgrab *g, GLsizeiptr size) {
	GLint pixel_pack_buffer = 0;
	glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &pixel_pack_buffer);

	glGenBuffers(1, &g->readpixels.pbo);
	glBindBuffer(GL_PIXEL_PACK_BUFFER, g->readpixels.pbo);
	glBufferData(GL_PIXEL_PACK_BUFFER, size, NULL, GL_STREAM_READ);

	glBindBuffer(GL_PIXEL_PACK_BUFFER, pixel_pack_buffer);

	g->engine_shoot = readpixels_shoot;
	g->engine_copy = readpixels_copy;
	g->engine_cleanup = readpixels_cleanup;
}

static void debug_callback(GLenum source, GLenum type, GLuint id, GLenum severity,
	GLsizei length, const GLchar *message, const void *null) {
	fprintf(stderr, "glgrab: GL: %.*s\n", (int)length, message);
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

static uint32_t align(uint32_t x, short shift) {
	const uint32_t mask = ~(UINT32_C(-1) << shift);
	return (x + mask) >> shift;
}

bool glgrab_take_frame(struct glgrab *g, GLenum buffer, uint32_t width, uint32_t height) {
	if (!try_lock(g))
		return false;

	const uint32_t width32 = align(width, width_align);
	const uint32_t height2 = align(height, height_align);

	bool res = false;
	bool resize = true;

	check_error(g, "before grabbing");

	GLvoid *debug_callback_function = NULL;
	glGetPointerv(GL_DEBUG_CALLBACK_FUNCTION, &debug_callback_function);

	GLvoid *debug_callback_user_param = NULL;
	glGetPointerv(GL_DEBUG_CALLBACK_USER_PARAM, &debug_callback_user_param);

	glDebugMessageCallback(debug_callback, NULL);

	GLboolean debug_output = glIsEnabled(GL_DEBUG_OUTPUT);
	glEnable(GL_DEBUG_OUTPUT);

	if (g->frame != NULL) {
		bool copied = g->engine_copy(g);

		resize = g->frame->width != width || g->frame->height != height;

		if (copied && !check_error(g, "reading PBO")) {
			mrb_commit(&g->rb);
		}
	}

	g->frame = mrb_reserve(&g->rb, sizeof(struct glgrab_frame) + width32 * height2 * 96);
	if (g->frame != NULL) {
		const uint32_t padded_width = width32 << width_align;
		const uint32_t padded_height = height2 << height_align;

		GLint draw_buffers_n = 0;
		glGetIntegerv(GL_MAX_DRAW_BUFFERS, &draw_buffers_n);

		GLenum draw_buffers[draw_buffers_n];
		for (int i = 0; i < draw_buffers_n; i++) {
			GLint buf;
			glGetIntegerv(GL_DRAW_BUFFER0 + i, &buf);
			draw_buffers[i] = buf;
		}

		while (draw_buffers_n > 0 && draw_buffers[draw_buffers_n - 1] == GL_NONE) {
			--draw_buffers_n;
		}

		GLint read_buffer = 0;
		glGetIntegerv(GL_READ_BUFFER, &read_buffer);

		GLint read_fbo = 0, draw_fbo = 0;
		glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &read_fbo);
		glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &draw_fbo);

		glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g->fbo);

		if (resize) {
			if (g->engine_cleanup) {
				g->engine_cleanup(g);
				g->engine_cleanup = NULL;
			}

			if (g->tex) {
				glDeleteTextures(1, &g->tex);
			}

			glGenTextures(1, &g->tex);

			GLint tex = 0;
			glGetIntegerv(GL_TEXTURE_BINDING_RECTANGLE, &tex);

			glBindTexture(GL_TEXTURE_RECTANGLE, g->tex);
			glTexStorage2D(GL_TEXTURE_RECTANGLE, 1, GL_RGB8, width, height);
			glBindTexture(GL_TEXTURE_RECTANGLE, tex);

			glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_RECTANGLE, g->tex, 0);

			size_t size = padded_width * padded_height * 4;

			if (!cuda_init(g, size)) {
				readpixels_init(g, size);
			}
		}

		glDrawBuffers(1, &(GLenum){GL_COLOR_ATTACHMENT0});

		glBindFramebuffer(GL_READ_FRAMEBUFFER, draw_fbo);
		glReadBuffer(buffer);

		glBlitFramebuffer(0, height, width, 0, 0, 0, width, height, GL_COLOR_BUFFER_BIT, GL_NEAREST);

		glBindFramebuffer(GL_READ_FRAMEBUFFER, g->fbo);
		glReadBuffer(GL_COLOR_ATTACHMENT0);

		bool shot = g->engine_shoot(g, width, height, padded_width);

		glBindFramebuffer(GL_READ_FRAMEBUFFER, read_fbo);
		glBindFramebuffer(GL_DRAW_FRAMEBUFFER, draw_fbo);

		if (draw_buffers_n == 1) {
			glDrawBuffer(draw_buffers[0]);
		} else {
			glDrawBuffers(draw_buffers_n, draw_buffers);
		}

		glReadBuffer(read_buffer);

		if (!shot || check_error(g, "filling PBO")) {
			g->frame = NULL;
		} else {
			g->frame->width = width;
			g->frame->height = height;
			g->frame->padded_width = padded_width;
			g->frame->padded_height = padded_height;
			g->frame->ns = now() - g->start_time;
			res = true;
		}
	} else {
		fprintf(stderr, "glgrab: Failed to allocate frame %" PRIu32 "x%" PRIu32 " in buffer\n",
			width, height);
	}

	glDebugMessageCallback(debug_callback_function, debug_callback_user_param);

	if (!debug_output) {
		glDisable(GL_DEBUG_OUTPUT);
	}

	release(g, ready);
	return res;
}
