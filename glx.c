/* LD_PRELOAD'ed hooks for GLX
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
	bool lock;
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
	while (__atomic_exchange_n(&g->lock, true, __ATOMIC_ACQUIRE))
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

int glgrab_XDestroyWindow(int (*real)(Display *, Window), Display *dpy, Window window) {
	int res = real(dpy, window);
	forget_window(&glx, window);
	return res;
}

void glgrab_glXDestroyContext(void (*real)(Display *, GLXContext), Display *dpy, GLXContext ctx) {
	GLXContext curr = ctx;
	__atomic_compare_exchange_n(&glx.ctx, &curr, NULL, false, __ATOMIC_RELEASE, __ATOMIC_RELAXED);
	real(dpy, ctx);
}

static void get_window_size(Display *dpy, Window window, unsigned *width, unsigned *height) {
	unsigned u;
	int i;
	Window r;
	XGetGeometry(dpy, window, &r, &i, &i, width, height, &u, &u);
}

static bool get_window_size_error;

static int get_window_size_error_handler(Display *dpy, XErrorEvent *ev) {
	get_window_size_error = true;
	return 0;
}

static bool get_window_size_safe(Display *dpy, Window window, unsigned *width, unsigned *height) {
	XSync(dpy, False);
	int (*orig)(Display *, XErrorEvent *) = XSetErrorHandler(get_window_size_error_handler);

	get_window_size_error = false;
	get_window_size(dpy, window, width, height);

	XSetErrorHandler(orig);
	return !get_window_size_error;
}

static void take_frame(struct glxgrab *g, Display *dpy, GLXContext ctx, GLXDrawable drawable) {
	unsigned width = 0, height = 0;

	lock(g);
	struct winmap *m = find_x11(g, drawable);

	if (m == NULL) {
		unlock(g);
		Window window = get_window_size_safe(dpy, drawable, &width, &height) ? drawable : None;
		register_window(g, window, drawable);

		if (window == None)
			return;
	} else {
		Window window = m->x11;
		unlock(g);

		if (window == None)
			return;

		get_window_size(dpy, window, &width, &height);
	}

	GLXContext curr = NULL;
	if (__atomic_compare_exchange_n(&g->ctx, &curr, ctx, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
		if (glgrab_init_from_env(&g->gl) != 0 || !glgrab_reset(&g->gl))
			return;
	} else {
		if (curr != ctx)
			return;
	}

	glgrab_take_frame(&g->gl, width, height);
}

void glgrab_glXSwapBuffers(void (*real)(Display *, GLXDrawable), Display *dpy, GLXDrawable drawable) {
	GLXContext ctx = glXGetCurrentContext();

	if (ctx != NULL && glXGetCurrentDrawable() == drawable && glXGetCurrentReadDrawable() == drawable)
		take_frame(&glx, dpy, ctx, drawable);

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
		bind_hook(h, "glgrab_XDestroyWindow");

		dlclose(h);
	} else {
		fprintf(stderr, "glgrab: failed to dlopen() myself: %s\n", dlerror());
	}
}

static void __attribute__((destructor)) destroy(void) {
	glgrab_destroy(&glx.gl);
}
