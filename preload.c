/* preload.c - LD_PRELOAD'ed hooks
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

#include "mrb.h"
#include "glgrab.h"

#include <X11/Xlib.h>
#include <GL/glx.h>
#include <dlfcn.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#pragma GCC visibility push(default)

#define DEFAULT_MRB_SIZE (256ULL << 20)

static struct mrb rb;
static time_t start_time;
static const char *mrb_path;

static uint64_t now(void) {
	struct timespec ts = {0, 0};
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * UINT64_C(1000000000) + ts.tv_nsec;
}

static unsigned long long str2int(const char *s, unsigned long long def) {
	if (!s)
		return def;

	char *end;
	errno = 0;

	long long int x = strtoll(s, &end, 0);
	return errno == 0 && *end == '\0' && x >= 0 ? x : def;
}

static bool init_mrb(void) {
	static enum state { virgin, going, done, failed } volatile state;

	enum state res = virgin;
	if (__atomic_compare_exchange_n(&state, &res, going, false, __ATOMIC_CONSUME, __ATOMIC_CONSUME)) {
		res = failed;

		uint64_t size = str2int(getenv("GLGRAB_BUFSIZE"), DEFAULT_MRB_SIZE);
		int err = mrb_create(&rb, mrb_path, size, str2int(getenv("GLGRAB_MAXFRAME"), size));
		if (err) {
			fprintf(stderr,
				"glgrab: failed to create ring buffer \"%s\" size %" PRIu64 ": %s\n",
				mrb_path, size, strerror(err));
		} else {
			start_time = now();
			res = done;
		}

		__atomic_store_n(&state, res, __ATOMIC_RELEASE);
	}

	return res == done;
}

static bool x11_error_found;

static int x11_error_handler(Display *dpy, XErrorEvent *ev) {
	x11_error_found = true;
	return 0;
}

static void get_window_size_x11(Display *dpy, XID win, unsigned *width, unsigned *height) {
	Window w;
	unsigned u;
	int i;
	XGetGeometry(dpy, win, &w, &i, &i, width, height, &u, &u);
}

static void get_window_size_glx(Display *dpy, XID drawable, unsigned *width, unsigned *height) {
	glXQueryDrawable(dpy, drawable, GLX_WIDTH, width);
	glXQueryDrawable(dpy, drawable, GLX_HEIGHT, height);
}

static volatile GLXContext mainctx;

void glgrab_glXDestroyContext(void (*real)(Display *, GLXContext), Display *dpy, GLXContext ctx) {
	GLXContext current = ctx;
	__atomic_compare_exchange_n(&mainctx, &current, NULL, false, __ATOMIC_RELEASE, __ATOMIC_RELAXED);
	real(dpy, ctx);
}

static unsigned linewidth(unsigned w) {
	return (w * 4 + 7) & ~7U;
}

void glgrab_glXSwapBuffers(void (*real)(Display *, GLXDrawable), Display *dpy, GLXDrawable drawable) {
	static volatile bool running = false;

	if (!mrb_path || !init_mrb() || __atomic_exchange_n(&running, true, __ATOMIC_ACQ_REL)) {
		real(dpy, drawable);
		return;
	}

	static struct glgrab_frame *frame;
	static GLuint pbo;
	static void (*get_window_size)(Display *, XID, unsigned *, unsigned *);

	unsigned width = 0, height = 0;
	GLXContext ctx = NULL, current = glXGetCurrentContext();

	if (__atomic_compare_exchange_n(&mainctx, &ctx, current, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
		ctx = current;
		glGenBuffers(1, &pbo);
		frame = NULL;

		XErrorHandler error_handler = XSetErrorHandler(x11_error_handler);
		x11_error_found = false;
		get_window_size_glx(dpy, drawable, &width, &height);
		XSync(dpy, false);
		XSetErrorHandler(error_handler);

		if (x11_error_found) {
			get_window_size = get_window_size_x11;
			get_window_size(dpy, drawable, &width, &height);
		} else
			get_window_size = get_window_size_glx;
	} else if (ctx != current) {
		real(dpy, drawable);
		return;
	} else {
		get_window_size(dpy, drawable, &width, &height);
	}

	GLint pixel_pack_buffer;
	glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &pixel_pack_buffer);
	glBindBuffer(GL_PIXEL_PACK_BUFFER, pbo);

	if (frame != NULL) {
		glGetBufferSubData(GL_PIXEL_PACK_BUFFER, 0, linewidth(frame->width) * frame->height, frame->data);

		frame = NULL;
		mrb_commit(&rb);
	}

	frame = mrb_reserve(&rb, sizeof(struct glgrab_frame) + linewidth(width) * height);
	if (frame != NULL) {
		GLint read_buffer, pack_alignment;
		glGetIntegerv(GL_READ_BUFFER, &read_buffer);
		glGetIntegerv(GL_PACK_ALIGNMENT, &pack_alignment);

		glReadBuffer(GL_BACK);
		glPixelStorei(GL_PACK_ALIGNMENT, 8);

		glBufferData(GL_PIXEL_PACK_BUFFER, linewidth(width) * height, NULL, GL_STREAM_READ);
		glReadPixels(0, 0, width, height, GL_BGRA, GL_UNSIGNED_BYTE, NULL);

		glReadBuffer(read_buffer);
		glPixelStorei(GL_PACK_ALIGNMENT, pack_alignment);
	} else {
		fprintf(stderr, "glgrab: failed to allocate frame %ux%u\n", width, height);
	}

	glBindBuffer(GL_PIXEL_PACK_BUFFER, pixel_pack_buffer);

	real(dpy, drawable);

	if (frame != NULL) {
		frame->width = width;
		frame->height = height;
		frame->ns = now() - start_time;
	}

	__atomic_store_n(&running, false, __ATOMIC_RELEASE);
}

static void __attribute__((constructor)) init(void) {
	if ((mrb_path = getenv("GLGRAB_MRB"))) {
		void *h = dlopen(NULL, RTLD_LAZY);

		if (h) {
			if (!dlsym(h, "glgrab_glXSwapBuffers"))
				fprintf(stderr, "glgrab: failed to bind glXSwapBuffers hook: %s\n", dlerror());

			if (!dlsym(h, "glgrab_glXDestroyContext"))
				fprintf(stderr, "glgrab: failed to bind glXDestroyContext hook: %s\n", dlerror());

			dlclose(h);
		} else {
			fprintf(stderr, "glgrab: failed to dlopen() myself: %s\n", dlerror());
		}
	}
}

static void __attribute__((destructor)) destroy(void) {
	mrb_shutdown(&rb);
}
