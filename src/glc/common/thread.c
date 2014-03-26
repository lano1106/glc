/**
 * \file glc/common/thread.c
 * \brief generic stream processor thread adapted from original work (glc) from Pyry Haulos <pyry.haulos@gmail.com>
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
 * \addtogroup thread
 *  \{
 */

#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <signal.h>
#include <unistd.h>
#include <packetstream.h>
#include <errno.h>

#include "glc.h"
#include "core.h"
#include "thread.h"
#include "util.h"
#include "log.h"
#include "state.h"
#include "optimization.h"

/**
 * \brief thread private variables
 */
struct glc_thread_private_s {
	glc_t *glc;
	ps_buffer_t *from;
	ps_buffer_t *to;

	pthread_t *pthread_thread;
	pthread_mutex_t open, finish;

	glc_thread_t *thread;
	size_t running_threads;

	int stop;
	int ret;
};

static void *glc_thread(void *argptr);
static int glc_thread_block_signals(void);
static int glc_thread_set_rt_priority(glc_t *glc, int ask_rt);

int glc_thread_create(glc_t *glc, glc_thread_t *thread, ps_buffer_t *from,
			ps_buffer_t *to)
{
	int ret;
	struct glc_thread_private_s *private;
	size_t t;

	if (unlikely(thread->threads < 1))
		return EINVAL;

	if (unlikely(!(private = (struct glc_thread_private_s *)
		calloc(1, sizeof(struct glc_thread_private_s)))))
		return ENOMEM;

	thread->priv    = private;
	private->glc    = glc;
	private->from   = from;
	private->to     = to;
	private->thread = thread;

	pthread_mutex_init(&private->open, NULL);
	pthread_mutex_init(&private->finish, NULL);

	private->pthread_thread = malloc(sizeof(pthread_t) * thread->threads);
	for (t = 0; t < thread->threads; t++) {
		private->running_threads++;
		if (unlikely((ret = pthread_create(&private->pthread_thread[t], NULL,
					  glc_thread, private)))) {
			glc_log(private->glc, GLC_ERROR, "glc_thread",
				 "can't create thread: %s (%d)", strerror(ret), ret);
			private->running_threads--;
			return ret;
		}
	}

	return 0;
}

int glc_thread_wait(glc_thread_t *thread)
{
	struct glc_thread_private_s *private = thread->priv;
	int ret;
	size_t t;

	for (t = 0; t < thread->threads; t++) {
		if (unlikely((ret = pthread_join(private->pthread_thread[t], NULL)))) {
			glc_log(private->glc, GLC_ERROR, "glc_thread",
				 "can't join thread: %s (%d)", strerror(ret), ret);
			return ret;
		}
	}

	free(private->pthread_thread);
	pthread_mutex_destroy(&private->finish);
	pthread_mutex_destroy(&private->open);
	free(private);
	thread->priv = NULL;

	return 0;
}

/**
 * \brief thread loop
 *
 * Actual reading, writing and calling callbacks is
 * done here.
 * \param argptr pointer to thread state structure
 * \return always NULL
 */
void *glc_thread(void *argptr)
{
	int has_locked, ret, write_size_set, packets_init;

	struct glc_thread_private_s *private = (struct glc_thread_private_s *) argptr;
	glc_thread_t *thread = private->thread;
	glc_thread_state_t state;

	ps_packet_t read, write;

	write_size_set = ret = has_locked = packets_init = 0;
	state.flags = state.read_size = state.write_size = 0;
	state.ptr   = thread->ptr;
	state.from  = private->from;

	glc_thread_block_signals();
	glc_thread_set_rt_priority(private->glc, thread->ask_rt);

	if (thread->flags & GLC_THREAD_READ) {
		if (unlikely((ret = ps_packet_init(&read, private->from))))
			goto err;
	}

	if (thread->flags & GLC_THREAD_WRITE) {
		if (unlikely((ps_packet_init(&write, private->to))))
			goto err;
	}

	/* safe to destroy packets etc. */
	packets_init = 1;

	/* create callback */
	if (thread->thread_create_callback) {
		if (unlikely((ret = thread->thread_create_callback(state.ptr, &state.threadptr))))
			goto err;
	}

	do {
		/* open callback */
		if (thread->open_callback) {
			if (unlikely((ret = thread->open_callback(&state))))
				goto err;
		}

		if ((thread->flags & GLC_THREAD_WRITE) && (thread->flags & GLC_THREAD_READ)) {
			pthread_mutex_lock(&private->open); /* preserve packet order */
			has_locked = 1;
		}

		if ((thread->flags & GLC_THREAD_READ) && (!(state.flags & GLC_THREAD_STATE_SKIP_READ))) {
			if (unlikely((ret = ps_packet_open(&read, PS_PACKET_READ))))
				goto err;
			if (unlikely((ret = ps_packet_read(&read, &state.header,
						  sizeof(glc_message_header_t)))))
				goto err;
			if (unlikely((ret = ps_packet_getsize(&read, &state.read_size))))
				goto err;
			state.read_size -= sizeof(glc_message_header_t);
			state.write_size = state.read_size;

			/* header callback */
			if (thread->header_callback) {
				if (unlikely((ret = thread->header_callback(&state))))
					goto err;
			}

			if (unlikely((ret = ps_packet_dma(&read, (void *) &state.read_data,
						 state.read_size, PS_ACCEPT_FAKE_DMA))))
				goto err;

			/* read callback */
			if (thread->read_callback) {
				if (unlikely((ret = thread->read_callback(&state))))
					goto err;
			}
		}

		if ((thread->flags & GLC_THREAD_WRITE) &&
		    (!(state.flags & GLC_THREAD_STATE_SKIP_WRITE))) {
			if (unlikely((ret = ps_packet_open(&write, PS_PACKET_WRITE))))
				goto err;

			if (has_locked) {
				has_locked = 0;
				pthread_mutex_unlock(&private->open);
			}

			/* reserve space for header */
			if (unlikely((ret = ps_packet_seek(&write,
							sizeof(glc_message_header_t)))))
				goto err;

			if (!(state.flags & GLC_THREAD_STATE_UNKNOWN_FINAL_SIZE)) {
				/* 'unlock' write */
				if (unlikely((ret = ps_packet_setsize(&write,
					   sizeof(glc_message_header_t) + state.write_size))))
					goto err;
				write_size_set = 1;
			}

			if (state.flags & GLC_THREAD_COPY) {
				/* should be faster, no need for fake dma */
				if (unlikely((ret = ps_packet_write(&write, state.read_data,
							state.write_size))))
					goto err;
			} else {
				if (unlikely((ret = ps_packet_dma(&write,
							(void *) &state.write_data,
							 state.write_size, PS_ACCEPT_FAKE_DMA))))
						goto err;

				/* write callback */
				if (thread->write_callback) {
					if (unlikely((ret = thread->write_callback(&state))))
						goto err;
				}
			}

			/* write header */
			if (unlikely((ret = ps_packet_seek(&write, 0))))
				goto err;
			if (unlikely((ret = ps_packet_write(&write,
					&state.header, sizeof(glc_message_header_t)))))
				goto err;
		}

		/* in case of we skipped writing */
		if (has_locked) {
			has_locked = 0;
			pthread_mutex_unlock(&private->open);
		}

		if ((thread->flags & GLC_THREAD_READ) &&
		    (!(state.flags & GLC_THREAD_STATE_SKIP_READ))) {
			ps_packet_close(&read);
			state.read_data = NULL;
			state.read_size = 0;
		}

		if ((thread->flags & GLC_THREAD_WRITE) &&
		    (!(state.flags & GLC_THREAD_STATE_SKIP_WRITE))) {
			if (!write_size_set) {
				if (unlikely((ret = ps_packet_setsize(&write,
					sizeof(glc_message_header_t) + state.write_size))))
					goto err;
			}
			ps_packet_close(&write);
			state.write_data = NULL;
			state.write_size = 0;
		}

		/* close callback */
		if (thread->close_callback) {
			if (unlikely((ret = thread->close_callback(&state))))
				goto err;
		}

		if (state.flags & GLC_THREAD_STOP)
			break; /* no error, just stop, please */

		state.flags = 0;
		write_size_set = 0;
	} while ((!glc_state_test(private->glc, GLC_STATE_CANCEL)) &&
		 (state.header.type != GLC_MESSAGE_CLOSE) &&
		 (!private->stop));

finish:
	if (packets_init) {
		if (thread->flags & GLC_THREAD_READ)
			ps_packet_destroy(&read);
		if (thread->flags & GLC_THREAD_WRITE)
			ps_packet_destroy(&write);
	}

	/* wake up remaining threads */
	if ((thread->flags & GLC_THREAD_READ) && (!private->stop)) {
		private->stop = 1;
		ps_buffer_cancel(private->from);

		/* error might have happened @ write buffer
		   so there could be blocking threads */
		if ((glc_state_test(private->glc, GLC_STATE_CANCEL)) &&
		    (thread->flags & GLC_THREAD_WRITE))
			ps_buffer_cancel(private->to);
	}

	/* thread finish callback */
	if (thread->thread_finish_callback)
		thread->thread_finish_callback(state.ptr, state.threadptr, ret);

	pthread_mutex_lock(&private->finish);
	private->running_threads--;

	/* let other threads know about the error */
	if (ret)
		private->ret = ret;

	if (private->running_threads > 0) {
		pthread_mutex_unlock(&private->finish);
		return NULL;
	}

	/* it is safe to unlock now */
	pthread_mutex_unlock(&private->finish);

	/* finish callback */
	if (thread->finish_callback)
		thread->finish_callback(state.ptr, private->ret);

	return NULL;

err:
	if (has_locked)
		pthread_mutex_unlock(&private->open);

	if (ret == EINTR)
		ret = 0;
	else {
		glc_state_set(private->glc, GLC_STATE_CANCEL);
		glc_log(private->glc, GLC_ERROR, "glc_thread", "%s (%d)", strerror(ret), ret);
	}

	goto finish;
}

int glc_thread_set_rt_priority(glc_t *glc, int ask_rt)
{
	int ret = 0;
	struct sched_param param;
	if (ask_rt && glc_allow_rt(glc)) {
		param.sched_priority = sched_get_priority_min(SCHED_RR);
		ret = pthread_setschedparam(pthread_self(), SCHED_RR, &param);
		if (unlikely(ret))
			glc_log(glc, GLC_ERROR, "glc_thread", "failed to set rtprio: %s (%d)",
				strerror(ret), ret);
	}
	return ret;
}

/*
 * Signals should be handled by the main thread, nowhere else.
 * I'm using POSIX signal interface here, until someone tells me
 * that I should use signal/sigset instead
 *
 */
int glc_thread_block_signals(void)
{
	sigset_t ss;

	sigfillset(&ss);

	/* These ones we want */
	sigdelset(&ss, SIGKILL);
	sigdelset(&ss, SIGSTOP);
	sigdelset(&ss, SIGSEGV);
	sigdelset(&ss, SIGCHLD);
	sigdelset(&ss, SIGBUS);
	sigdelset(&ss, SIGALRM);
	sigdelset(&ss, SIGPROF);
	sigdelset(&ss, SIGVTALRM);
#ifndef NODEBUG
	// Don't block SIGINT in debug so we can always break in the debugger
	sigdelset(&ss, SIGINT);
#endif
        return pthread_sigmask(SIG_BLOCK, &ss, NULL);
}

typedef struct {
	void *(*start_routine) (void *);
	void *arg;
	glc_t *glc;
	int   ask_rt;
} glc_simple_thread_param_t;

static void *glc_simple_thread_start_routine(void *arg)
{
	glc_simple_thread_param_t *param = (glc_simple_thread_param_t*)arg;
	void *res;

	glc_thread_block_signals();
	glc_thread_set_rt_priority(param->glc, param->ask_rt);
	res  = param->start_routine(param->arg);
	free(param);
	return res;
}

int glc_simple_thread_create(glc_t *glc, glc_simple_thread_t *thread,
				void *(*start_routine) (void *), void *arg)
{
	int ret;
	glc_simple_thread_param_t *param;

	if (unlikely(thread->running))
		return EAGAIN;

	param = (glc_simple_thread_param_t *) malloc(sizeof(glc_simple_thread_param_t));
	if (!param)
		return ENOMEM;

	param->start_routine = start_routine;
	param->arg           = arg;
	param->glc           = glc;
	param->ask_rt        = thread->ask_rt;

	/* May need to set before starting the thread as some threads
	 * might use this flag as a stop condition.
	 */
	thread->running = 1;
	ret = pthread_create(&thread->thread, NULL,
				glc_simple_thread_start_routine, param);

	if (unlikely(ret)) {
		thread->running = 0;
		glc_log(glc, GLC_ERROR, "glc_thread",
			 "can't create thread: %s (%d)", strerror(ret), ret);
		free(param);
	}

	return ret;
}

int glc_simple_thread_wait(glc_t *glc, glc_simple_thread_t *thread)
{
	int ret;
        if (unlikely(!thread->running))
                return EAGAIN;

	/*
	 * May need to set before joining the thread as some threads
	 * might use this flag as a stop condition.
	 */
        thread->running = 0;
        if (unlikely((ret = pthread_join(thread->thread, NULL))))
		glc_log(glc, GLC_ERROR, "glc_thread",
			"can't join thread: %s (%d)", strerror(ret), ret);

        return ret;
}

/**  \} */
