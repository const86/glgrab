/* audit.h - rtld-audit helper
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

#pragma once

typedef void (*func_p)(void);
typedef func_p *func_pp;

struct sub {
	const char *name;
	func_pp real;
	func_p fake;
};

struct hook {
	const char *name;
	func_pp hook;
};

extern const struct hook hooks[];
extern const struct sub subs[];

func_p apply_sub(const char *name, func_p real);
