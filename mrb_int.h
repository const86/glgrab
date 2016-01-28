/* MRB - internal definitions
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

#include "mrb.h"
#include <sys/mman.h>

struct mrb_hdr {
	uint32_t active;
	int16_t align_bits;
	int16_t off_bits;
	uint64_t max_item_size;
	mrb_ptr head;
	mrb_ptr tail;
} __attribute__((packed));

#ifndef MAP_POPULATE
#define MAP_POPULATE 0
#endif

static struct mrb_item mrb_item_unpack(const struct mrb *q, mrb_ptr p) {
	const int16_t seq_bits = 64 - q->off_bits;
	return (struct mrb_item){p >> q->off_bits,
			p << seq_bits >> (seq_bits - q->align_bits)};
}

static uint64_t roundup(uint64_t a, uint16_t s) {
	uint16_t m = (1 << s) - 1;
	return (a + m) & ~m;
}
