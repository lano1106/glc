/**
 * \file glc/core/pipe.c
 * \brief Pipe implementation of the sink interface.
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

#include <stdlib.h> // for calloc()
#include <string.h> // for strerror()
#include <unistd.h> // for pipe() and fork()
#include <errno.h>
#include <signal.h>
#include <time.h>
#include <sys/wait.h>
#include <sys/epoll.h>

#include <glc/common/state.h>
#include <glc/common/log.h>
#include <glc/common/thread.h>
#include <glc/common/util.h>
#include <glc/common/signal.h>

#include <glc/core/tracker.h>

#include "pipe.h"
#include "frame_writers.h"
#include "optimization.h"

#define PIPE_WRITING      0x01
#define PIPE_RUNNING      0x02
#define PIPE_INFO_WRITTEN 0x04

struct pipe_stream_params_s
{
	const char *exec_file;
	const char *target_file;
	const char *host_app_name;
	double fps;
};

struct pipe_runtime_s
{
	int w_pipefd;
	int pipe_ready;
	int epollfd;
	frame_writer_t writer;
	pid_t consumer_proc;
	glc_flags_t flags;
	glc_stream_id_t id;
	struct timespec wait_time;
	int write_frame_ret;
};

typedef struct {
	struct sink_s sink_base;
	glc_t *glc;
	glc_thread_t thread;
	tracker_t state_tracker;
	struct pipe_runtime_s       runtime;
	struct pipe_stream_params_s params;
	callback_request_func_t     callback;
	int (*stop_capture_cb)();
	ps_buffer_t *from;
} pipe_sink_t;

static void pipe_finish_callback(void *ptr, int err);
static int pipe_close_callback(glc_thread_state_t *state);
static int pipe_read_callback(glc_thread_state_t *state);
static int pipe_create_callback(void *ptr, void **threadptr);

static int pipe_can_resume(sink_t sink);
static int pipe_set_sync(sink_t sink, int sync);
static int pipe_set_callback(sink_t sink, callback_request_func_t callback);
static int pipe_open_target(sink_t sink, const char *filename);
static int pipe_close_target(sink_t sink);
static int pipe_write_info(sink_t sink, glc_stream_info_t *info,
			const char *info_name, const char *info_date);
static int pipe_write_eof(sink_t sink);
static int pipe_write_state(sink_t sink);
static int pipe_write_process_start(sink_t sink, ps_buffer_t *from);
static int pipe_write_process_wait(sink_t sink);
static int pipe_sink_destroy(sink_t sink);
static void close_pipe(glc_t *glc, struct pipe_runtime_s *rt);

static sink_ops_t pipe_sink_ops = {
	.can_resume          = pipe_can_resume,
	.set_sync            = pipe_set_sync,
	.set_callback        = pipe_set_callback,
	.open_target         = pipe_open_target,
	.close_target        = pipe_close_target,
	.write_info          = pipe_write_info,
	.write_eof           = pipe_write_eof,
	.write_state         = pipe_write_state,
	.write_process_start = pipe_write_process_start,
	.write_process_wait  = pipe_write_process_wait,
	.destroy             = pipe_sink_destroy,
};

int pipe_sink_init(sink_t *sink, glc_t *glc, const char *exec_file, int invert,
		   int (*stop_capture_cb)())
{
	int ret;
	pipe_sink_t *pipe_sink = (pipe_sink_t*)calloc(1,sizeof(pipe_sink_t));
	*sink = (sink_t)pipe_sink;
	if (unlikely(!pipe_sink))
		return ENOMEM;
	pipe_sink->runtime.epollfd = epoll_create(1);
	if (unlikely(pipe_sink->runtime.epollfd < 0)) {
		*sink = NULL;
		free(pipe_sink);
		return errno;
	}

	if (invert)
		ret = glcs_invert_create(&pipe_sink->runtime.writer);
	else
		ret = glcs_std_create(&pipe_sink->runtime.writer);
	if (unlikely(ret)) {
		close(pipe_sink->runtime.epollfd);
		*sink = NULL;
		free(pipe_sink);
		return ret;
	}

	pipe_sink->sink_base.ops   = &pipe_sink_ops;
	pipe_sink->glc             = glc;
	pipe_sink->stop_capture_cb = stop_capture_cb;
	pipe_sink->thread.flags    = GLC_THREAD_READ;
	pipe_sink->thread.ptr      = pipe_sink;
	pipe_sink->thread.thread_create_callback = &pipe_create_callback;
	pipe_sink->thread.read_callback   = &pipe_read_callback;
	pipe_sink->thread.close_callback  = &pipe_close_callback;
	pipe_sink->thread.finish_callback = &pipe_finish_callback;
	pipe_sink->thread.threads = 1;

	tracker_init(&pipe_sink->state_tracker, pipe_sink->glc);

	pipe_sink->params.exec_file = exec_file;
	pipe_sink->params.fps       = 0.0;
	pipe_sink->runtime.w_pipefd = -1;

	return 0;
}

int pipe_sink_destroy(sink_t sink)
{
	pipe_sink_t *pipe_sink = (pipe_sink_t*)sink;
	tracker_destroy(pipe_sink->state_tracker);
	free((char *)pipe_sink->params.host_app_name);
	pipe_sink->runtime.writer->ops->destroy(pipe_sink->runtime.writer);
	close(pipe_sink->runtime.epollfd);
	free(pipe_sink);
	return 0;
}

int pipe_can_resume(sink_t sink)
{
	pipe_sink_t *pipe_sink = (pipe_sink_t*)sink;
	if (pipe_sink->from)
		ps_buffer_drain(pipe_sink->from);
	return 0;
}

/*
 * Since we are toying with pipes which have the potential
 * to abruptly terminate the process by delivering a SIGPIPE
 * signal, if the host has not configured the signal disposition,
 * we can specify to ignore it.
 */
int pipe_create_callback(void *ptr, void **threadptr)
{
	pipe_sink_t *pipe_sink = (pipe_sink_t *)ptr;
	struct sigaction oact;

	if (unlikely(glcs_signal_init_thread_disposition(pipe_sink->glc)))
		return -1;

	if (unlikely(sigaction(SIGPIPE, NULL, &oact) < 0)) {
		glc_log(pipe_sink->glc, GLC_WARN, "pipe",
			"failed to query SIGPIPE disposition: %s (%d)",
			strerror(errno), errno);
		goto func_exit;
	}
	if (oact.sa_handler == SIG_DFL && oact.sa_sigaction == (void *)SIG_DFL) {
		struct sigaction act;
		act.sa_handler = SIG_IGN;
		sigemptyset(&act.sa_mask);
		act.sa_flags = 0;
		if (unlikely(sigaction(SIGPIPE, &act, NULL) < 0))
			glc_log(pipe_sink->glc, GLC_WARN, "pipe",
				"failed to set SIGPIPE disposition: %s (%d)",
				strerror(errno), errno);
		else
			glc_log(pipe_sink->glc, GLC_INFO, "pipe",
				"successfully requested to ignore SIGPIPE");
	} else if (oact.sa_handler == SIG_IGN && oact.sa_sigaction == (void *)SIG_IGN)
		glc_log(pipe_sink->glc, GLC_INFO, "pipe",
			"'%s' host app already ignore SIGPIPE",
			pipe_sink->params.host_app_name);
	else
		glc_log(pipe_sink->glc, GLC_WARN, "pipe",
			"'%s' host app is handling SIGPIPE. There is a risk of interfering with it",
			pipe_sink->params.host_app_name);
func_exit:
	return 0;
}

void pipe_finish_callback(void *ptr, int err)
{
	pipe_sink_t *pipe_sink = (pipe_sink_t*)ptr;

	close_pipe(pipe_sink->glc, &pipe_sink->runtime);
	if (unlikely(err))
		glc_log(pipe_sink->glc, GLC_ERROR, "pipe", "%s (%d)",
			strerror(err), err);
}

typedef struct {
	glc_video_format_message_t *format;
	glc_stream_id_t id;
} callback_param_t;

static int find_state_callback(glc_message_header_t *header, void *message,
			size_t message_size, void *arg)
{
	callback_param_t *param = (callback_param_t*)arg;
	glc_video_format_message_t *format;
	if (header->type == GLC_MESSAGE_VIDEO_FORMAT) {
		format = (glc_video_format_message_t *)message;
		if (format->id == param->id) {
			param->format = format;
			return 1;
		}
	}
	return 0;
}

static glc_video_format_message_t *get_video_format(pipe_sink_t *pipe_sink, glc_stream_id_t id)
{
	callback_param_t param;
	param.format = NULL;
	param.id = id;

	tracker_iterate_state(pipe_sink->state_tracker, find_state_callback, &param);

	if (unlikely(!param.format))
		glc_log(pipe_sink->glc, GLC_ERROR, "pipe",
			"format not found for stream %d",
			id);

	return param.format;
}

/*
 * open_pipe() and close_pipe() are called from the pipe_sink thread.
 * There is no special reason for doing so right now but I consider that way
 * better design as handling the pipe can incur some blocking and it could interfere
 * with the host app to block in one of its threads. Also it keeps the door open to handling
 * pipe and/or child process signals as by default, every glcs threads are blocking
 * most signals (see common/thread.c). To change that we could unblock some signals in
 * pipe_create_callback().
 */
static int open_pipe(pipe_sink_t *pipe_sink, glc_video_format_message_t *format)
{
	int ret = 0;
	int stream_pipe[2];
	pid_t pid;
	sigset_t set, oset;
	struct sigaction oact;
	struct epoll_event event;
	int frame_size, r;
	int bpp = glc_util_get_videofmt_bpp(format->format);

	if (unlikely(bpp<=0)) {
		glc_log(pipe_sink->glc, GLC_ERROR, "pipe", "unsupported pixel format: %s",
			glc_util_videofmt_to_str(format->format));
		return EINVAL;
	}

	r = format->width*bpp;
	if (unlikely(pipe_sink->runtime.writer->ops->configure(pipe_sink->runtime.writer,
		r, format->height))) {
		glc_log(pipe_sink->glc, GLC_ERROR, "pipe", "frame writer init failed");
		return EINVAL;
	}

	if (format->flags & GLC_VIDEO_DWORD_ALIGNED && r%8 != 0) {
		glc_log(pipe_sink->glc, GLC_ERROR, "pipe",
			"video width not perfectly aligned. "
			"Might not be ideal for every output processes. "
			"Recommend change video width to be a multiple of 8.");
		return EINVAL;
	}

	if (unlikely((ret = pipe(stream_pipe)) < 0)) {
		glc_log(pipe_sink->glc, GLC_ERROR, "pipe", "error creating pipe: %s (%d)",
			strerror(errno), errno);
		return errno;
	}

	/*
	 * Set pipe non blocking to detect if writing a frame take longer than
	 * 1 sec/fps.
	 */
	glc_util_set_nonblocking(stream_pipe[1]);
	event.data.fd = stream_pipe[1];
	event.events  = EPOLLOUT|EPOLLET;
	ret = epoll_ctl(pipe_sink->runtime.epollfd, EPOLL_CTL_ADD,
			stream_pipe[1], &event);
	if (unlikely(ret)) {
		ret = errno;
		glc_log(pipe_sink->glc, GLC_ERROR, "pipe",
			"epoll_ctl() failed to add the pipe fd into the set: %s (%d)",
			strerror(errno), errno);
		goto err;
	}

	frame_size = r * format->height;
	glc_util_set_pipe_size(pipe_sink->glc,stream_pipe[1], 15*frame_size);

	/*
	 * Check SIGCHLD disposition and issue warning if there is a risk to interfere
	 * with the host application.
	 */
	if (unlikely(sigaction(SIGCHLD, NULL, &oact) < 0)) {
		ret = errno;
		glc_log(pipe_sink->glc, GLC_ERROR, "pipe",
			"sigaction() error: %s (%d)", strerror(errno), errno);
		goto err;
	}

	if ((oact.sa_handler != SIG_DFL && oact.sa_sigaction != (void*)SIG_DFL) &&
	    (oact.sa_handler != SIG_IGN && oact.sa_sigaction != (void*)SIG_IGN))
		glc_log(pipe_sink->glc, GLC_WARN, "pipe",
			"'%s' host app is handling SIGCHLD. "
			"Using pipe sink represent a small risk to interfere with it",
			pipe_sink->params.host_app_name);

	/*
	 * fork exec new process
	 */
	sigfillset(&set);
	pthread_sigmask(SIG_SETMASK, &set, &oset);
	pid = fork();
	if (pid < 0) {
		ret = errno;
		glc_log(pipe_sink->glc, GLC_ERROR, "pipe",
			"fork() call failed: %s (%d)",
			strerror(errno), errno);
		goto err;
	} else if (!pid) { /* child */
		char video_size[16];
		char framerate[16];

		/* prepare argv */
		if (unlikely(snprintf(video_size, sizeof(video_size), "%dx%d",
			format->width, format->height) >= sizeof(video_size)))
			_exit(126);

		if (unlikely(snprintf(framerate, sizeof(framerate), "%f",
			pipe_sink->params.fps) >= sizeof(framerate)))
			_exit(125);

		dup2(stream_pipe[0], STDIN_FILENO);
		close(stream_pipe[0]);
		close(stream_pipe[1]);

		/* close all other fds */
		glc_util_close_fds(3);

		/* reset every signal dispositions to their default */
		glcs_signal_reset();

		sigemptyset(&set);
		sigprocmask(SIG_SETMASK, &set, NULL);

		/* exec */
		execl(pipe_sink->params.exec_file,
			basename(pipe_sink->params.exec_file),
			video_size,
			glc_util_videofmt_to_str(format->format),
			framerate,
			pipe_sink->params.target_file,
			(char *)NULL);
		_exit(127); /* exec failed */
	}
	/* else parent */
	pipe_sink->runtime.w_pipefd      = stream_pipe[1];
	pipe_sink->runtime.pipe_ready    = 1;
	pipe_sink->runtime.consumer_proc = pid;
	close(stream_pipe[0]);
	glc_log(pipe_sink->glc, GLC_INFO, "pipe",
		"'%s' (%d) has been started", pipe_sink->params.exec_file, pid);
	pthread_sigmask(SIG_SETMASK, &oset, NULL);
	return ret;
err:
	close(stream_pipe[0]);
	close(stream_pipe[1]);
	return ret;
}

static int wait_pipe(pipe_sink_t *pipe_sink, int timeout_ms)
{
	int ret;
	struct epoll_event event;
	glc_log(pipe_sink->glc, GLC_DEBUG, "pipe", "wait for pipe");
	do {
		ret = epoll_wait(pipe_sink->runtime.epollfd, &event, 1, timeout_ms);
	} while (unlikely(ret < 0 && errno == EINTR));
	if (unlikely(!ret)) {
		glc_log(pipe_sink->glc, GLC_ERROR, "pipe",
			"epoll to after %d ms. Child process too slow", timeout_ms);
		ret = ETIMEDOUT;
	} else if (unlikely(ret < 0)) {
		glc_log(pipe_sink->glc, GLC_ERROR, "pipe",
			"epoll error: %s (%d)", strerror(errno), errno);
	} else {
		if (unlikely(event.events & EPOLLERR)) {
			glc_log(pipe_sink->glc, GLC_ERROR, "pipe",
			"epoll detected an error on pipe fd");
			ret = -1;
		} else if (unlikely(event.events & EPOLLHUP)) {
			glc_log(pipe_sink->glc, GLC_ERROR, "pipe",
			"pipe fd hang up");
			ret = -1;
		} else {
			glc_log(pipe_sink->glc, GLC_DEBUG, "pipe",
				"pipe ready");
			pipe_sink->runtime.pipe_ready = 1;
			ret = 0;
		}
	}
	return ret;
}

static int write_video_frame(pipe_sink_t *pipe_sink,
			char *frame_data)
{
	int ret;
	int timeout_ms = pipe_sink->runtime.wait_time.tv_sec*1000 +
			 pipe_sink->runtime.wait_time.tv_nsec/1000000L;
	pipe_sink->runtime.writer->ops->write_init(pipe_sink->runtime.writer, frame_data);
	do {
		if (unlikely(!pipe_sink->runtime.pipe_ready)) {
			if(unlikely((ret = wait_pipe(pipe_sink,timeout_ms))))
				return ret;
		}
		ret = pipe_sink->runtime.writer->ops->write(pipe_sink->runtime.writer,
							pipe_sink->runtime.w_pipefd);
		if (unlikely(ret < 0)) {
			if (unlikely(errno == EAGAIN)) {
				pipe_sink->runtime.pipe_ready = 0;
			}
			else if (unlikely(errno != EINTR)) {
				ret = errno;
				glc_log(pipe_sink->glc, GLC_ERROR, "pipe",
					"writing frame to pipe failed: %s (%d)",
					strerror(errno), errno);
				return ret;
			}
		} else if (unlikely(ret > 0))
				pipe_sink->runtime.pipe_ready = 0;
	} while(ret);
	return 0;
}

int pipe_close_callback(glc_thread_state_t *state)
{
	pipe_sink_t *pipe_sink = (pipe_sink_t*) state->ptr;
	if (unlikely(pipe_sink->runtime.write_frame_ret)) {
		pipe_sink->runtime.write_frame_ret = 0;
		pipe_sink->from = state->from;
		pipe_sink->stop_capture_cb();
		pipe_sink->from = NULL;
	}
	return 0;
}

int pipe_read_callback(glc_thread_state_t *state)
{
	pipe_sink_t *pipe_sink = (pipe_sink_t*) state->ptr;
	glc_callback_request_t *callback_req;
	int ret = 0;

	switch(state->header.type)
	{
		case GLC_CALLBACK_REQUEST:
			/* callbacks may manipulate target so remove PIPE_RUNING flag */
			pipe_sink->runtime.flags &= ~PIPE_RUNNING;
			callback_req = (glc_callback_request_t*) state->read_data;
			pipe_sink->callback(callback_req->arg);
			pipe_sink->runtime.flags |= PIPE_RUNNING;
			break;
		case GLC_MESSAGE_VIDEO_FORMAT:
		case GLC_MESSAGE_COLOR:
			tracker_submit(pipe_sink->state_tracker, &state->header,
				state->read_data, state->read_size);
			break;
		case GLC_MESSAGE_VIDEO_FRAME:
		{
			glc_video_frame_header_t *pic_hdr =
				(glc_video_frame_header_t *)state->read_data;

			if (likely(pipe_sink->runtime.w_pipefd < 0)) {
				glc_video_format_message_t *format;
				if (unlikely(!(format = get_video_format(pipe_sink,pic_hdr->id)))) {
					return 1;
				}

				// open pipe for this stream
				if (unlikely((ret = open_pipe(pipe_sink, format))))
					return ret;

				// if successful, record the stream id played
				pipe_sink->runtime.id = pic_hdr->id;
			} else {
				if (unlikely(pic_hdr->id != pipe_sink->runtime.id))
					return 0;
			}
			pipe_sink->runtime.write_frame_ret = write_video_frame(pipe_sink,
					&state->read_data[sizeof(glc_video_frame_header_t)]
				);
			break;
		}
		case GLC_MESSAGE_CLOSE: // noop
			break;
		default:
			glc_log(pipe_sink->glc, GLC_WARN, "pipe", "unexpected packet type %s (%u)",
				glc_util_msgtype_to_str(state->header.type),
				(unsigned)state->header.type);
			break;
	}
	return ret;
}

int pipe_set_sync(sink_t sink, int sync)
{
	pipe_sink_t *pipe_sink = (pipe_sink_t*)sink;
	glc_log(pipe_sink->glc, GLC_DEBUG, "pipe", "pipe_set_sync");
	return 0;
}

int pipe_set_callback(sink_t sink, callback_request_func_t callback)
{
	pipe_sink_t *pipe_sink = (pipe_sink_t*)sink;
	glc_log(pipe_sink->glc, GLC_DEBUG, "pipe", "pipe_set_callback");
	pipe_sink->callback = callback;
	return 0;
}

int pipe_open_target(sink_t sink, const char *filename)
{
	pipe_sink_t *pipe_sink = (pipe_sink_t*)sink;
	pipe_sink->params.target_file = filename;
	pipe_sink->runtime.flags     |= PIPE_WRITING;
	return 0;
}

/*
 * close_pipe()
 * Close the pipe fd and collect child process status to avoid it to become
 * a zombie.
 */
void close_pipe(glc_t *glc, struct pipe_runtime_s *rt)
{
	if (rt->w_pipefd >= 0) {
		int ret, status, i;
		struct timespec kill_wait_time;

		/* closing the pipe should terminate the child */
		epoll_ctl(rt->epollfd, EPOLL_CTL_DEL, rt->w_pipefd, NULL);
		close(rt->w_pipefd);
		rt->w_pipefd = -1;

		ret = glcs_signal_timed_waitpid(glc, rt->consumer_proc, &status, &rt->wait_time);
		if (!ret || errno == ECHILD)
			goto child_gone;

		kill_wait_time = rt->wait_time;
		/*
		 * very important to be patient here because by sending SIGKILL
		 * there is a risk that some system resources do not get properly released
		 * forcing a reboot to clean up this unfortunate state.
		 */
		kill_wait_time.tv_sec += 2;

		/* ask one more time nicely */
		for (i = 0; i < 3; ++i) {
			glc_log(glc, GLC_DEBUG, "pipe", "sending SIGINT to child pid %d",
				rt->consumer_proc);
			kill(rt->consumer_proc, SIGINT);
			ret = glcs_signal_timed_waitpid(glc, rt->consumer_proc, &status, &kill_wait_time);
			if (!ret || errno == ECHILD)
				goto child_gone;
		}

		glc_log(glc, GLC_DEBUG, "pipe", "sending SIGKILL to child pid %d",
			rt->consumer_proc);
		kill(rt->consumer_proc, SIGKILL);
		waitpid(rt->consumer_proc, &status, 0);
child_gone:
		if (glc_log_get_level(glc) >= GLC_INFO)
			glcs_signal_pr_exit(glc, rt->consumer_proc, status);
		rt->consumer_proc = 0;
	}
}

/*
 * Not really needed but enfore how the sink API is used to ensure that
 * user code will work with stricter sinks (ie.: file)
 */
static inline int is_write_open_not_running(struct pipe_runtime_s *rt)
{
	return (rt->flags & PIPE_WRITING) &&
		!(rt->flags & PIPE_RUNNING);
}

int pipe_close_target(sink_t sink)
{
	pipe_sink_t *pipe_sink = (pipe_sink_t*)sink;
	if (unlikely(!is_write_open_not_running(&pipe_sink->runtime)))
		return EAGAIN;
	pipe_sink->params.target_file = NULL;
	pipe_sink->runtime.flags     &= ~(PIPE_WRITING | PIPE_INFO_WRITTEN);
	return 0;
}

/*
 * provide the fps.
 */
int pipe_write_info(sink_t sink, glc_stream_info_t *info,
			const char *info_name, const char *info_date)
{
	pipe_sink_t *pipe_sink = (pipe_sink_t*)sink;
	if (unlikely(!is_write_open_not_running(&pipe_sink->runtime)))
		return EAGAIN;
	glc_log(pipe_sink->glc, GLC_INFO, "pipe", "%s (%u) capture on %s at %f fps",
		info_name, info->pid, info_date, info->fps);
	pipe_sink->params.fps = info->fps;
	pipe_sink->runtime.wait_time.tv_nsec  = 5*(1000000000L/info->fps);
	pipe_sink->runtime.wait_time.tv_sec   = pipe_sink->runtime.wait_time.tv_nsec/1000000000L;
	pipe_sink->runtime.wait_time.tv_nsec %= 1000000000L;
	if (unlikely(!pipe_sink->params.host_app_name))
		pipe_sink->params.host_app_name = strdup(info_name);
	pipe_sink->runtime.flags |= PIPE_INFO_WRITTEN;
	return 0;
}

int pipe_write_eof(sink_t sink)
{
	pipe_sink_t *pipe_sink = (pipe_sink_t*)sink;
	if (unlikely(!is_write_open_not_running(&pipe_sink->runtime)))
		return EAGAIN;
	close_pipe(pipe_sink->glc, &pipe_sink->runtime);
	return 0;
}

int pipe_write_state(sink_t sink)
{
	pipe_sink_t *pipe_sink = (pipe_sink_t*)sink;
	if (unlikely(!is_write_open_not_running(&pipe_sink->runtime)))
		return EAGAIN;
	glc_log(pipe_sink->glc, GLC_DEBUG, "pipe", "pipe_write_state");
	return 0;
}

int pipe_write_process_start(sink_t sink, ps_buffer_t *from)
{
	int ret;
	pipe_sink_t *pipe_sink = (pipe_sink_t*)sink;
	if (unlikely(!is_write_open_not_running(&pipe_sink->runtime) ||
			!(pipe_sink->runtime.flags & PIPE_INFO_WRITTEN)))
		return EAGAIN;

	if (unlikely((ret = glc_thread_create(pipe_sink->glc, &pipe_sink->thread,
					from, NULL))))
		return ret;

	pipe_sink->runtime.flags |= PIPE_RUNNING;

	return 0;
}

int pipe_write_process_wait(sink_t sink)
{
	pipe_sink_t *pipe_sink = (pipe_sink_t*)sink;
	if (unlikely(!(pipe_sink->runtime.flags & PIPE_RUNNING) ||
		!(pipe_sink->runtime.flags & PIPE_WRITING) ||
		!(pipe_sink->runtime.flags & PIPE_INFO_WRITTEN)))
		return EAGAIN;

	glc_thread_wait(&pipe_sink->thread);
	pipe_sink->runtime.flags &= ~PIPE_RUNNING;
	return 0;
}

