/**
 * \file glc/core/file.c
 * \brief file io adapted from original work (glc) from Pyry Haulos <pyry.haulos@gmail.com>
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
 * \addtogroup file
 *  \{
 */

/*
 * This is needed to access the stdio unlocked functions.
 * Our io streams are private and aren't shared among multiple
 * threads.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <fcntl.h>

#include <glc/common/state.h>
#include <glc/common/core.h>
#include <glc/common/log.h>
#include <glc/common/thread.h>
#include <glc/common/util.h>

#include <glc/core/tracker.h>

#include "file.h"
#include "optimization.h"

#define FILE_READING       0x1
#define FILE_WRITING       0x2
#define FILE_RUNNING       0x4
#define FILE_INFO_WRITTEN  0x8
#define FILE_INFO_READ    0x10
#define FILE_INFO_VALID   0x20

struct file_private_s {
	glc_t *glc;
	glc_flags_t flags;
	/*
	 * Using stdio may help performance by:
	 * - reducing syscalls
	 * - Use buffering to preserve block size aligment (usually 4KB)
	 * - On 64-bits platform, on the readside, stdio may even use mmap to improve
	 *   performance.
	 */
	FILE *handle;
};

typedef struct {
	struct sink_s sink_base;
	struct file_private_s mpriv;
	glc_thread_t thread;
	tracker_t state_tracker;
	callback_request_func_t callback;
	int sync;
} file_sink_t;

typedef struct {
	struct source_s source_base;
	struct file_private_s mpriv;
	u_int32_t stream_version;
} file_source_t;

static void file_finish_callback(void *ptr, int err);
static int file_read_callback(glc_thread_state_t *state);
static int file_write_message(file_sink_t *file, glc_message_header_t *header,
			      void *message, size_t message_size);
static int file_write_state_callback(glc_message_header_t *header, void *message,
				     size_t message_size, void *arg);
static int file_test_stream_version(u_int32_t version);
static int file_set_target(struct file_private_s *mpriv, int fd);

static int file_can_resume(sink_t sink);
static int file_set_sync(sink_t sink, int sync);
static int file_set_callback(sink_t sink, callback_request_func_t callback);
static int file_open_target(sink_t sink, const char *filename);
static int file_close_target(sink_t sink);
static int file_write_info(sink_t sink, glc_stream_info_t *info,
			const char *info_name, const char *info_date);
static int file_write_eof(sink_t sink);
static int file_write_state(sink_t sink);
static int file_write_process_start(sink_t sink, ps_buffer_t *from);
static int file_write_process_wait(sink_t sink);
static int file_sink_destroy(sink_t sink);

static int file_set_source(struct file_private_s *mpriv, int fd);
static int file_open_source(source_t source, const char *filename);
static int file_close_source(source_t source);
static int file_read_info(source_t source, glc_stream_info_t *info,
			char **info_name, char **info_date);
static int file_read(source_t source, ps_buffer_t *to);
static int file_source_destroy(source_t source);

static sink_ops_t file_sink_ops = {
	.can_resume          = file_can_resume,
	.set_sync            = file_set_sync,
	.set_callback        = file_set_callback,
	.open_target         = file_open_target,
	.close_target        = file_close_target,
	.write_info          = file_write_info,
	.write_eof           = file_write_eof,
	.write_state         = file_write_state,
	.write_process_start = file_write_process_start,
	.write_process_wait  = file_write_process_wait,
	.destroy             = file_sink_destroy,
};

static source_ops_t file_source_ops = {
	.open_source         = file_open_source,
	.close_source        = file_close_source,
	.read_info           = file_read_info,
	.read                = file_read,
	.destroy             = file_source_destroy,
};

int file_sink_init(sink_t *sink, glc_t *glc)
{
	file_sink_t *file = (file_sink_t*)calloc(1, sizeof(file_sink_t));
	*sink = (sink_t)file;
	if (!file)
		return ENOMEM;

	file->sink_base.ops  = &file_sink_ops;
	file->mpriv.glc      = glc;
	file->thread.flags   = GLC_THREAD_READ;
	file->thread.ptr     = file;
	file->thread.read_callback   = &file_read_callback;
	file->thread.finish_callback = &file_finish_callback;
	file->thread.threads = 1;

	tracker_init(&file->state_tracker, file->mpriv.glc);

	return 0;
}

int file_sink_destroy(sink_t sink)
{
	file_sink_t *file = (file_sink_t*)sink;
	tracker_destroy(file->state_tracker);
	free(file);
	return 0;
}

int file_can_resume(sink_t sink)
{
	return 1;
}

int file_source_init(source_t *source, glc_t *glc)
{
	file_source_t *file = (file_source_t*)calloc(1, sizeof(file_source_t));
	*source = (source_t)file;
	if (!file)
		return ENOMEM;

	file->source_base.ops = &file_source_ops;
	file->mpriv.glc       = glc;
	return 0;
}

int file_source_destroy(source_t source)
{
	free(source);
	return 0;
}

int file_set_sync(sink_t sink, int sync)
{
	file_sink_t *file = (file_sink_t*)sink;
	file->sync = sync;
	return 0;
}

int file_set_callback(sink_t sink, callback_request_func_t callback)
{
	file_sink_t *file = (file_sink_t*)sink;
	file->callback = callback;
	return 0;
}

/*
 * Default file access permissions for new files.
 */
#define FILE_MODE (S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH)

int file_open_target(sink_t sink, const char *filename)
{
	int fd, ret = 0;
	file_sink_t *file = (file_sink_t*)sink;
	if (unlikely(file->mpriv.handle))
		return EBUSY;

	glc_log(file->mpriv.glc, GLC_INFO, "file",
		 "opening %s for writing stream (%s)",
		 filename,
		 file->sync ? "sync" : "no sync");

	fd = open(filename, O_CREAT | O_WRONLY | (file->sync ? O_SYNC : 0), FILE_MODE);

	if (unlikely(fd < 0)) {
		glc_log(file->mpriv.glc, GLC_ERROR, "file", "can't open %s: %s (%d)",
			 filename, strerror(errno), errno);
		return errno;
	}

	if (unlikely((ret = file_set_target(&file->mpriv, fd))))
		close(fd);

	return ret;
}

static int lock_reg(int fd, int cmd, int type, off_t offset, int whence, off_t len)
{
	struct flock lock;

	lock.l_type   = type;   /* F_RDLCK, F_WRLCK, F_UNLCK */
	lock.l_start  = offset; /* byte offset, relative to l_whence */
	lock.l_whence = whence; /* SEEK_SET, SEEK_CUR, SEEK_END */
	lock.l_len    = len;    /* #bytes (0 means to EOF) */

	return fcntl(fd, cmd, &lock);
}

#define write_lock(fd, offset, whence, len) \
        lock_reg((fd), F_SETLK, F_WRLCK, (offset), (whence), (len))
#define lockfile(fd) write_lock((fd), 0, SEEK_SET, 0)

int file_set_target(struct file_private_s *mpriv, int fd)
{
	struct stat statbuf;
	if (unlikely(mpriv->handle))
		return EBUSY;

	/*
	 * turn on set-group-ID and turn off group-execute.
	 * Required for mandatory locking. Must also enable it
	 * when mounting the fs with the generic fs mount
	 * option 'mand'. See mount command man page for details.
	 */

        if (unlikely(fstat(fd, &statbuf) < 0)) {
		glc_log(mpriv->glc, GLC_ERROR, "file",
			"fstat error: %s (%d)", strerror(errno), errno);
		return errno;
	}
        if (unlikely(fchmod(fd, (statbuf.st_mode & ~S_IXGRP) | S_ISGID) < 0)) {
		glc_log(mpriv->glc, GLC_ERROR, "file",
			"fchmod error: %s (%d)", strerror(errno), errno);
		return errno;
	}

	if (unlikely(lockfile(fd) < 0)) {
		glc_log(mpriv->glc, GLC_ERROR, "file",
			 "can't lock file: %s (%d)", strerror(errno), errno);
		return errno;
	}

	/* truncate file when we have locked it */
	lseek(fd, 0, SEEK_SET);
	if (unlikely(ftruncate(fd, 0) != 0))
		glc_log(mpriv->glc, GLC_WARN, "file", "ftruncate error: %s (%d)",
			strerror(errno), errno);

	mpriv->handle = fdopen(fd, "w");
	if (unlikely(!mpriv->handle)) {
		glc_log(mpriv->glc, GLC_ERROR, "file", "fdopen error: %s (%d)",
			strerror(errno), errno);
		return errno;
	}
	mpriv->flags |= FILE_WRITING;
	return 0;
}

static inline int is_write_open_not_running(struct file_private_s *mpriv)
{
	return mpriv->handle && (mpriv->flags & FILE_WRITING) &&
		!(mpriv->flags & FILE_RUNNING);
}

int file_close_target(sink_t sink)
{
	file_sink_t *file = (file_sink_t*)sink;
	if (unlikely(!is_write_open_not_running(&file->mpriv)))
		return EAGAIN;

	if (unlikely(fclose(file->mpriv.handle)))
		glc_log(file->mpriv.glc, GLC_ERROR, "file",
			 "can't close file: %s (%d)",
			 strerror(errno), errno);

	file->mpriv.handle = NULL;
	file->mpriv.flags &= ~(FILE_WRITING | FILE_INFO_WRITTEN);

	return 0;
}

int file_write_info(sink_t sink, glc_stream_info_t *info,
		    const char *info_name, const char *info_date)
{
	file_sink_t *file = (file_sink_t*)sink;
	if (unlikely(!is_write_open_not_running(&file->mpriv)))
		return EAGAIN;

	if (unlikely(fwrite_unlocked(info,
		sizeof(glc_stream_info_t), 1, file->mpriv.handle) != 1))
		goto err;
	if (unlikely(fwrite_unlocked(info_name,
		info->name_size, 1, file->mpriv.handle) != 1))
		goto err;
	if (unlikely(fwrite_unlocked(info_date,
		info->date_size, 1, file->mpriv.handle) != 1))
		goto err;

	if (unlikely(file->sync))
		if (unlikely(fflush_unlocked(file->mpriv.handle)))
			goto err;

	file->mpriv.flags |= FILE_INFO_WRITTEN;
	return 0;
err:
	glc_log(file->mpriv.glc, GLC_ERROR, "file",
		 "can't write stream information: %s (%d)",
		 strerror(errno), errno);
	return errno;
}

int file_write_message(file_sink_t *file, glc_message_header_t *header,
			void *message, size_t message_size)
{
	glc_size_t glc_size = (glc_size_t) message_size;

	if (unlikely(fwrite_unlocked(&glc_size, sizeof(glc_size_t),
				1, file->mpriv.handle) != 1))
		goto err;
	if (unlikely(fwrite_unlocked(header, sizeof(glc_message_header_t),
				1, file->mpriv.handle) != 1))
		goto err;
	if (likely(message_size > 0))
		if (unlikely(fwrite_unlocked(message, message_size,
				1, file->mpriv.handle) != 1))
			goto err;

	if (unlikely(file->sync))
		if (unlikely(fflush_unlocked(file->mpriv.handle)))
			goto err;
	return 0;
err:
	return errno;
}

int file_write_eof(sink_t sink)
{
	int ret;
	file_sink_t *file = (file_sink_t*)sink;
	glc_message_header_t hdr;

	if (unlikely(!is_write_open_not_running(&file->mpriv))) {
	    ret = EAGAIN;
	    goto err;
	}

	hdr.type = GLC_MESSAGE_CLOSE;
	if (unlikely((ret = file_write_message(file, &hdr, NULL, 0))))
		goto err;

	return 0;
err:
	glc_log(file->mpriv.glc, GLC_ERROR, "file",
		 "can't write eof: %s (%d)",
		 strerror(ret), ret);
	return ret;
}

int file_write_state_callback(glc_message_header_t *header, void *message,
				size_t message_size, void *arg)
{
	file_sink_t *file = arg;
	return file_write_message(file, header, message, message_size);
}

/*
 * Allow to create a self contained glc file after reloading the capture since
 * format messages are generated only once by capture modules at the beginning
 * of the initial capture session.
 */
int file_write_state(sink_t sink)
{
	int ret;
	file_sink_t *file = (file_sink_t*)sink;
	if (unlikely(!is_write_open_not_running(&file->mpriv))) {
	    ret = EAGAIN;
	    goto err;
	}

	if (unlikely((ret = tracker_iterate_state(file->state_tracker,
						  &file_write_state_callback, file))))
		goto err;

	return 0;
err:
	glc_log(file->mpriv.glc, GLC_ERROR, "file",
		 "can't write state: %s (%d)",
		 strerror(ret), ret);
	return ret;
}

int file_write_process_start(sink_t sink, ps_buffer_t *from)
{
	int ret;
	file_sink_t *file = (file_sink_t*)sink;
	if (unlikely(!is_write_open_not_running(&file->mpriv) ||
		     !(file->mpriv.flags & FILE_INFO_WRITTEN)))
		return EAGAIN;

	if (unlikely((ret = glc_thread_create(file->mpriv.glc, &file->thread,
					from, NULL))))
		return ret;
	/** \todo cancel buffer if this fails? */
	file->mpriv.flags |= FILE_RUNNING;

	return 0;
}

int file_write_process_wait(sink_t sink)
{
	file_sink_t *file = (file_sink_t*)sink;
	if (unlikely(!file->mpriv.handle ||
		!(file->mpriv.flags & FILE_RUNNING) ||
		!(file->mpriv.flags & FILE_WRITING) ||
		!(file->mpriv.flags & FILE_INFO_WRITTEN)))
		return EAGAIN;

	glc_thread_wait(&file->thread);
	file->mpriv.flags &= ~FILE_RUNNING;

	return 0;
}

void file_finish_callback(void *ptr, int err)
{
	file_sink_t *file = (file_sink_t*) ptr;

	if (unlikely(err))
		glc_log(file->mpriv.glc, GLC_ERROR, "file", "%s (%d)",
			strerror(err), err);
}

int file_read_callback(glc_thread_state_t *state)
{
	file_sink_t *file = (file_sink_t*) state->ptr;
	glc_container_message_header_t *container;
	glc_size_t glc_size;
	glc_callback_request_t *callback_req;

	/* let state tracker to process this message */
	tracker_submit(file->state_tracker, &state->header, state->read_data, state->read_size);

	if (state->header.type == GLC_CALLBACK_REQUEST) {
		/* callback request messages are never written to disk */
		if (file->callback != NULL) {
			/* callbacks may manipulate target file so remove FILE_RUNNING flag */
			file->mpriv.flags &= ~FILE_RUNNING;
			callback_req = (glc_callback_request_t *) state->read_data;
			file->callback(callback_req->arg);
			file->mpriv.flags |= FILE_RUNNING;
		}
	} else if (state->header.type == GLC_MESSAGE_CONTAINER) {
		container = (glc_container_message_header_t *) state->read_data;
		if (unlikely(fwrite_unlocked(state->read_data,
			sizeof(glc_container_message_header_t) + container->size,
			1, file->mpriv.handle)
		    != 1))
			goto err;
		if (unlikely(file->sync))
			if (unlikely(fflush_unlocked(file->mpriv.handle)))
				goto err;
	} else {
		/* emulate container message */
		glc_size = state->read_size;
		if (unlikely(fwrite_unlocked(&glc_size,
				   sizeof(glc_size_t), 1, file->mpriv.handle) != 1))
			goto err;
		if (unlikely(fwrite_unlocked(&state->header, sizeof(glc_message_header_t),
					1, file->mpriv.handle)
		    != 1))
			goto err;
		if (unlikely(fwrite_unlocked(state->read_data,
				   state->read_size, 1, file->mpriv.handle) != 1))
			goto err;
		if (unlikely(file->sync))
			if (unlikely(fflush_unlocked(file->mpriv.handle)))
				goto err;
	}

	return 0;

err:
	glc_log(file->mpriv.glc, GLC_ERROR, "file", "%s (%d)", strerror(errno), errno);
	return errno;
}

int file_open_source(source_t source, const char *filename)
{
	int fd, ret = 0;
	file_source_t *file = (file_source_t*)source;
	if (unlikely(file->mpriv.handle))
		return EBUSY;

	glc_log(file->mpriv.glc, GLC_INFO, "file",
		 "opening %s for reading stream", filename);

	fd = open(filename, O_RDONLY);

	if (unlikely(fd == -1)) {
		glc_log(file->mpriv.glc, GLC_ERROR, "file", "can't open %s: %s (%d)",
			 filename, strerror(errno), errno);
		return errno;
	}

	/* Attempt to hint the kernel on our pattern usage  */
	posix_fadvise(fd, 0, 0, POSIX_FADV_SEQUENTIAL);

	if (unlikely((ret = file_set_source(&file->mpriv, fd))))
		close(fd);

	return ret;
}

int file_set_source(struct file_private_s *mpriv, int fd)
{
	if (unlikely(mpriv->handle))
		return EBUSY;

	/* seek to beginning */
	lseek(fd, 0, SEEK_SET);

	mpriv->handle = fdopen(fd, "r");
	if (unlikely(!mpriv->handle)) {
		glc_log(mpriv->glc, GLC_ERROR, "file", "fdopen error: %s (%d)",
			strerror(errno), errno);
		return errno;
	}
	mpriv->flags |= FILE_READING;
	return 0;
}

static inline int is_read_open(struct file_private_s *mpriv)
{
	return mpriv->handle && (mpriv->flags & FILE_READING);
}

int file_close_source(source_t source)
{
	file_source_t *file = (file_source_t*)source;
	if (unlikely(!is_read_open(&file->mpriv)))
		return EAGAIN;

	if (unlikely(fclose(file->mpriv.handle)))
		glc_log(file->mpriv.glc, GLC_ERROR, "file",
			 "can't close file: %s (%d)",
			 strerror(errno), errno);

	file->mpriv.handle = NULL;
	file->mpriv.flags &= ~(FILE_READING | FILE_INFO_READ | FILE_INFO_VALID);

	return 0;	
}

int file_test_stream_version(u_int32_t version)
{
	/*
	 * current version is always supported.
	 * The new addition to 0x05 version is that timestamp
	 * are now in nanoseconds.
	 * To not have to care about this difference all around the
	 * code, we normalize timestamps in this module
	 * by making sure that all outgoing timestamps are in
	 * nanoseconds.
	 */
	if (likely(version == GLC_STREAM_VERSION)) {
		return 0;
	} else if (version == 0x03 || version ==0x04) {
		/*
		 0.5.5 was last version to use 0x03.
		 Only change between 0x03 and 0x04 is header and
		 size order in on-disk packet header.
		*/
		return 0;
	}
	return ENOTSUP;
}

int file_read_info(source_t source, glc_stream_info_t *info,
		   char **info_name, char **info_date)
{
	file_source_t *file = (file_source_t*)source;
	*info_name = NULL;
	*info_date = NULL;
	if (unlikely(!is_read_open(&file->mpriv)))
		return EAGAIN;

	if (unlikely(fread_unlocked(info, sizeof(glc_stream_info_t), 1, file->mpriv.handle) !=
			 1)) {
		glc_log(file->mpriv.glc, GLC_ERROR, "file",
			 "can't read stream info header");
		return errno;
	}
	file->mpriv.flags |= FILE_INFO_READ;

	if (unlikely(info->signature != GLC_SIGNATURE)) {
		glc_log(file->mpriv.glc, GLC_ERROR, "file",
			 "signature 0x%08x does not match 0x%08x",
			 info->signature, GLC_SIGNATURE);
		return EINVAL;
	}

	if (file_test_stream_version(info->version)) {
		glc_log(file->mpriv.glc, GLC_ERROR, "file",
			 "unsupported stream version 0x%02x", info->version);
		return ENOTSUP;
	}
	glc_log(file->mpriv.glc, GLC_INFO, "file", "stream version 0x%02x", info->version);
	file->stream_version = info->version; /* copy version */

	if (info->name_size > 0) {
		*info_name = (char *) malloc(info->name_size);
		if (unlikely(fread_unlocked(*info_name, info->name_size, 1, file->mpriv.handle) != 1))
			return errno;
	}

	if (info->date_size > 0) {
		*info_date = (char *) malloc(info->date_size);
		if (unlikely(fread_unlocked(*info_date, info->date_size, 1, file->mpriv.handle) != 1))
			return errno;
	}

	file->mpriv.flags |= FILE_INFO_VALID;
	return 0;
}

int file_read(source_t source, ps_buffer_t *to)
{
	file_source_t *file = (file_source_t*)source;
	int ret = 0;
	glc_message_header_t header;
	size_t packet_size = 0;
	ps_packet_t packet;
	char *dma;
	glc_size_t glc_ps;

	if (unlikely(!is_read_open(&file->mpriv)))
		return EAGAIN;

	if (unlikely(!(file->mpriv.flags & FILE_INFO_READ))) {
		glc_log(file->mpriv.glc, GLC_ERROR, "file",
			 "stream info header not read");
		return EAGAIN;
	}

	if (unlikely(!(file->mpriv.flags & FILE_INFO_VALID))) {
		glc_log(file->mpriv.glc, GLC_ERROR, "file",
			 "stream info header not valid");
		file->mpriv.flags &= ~FILE_INFO_READ;
		return EINVAL;
	}

	ps_packet_init(&packet, to);

	do {
		if (unlikely(file->stream_version == 0x03)) {
			/* old order */
			if (unlikely(fread_unlocked(&header,
				sizeof(glc_message_header_t), 1, file->mpriv.handle) != 1))
				goto send_eof;
			if (unlikely(fread_unlocked(&glc_ps, sizeof(glc_size_t),
						1, file->mpriv.handle) !=
				1))
				goto send_eof;
		} else {
			/* same header format as in container messages */
			if (unlikely(fread_unlocked(&glc_ps, sizeof(glc_size_t), 1, file->mpriv.handle) !=
				1))
				goto send_eof;
			if (unlikely(fread_unlocked(&header,
				sizeof(glc_message_header_t), 1, file->mpriv.handle) != 1))
				goto send_eof;
		}

		packet_size = glc_ps;

		if (unlikely((ret = ps_packet_open(&packet, PS_PACKET_WRITE))))
			goto err;
		if (unlikely((ret = ps_packet_write(&packet, &header,
						sizeof(glc_message_header_t)))))
			goto err;
		if (unlikely((ret = ps_packet_dma(&packet, (void **)&dma,
					packet_size, PS_ACCEPT_FAKE_DMA))))
			goto err;

		if (unlikely(fread_unlocked(dma, 1, packet_size, file->mpriv.handle) != packet_size))
			goto read_fail;

		if (unlikely(file->stream_version < 0x05)) {
			if (header.type == GLC_MESSAGE_VIDEO_FRAME ||
			    header.type == GLC_MESSAGE_AUDIO_DATA) {
				/*
				 * because glc_video_frame_header_t and glc_audio_data_header_t
				 * start with the same data members, it is ok use the same pointer
				 * type for both types.
				 */
				glc_video_frame_header_t *data_hdr = (glc_video_frame_header_t *)dma;
				/* transform uSec in nsec */
				data_hdr->time *= 1000;
			}
		}

		if (unlikely((ret = ps_packet_close(&packet))))
			goto err;
	} while ((header.type != GLC_MESSAGE_CLOSE) &&
		 (!glc_state_test(file->mpriv.glc, GLC_STATE_CANCEL)));

finish:
	ps_packet_destroy(&packet);

	file->mpriv.flags &= ~(FILE_INFO_READ | FILE_INFO_VALID);
	return 0;

send_eof:
	header.type = GLC_MESSAGE_CLOSE;
	ps_packet_open(&packet, PS_PACKET_WRITE);
	ps_packet_write(&packet, &header, sizeof(glc_message_header_t));
	ps_packet_close(&packet);

	glc_log(file->mpriv.glc, GLC_ERROR, "file", "unexpected EOF");
	goto finish;

read_fail:
	ret = EBADMSG;
	glc_log(file->mpriv.glc, GLC_ERROR, "file",
		"read_file while reading a packet type %s (%d) at offset %ld",
		glc_util_msgtype_to_str(header.type), header.type, ftell(file->mpriv.handle));
err:
	if (ret == EINTR)
		goto finish; /* just cancel */

	glc_log(file->mpriv.glc, GLC_ERROR, "file", "%s (%d)", strerror(ret), ret);
	glc_log(file->mpriv.glc, GLC_DEBUG, "file", "packet size is %zd", packet_size);
	ps_buffer_cancel(to);

	file->mpriv.flags &= ~(FILE_INFO_READ | FILE_INFO_VALID);
	return ret;
}

/**  \} */

