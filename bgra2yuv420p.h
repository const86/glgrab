/* Fast BGRA -> YUV420p conversion
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

#ifndef _BGRA2YUV420P_H
#define _BGRA2YUV420P_H

#include <stddef.h>
#include <stdint.h>

void bgra2yuv420p(const uint8_t *restrict bgra, int bgra_stride,
	uint8_t *restrict y, int y_stride, uint8_t *restrict u, int u_stride,
	uint8_t *restrict v, int v_stride, size_t width, size_t height);

#endif
