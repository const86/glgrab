/* MRB writer implementation
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
#define _XOPEN_SOURCE 700
#define _FILE_OFFSET_BITS 64

#include "mrb_int.h"
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>

#ifdef __BIGGEST_ALIGNMENT__
#define ALIGN __BIGGEST_ALIGNMENT__
#else
#define ALIGN 32
#endif

static uint16_t ilog(uint64_t a) {
	uint16_t l = 0;

	while (a >>= 1) {
		++l;
	}

	return l;
}

static mrb_ptr mrb_item_pack(const struct mrb *q, struct mrb_item i) {
	return (i.seq << q->off_bits) | (i.off >> q->align_bits);
}

int mrb_create(struct mrb *q, const char *path, uint64_t size, uint64_t max_item_size) {
	const long pagesize = sysconf(_SC_PAGESIZE);
	size = roundup(size, ilog(pagesize));
	max_item_size = roundup(max_item_size, ilog(pagesize));

	const int fd = open(path, O_CREAT | O_EXCL | O_RDWR, S_IRUSR | S_IRGRP | S_IROTH);
	if (fd == -1)
		goto err;

	const off_t filesize = size + pagesize;

	if (ftruncate(fd, filesize) != 0)
		goto err_close;

	char *const addr = mmap(NULL, filesize + max_item_size, PROT_READ | PROT_WRITE,
				MAP_SHARED | MAP_POPULATE, fd, 0);
	if (addr == MAP_FAILED)
		goto err_close;

	if (max_item_size) {
#ifdef __linux__
		if (remap_file_pages(addr + filesize, max_item_size, 0, 1, 0) != 0)
			goto err_munmap;
#else
		if (mmap(addr + filesize, max_item_size, PROT_READ | PROT_WRITE,
				MAP_SHARED | MAP_POPULATE | MAP_FIXED, fd, pagesize) == MAP_FAILED)
			goto err_munmap;
#endif
	}

	if (close(fd) != 0)
		goto err_munmap;

	memset(q, 0, sizeof(*q));

	q->header = (struct mrb_hdr *)addr;
	q->base = addr + pagesize;

	q->size = size;
	q->header->max_item_size = q->max_item_size = max_item_size;

	q->header->align_bits = q->align_bits = ilog(ALIGN);
	q->header->off_bits = q->off_bits = ilog(size) - q->align_bits;
	q->data_offset = roundup(sizeof(mrb_ptr), q->align_bits);

	q->header->tail = mrb_item_pack(q, (struct mrb_item){1, 0});

	__atomic_store_n(&q->header->active, true, __ATOMIC_RELEASE);

	return 0;

err_munmap:
	munmap(addr, filesize + max_item_size);
err_close:
	close(fd);
	unlink(path);
err:
	return errno;
}

void *mrb_reserve(struct mrb *q, uint64_t size) {
	const struct mrb_item tail = mrb_item_unpack(q, q->header->tail);
	struct mrb_item next = tail;
	next.off += q->data_offset + roundup(size, q->align_bits);

	if (next.off >= q->size) {
		next.off -= q->size;

		if (next.off >= q->max_item_size || next.off > tail.off)
			return NULL;
	}

	if (++next.seq << q->off_bits == 0) {
		++next.seq;
	}

	mrb_ptr headp = q->header->head;
	while (headp) {
		struct mrb_item head = mrb_item_unpack(q, headp);
		if (head.off == tail.off) {
			headp = 0;
			break;
		}

		if (tail.off < next.off && (head.off < tail.off || next.off <= head.off))
			break;
		if (next.off <= head.off && head.off < tail.off)
			break;

		headp = *(mrb_ptr *)(q->base + head.off);
	}

	__atomic_store_n(&q->header->head, headp, __ATOMIC_RELAXED);
	__atomic_store_n((mrb_ptr *)(q->base + tail.off), mrb_item_pack(q, next), __ATOMIC_RELEASE);

	q->next = next;
	return q->base + tail.off + q->data_offset;
}

void mrb_commit(struct mrb *q) {
	if (q->next.seq) {
		if (q->header->head == 0) {
			mrb_ptr head = q->header->tail;
			__atomic_store_n(&q->header->tail, mrb_item_pack(q, q->next), __ATOMIC_RELEASE);
			__atomic_store_n(&q->header->head, head, __ATOMIC_RELAXED);
		} else {
			__atomic_store_n(&q->header->tail, mrb_item_pack(q, q->next), __ATOMIC_RELEASE);
		}

		q->next.seq = 0;
	}
}


int mrb_shutdown(struct mrb *q) {
	errno = 0;

	if (q->base) {
		const long pagesize = sysconf(_SC_PAGESIZE);
		__atomic_store_n(&q->header->active, false, __ATOMIC_RELEASE);
		munmap((char *)q->base - pagesize, pagesize + q->size + q->max_item_size);
		memset(q, 0, sizeof(*q));
	}

	return errno;
}
