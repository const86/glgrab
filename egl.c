/* LD_PRELOAD'ed hooks for EGL
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

#include <EGL/egl.h>
#include <dlfcn.h>
#include <stdio.h>

#pragma GCC visibility push(default)

struct eglgrab {
	struct glgrab gl;
	EGLContext ctx;
};

static struct eglgrab egl = {
	.ctx = EGL_NO_CONTEXT
};

EGLBoolean glgrab_eglDestroyContext(EGLBoolean (*real)(EGLDisplay, EGLContext), EGLDisplay dpy, EGLContext ctx) {
	EGLBoolean res = real(dpy, ctx);

	if (res == EGL_TRUE) {
		__atomic_compare_exchange_n(&egl.ctx, &ctx, EGL_NO_CONTEXT,
			false, __ATOMIC_RELEASE, __ATOMIC_RELAXED);
	}

	return res;
}

static void take_frame(struct eglgrab *g, EGLDisplay dpy, EGLContext ctx, EGLSurface surface) {
	EGLContext curr = EGL_NO_CONTEXT;
	if (__atomic_compare_exchange_n(&g->ctx, &curr, ctx, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
		if (glgrab_init_from_env(&g->gl) != 0 || !glgrab_reset(&g->gl))
			return;
	} else {
		if (curr != ctx)
			return;
	}

	EGLint width = 0, height = 0;

	if (eglQuerySurface(dpy, surface, EGL_WIDTH, &width) == EGL_FALSE ||
		eglQuerySurface(dpy, surface, EGL_HEIGHT, &height) == EGL_FALSE)
		return;

	glgrab_take_frame(&g->gl, GL_BACK, width, height);
}

EGLBoolean glgrab_eglSwapBuffers(EGLBoolean (*real)(EGLDisplay, EGLSurface), EGLDisplay *dpy, EGLSurface surface) {
	if (eglQueryAPI() == EGL_OPENGL_API) {
		EGLContext ctx = eglGetCurrentContext();

		if (ctx != EGL_NO_CONTEXT && eglGetCurrentSurface(EGL_READ) == surface &&
			eglGetCurrentSurface(EGL_DRAW) == surface)
			take_frame(&egl, dpy, ctx, surface);
	}

	return real(dpy, surface);
}

static void bind_hook(void *h, const char *name) {
	if (!dlsym(h, name))
		fprintf(stderr, "glgrab: failed to bind %s hook: %s\n", name, dlerror());
}

static void __attribute__((constructor)) init(void) {
	void *h = dlopen(NULL, RTLD_LAZY);

	if (h != NULL) {
		bind_hook(h, "glgrab_eglSwapBuffers");
		bind_hook(h, "glgrab_eglDestroyContext");

		dlclose(h);
	} else {
		fprintf(stderr, "glgrab: failed to dlopen() myself: %s\n", dlerror());
	}
}

static void __attribute__((destructor)) destroy(void) {
	glgrab_destroy(&egl.gl);
}
