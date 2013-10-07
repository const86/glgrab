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

#include "glgrab.h"

#include <X11/Xlib.h>
#include <GL/glx.h>
#include <dlfcn.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/queue.h>

#pragma GCC visibility push(default)

struct glxgrab {
	struct glgrab gl;
	LIST_HEAD(, winmap) winmap;
	GLXContext ctx;
	volatile bool lock;
};

struct winmap {
	LIST_ENTRY(winmap) list;
	Window x11;
	GLXWindow glx;
};

static struct glxgrab glx = {
	.winmap = LIST_HEAD_INITIALIZER(glx.winmap)
};

static void lock(struct glxgrab *g) {
	while (__atomic_exchange_n(&g->lock, true, __ATOMIC_ACQ_REL))
		sched_yield();
}

static void unlock(struct glxgrab *g) {
	__atomic_store_n(&g->lock, false, __ATOMIC_RELEASE);
}

static void register_window(struct glxgrab *g, Window win, GLXWindow glxwin) {
	struct winmap *m = malloc(sizeof(*m));
	if (m == NULL)
		return;

	m->x11 = win;
	m->glx = glxwin;

	lock(g);
	LIST_INSERT_HEAD(&g->winmap, m, list);
	unlock(g);
}

static struct winmap *find_x11(struct glxgrab *g, GLXWindow glxwin) {
	struct winmap *m;

	LIST_FOREACH(m, &g->winmap, list) {
		if (m->glx == glxwin)
			break;
	}

	return m;
}

static void forget_window(struct glxgrab *g, GLXWindow glxwin) {
	lock(g);
	struct winmap *m = find_x11(g, glxwin);
	if (m != NULL)
		LIST_REMOVE(m, list);

	unlock(g);

	if (m != NULL)
		free(m);
}

GLXWindow glgrab_glXCreateWindow(PFNGLXCREATEWINDOWPROC real,
	Display *dpy, GLXFBConfig config, Window win, const int *attribList) {
	GLXWindow glxwin = real(dpy, config, win, attribList);

	if (glxwin != None)
		register_window(&glx, win, glxwin);

	return glxwin;
}

void glgrab_glXDestroyWindow(PFNGLXDESTROYWINDOWPROC real, Display *dpy, GLXWindow window) {
	real(dpy, window);
	forget_window(&glx, window);
}

GLXPixmap glgrab_glXCreatePixmap(PFNGLXCREATEPIXMAPPROC real,
	Display *dpy, GLXFBConfig config, Pixmap pixmap, const int *attribList) {
	GLXPixmap glxpixmap = real(dpy, config, pixmap, attribList);

	if (glxpixmap != None)
		register_window(&glx, None, glxpixmap);

	return glxpixmap;
}

void glgrab_glXDestroyPixmap(PFNGLXDESTROYWINDOWPROC real, Display *dpy, GLXPixmap pixmap) {
	real(dpy, pixmap);
	forget_window(&glx, pixmap);
}

GLXPbuffer glgrab_glXCreatePbuffer(PFNGLXCREATEPBUFFERPROC real,
	Display *dpy, GLXFBConfig config, const int *attribList) {
	GLXPbuffer pbuf = real(dpy, config, attribList);

	if (pbuf != None)
		register_window(&glx, None, pbuf);

	return pbuf;
}

void glgrab_glXDestroyPbuffer(PFNGLXDESTROYPBUFFERPROC real, Display *dpy, GLXPbuffer pbuf) {
	real(dpy, pbuf);
	forget_window(&glx, pbuf);
}

void glgrab_glXDestroyContext(void (*real)(Display *, GLXContext), Display *dpy, GLXContext ctx) {
	lock(&glx);
	if (glx.ctx == ctx)
		glx.ctx = NULL;

	unlock(&glx);
	real(dpy, ctx);
}

void glgrab_glXSwapBuffers(void (*real)(Display *, GLXDrawable), Display *dpy, GLXDrawable drawable) {
	GLXContext ctx = glXGetCurrentContext();
	int err = 0;

	if (ctx != NULL && (err = glgrab_init_from_env(&glx.gl)) == 0) {
		lock(&glx);

		struct winmap *m = find_x11(&glx, drawable);
		Window win = m == NULL ? drawable : m->x11;

		bool match = true;
		if (win != None && glx.ctx == NULL && glgrab_reset(&glx.gl))
			glx.ctx = ctx;
		else if (glx.ctx != ctx)
			match = false;

		unlock(&glx);

		if (match) {
			unsigned width, height, u;
			int i;
			Window r;
			XGetGeometry(dpy, win, &r, &i, &i, &width, &height, &u, &u);

			if (!glgrab_take_frame(&glx.gl, width, height)) {
				fputs("glgrab: failed to capture frame\n", stderr);
			}
		}
	} else if (err != 0) {
		fprintf(stderr, "glgrab: failed to create ring buffer \"%s\" size (%s): %s\n",
			getenv("GLGRAB_MRB"), getenv("GLGRAB_BUFSIZE"), strerror(err));
	}

	real(dpy, drawable);
}

static void bind_hook(void *h, const char *name) {
	if (!dlsym(h, name))
		fprintf(stderr, "glgrab: failed to bind %s hook: %s\n", name, dlerror());
}

static void __attribute__((constructor)) init(void) {
	void *h = dlopen(NULL, RTLD_LAZY);

	if (h != NULL) {
		bind_hook(h, "glgrab_glXSwapBuffers");
		bind_hook(h, "glgrab_glXDestroyContext");
		bind_hook(h, "glgrab_glXCreateWindow");
		bind_hook(h, "glgrab_glXDestroyWindow");
		bind_hook(h, "glgrab_glXCreatePixmap");
		bind_hook(h, "glgrab_glXDestroyPixmap");
		bind_hook(h, "glgrab_glXCreatePbuffer");
		bind_hook(h, "glgrab_glXDestroyPbuffer");

		dlclose(h);
	} else {
		fprintf(stderr, "glgrab: failed to dlopen() myself: %s\n", dlerror());
	}
}

static void __attribute__((destructor)) destroy(void) {
	glgrab_destroy(&glx.gl);
}
