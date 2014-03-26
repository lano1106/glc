/**
 * \file glc/core/pack.c
 * \brief stream compression adapted from original work (glc) from Pyry Haulos <pyry.haulos@gmail.com>
 * \author Olivier Langlois <olivier@trillion01.com>
 * \date 2014

    Copyright 2014 Olivier Langlois

    This file is part of glcs.

    glcs is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    glcs is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with glcs.  If not, see <http://www.gnu.org/licenses/>.

 */

/**
 * \addtogroup pack
 *  \{
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <stdint.h>
#include <inttypes.h>

#include <glc/common/glc.h>
#include <glc/common/core.h>
#include <glc/common/log.h>
#include <glc/common/thread.h>
#include <glc/common/util.h>

#include "pack.h"
#include "optimization.h"

#ifdef __MINILZO
# include <minilzo.h>
# define __lzo_compress lzo1x_1_compress
# define __lzo_decompress lzo1x_decompress
# define __lzo_worstcase(size) size + (size / 16) + 64 + 3
# define __lzo_wrk_mem LZO1X_1_MEM_COMPRESS
# define __LZO
#elif defined __LZO
# include <lzo/lzo1x.h>
# define __lzo_compress lzo1x_1_11_compress
# define __lzo_decompress lzo1x_decompress
# define __lzo_worstcase(size) size + (size / 16) + 64 + 3
# define __lzo_wrk_mem LZO1X_1_11_MEM_COMPRESS
#endif

#ifdef __QUICKLZ
# include <quicklz.h>
#define __quicklz_worstcase(size) size + 400
#endif

#ifdef __LZJB
# include <lzjb.h>
#endif

struct pack_stat_s {
	uint64_t pack_size;
	uint64_t unpack_size;
};

typedef struct pack_stat_s pack_stat_t;

struct pack_s {
	glc_t *glc;
	glc_thread_t thread;
	size_t compress_min;
	int running;
	int compression;
	pack_stat_t stats;
};

struct unpack_s {
	glc_t *glc;
	glc_thread_t thread;
	int running;
	pack_stat_t stats;
};

static int pack_thread_create_callback(void *ptr, void **threadptr);
static void pack_thread_finish_callback(void *ptr, void *threadptr, int err);
static int pack_read_callback(glc_thread_state_t *state);
static int pack_quicklz_write_callback(glc_thread_state_t *state);
static int pack_lzo_write_callback(glc_thread_state_t *state);
static int pack_lzjb_write_callback(glc_thread_state_t *state);
static void pack_finish_callback(void *ptr, int err);

static void unpack_thread_finish_callback(void *ptr, void *threadptr, int err);
static int unpack_read_callback(glc_thread_state_t *state);
static int unpack_write_callback(glc_thread_state_t *state);
static void unpack_finish_callback(void *ptr, int err);
static void print_stats(glc_t *glc, pack_stat_t *stat);

int pack_init(pack_t *pack, glc_t *glc)
{
#if !defined(__QUICKLZ) && !defined(__LZO) && !defined(__LZJB)
	glc_log(glc, GLC_ERROR, "pack",
		 "no supported compression algorithms found");
	return ENOTSUP;
#else
	*pack = (pack_t) calloc(1, sizeof(struct pack_s));

	(*pack)->glc = glc;
	(*pack)->compress_min = 1024;

	(*pack)->thread.flags = GLC_THREAD_WRITE | GLC_THREAD_READ;
	(*pack)->thread.ptr = *pack;
	(*pack)->thread.thread_create_callback = &pack_thread_create_callback;
	(*pack)->thread.thread_finish_callback = &pack_thread_finish_callback;
	(*pack)->thread.read_callback = &pack_read_callback;
	(*pack)->thread.finish_callback = &pack_finish_callback;
	(*pack)->thread.threads = glc_threads_hint(glc);

	return 0;
#endif
}

int pack_set_compression(pack_t pack, int compression)
{
	if (unlikely(pack->running))
		return EALREADY;

	if (compression == PACK_QUICKLZ) {
#ifdef __QUICKLZ
		pack->thread.write_callback = &pack_quicklz_write_callback;
		glc_log(pack->glc, GLC_INFO, "pack",
			 "compressing using QuickLZ");
#else
		glc_log(pack->glc, GLC_ERROR, "pack",
			 "QuickLZ not supported");
		return ENOTSUP;
#endif
	} else if (compression == PACK_LZO) {
#ifdef __LZO
		pack->thread.write_callback = &pack_lzo_write_callback;
		glc_log(pack->glc, GLC_INFO, "pack",
			 "compressing using LZO");
		lzo_init();
#else
		glc_log(pack->glc, GLC_ERROR, "pack",
			 "LZO not supported");
		return ENOTSUP;
#endif
	} else if (compression == PACK_LZJB) {
#ifdef __LZJB
		pack->thread.write_callback = &pack_lzjb_write_callback;
		glc_log(pack->glc, GLC_INFO, "pack",
			"compressing using LZJB");
#else
		glc_log(pack->glc, GLC_ERROR, "pack",
			"LZJB not supported");
		return ENOTSUP;
#endif
	} else {
		glc_log(pack->glc, GLC_ERROR, "pack",
			 "unknown/unsupported compression algorithm 0x%02x",
			 compression);
		return ENOTSUP;
	}

	pack->compression = compression;
	return 0;
}

int pack_set_minimum_size(pack_t pack, size_t min_size)
{
	if (unlikely(pack->running))
		return EALREADY;

	if (unlikely(min_size < 0))
		return EINVAL;

	pack->compress_min = min_size;
	return 0;
}

int pack_process_start(pack_t pack, ps_buffer_t *from, ps_buffer_t *to)
{
	int ret;
	if (unlikely(pack->running))
		return EAGAIN;

	if (unlikely(!pack->compression)) {
		glc_log(pack->glc, GLC_ERROR, "pack",
			"attempt to start pack before setting the compression");
		return EINVAL;
	}

	if (unlikely((ret = glc_thread_create(pack->glc, &pack->thread, from, to))))
		return ret;
	pack->running = 1;

	return 0;
}

int pack_process_wait(pack_t pack)
{
	if (unlikely(!pack->running))
		return EAGAIN;

	glc_thread_wait(&pack->thread);
	pack->running = 0;

	return 0;
}

int pack_destroy(pack_t pack)
{
	print_stats(pack->glc,&pack->stats);
	free(pack);
	return 0;
}

void pack_finish_callback(void *ptr, int err)
{
	pack_t pack = (pack_t) ptr;

	if (unlikely(err))
		glc_log(pack->glc, GLC_ERROR, "pack", "%s (%d)", strerror(err), err);
}

int pack_thread_create_callback(void *ptr, void **threadptr)
{
	pack_t pack = (pack_t) ptr;

	if (pack->compression == PACK_QUICKLZ) {
#ifdef __QUICKLZ
		*threadptr = malloc(sizeof(qlz_state_compress));
#endif
	} else if (pack->compression == PACK_LZO) {
#ifdef __LZO
		*threadptr = malloc(__lzo_wrk_mem);
#endif
	}

	return 0;
}

void pack_thread_finish_callback(void *ptr, void *threadptr, int err)
{
	free(threadptr);
}

int pack_read_callback(glc_thread_state_t *state)
{
	pack_t pack = (pack_t) state->ptr;

	__sync_fetch_and_add(&pack->stats.unpack_size, state->read_size);

	/* compress only audio and pictures */
	if ((state->read_size > pack->compress_min) &&
	    ((state->header.type == GLC_MESSAGE_VIDEO_FRAME) ||
	     (state->header.type == GLC_MESSAGE_AUDIO_DATA))) {
		if (pack->compression == PACK_QUICKLZ) {
#ifdef __QUICKLZ
			state->write_size = sizeof(glc_container_message_header_t)
					    + sizeof(glc_quicklz_header_t)
					    + __quicklz_worstcase(state->read_size);
#else
			goto copy;
#endif
		} else if (pack->compression == PACK_LZO) {
#ifdef __LZO
			state->write_size = sizeof(glc_container_message_header_t)
					    + sizeof(glc_lzo_header_t)
			                    + __lzo_worstcase(state->read_size);
#else
			goto copy;
#endif
		} else if (pack->compression == PACK_LZJB) {
#ifdef __LZJB
			state->write_size = sizeof(glc_container_message_header_t)
					    + sizeof(glc_lzjb_header_t)
					    + __lzjb_worstcase(state->read_size);
#else
			goto copy;
#endif
		} else
			goto copy;

		return 0;
	}
copy:
	__sync_fetch_and_add(&pack->stats.pack_size, state->read_size);
	state->flags |= GLC_THREAD_COPY;
	return 0;
}

int pack_lzo_write_callback(glc_thread_state_t *state)
{
#ifdef __LZO
	glc_container_message_header_t *container = (glc_container_message_header_t *) state->write_data;
	glc_lzo_header_t *lzo_header =
		(glc_lzo_header_t *) &state->write_data[sizeof(glc_container_message_header_t)];
	lzo_uint compressed_size;

	__lzo_compress((unsigned char *) state->read_data, state->read_size,
		       (unsigned char *) &state->write_data[sizeof(glc_lzo_header_t) +
		       					    sizeof(glc_container_message_header_t)],
		       &compressed_size, (lzo_voidp) state->threadptr);

	lzo_header->size = (glc_size_t) state->read_size;
	memcpy(&lzo_header->header, &state->header, sizeof(glc_message_header_t));

	container->size = compressed_size + sizeof(glc_lzo_header_t);
	container->header.type = GLC_MESSAGE_LZO;

	state->header.type = GLC_MESSAGE_CONTAINER;

	__sync_fetch_and_add(&((pack_t) state->ptr)->stats.pack_size,
				compressed_size);

	return 0;
#else
	return ENOTSUP;
#endif
}

int pack_quicklz_write_callback(glc_thread_state_t *state)
{
#ifdef __QUICKLZ
	glc_container_message_header_t *container = (glc_container_message_header_t *) state->write_data;
	glc_quicklz_header_t *quicklz_header =
		(glc_quicklz_header_t *) &state->write_data[sizeof(glc_container_message_header_t)];
	size_t compressed_size =
	qlz_compress((const void *) state->read_data,
			(void *) &state->write_data[sizeof(glc_quicklz_header_t) +
			 			    sizeof(glc_container_message_header_t)],
			 state->read_size,
			 (qlz_state_compress *) state->threadptr);

	quicklz_header->size = (glc_size_t) state->read_size;
	memcpy(&quicklz_header->header, &state->header, sizeof(glc_message_header_t));

	container->size = compressed_size + sizeof(glc_quicklz_header_t);
	container->header.type = GLC_MESSAGE_QUICKLZ;

	state->header.type = GLC_MESSAGE_CONTAINER;

	__sync_fetch_and_add(&((pack_t) state->ptr)->stats.pack_size,
				compressed_size);

	return 0;
#else
	return ENOTSUP;
#endif
}

int pack_lzjb_write_callback(glc_thread_state_t *state)
{
#ifdef __LZJB
	glc_container_message_header_t *container = (glc_container_message_header_t *) state->write_data;
	glc_lzjb_header_t *lzjb_header =
		(glc_lzjb_header_t *) &state->write_data[sizeof(glc_container_message_header_t)];

	size_t compressed_size = lzjb_compress(state->read_data,
					       &state->write_data[sizeof(glc_lzjb_header_t) +
					       			  sizeof(glc_container_message_header_t)],
					       state->read_size);

	lzjb_header->size = (glc_size_t) state->read_size;
	memcpy(&lzjb_header->header, &state->header, sizeof(glc_message_header_t));

	container->size = compressed_size + sizeof(glc_lzjb_header_t);
	container->header.type = GLC_MESSAGE_LZJB;

	state->header.type = GLC_MESSAGE_CONTAINER;

	__sync_fetch_and_add(&((pack_t) state->ptr)->stats.pack_size,
				compressed_size);

	return 0;
#else
	return ENOTSUP;
#endif
}

int unpack_init(unpack_t *unpack, glc_t *glc)
{
	*unpack = (unpack_t) calloc(1, sizeof(struct unpack_s));

	(*unpack)->glc = glc;

	(*unpack)->thread.flags = GLC_THREAD_WRITE | GLC_THREAD_READ;
	(*unpack)->thread.ptr = *unpack;
	(*unpack)->thread.thread_finish_callback = &unpack_thread_finish_callback;
	(*unpack)->thread.read_callback = &unpack_read_callback;
	(*unpack)->thread.write_callback = &unpack_write_callback;
	(*unpack)->thread.finish_callback = &unpack_finish_callback;
	(*unpack)->thread.threads = glc_threads_hint(glc);

#ifdef __LZO
	lzo_init();
#endif

	return 0;
}

int unpack_process_start(unpack_t unpack, ps_buffer_t *from, ps_buffer_t *to)
{
	int ret;
	if (unlikely(unpack->running))
		return EAGAIN;

	if (unlikely((ret = glc_thread_create(unpack->glc, &unpack->thread, from, to))))
		return ret;
	unpack->running = 1;

	return 0;
}

int unpack_process_wait(unpack_t unpack)
{
	if (unlikely(!unpack->running))
		return EAGAIN;

	glc_thread_wait(&unpack->thread);
	unpack->running = 0;

	return 0;
}

int unpack_destroy(unpack_t unpack)
{
	print_stats(unpack->glc, &unpack->stats);
	free(unpack);
	return 0;
}

void unpack_finish_callback(void *ptr, int err)
{
	unpack_t unpack = (unpack_t) ptr;

	if (unlikely(err))
		glc_log(unpack->glc, GLC_ERROR, "unpack", "%s (%d)", strerror(err), err);
}

void unpack_thread_finish_callback(void *ptr, void *threadptr, int err)
{
	free(threadptr);
}

int unpack_read_callback(glc_thread_state_t *state)
{
	unpack_t unpack = (unpack_t) state->ptr;

	if (state->header.type == GLC_MESSAGE_LZO) {
#ifdef __LZO
		state->write_size = ((glc_lzo_header_t *) state->read_data)->size;
		return 0;
#else
		glc_log(unpack->glc,
			 GLC_ERROR, "unpack", "LZO not supported");
		return ENOTSUP;
#endif
	} else if (state->header.type == GLC_MESSAGE_QUICKLZ) {
#ifdef __QUICKLZ
		state->write_size = ((glc_quicklz_header_t *) state->read_data)->size;
		return 0;
#else
		glc_log(unpack->glc,
			 GLC_ERROR, "unpack", "QuickLZ not supported");
		return ENOTSUP;
#endif
	} else if (state->header.type == GLC_MESSAGE_LZJB) {
#ifdef __LZJB
		state->write_size = ((glc_lzjb_header_t *) state->read_data)->size;
		return 0;
#else
		glc_log(unpack->glc,
			GLC_ERROR, "unpack", "LZJB not supported");
		return ENOTSUP;
#endif
	}
	__sync_fetch_and_add(&unpack->stats.pack_size, state->read_size);
	__sync_fetch_and_add(&unpack->stats.unpack_size, state->read_size);
	state->flags |= GLC_THREAD_COPY;
	return 0;
}

int unpack_write_callback(glc_thread_state_t *state)
{
	unpack_t unpack = (unpack_t) state->ptr;

	if (state->header.type == GLC_MESSAGE_LZO) {
#ifdef __LZO
		__sync_fetch_and_add(&unpack->stats.pack_size, state->read_size - sizeof(glc_lzo_header_t));
		memcpy(&state->header, &((glc_lzo_header_t *) state->read_data)->header,
		       sizeof(glc_message_header_t));
		__lzo_decompress((unsigned char *) &state->read_data[sizeof(glc_lzo_header_t)],
				state->read_size - sizeof(glc_lzo_header_t),
				(unsigned char *) state->write_data,
				(lzo_uintp) &state->write_size,
				NULL);
#else
		return ENOTSUP;
#endif
	} else if (state->header.type == GLC_MESSAGE_QUICKLZ) {
#ifdef __QUICKLZ
		__sync_fetch_and_add(&unpack->stats.pack_size,
					state->read_size - sizeof(glc_quicklz_header_t));
		memcpy(&state->header, &((glc_quicklz_header_t *) state->read_data)->header,
		       sizeof(glc_message_header_t));
		if (!state->threadptr)
			state->threadptr = malloc(sizeof(qlz_state_decompress));
		qlz_decompress((const void *) &state->read_data[sizeof(glc_quicklz_header_t)],
				(void *) state->write_data,
				(qlz_state_decompress *) state->threadptr);
#else
		return ENOTSUP;
#endif
	} else if (state->header.type == GLC_MESSAGE_LZJB) {
#ifdef __LZJB
		__sync_fetch_and_add(&unpack->stats.pack_size, state->read_size - sizeof(glc_lzjb_header_t));
		memcpy(&state->header, &((glc_quicklz_header_t *) state->read_data)->header,
		       sizeof(glc_message_header_t));
		lzjb_decompress(&state->read_data[sizeof(glc_lzjb_header_t)],
				state->write_data,
				state->read_size - sizeof(glc_lzjb_header_t),
				state->write_size);
#else
		return ENOTSUP;
#endif
	} else
		return ENOTSUP;
	__sync_fetch_and_add(&unpack->stats.unpack_size, state->write_size);
	return 0;
}

void print_stats(glc_t *glc, pack_stat_t *stat)
{
	double ratio;
	if (!stat->unpack_size)
		ratio = 0.0;
	else
		ratio = (double)stat->pack_size/(double)stat->unpack_size;

	glc_log(glc, GLC_PERF, "pack",
		"unpack_size: %" PRIu64 " pack_size: %" PRIu64 " %%remn: %.1f",
		stat->unpack_size, stat->pack_size, ratio*100);
}

/**  \} */
