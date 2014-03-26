/**
 * \file glc/common/core.c
 * \brief glc core adapted from original work (glc) from Pyry Haulos <pyry.haulos@gmail.com>
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
 * \addtogroup common_core
 *  \{
 */

#include <stdlib.h>
#include <time.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>

#include "glc.h"
#include "core.h"
#include "log.h"
#include "util.h"
#include "optimization.h"

struct glc_core_s {
	struct timespec init_time;
	long int single_process_num;
	long int multi_process_num;
	long int threads_hint;
	int      allow_rt;
};

const char *glc_version()
{
	return GLC_VERSION;
}

int glc_init(glc_t *glc)
{
	int ret = 0;

	/* clear 'em */
	glc->core  = NULL;
	glc->state = NULL;
	glc->util  = NULL;
	glc->log   = NULL;

	glc->core = (glc_core_t) calloc(1, sizeof(struct glc_core_s));

	clock_gettime(CLOCK_MONOTONIC, &glc->core->init_time);

	glc->core->threads_hint = 1; /* safe conservative default value */

	if (unlikely((ret = glc_log_init(glc))))
		return ret;
	ret = glc_util_init(glc);
	return ret;
}

int glc_destroy(glc_t *glc)
{
	glc_util_destroy(glc);
	glc_log_destroy(glc);

	free(glc->core);

	/* and clear */
	glc->core = NULL;
	glc->state = NULL;
	glc->util = NULL;
	glc->log = NULL;

	return 0;
}

glc_utime_t glc_time(glc_t *glc)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);

	ts.tv_sec -= glc->core->init_time.tv_sec;
	ts.tv_nsec -= glc->core->init_time.tv_nsec;

	if (ts.tv_nsec < 0) {
		ts.tv_sec--;
		ts.tv_nsec += 1000000000;
	}

	return (glc_utime_t) ts.tv_sec * (glc_utime_t) 1000000000 + (glc_utime_t) ts.tv_nsec;
}

long int glc_threads_hint(glc_t *glc)
{
	return glc->core->threads_hint;
}

int glc_set_threads_hint(glc_t *glc, long int count)
{
	if (unlikely(count <= 0))
		return EINVAL;
	glc->core->threads_hint = count;
	return 0;
}

void glc_account_threads(glc_t *glc, long int single, long int multi)
{
	glc->core->single_process_num += single;
	glc->core->multi_process_num  += multi;
}

void glc_compute_threads_hint(glc_t *glc)
{
	if (unlikely(!glc->core->multi_process_num))
		glc->core->multi_process_num = 1; /* Avoid division by 0 */
	glc->core->threads_hint  = sysconf(_SC_NPROCESSORS_ONLN) - glc->core->single_process_num;
	glc->core->threads_hint /= glc->core->multi_process_num;
	if (unlikely(glc->core->threads_hint <  1))
		glc->core->threads_hint = 1;
	glc_log(glc, GLC_INFO, "core",
		"single proc num %ld multi proc num %ld, threads num per multi proc %ld",
		glc->core->single_process_num, glc->core->multi_process_num,
		glc->core->threads_hint);
}

void glc_set_allow_rt(glc_t *glc, int allow)
{
	glc->core->allow_rt = allow;
}

int glc_allow_rt(glc_t *glc)
{
	return glc->core->allow_rt;
}

/**  \} */
