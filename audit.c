/* audit.c - rtld-audit helper
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

#include <GL/glx.h>
#include <link.h>
#include <string.h>

#pragma GCC visibility push(default)

typedef void (*glXSwapBuffers_t)(Display *, GLXDrawable);
static glXSwapBuffers_t real_glXSwapBuffers;

typedef void (*glXSwapBuffers_hook_t)(glXSwapBuffers_t, Display *, GLXDrawable);
static glXSwapBuffers_hook_t glgrab_glXSwapBuffers;

static void fake_glXSwapBuffers(Display *dpy, GLXDrawable drawable) {
	if (glgrab_glXSwapBuffers) {
		glgrab_glXSwapBuffers(real_glXSwapBuffers, dpy, drawable);
	} else {
		real_glXSwapBuffers(dpy, drawable);
	}
}

static PFNGLXGETPROCADDRESSPROC real_glXGetProcAddressARB;

static __GLXextFuncPtr fake_glXGetProcAddressARB(const GLubyte *procname) {
	__GLXextFuncPtr addr = real_glXGetProcAddressARB(procname);

	if (strcmp((const char *)procname, "glXSwapBuffers") == 0) {
		real_glXSwapBuffers = (glXSwapBuffers_t)addr;
		addr = (__GLXextFuncPtr)&fake_glXSwapBuffers;
	}

	return addr;
}

unsigned la_version(unsigned version) {
	return 1;
}

unsigned la_objopen(struct link_map *map, Lmid_t lmid, uintptr_t *cookie) {
	ElfW(Dyn) *last, *pltrelsz = NULL;
	for (last = map->l_ld; last->d_tag != DT_NULL; ++last) {
		if (last->d_tag == DT_PLTRELSZ)
			pltrelsz = last;
	}

	if (pltrelsz == NULL) {
		pltrelsz = last++;
		last->d_tag = DT_NULL;
		pltrelsz->d_tag = DT_PLTRELSZ;
		pltrelsz->d_un.d_val = 0;

		ElfW(Dyn) **p = &map->l_ld + 1;
		while (*p < map->l_ld || *p > last)
			++p;

		p[DT_PLTRELSZ - map->l_ld->d_tag] = pltrelsz;
	}

	return LA_FLG_BINDTO | LA_FLG_BINDFROM;
}

#ifdef __LP64__
#define la_symbind la_symbind64
#else
#define la_symbind la_symbind32
#endif

uintptr_t la_symbind(ElfW(Sym) *sym, unsigned int ndx, uintptr_t *refcook,
		uintptr_t *defcook, unsigned int *flags, const char *symname) {
	uintptr_t addr = sym->st_value;

	if (strcmp(symname, "glXSwapBuffers") == 0) {
		real_glXSwapBuffers = (glXSwapBuffers_t)addr;
		addr = (uintptr_t)&fake_glXSwapBuffers;
	} else if (strcmp(symname, "glXGetProcAddressARB") == 0) {
		real_glXGetProcAddressARB = (PFNGLXGETPROCADDRESSPROC)addr;
		addr = (uintptr_t)&fake_glXGetProcAddressARB;
	} else if (strcmp(symname, "glgrab_glXSwapBuffers") == 0) {
		glgrab_glXSwapBuffers = (glXSwapBuffers_hook_t)addr;
	}

	return addr;
}

