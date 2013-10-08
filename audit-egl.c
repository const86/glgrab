/* rtld-audit helper for EGL
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

#define _GNU_SOURCE

#include "audit.h"

#include <EGL/egl.h>

typedef EGLBoolean (*eglSwapBuffers_t)(EGLDisplay, EGLSurface);
static eglSwapBuffers_t real_eglSwapBuffers;
static EGLBoolean (*glgrab_eglSwapBuffers)(eglSwapBuffers_t, EGLDisplay, EGLSurface);

typedef EGLBoolean (*eglDestroyContext_t)(EGLDisplay, EGLContext);
static eglDestroyContext_t real_eglDestroyContext;
static EGLBoolean (*glgrab_eglDestroyContext)(eglDestroyContext_t, EGLDisplay, EGLContext);

static void (*(*real_eglGetProcAddress)(const char *))(void);

const struct hook hooks[] = {
	{"glgrab_eglSwapBuffers", (func_pp)&glgrab_eglSwapBuffers},
	{"glgrab_eglDestroyContext", (func_pp)&glgrab_eglDestroyContext},
	{0}
};

static EGLBoolean fake_eglSwapBuffers(EGLDisplay dpy, EGLSurface surface) {
	if (glgrab_eglSwapBuffers) {
		return glgrab_eglSwapBuffers(real_eglSwapBuffers, dpy, surface);
	} else {
		return real_eglSwapBuffers(dpy, surface);
	}
}

static EGLBoolean fake_eglDestroyContext(EGLDisplay dpy, EGLSurface surface) {
	if (glgrab_eglDestroyContext) {
		return glgrab_eglDestroyContext(real_eglDestroyContext, dpy, surface);
	} else {
		return real_eglDestroyContext(dpy, surface);
	}
}

static void (*fake_eglGetProcAddress(const char *))(void);

const struct sub subs[] = {
	{"eglGetProcAddress", (func_pp)&real_eglGetProcAddress, (func_p)fake_eglGetProcAddress},
	{"eglSwapBuffers", (func_pp)&real_eglSwapBuffers, (func_p)fake_eglSwapBuffers},
	{"eglDestroyContext", (func_pp)&real_eglDestroyContext, (func_p)fake_eglDestroyContext},
	{0}
};

static void (*fake_eglGetProcAddress(const char *procname))(void) {
	return apply_sub(procname, real_eglGetProcAddress(procname));
}
