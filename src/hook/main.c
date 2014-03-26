/**
 * \file hook/main.c
 * \brief main wrapper library adapted from original work (glc) from Pyry Haulos <pyry.haulos@gmail.com>
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
 * \addtogroup hook
 *  \{
 * \defgroup main main wrapper library
 *  \{
 */

#include <stdlib.h>
#include <dlfcn.h>
#include <string.h>
#include <elfhacks.h>
#include <unistd.h>
#include <fnmatch.h>
#include <sched.h>
#include <pthread.h>

#include <glc/common/glc.h>
#include <glc/common/core.h>
#include <glc/common/log.h>
#include <glc/common/util.h>
#include <glc/common/state.h>
#include <glc/core/pack.h>
#include <glc/core/file.h>
#include <glc/core/pipe.h>

#include "lib.h"

#define MAIN_PIPE_VFLIP            0x1
#define MAIN_COMPRESS_NONE         0x2
#define MAIN_COMPRESS_QUICKLZ      0x4
#define MAIN_COMPRESS_LZO          0x8
#define MAIN_CUSTOM_LOG           0x10
#define MAIN_SYNC                 0x20
#define MAIN_COMPRESS_LZJB        0x40
#define MAIN_START                0x80

#define SINK_CB_RELOAD_ARG         (void *)0x1
#define SINK_CB_STOP_ARG           (void *)0x2

struct main_private_s {
	glc_t glc;
	glc_flags_t flags;

	ps_buffer_t *uncompressed;
	ps_buffer_t *compressed;
	size_t uncompressed_size, compressed_size;

	sink_t sink;
	pack_t pack;

	unsigned int capture_id;
	const char *pipe_exec_file;
	const char *stream_file_fmt;
	char *stream_file;

	glc_utime_t stop_time;

	/*
	 * Synchronize capture start/stop that can be initiated from
	 * the x11 thread or the sink thread.
	 */
	pthread_mutex_t capture_action_lock;
};

__PRIVATE glc_lib_t lib = {NULL, /* dlopen */
			   NULL, /* dlsym */
			   NULL, /* dlvsym */
			   NULL, /* __libc_dlsym */
			   0, /* running */
			   PTHREAD_ONCE_INIT, /* init_once */
			   0, /* flags */
			   };
__PRIVATE struct main_private_s mpriv = {
	.capture_action_lock = PTHREAD_MUTEX_INITIALIZER,
};

__PRIVATE int  init_buffers();
__PRIVATE void lib_close();
__PRIVATE int  load_environ();
__PRIVATE void get_real_libc_dlsym();
static void stream_sink_callback(void *arg);
static int open_stream();
static int close_stream();
static int reload_stream();
static int send_cb_request(void *req_arg);
static int start_capture_impl();

void init_glc()
{
	int ret;
	char *env_val;
	mpriv.flags       = 0;
	mpriv.capture_id  = 0;
	mpriv.stop_time   = 0;
	mpriv.stream_file = NULL;
	mpriv.stream_file_fmt = "%app%-%pid%-%capture%.glc";

	/* init glc first */
	glc_init(&mpriv.glc);
	/* initialize state */
	glc_state_init(&mpriv.glc);

	load_environ();
	glc_util_log_version(&mpriv.glc);

	if (unlikely((ret = init_buffers())))
		goto err;
	if (unlikely((ret = opengl_init(&mpriv.glc))))
		goto err;
	if (unlikely((ret = alsa_init(&mpriv.glc))))
		goto err;
	if (unlikely((ret = x11_init(&mpriv.glc))))
		goto err;

	glc_util_log_info(&mpriv.glc);

	if (mpriv.flags & MAIN_START)
		start_capture_impl();

	atexit(lib_close);

	glc_log(&mpriv.glc, GLC_INFO, "main", "glc initialized");
	env_val = getenv("LD_PRELOAD");
	if (unlikely(!env_val))
		env_val = "(null)";
	glc_log(&mpriv.glc, GLC_DEBUG, "main", "LD_PRELOAD=%s", env_val);

	/*
	 * Unset LD_PRELOAD to avoid to have more than 1 app captured.
	 * Otherwise spawned children would interfere when initialising
	 * glcs such as resetting the log file.
	 *
	 * We could be more careful and just remove libglc-hook.so
	 * if this variable is used for other things...
	 */
	unsetenv("LD_PRELOAD");

	return;
err:
	fprintf(stderr, "(glc) %s (%d)\n", strerror(ret), ret);
	exit(ret); /* glc initialization is critical */
}

int load_environ()
{
	char *log_file;
	char *env_val;

	if ((env_val = getenv("GLC_START"))) {
		if (atoi(env_val))
			mpriv.flags |= MAIN_START;
	}

	if ((env_val = getenv("GLC_FILE")))
		mpriv.stream_file_fmt = env_val;

	if ((env_val = getenv("GLC_LOG")))
		glc_log_set_level(&mpriv.glc, atoi(env_val));

	if ((env_val = getenv("GLC_LOG_FILE"))) {
		/* limit log file name to 1023 chars. */
		log_file = malloc(1024);
		snprintf(log_file, 1024, env_val, getpid());
		log_file[1023] = '\0';

		glc_log_open_file(&mpriv.glc, log_file);
		free(log_file);
		mpriv.flags |= MAIN_CUSTOM_LOG;
	}

	if ((env_val = getenv("GLC_SYNC"))) {
		if (atoi(env_val))
			mpriv.flags |= MAIN_SYNC;
	}

	mpriv.uncompressed_size = 1024 * 1024 * 25;
	if ((env_val = getenv("GLC_UNCOMPRESSED_BUFFER_SIZE")))
		mpriv.uncompressed_size = atoi(env_val) * 1024 * 1024;

	mpriv.compressed_size = 1024 * 1024 * 50;
	if ((env_val = getenv("GLC_COMPRESSED_BUFFER_SIZE")))
		mpriv.compressed_size = atoi(env_val) * 1024 * 1024;

	if ((env_val = getenv("GLC_PIPE"))) {
		if (likely(!access(env_val,X_OK)))
			mpriv.pipe_exec_file = env_val;
		else
			glc_log(&mpriv.glc, GLC_ERROR, "main",
				"cannot exexute '%s': %s (%d) - will fall back to file sink",
				env_val, strerror(errno), errno);
		if ((env_val = getenv("GLC_PIPE_INVERT"))) {
			if (atoi(env_val))
				mpriv.flags |= MAIN_PIPE_VFLIP;
		}
	}

	/*
	 * pipe sink sends only raw uncompressed data.
	 */
	if (!mpriv.pipe_exec_file) {
		if ((env_val = getenv("GLC_COMPRESS"))) {
			if (!strcmp(env_val, "lzo"))
				mpriv.flags |= MAIN_COMPRESS_LZO;
			else if (!strcmp(env_val, "quicklz"))
				mpriv.flags |= MAIN_COMPRESS_QUICKLZ;
			else if (!strcmp(env_val, "lzjb"))
				mpriv.flags |= MAIN_COMPRESS_LZJB;
			else
				mpriv.flags |= MAIN_COMPRESS_NONE;
		} else
			mpriv.flags |= MAIN_COMPRESS_LZO;
	} else
		 mpriv.flags |= MAIN_COMPRESS_NONE;

	if ((env_val = getenv("GLC_RTPRIO")))
		glc_set_allow_rt(&mpriv.glc, atoi(env_val));

	glc_account_threads(&mpriv.glc,1,!(mpriv.flags & MAIN_COMPRESS_NONE));

	glc_log(&mpriv.glc, GLC_DEBUG, "main", "flags: %08X", mpriv.flags);

	return 0;
}

int init_buffers()
{
	int ret;
	ps_bufferattr_t attr;
	ps_bufferattr_init(&attr);

	if (glc_log_get_level(&mpriv.glc) >= GLC_PERF)
		ps_bufferattr_setflags(&attr, PS_BUFFER_STATS);

	ps_bufferattr_setsize(&attr, mpriv.uncompressed_size);
	mpriv.uncompressed = (ps_buffer_t *) malloc(sizeof(ps_buffer_t));
	if (unlikely((ret = ps_buffer_init(mpriv.uncompressed, &attr))))
		return ret;

	if (!(mpriv.flags & MAIN_COMPRESS_NONE)) {
		ps_bufferattr_setsize(&attr, mpriv.compressed_size);
		mpriv.compressed = (ps_buffer_t *) malloc(sizeof(ps_buffer_t));
		if (unlikely((ret = ps_buffer_init(mpriv.compressed, &attr))))
			return ret;
	}

	ps_bufferattr_destroy(&attr);
	return 0;
}

int open_stream()
{
	glc_stream_info_t *stream_info;
	char *info_name;
	char info_date[26];
	int ret;

	glc_util_info_create(&mpriv.glc, &stream_info, &info_name, info_date);
	mpriv.stream_file = glc_util_format_filename(mpriv.stream_file_fmt,
						mpriv.capture_id);

	if (unlikely((ret = mpriv.sink->ops->set_sync(mpriv.sink,
						(mpriv.flags & MAIN_SYNC) ? 1 : 0))))
		goto at_exit;
	if (unlikely((ret = mpriv.sink->ops->open_target(mpriv.sink,
							mpriv.stream_file))))
		goto at_exit;
	ret = mpriv.sink->ops->write_info(mpriv.sink, stream_info,
				info_name, info_date);

	/*
	 * reset state time
	 * This is done so for every saved file stream, the initial timestamp
	 * will be 0.
	 */
	glc_state_time_reset(&mpriv.glc);
	mpriv.stop_time = 0;

at_exit:
	free(stream_info);
	free(info_name);
	return ret;
}

int close_stream()
{
	int ret;
	/*
	 * By closing the sink before freeing the target name makes it safe for
	 * the sink to keep in ref the target name passed to open_target()
	 */
	ret = mpriv.sink->ops->close_target(mpriv.sink);
	free(mpriv.stream_file);
	mpriv.stream_file = NULL;

	return ret;
}

void stream_sink_callback(void *arg)
{
	/* this is called when callback request arrives to file object */
	int ret;

	if (arg == SINK_CB_RELOAD_ARG)
	{
		glc_log(&mpriv.glc, GLC_INFO, "main", "reloading stream");

		if (unlikely((ret = mpriv.sink->ops->write_eof(mpriv.sink))))
			goto err;
		if (unlikely((ret = close_stream())))
			goto err;
		if (unlikely((ret = open_stream())))
			goto err;
		if (unlikely((ret = mpriv.sink->ops->write_state(mpriv.sink))))
			goto err;
	}
	else if (arg == SINK_CB_STOP_ARG)
	{
		glc_log(&mpriv.glc, GLC_INFO, "main", "stopping stream");
		if (unlikely((ret = mpriv.sink->ops->write_eof(mpriv.sink))))
			goto err;
	}
	else
	{
		glc_log(&mpriv.glc, GLC_ERROR, "main",
			"unknown stream_sink_cb arg value: %p", arg);
	}
	return;
err:
	glc_log(&mpriv.glc, GLC_ERROR, "main",
		"error during stream sink cb (%p): %s (%d)\n",
		arg, strerror(ret), ret);
}

int send_cb_request(void *req_arg)
{
	glc_message_header_t hdr;
	hdr.type = GLC_CALLBACK_REQUEST;
	glc_callback_request_t callback_req;
	callback_req.arg = req_arg;

	/* synchronize with opengl top buffer */
	return opengl_push_message(&hdr, &callback_req,
				sizeof(glc_callback_request_t));
}

inline int reload_stream()
{
	return send_cb_request(SINK_CB_RELOAD_ARG);
}

static inline int stop_stream()
{
	return send_cb_request(SINK_CB_STOP_ARG);
}

static inline int is_stream_open()
{
	return mpriv.stream_file != NULL;
}

static inline void increment_capture()
{
	mpriv.capture_id++;
}

int reload_capture()
{
	/*
	 * The stream will not be open on the first capture
	 * if initiated by reload.
	 */
	if (is_stream_open()) {
		increment_capture();
		reload_stream();
	}
	return start_capture_impl();
}

void start_capture()
{
	if (mpriv.sink && !mpriv.sink->ops->can_resume(mpriv.sink))
		reload_capture();
	else
		start_capture_impl();
}

int start_capture_impl()
{
	int ret;
	if (unlikely(ret = pthread_mutex_lock(&mpriv.capture_action_lock)))
		goto err_nolock;
	if (unlikely(lib.flags & LIB_CAPTURING)) {
		ret = EAGAIN;
		goto func_exit;
	}

	if (unlikely(!lib.running)) {
		if (unlikely((ret = start_glc())))
			goto err;
	}

	glc_state_time_add_diff(&mpriv.glc,
				glc_state_time(&mpriv.glc) - mpriv.stop_time);

	if (unlikely((ret = alsa_capture_start_all())))
		goto err;
	if (unlikely((ret = opengl_capture_start())))
		goto err;

	lib.flags |= LIB_CAPTURING;
	glc_log(&mpriv.glc, GLC_INFO, "main", "started capturing");

func_exit:
	pthread_mutex_unlock(&mpriv.capture_action_lock);
	return ret;
err:
	pthread_mutex_unlock(&mpriv.capture_action_lock);
err_nolock:
	glc_log(&mpriv.glc, GLC_ERROR, "main",
		"can't start capturing: %s (%d)", strerror(ret), ret);
	return ret;
}

int stop_capture()
{
	int ret;

	if (unlikely(ret = pthread_mutex_lock(&mpriv.capture_action_lock)))
		goto err_nolock;

	if (unlikely(!(lib.flags & LIB_CAPTURING))) {
		ret = EAGAIN;
		goto func_exit;
	}

	if (unlikely((ret = alsa_capture_stop_all())))
		goto err;
	if (unlikely((ret = opengl_capture_stop())))
		goto err;

	if (!mpriv.sink->ops->can_resume(mpriv.sink))
		stop_stream();

	lib.flags &= ~LIB_CAPTURING;
	mpriv.stop_time = glc_state_time(&mpriv.glc);
	glc_log(&mpriv.glc, GLC_INFO, "main", "stopped capturing");
func_exit:
	pthread_mutex_unlock(&mpriv.capture_action_lock);
	return ret;
err:
	pthread_mutex_unlock(&mpriv.capture_action_lock);
err_nolock:
	glc_log(&mpriv.glc, GLC_ERROR, "main",
		"can't stop capturing: %s (%d)", strerror(ret), ret);
	return ret;
}

int start_glc()
{
	int ret;

	if (lib.running)
		return EINVAL;

	glc_log(&mpriv.glc, GLC_INFO, "main", "starting glc");

	glc_compute_threads_hint(&mpriv.glc);

	/* initialize sink & write stream info */
	if (mpriv.pipe_exec_file) {
		if (unlikely((ret = pipe_sink_init(&mpriv.sink, &mpriv.glc,
						mpriv.pipe_exec_file,
						mpriv.flags & MAIN_PIPE_VFLIP,
						&stop_capture))))
			return ret;
	} else {
		if (unlikely((ret = file_sink_init(&mpriv.sink, &mpriv.glc))))
			return ret;
	}
	if (unlikely((ret = mpriv.sink->ops->set_callback(mpriv.sink,
							&stream_sink_callback))))
		return ret;
	if (unlikely((ret = open_stream())))
		return ret;

	if (!(mpriv.flags & MAIN_COMPRESS_NONE)) {
		if (unlikely((ret = mpriv.sink->ops->write_process_start(mpriv.sink,
								mpriv.compressed))))
			return ret;

		if (unlikely((ret = pack_init(&mpriv.pack, &mpriv.glc))))
			return ret;

		if (mpriv.flags & MAIN_COMPRESS_QUICKLZ)
			pack_set_compression(mpriv.pack, PACK_QUICKLZ);
		else if (mpriv.flags & MAIN_COMPRESS_LZO)
			pack_set_compression(mpriv.pack, PACK_LZO);
		else if (mpriv.flags & MAIN_COMPRESS_LZJB)
			pack_set_compression(mpriv.pack, PACK_LZJB);

		if (unlikely((ret = pack_process_start(mpriv.pack, mpriv.uncompressed,
						       mpriv.compressed))))
			return ret;
	} else {
		glc_log(&mpriv.glc, GLC_WARN, "main", "compression disabled");
		if (unlikely((ret = mpriv.sink->ops->write_process_start(mpriv.sink,
								mpriv.uncompressed))))
			return ret;
	}

	if (unlikely((ret = alsa_start(mpriv.uncompressed))))
		return ret;
	if (unlikely((ret = opengl_start(mpriv.uncompressed))))
		return ret;

	lib.running = 1;
	glc_log(&mpriv.glc, GLC_INFO, "main", "glc running");

	return 0;
}

void lib_close()
{
	int ret;
	ps_stats_t stats;
	/*
	 There is a small possibility that a capture operation in another
	 thread is still active. This should be called only in exit() or
	 at return from main loop so we choose performance and not safety.

	 Adding a rwlock for all capture operations might inflict a noticeable
	 cost, at least in complexity.

	 Note that this comment, only apply to the host process threads
	 calling opengl functions. All glc created threads are all
	 properly disposed.
	*/

	glc_log(&mpriv.glc, GLC_INFO, "main", "closing glc");

	if (unlikely((ret = alsa_close())))
		goto err;
	if (unlikely((ret = opengl_close())))
		goto err;

	if (lib.running) {
	/*
	 opengl_close() is inserting an eof message in the stream.
	 as the downstream threads process that message, they will all
	 exit.
	 */
		if (!(mpriv.flags & MAIN_COMPRESS_NONE)) {
			pack_process_wait(mpriv.pack);
			pack_destroy(mpriv.pack);
		}
		mpriv.sink->ops->write_process_wait(mpriv.sink);
		close_stream();
		mpriv.sink->ops->destroy(mpriv.sink);
		mpriv.sink = NULL;
	}

	if (mpriv.compressed) {
		if(!ps_buffer_stats(mpriv.compressed, &stats)) {
			glc_log(&mpriv.glc, GLC_PERF, "main", "compressed buffer stats:");
			ps_stats_text(&stats, glc_log_get_stream(&mpriv.glc));
		}
		ps_buffer_destroy(mpriv.compressed);
		free(mpriv.compressed);
	}

	if(!ps_buffer_stats(mpriv.uncompressed, &stats)) {
		glc_log(&mpriv.glc, GLC_PERF, "main", "uncompressed buffer stats:");
		ps_stats_text(&stats, glc_log_get_stream(&mpriv.glc));
	}
	ps_buffer_destroy(mpriv.uncompressed);
	free(mpriv.uncompressed);

	if (mpriv.flags & MAIN_CUSTOM_LOG)
		glc_log_close(&mpriv.glc);

	glc_state_destroy(&mpriv.glc);
	glc_destroy(&mpriv.glc);

	free(mpriv.stream_file);
	return;
err:
	fprintf(stderr, "(glc) cleanup: %s (%d)\n", strerror(ret), ret);
	return;
}

void get_real_dlsym()
{
	eh_obj_t libdl;

	if (unlikely(eh_find_obj(&libdl, "*libdl.so*"))) {
		fprintf(stderr, "(glc) libdl.so is not present in memory\n");
		exit(1);
	}

	if (unlikely(eh_find_sym(&libdl, "dlopen", (void *) &lib.dlopen))) {
		fprintf(stderr, "(glc) can't get real dlopen()\n");
		exit(1);
	}

	if (unlikely(eh_find_sym(&libdl, "dlsym", (void *) &lib.dlsym))) {
		fprintf(stderr, "(glc) can't get real dlsym()\n");
		exit(1);
	}

	if (unlikely(eh_find_sym(&libdl, "dlvsym", (void *) &lib.dlvsym))) {
		fprintf(stderr, "(glc) can't get real dlvsym()\n");
		exit(1);
	}

	eh_destroy_obj(&libdl);
}

void get_real___libc_dlsym()
{
	eh_obj_t libc;

	if (unlikely(eh_find_obj(&libc, "*libc.so*"))) {
		fprintf(stderr, "(glc) libc.so is not present in memory\n");
		exit(1);
	}

	if (unlikely(eh_find_sym(&libc, "__libc_dlsym", (void *) &lib.__libc_dlsym))) {
		fprintf(stderr, "(glc) can't get real __libc_dlsym()\n");
		exit(1);
	}

	eh_destroy_obj(&libc);
}

void *wrapped_func(const char *symbol)
{
	if (!strcmp(symbol, "glXGetProcAddressARB"))
		return &__opengl_glXGetProcAddressARB;
	else if (!strcmp(symbol, "glXSwapBuffers"))
		return &__opengl_glXSwapBuffers;
	else if (!strcmp(symbol, "glFinish"))
		return &__opengl_glFinish;
	else if (!strcmp(symbol, "glXCreateWindow"))
		return &__opengl_glXCreateWindow;
	else if (!strcmp(symbol, "snd_pcm_open"))
		return &__alsa_snd_pcm_open;
	else if (!strcmp(symbol, "snd_pcm_close"))
		return &__alsa_snd_pcm_close;
	else if (!strcmp(symbol, "snd_pcm_open_lconf"))
		return &__alsa_snd_pcm_open_lconf;
	else if (!strcmp(symbol, "snd_pcm_hw_params"))
		return &__alsa_snd_pcm_hw_params;
	else if (!strcmp(symbol, "snd_pcm_writei"))
		return &__alsa_snd_pcm_writei;
	else if (!strcmp(symbol, "snd_pcm_writen"))
		return &__alsa_snd_pcm_writen;
	else if (!strcmp(symbol, "snd_pcm_mmap_writei"))
		return &__alsa_snd_pcm_mmap_writei;
	else if (!strcmp(symbol, "snd_pcm_mmap_writen"))
		return &__alsa_snd_pcm_mmap_writen;
	else if (!strcmp(symbol, "snd_pcm_mmap_begin"))
		return &__alsa_snd_pcm_mmap_begin;
	else if (!strcmp(symbol, "snd_pcm_mmap_commit"))
		return &__alsa_snd_pcm_mmap_commit;
	else if (!strcmp(symbol, "XNextEvent"))
		return &__x11_XNextEvent;
	else if (!strcmp(symbol, "XPeekEvent"))
		return &__x11_XPeekEvent;
	else if (!strcmp(symbol, "XWindowEvent"))
		return &__x11_XWindowEvent;
	else if (!strcmp(symbol, "XMaskEvent"))
		return &__x11_XMaskEvent;
	else if (!strcmp(symbol, "XCheckWindowEvent"))
		return &__x11_XCheckWindowEvent;
	else if (!strcmp(symbol, "XCheckMaskEvent"))
		return &__x11_XCheckMaskEvent;
	else if (!strcmp(symbol, "XCheckTypedEvent"))
		return &__x11_XCheckTypedEvent;
	else if (!strcmp(symbol, "XCheckTypedWindowEvent"))
		return &__x11_XCheckTypedWindowEvent;
	else if (!strcmp(symbol, "XIfEvent"))
		return &__x11_XIfEvent;
	else if (!strcmp(symbol, "XCheckIfEvent"))
		return &__x11_XCheckIfEvent;
	else if (!strcmp(symbol, "XPeekIfEvent"))
		return &__x11_XPeekIfEvent;
	else if (!strcmp(symbol, "XF86VidModeSetGamma"))
		return &__x11_XF86VidModeSetGamma;
	else if (!strcmp(symbol, "dlopen"))
		return &__main_dlopen;
	else if (!strcmp(symbol, "dlsym"))
		return &__main_dlsym;
	else if (!strcmp(symbol, "dlvsym"))
		return &__main_dlvsym;
	else if (!strcmp(symbol, "__libc_dlsym"))
		return &__main___libc_dlsym;
	else
		return NULL;
}

__PUBLIC void *dlopen(const char *filename, int flag)
{
	return __main_dlopen(filename, flag);
}

void *__main_dlopen(const char *filename, int flag)
{
	if (lib.dlopen == NULL)
		get_real_dlsym();

	void *ret = lib.dlopen(filename, flag);

	if ((ret != NULL) && (filename != NULL)) {
		if ((!fnmatch("*libasound.so*", filename, 0)) ||
		    (!fnmatch("*libasound_module_*.so*", filename, 0)))
			alsa_unhook_so(filename); /* no audio stream duplication, thanks */
	}

	return ret;
}

__PUBLIC void *dlsym(void *handle, const char *symbol)
{
	return __main_dlsym(handle, symbol);
}

void *__main_dlsym(void *handle, const char *symbol)
{
	if (lib.dlsym == NULL)
		get_real_dlsym();

	void *ret = wrapped_func(symbol);
	if (ret)
		return ret;

	return lib.dlsym(handle, symbol);
}

__PUBLIC void *dlvsym(void *handle, const char *symbol, const char *version)
{
	return __main_dlvsym(handle, symbol, version);
}

void *__main_dlvsym(void *handle, const char *symbol, const char *version)
{
	if (lib.dlvsym == NULL)
		get_real_dlsym();

	void *ret = wrapped_func(symbol); /* should we too check for version? */
	if (ret)
		return ret;

	return lib.dlvsym(handle, symbol, version);
}

__PUBLIC void *__libc_dlsym(void *handle, const char *symbol)
{
	return __main___libc_dlsym(handle, symbol);
}

void *__main___libc_dlsym(void *handle, const char *symbol)
{
	if (lib.__libc_dlsym == NULL)
		get_real___libc_dlsym();

	void *ret = wrapped_func(symbol);
	if (ret)
		return ret;

	return lib.__libc_dlsym(handle, symbol);
}

/**  \} */
/**  \} */
