/* GLGrab data format
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

#ifndef _GLGRAB_H
#define _GLGRAB_H

#include <GL/gl.h>
#include <stdbool.h>
#include <stdint.h>
#include "mrb.h"

/**
 * Structure of grabbed frame as passed through MRB.
 * Pixel data is in BGRA 8:8:8:8 format packed in lines
 * aligned by 8 byte. I.e. in case of odd width image contains
 * stray pixel at end of each line.
 * Timestamps are read after glXSwapBuffers() returns and merely
 * correspond to time of displaying.
 */
struct glgrab_frame {
	uint64_t ns; ///< Timestamp in nanoseconds starting from unknown point
	uint32_t height; ///< Height of image
	uint32_t width; ///< Width of image
	unsigned char data[] __attribute__((aligned(8)));
};

extern struct AVInputFormat glgrab_avformat;

struct glgrab {
	struct mrb rb;
	uint64_t start_time;

	struct glgrab_frame *frame;
	GLuint fbo;
	GLuint rbo;
	GLuint pbo;

	int state;
	GLenum last_error;
};

int glgrab_init(struct glgrab *, const char *path, uint64_t bufsize, uint64_t max_frame_size);
int glgrab_init_from_env(struct glgrab *);
bool glgrab_destroy(struct glgrab *);

bool glgrab_reset(struct glgrab *);
bool glgrab_take_frame(struct glgrab *, uint32_t width, uint32_t height);

#endif
