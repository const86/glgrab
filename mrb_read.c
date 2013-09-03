/* MRB reader implementation
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
#include <sys/stat.h>
#include <unistd.h>

int mrb_open(struct mrb *q, const char *path) {
	const long pagesize = sysconf(_SC_PAGESIZE);

	const int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOCTTY);
	if (fd == -1)
		goto err;

	struct stat st;
	if (fstat(fd, &st) != 0)
		goto err_close;

	struct mrb_hdr header;
	if (pread(fd, &header, sizeof(header), 0) != sizeof(header))
		goto err_close;

	if (!header.active) {
		errno = EAGAIN;
		goto err_close;
	}

	char *const addr = mmap(NULL, st.st_size + header.max_item_size, PROT_READ,
				MAP_SHARED | MAP_POPULATE, fd, 0);
	if (addr == MAP_FAILED)
		goto err_close;

	if (header.max_item_size &&
		mmap(addr + st.st_size, header.max_item_size, PROT_READ,
			MAP_SHARED | MAP_POPULATE | MAP_FIXED, fd, pagesize) == MAP_FAILED)
		goto err_munmap;

	if (close(fd) != 0)
		goto err_munmap;

	memset(q, 0, sizeof(*q));

	q->header = (struct mrb_hdr *)addr;
	q->base = addr + pagesize;

	q->size = st.st_size - pagesize;
	q->max_item_size = header.max_item_size;

	q->align_bits = q->header->align_bits;
	q->off_bits = q->header->off_bits;
	q->data_offset = roundup(sizeof(mrb_ptr), q->align_bits);

	return 0;

err_munmap:
	munmap(addr, st.st_size + header.max_item_size);
err_close:
	close(fd);
err:
	return errno;
}

int mrb_close(struct mrb *q) {
	errno = 0;

	if (q->base) {
		const long pagesize = sysconf(_SC_PAGESIZE);
		munmap((char *)q->base - pagesize, pagesize + q->size + q->max_item_size);
		memset(q, 0, sizeof(*q));
	}

	return errno;
}

bool mrb_check(struct mrb *q) {
	if (q->next.seq == 0)
		return false;

	struct mrb_item head = mrb_item_unpack(q, __atomic_load_n(&q->header->head, __ATOMIC_CONSUME));

	if (head.seq == 0)
		return false;

	if (q->next.seq >= head.seq)
		return true;

	struct mrb_item tail = mrb_item_unpack(q, __atomic_load_n(&q->header->tail, __ATOMIC_CONSUME));
	return q->next.seq < tail.seq && tail.seq < head.seq;
}

bool mrb_reveal(struct mrb *q, const void **p) {
	if (!mrb_check(q)) {
		q->next = mrb_item_unpack(q, __atomic_load_n(&q->header->head, __ATOMIC_ACQUIRE));
	}

	if (q->next.seq == 0 ||
		q->next.seq == mrb_item_unpack(q, __atomic_load_n(&q->header->tail, __ATOMIC_ACQUIRE)).seq) {
		*p = NULL;
		return !__atomic_load_n(&q->header->active, __ATOMIC_CONSUME);
	}

	*p = q->base + q->next.off + q->data_offset;
	return true;
}

void mrb_release(struct mrb *q) {
	struct mrb_item next = mrb_item_unpack(q, __atomic_load_n((mrb_ptr *)(q->base + q->next.off), __ATOMIC_CONSUME));

	if (mrb_check(q)) {
		q->next = next;
	} else {
		q->next = mrb_item_unpack(q, __atomic_load_n(&q->header->head, __ATOMIC_CONSUME));
	}
}
