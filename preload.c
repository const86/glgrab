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

#include "mrb.h"
#include "glgrab.h"

#include <X11/Xlib.h>
#include <GL/glx.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define DEFAULT_MRB_SIZE (256ULL << 20)
#define DEFAULT_MAX_FRAME_SIZE (4ULL * 4096 * 4096)

static struct mrb rb;
static bool started = false;
static time_t start_time;

void hook_glXSwapBuffers(void (*real)(Display *, GLXDrawable), Display *dpy, GLXDrawable drawable) {
	if (!real) return;

	if (!started)
		goto fail;

	unsigned width = 0, height = 0;
	glXQueryDrawable(dpy, drawable, GLX_WIDTH, &width);
	glXQueryDrawable(dpy, drawable, GLX_HEIGHT, &height);

	unsigned stride = (width * 4 + 7U) & ~7U;
	struct glgrab_frame *frame = mrb_reserve(&rb, sizeof(struct glgrab_frame) + stride * height);
	if (!frame)
		goto fail;

	frame->width = width;
	frame->height = height;

	glPushAttrib(GL_PIXEL_MODE_BIT);
	glPushClientAttrib(GL_CLIENT_PIXEL_STORE_BIT);
	glReadBuffer(GL_BACK);
	glPixelStorei(GL_PACK_ALIGNMENT, 8);
	glReadPixels(0, 0, width, height, GL_BGRA, GL_UNSIGNED_BYTE, frame->data);
	glPopClientAttrib();
	glPopAttrib();

	real(dpy, drawable);

	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	frame->ns = (ts.tv_sec - start_time) * 1000000000ULL + ts.tv_nsec;

	mrb_commit(&rb);
	return;

fail:
	real(dpy, drawable);
}

static unsigned long long str2int(const char *s, unsigned long long def) {
	if (!s)
		return def;

	char *end;
	errno = 0;

	long long int x = strtoll(s, &end, 0);
	return errno == 0 && *end == '\0' && x >= 0 ? x : def;
}

void __attribute__((constructor)) init(void) {
	const char *prefix = getenv("GLGRAB_PREFIX");
	if (prefix == NULL)
		return;

	char name[strlen(prefix) + 30];
	snprintf(name, sizeof(name), "%s-%llu.mrb", prefix, (unsigned long long)getpid());

	int err = mrb_create(&rb, name,
			str2int(getenv("GLGRAB_BUFSIZE"), DEFAULT_MRB_SIZE),
			str2int(getenv("GLGRAB_MAXFRAME"), DEFAULT_MAX_FRAME_SIZE));
	if (err) {
		fprintf(stderr, "glgrab: failed to create ring buffer \"%s\": %s\n",
			name, strerror(err));
		return;
	}

	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	start_time = ts.tv_sec;

	hook_glXSwapBuffers(NULL, NULL, 0);
	started = true;
}

void __attribute__((destructor)) destroy(void) {
	mrb_shutdown(&rb);
}
