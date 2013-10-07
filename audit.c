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

#include "audit.h"

#include <link.h>
#include <string.h>

func_p apply_sub(const char *name, func_p real) {
	for (const struct sub *p = subs; p->name != NULL; p++) {
		if (strcmp(name, p->name) == 0) {
			*p->real = real;
			return p->fake;
		}
	}

	return real;
}

#pragma GCC visibility push(default)

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
	for (const struct hook *p = hooks; p->name != NULL; p++) {
		if (strcmp(symname, p->name) == 0) {
			*p->hook = (func_p)sym->st_value;
			return sym->st_value;
		}
	}

	return (uintptr_t)apply_sub(symname, (func_p)sym->st_value);
}

