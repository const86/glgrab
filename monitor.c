/* monitor - simple application monitoring GLGrab output
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

#include "glgrab.h"
#include "mrb.h"
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

int main(int argc, char **argv) {
	if (argc != 2) {
		fprintf(stderr, "Usage: %s MRB-FILE\n", argv[0]);
		return 1;
	}

	struct mrb rb;
	int err = mrb_open(&rb, argv[1]);
	if (err) {
		fprintf(stderr, "%s: failed to open \"%s\": %s\n", argv[0], argv[1], strerror(err));
		return 2;
	}

	for (;;) {
		const void *p;
		while (!mrb_reveal(&rb, &p)) {
			const struct timespec ts = {0, 10000000};
			clock_nanosleep(CLOCK_MONOTONIC, 0, &ts, NULL);
		}

		if (!p)
			break;

		struct glgrab_frame frame;
		memcpy(&frame, p, sizeof(frame));
		if (mrb_check(&rb)) {
			printf("%.03f %" PRIu32 "x%" PRIu32 "\n",
				frame.ns * 1e-9, frame.width, frame.height);
		}

		mrb_release(&rb);
	}

	mrb_close(&rb);
}
