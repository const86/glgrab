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

/* MRB is a inter-process shared wait-free single-writer multiple-readers
 * ring buffer. Messages are variable length non-structured data. New messages
 * posted to the tail of a buffer may silently overwrite the oldest messages
 * at the head of that buffer. Thus writer shan't be blocked (or any other way
 * influenced) by a slow reader.
 *
 * A reader doesn't actually removes messages from the buffer, but monitors its
 * state and grab new messages if it is lucky enough to see it in time.
 * The library provides method for checking consistency of gathered data.
 * But delivery reliability is sacrificed for the write speed.
 */

#ifndef _GLGRAB_MRB_H
#define _GLGRAB_MRB_H

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

/* Writer part of API. Typical usage is as simple as:
 *
 * struct mrb b;
 * mrb_create(&b, "/path/to.mrb", 1 << 20, 16 << 10);
 * while (running) {
 *     void *p = mrb_reserve(&b, size);
 *     memcpy(p, &message, size);
 *     mrb_commit(&b);
 * }
 * mrb_shutdown(&b);
 */

/**
 * Create new file and initialize MRB structures inside.
 * For small buffers it's recommended to specify max_item_size==size.
 * You may need to limit max_item_size e.g. for large buffer on 32-bit
 * systems, because MRB is mmap()ed and total map size is approximately
 * size+max_item_size.
 *
 * @param path Path to non-existent file.
 * @param size Total size of the buffer in bytes.
 * @param max_item_size Hint for maximum message size. You may be rejected
 * to insert a message of size close enough to max_item_size or greater.
 *
 * @return 0 on success or error code as in errno.h.
 */
int mrb_create(struct mrb *, const char *path, uint64_t size, uint64_t max_item_size);

/**
 * Mark MRB as terminated and free all resources. You need to unlink() the file
 * used as MRB yourself. Actually you may unlink() any time after mrb_create()
 * returns, if you want no more new readers to appear.
 *
 * @return worst error code from underlying syscalls just FYI. You can neither
 * continue to use MRB nor try to recovery in case of error :(
 */
int mrb_shutdown(struct mrb *);

/**
 * Reserve space for new message at the tail of ring-buffer. Any old messages
 * occupying or overlapping reserved space are reclaimed and head of the buffer
 * is advanced. If you repeatedly call mrb_reserve() then previous reservations
 * are discarded but reclaimed messages are not restored.
 *
 * @param size size of the message in bytes
 *
 * @return pointer to buffer for the message or NULL if allocation is failed.
 * Writing outside of [p, p+size) will corrupt internal buffer structures!
 * Function fails if message with metadata wouldn't fit in MRB and may fail
 * if it wouldn't fit into max_item_size configured by mrb_create().
 */
void *mrb_reserve(struct mrb *, uint64_t size);

/**
 * Commit message to the queue and make it visible to readers.
 * Subsequent call to mrb_commit() without prior mrb_reserve() is no-op.
 * You may not access to committed message.
 */
void mrb_commit(struct mrb *);

/* Monitor API. Monitoring is somewhat more complicated than writing:
 *
 * struct header {
 *     // main message metadata
 *     char data[];
 * }
 *
 * struct mrb b;
 * mrb_open(&b, "/path/to.mrb");
 *
 * for (;;) {
 *     const void *p;
 *     while (!mrb_reveal(&b, &p)) {
 *         sleep(1);
 *     }
 *     if (!p) break;
 *
 *     struct header *orig = p, copy = *orig;
 *     if (mrb_check(&b)) {
 *         // grab message data from orig->data using copy of metadata
 *        if (mrb_check(&b)) {
 *            // grabbed data is good, use it
 *        }
 *     }
 *
 *     mrb_release(&b);
 * }
 *
 * mrb_close(&b);
 */

/**
 * Open MRB for monitoring.
 *
 * @return 0 on success or error code as in errno.h.
 */
int mrb_open(struct mrb *, const char *path);

/**
 * Close MRB.
 *
 * @return error code. See note about error handling in mrb_shutdown().
 */
int mrb_close(struct mrb *);

/**
 * Find next message in the buffer. You may not repeatedly call mrb_reveal()
 * without calling mrb_release().
 *
 * @param pdata Where to place direct pointer
 *
 * @return false if nothing new was found, otherwise the pointer is
 * stored in *pdata. If that pointer is NULL then buffer is terminated
 * and new messages will never appear. Otherwise you get pointer
 * to next message.
 */
bool mrb_reveal(struct mrb *, const void **pdata);

/**
 * Check if current message revealed by last mrb_reveal() is still valid.
 * You may not assume that data read from message buffer is consistent
 * unless you verify it after reading. Depending on message structure
 * you may need to make several check points.
 */
bool mrb_check(struct mrb *);

/**
 * Forget last revealed message. Pointer to its data becomes invalid.
 * You should read message data from the buffer and call mrb_release()
 * as soon as possible to minimized chance of losing next messages.
 */
void mrb_release(struct mrb *);

#endif
