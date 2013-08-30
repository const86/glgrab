/* MRB - Monitored Ring Buffer
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

#include <stdbool.h>
#include <stdint.h>

typedef uint64_t mrb_ptr;

struct mrb_item {
	uint64_t seq;
	uint64_t off;
};

struct mrb {
	volatile struct mrb_hdr *header;
	char *base;

	uint64_t size;
	uint64_t max_item_size;

	uint32_t data_offset;
	int16_t align_bits;
	int16_t off_bits;

	struct mrb_item next;
};

int mrb_create(struct mrb *, const char *path, uint64_t size, uint64_t max_item_size);
int mrb_shutdown(struct mrb *);

void *mrb_reserve(struct mrb *, uint64_t size);
void mrb_commit(struct mrb *);

int mrb_open(struct mrb *, const char *path);
int mrb_close(struct mrb *);

bool mrb_reveal(struct mrb *, const void **pdata);
bool mrb_check(struct mrb *);
void mrb_release(struct mrb *);
