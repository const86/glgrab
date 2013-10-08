/* rtld-audit helper for GLX
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

#include <GL/glx.h>
#include <X11/X.h>

typedef void (*glXSwapBuffers_t)(Display *, GLXDrawable);
static glXSwapBuffers_t real_glXSwapBuffers;
static void (*glgrab_glXSwapBuffers)(glXSwapBuffers_t, Display *, GLXDrawable);

typedef void (*glXDestroyContext_t)(Display *, GLXContext);
static glXDestroyContext_t real_glXDestroyContext;
static void (*glgrab_glXDestroyContext)(glXDestroyContext_t, Display *, GLXContext);

static PFNGLXCREATEWINDOWPROC real_glXCreateWindow;
static GLXWindow (*glgrab_glXCreateWindow)(PFNGLXCREATEWINDOWPROC, Display *, GLXFBConfig, Window, const int *);

static PFNGLXDESTROYWINDOWPROC real_glXDestroyWindow;
static void (*glgrab_glXDestroyWindow)(PFNGLXDESTROYWINDOWPROC, Display *, GLXWindow);

typedef int (*XDestroyWindow_t)(Display *, Window);
static XDestroyWindow_t real_XDestroyWindow;
static int (*glgrab_XDestroyWindow)(XDestroyWindow_t, Display *, Window);

static PFNGLXGETPROCADDRESSPROC real_glXGetProcAddress;

static PFNGLXGETPROCADDRESSPROC real_glXGetProcAddressARB;

const struct hook hooks[] = {
	{"glgrab_glXSwapBuffers", (func_pp)&glgrab_glXSwapBuffers},
	{"glgrab_glXDestroyContext", (func_pp)&glgrab_glXDestroyContext},
	{"glgrab_glXCreateWindow", (func_pp)&glgrab_glXCreateWindow},
	{"glgrab_glXDestroyWindow", (func_pp)&glgrab_glXDestroyWindow},
	{"glgrab_XDestroyWindow", (func_pp)&glgrab_XDestroyWindow},
	{0}
};

static void fake_glXSwapBuffers(Display *dpy, GLXDrawable drawable) {
	if (glgrab_glXSwapBuffers) {
		glgrab_glXSwapBuffers(real_glXSwapBuffers, dpy, drawable);
	} else {
		real_glXSwapBuffers(dpy, drawable);
	}
}

static void fake_glXDestroyContext(Display *dpy, GLXContext ctx) {
	if (glgrab_glXDestroyContext) {
		glgrab_glXDestroyContext(real_glXDestroyContext, dpy, ctx);
	} else {
		real_glXDestroyContext(dpy, ctx);
	}
}

static GLXWindow fake_glXCreateWindow(Display *dpy, GLXFBConfig config, Window win, const int *attribList) {
	if (glgrab_glXCreateWindow) {
		return glgrab_glXCreateWindow(real_glXCreateWindow, dpy, config, win, attribList);
	} else {
		return real_glXCreateWindow(dpy, config, win, attribList);
	}
}

static void fake_glXDestroyWindow(Display *dpy, GLXWindow window) {
	if (glgrab_glXDestroyWindow) {
		glgrab_glXDestroyWindow(real_glXDestroyWindow, dpy, window);
	} else {
		real_glXDestroyWindow(dpy, window);
	}
}

static int fake_XDestroyWindow(Display *dpy, Window window) {
	if (glgrab_XDestroyWindow) {
		return glgrab_XDestroyWindow(real_XDestroyWindow, dpy, window);
	} else {
		return real_XDestroyWindow(dpy, window);
	}
}

static __GLXextFuncPtr fake_glXGetProcAddress(const GLubyte *);
static __GLXextFuncPtr fake_glXGetProcAddressARB(const GLubyte *);

const struct sub subs[] = {
	{"glXGetProcAddress", (func_pp)&real_glXGetProcAddress, (func_p)fake_glXGetProcAddress},
	{"glXGetProcAddressARB", (func_pp)&real_glXGetProcAddressARB, (func_p)fake_glXGetProcAddressARB},
	{"glXSwapBuffers", (func_pp)&real_glXSwapBuffers, (func_p)fake_glXSwapBuffers},
	{"glXDestroyContext", (func_pp)&real_glXDestroyContext, (func_p)fake_glXDestroyContext},
	{"glXCreateWindow", (func_pp)&real_glXCreateWindow, (func_p)fake_glXCreateWindow},
	{"glXDestroyWindow", (func_pp)&real_glXDestroyWindow, (func_p)fake_glXDestroyWindow},
	{"XDestroyWindow", (func_pp)&real_XDestroyWindow, (func_p)fake_XDestroyWindow},
	{0}
};

static __GLXextFuncPtr fake_glXGetProcAddress(const GLubyte *procname) {
	return apply_sub((const char *)procname, (func_p)real_glXGetProcAddress(procname));
}

static __GLXextFuncPtr fake_glXGetProcAddressARB(const GLubyte *procname) {
	return apply_sub((const char *)procname, (func_p)real_glXGetProcAddressARB(procname));
}
