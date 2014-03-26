/**
 * \file glc/common/signal.c
 * \brief glcs signal related functions.
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

#include <signal.h>
#include <string.h> // for strerror()
#include <errno.h>
#include <pthread.h>
#include <sys/wait.h>
#include <glc/common/signal.h>
#include <glc/common/log.h>

#include "optimization.h"

/* Prototype __libc_allocate_rtsig() to avoid an implicit declaration. */
extern int __libc_allocate_rtsig(int);

/** async signal number */
static int glcs_signal_signo;

/*
 * Will be called automatically at lib load.
 * __libc_allocate_rtsig() is not thread safe so calling it prior
 * creating threads is a good idea.
 */
void glcs_signal_init(void) __attribute__ ((constructor));

void glcs_signal_init(void)
{
	/*
	 * The param indicates that we are not looking to have a high prio rtsig
	 */
        glcs_signal_signo = __libc_allocate_rtsig(0);
}

static void glcs_signal_handler(int signo)
{
	// empty for now
}

int glcs_signal_init_thread_disposition(glc_t *glc)
{
	int ret;
	struct sigaction act;
	sigset_t mask;

	glc_log(glc, GLC_DEBUG, "signal", "installing rtsig %d", glcs_signal_signo);
	act.sa_handler = glcs_signal_handler;
	act.sa_flags   = SA_INTERRUPT;
	sigemptyset(&act.sa_mask);
	if (unlikely((ret = sigaction(glcs_signal_signo, &act, NULL)) < 0))
	{
		glc_log(glc, GLC_ERROR, "signal",
			"failed to install glcs_signal handler: %s (%d)",
			strerror(errno), errno);
		return ret;
	}
	sigemptyset(&mask);
	sigaddset(&mask, glcs_signal_signo);
	if (unlikely((ret = pthread_sigmask(SIG_UNBLOCK, &mask, NULL)) < 0))
		glc_log(glc, GLC_ERROR, "signal",
			"failed to unblock glcs_signal: %s (%d)",
			strerror(errno), errno);
	return ret;
}

struct timed_waitpid_param
{
	pthread_t parent;
	const struct timespec *ts;
};

static void *timed_waitpid_threadfunc(void *arg)
{
	struct timed_waitpid_param *param = (struct timed_waitpid_param *)arg;
	pthread_setcanceltype(PTHREAD_CANCEL_ASYNCHRONOUS, NULL);
	clock_nanosleep(CLOCK_MONOTONIC, 0, param->ts, NULL);
	/*
	 * If everything goes well, the thread should be cancelled inside
	 * clock_nanosleep().
	 */
	pthread_kill(param->parent, glcs_signal_signo);
	return NULL;
}

int glcs_signal_timed_waitpid(glc_t *glc, pid_t pid, int *stat_loc,
				const struct timespec *ts)
{
	int ret;
	pthread_t thread;
	struct timed_waitpid_param param =
	{
		.parent = pthread_self(),
		.ts     = ts,
	};
	if (unlikely((ret = pthread_create(&thread, NULL,
					timed_waitpid_threadfunc, &param))))
	{
		errno = ret;
		return -1;
	}
	glc_log(glc, GLC_DEBUG, "signal", "wait for pid %d", pid);
	ret = waitpid(pid, stat_loc, 0);
	if (!ret || errno != EINTR)
		pthread_cancel(thread);
	else
		glc_log(glc, GLC_DEBUG, "signal", "waitpid() has timed out");
	pthread_join(thread,NULL);
	return ret;
}

/*
 * Code adapted from APUE from Stevens.
 */
void glcs_signal_pr_exit(glc_t *glc, pid_t pid, int status)
{
	if (WIFEXITED(status))
		glc_log(glc, GLC_INFO, "signal",
			"(%d) normal termination, exit status = %d",
			pid, WEXITSTATUS(status));
	else if (WIFSIGNALED(status))
		glc_log(glc, GLC_INFO, "signal",
			"(%d) abnormal termination, signal number = %d%s",
			pid, WTERMSIG(status),
			WCOREDUMP(status) ? " (core file generated)" : "");
	else if (WIFSTOPPED(status))
		glc_log(glc, GLC_INFO, "signal",
			"(%d) child stopped, signal number = %d",
			WSTOPSIG(status));
}

void glcs_signal_reset()
{
	unsigned i;
	struct sigaction act;
	act.sa_handler = SIG_DFL;
	act.sa_flags   = 0;
	sigemptyset(&act.sa_mask);

	for (i = 1; /* skip null sig */
		i < NSIG; ++i)
		sigaction(i, &act, NULL);
}

