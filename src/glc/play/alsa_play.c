/**
 * \file glc/play/alsa_play.c
 * \brief audio playback adapted from original work (glc) from Pyry Haulos <pyry.haulos@gmail.com>
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
 * \addtogroup alsa_play
 *  \{
 */

#include <stdlib.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <packetstream.h>
#include <alsa/asoundlib.h>
#include <sched.h>

#include <glc/common/glc.h>
#include <glc/common/core.h>
#include <glc/common/log.h>
#include <glc/common/util.h>
#include <glc/common/state.h>
#include <glc/common/thread.h>

#include "alsa_play.h"
#include "optimization.h"

struct alsa_play_s {
	glc_t *glc;
	glc_thread_t thread;
	int running;

	glc_utime_t silence_threshold;

	glc_stream_id_t id;
	snd_pcm_t *pcm;
	const char *device;

	unsigned int channels;
	unsigned int rate;
	glc_flags_t flags;
	glc_audio_format_t format;

	int fmt;

	void **bufs;
};

static int alsa_play_read_callback(glc_thread_state_t *state);
static void alsa_play_finish_callback(void *priv, int err);

static int alsa_play_hw(alsa_play_t alsa_play, glc_audio_format_message_t *fmt_msg);
static int alsa_play_play(alsa_play_t alsa_play, glc_audio_data_header_t *audio_msg, char *data);

static snd_pcm_format_t glc_fmt_to_pcm_fmt(glc_audio_format_t format);

static int alsa_play_xrun(alsa_play_t alsa_play, int err);

snd_pcm_format_t glc_fmt_to_pcm_fmt(glc_audio_format_t format)
{
	switch (format) {
		case GLC_AUDIO_S16_LE:
			return SND_PCM_FORMAT_S16_LE;
		case GLC_AUDIO_S24_LE:
			return SND_PCM_FORMAT_S24_LE;
		case GLC_AUDIO_S32_LE:
			return SND_PCM_FORMAT_S32_LE;
	}
	return 0;
}

int alsa_play_init(alsa_play_t *alsa_play, glc_t *glc)
{
	*alsa_play = (alsa_play_t) calloc(1, sizeof(struct alsa_play_s));

	(*alsa_play)->glc = glc;
	(*alsa_play)->device = "default";
	(*alsa_play)->id = 1;

	/* 200 ms */
	(*alsa_play)->silence_threshold = 200000000; /** \todo make configurable? */

	(*alsa_play)->thread.flags = GLC_THREAD_READ;
	(*alsa_play)->thread.ptr = *alsa_play;
	(*alsa_play)->thread.read_callback = &alsa_play_read_callback;
	(*alsa_play)->thread.finish_callback = &alsa_play_finish_callback;
	(*alsa_play)->thread.threads = 1;
	(*alsa_play)->thread.ask_rt  = 1;

	return 0;
}

int alsa_play_destroy(alsa_play_t alsa_play)
{
	free(alsa_play);
	return 0;
}

int alsa_play_set_alsa_playback_device(alsa_play_t alsa_play, const char *device)
{
	alsa_play->device = device;
	return 0;
}

int alsa_play_set_stream_id(alsa_play_t alsa_play, glc_stream_id_t id)
{
	alsa_play->id = id;
	return 0;
}

int alsa_play_process_start(alsa_play_t alsa_play, ps_buffer_t *from)
{
	int ret;
	if (unlikely(alsa_play->running))
		return EAGAIN;

	if ((ret = glc_thread_create(alsa_play->glc, &alsa_play->thread, from, NULL)))
		return ret;
	alsa_play->running = 1;

	return 0;
}

int alsa_play_process_wait(alsa_play_t alsa_play)
{
	if (unlikely(!alsa_play->running))
		return EAGAIN;

	glc_thread_wait(&alsa_play->thread);
	alsa_play->running = 0;

	return 0;
}

void alsa_play_finish_callback(void *priv, int err)
{
	alsa_play_t alsa_play = (alsa_play_t) priv;

	if (err)
		glc_log(alsa_play->glc, GLC_ERROR, "alsa_play", "%s (%d)",
			 strerror(err), err);

	if (alsa_play->pcm) {
		snd_pcm_drain(alsa_play->pcm);
		snd_pcm_close(alsa_play->pcm);
		alsa_play->pcm = NULL;
	}

	if (alsa_play->bufs) {
		free(alsa_play->bufs);
		alsa_play->bufs = NULL;
	}
}

int alsa_play_read_callback(glc_thread_state_t *state)
{
	int res;
	alsa_play_t alsa_play = (alsa_play_t ) state->ptr;

	if (unlikely(state->header.type == GLC_MESSAGE_AUDIO_FORMAT))
		res = alsa_play_hw(alsa_play, (glc_audio_format_message_t *) state->read_data);
	else if (likely(state->header.type == GLC_MESSAGE_AUDIO_DATA))
		res = alsa_play_play(alsa_play, (glc_audio_data_header_t *) state->read_data,
				       &state->read_data[sizeof(glc_audio_data_header_t)]);
	else
		res = 0;

	return res;
}

int alsa_play_hw(alsa_play_t alsa_play, glc_audio_format_message_t *fmt_msg)
{
	snd_pcm_hw_params_t *hw_params = NULL;
	snd_pcm_access_t access;
	unsigned int period_time;
	unsigned int buffer_time;
	int ret = 0;

	if (unlikely(fmt_msg->id != alsa_play->id))
		return 0;

	alsa_play->flags    = fmt_msg->flags;
	alsa_play->format   = fmt_msg->format;
	alsa_play->rate     = fmt_msg->rate;
	alsa_play->channels = fmt_msg->channels;

	if (alsa_play->pcm) /* re-open */
		snd_pcm_close(alsa_play->pcm);

	if (alsa_play->flags & GLC_AUDIO_INTERLEAVED)
		access = SND_PCM_ACCESS_RW_INTERLEAVED;
	else
		access = SND_PCM_ACCESS_RW_NONINTERLEAVED;

	if (unlikely((ret = snd_pcm_open(&alsa_play->pcm, alsa_play->device,
				SND_PCM_STREAM_PLAYBACK, 0)) < 0))
		goto err;
	snd_pcm_hw_params_alloca(&hw_params);
	if (unlikely((ret = snd_pcm_hw_params_any(alsa_play->pcm, hw_params)) < 0))
		goto err;
	if (unlikely((ret = snd_pcm_hw_params_set_access(alsa_play->pcm,
						hw_params, access)) < 0))
		goto err;
	if (unlikely((ret = snd_pcm_hw_params_set_format(alsa_play->pcm, hw_params,
						glc_fmt_to_pcm_fmt(alsa_play->format))) < 0))
		goto err;
	if (unlikely((ret = snd_pcm_hw_params_set_channels(alsa_play->pcm, hw_params,
						  alsa_play->channels)) < 0))
		goto err;
	if (unlikely((ret = snd_pcm_hw_params_set_rate(alsa_play->pcm, hw_params,
					      alsa_play->rate, 0)) < 0))
		goto err;

	if (unlikely((ret = snd_pcm_hw_params_get_buffer_time_max(hw_params,
						&buffer_time, 0))))
		goto err;

	if (buffer_time > 1000000) {
		glc_log(alsa_play->glc, GLC_INFO, "alsa_play",
			"buffer time max is %u usec. We will limit it to 1 sec",
			buffer_time);
		buffer_time = 1000000;
	}

	period_time = buffer_time / 4;
	alsa_play->silence_threshold = period_time*2000;

	if (unlikely((ret = snd_pcm_hw_params_set_period_time_near(alsa_play->pcm,
		hw_params, &period_time, 0)) < 0))
		goto err;

	if (unlikely((ret = snd_pcm_hw_params_set_buffer_time_near(alsa_play->pcm,
		hw_params, &buffer_time, 0)) < 0))
		goto err;
	if (unlikely((ret = snd_pcm_hw_params(alsa_play->pcm, hw_params)) < 0))
		goto err;

	alsa_play->bufs = (void **) malloc(sizeof(void *) * alsa_play->channels);

	glc_log(alsa_play->glc, GLC_INFO, "alsa_play",
		"opened pcm %s for playback. buffer_time: %u",
		alsa_play->device, buffer_time);

	return 0;
err:
	glc_log(alsa_play->glc, GLC_ERROR, "alsa_play",
		"can't initialize pcm %s: %s (%d)",
		alsa_play->device, snd_strerror(ret), ret);
	return -ret;
}

int alsa_play_play(alsa_play_t alsa_play, glc_audio_data_header_t *audio_hdr, char *data)
{
	snd_pcm_uframes_t frames, rem;
	snd_pcm_sframes_t ret = 0;
	unsigned int c;

	if (audio_hdr->id != alsa_play->id)
		return 0;

	if (!alsa_play->pcm) {
		glc_log(alsa_play->glc, GLC_ERROR, "alsa_play", "broken stream %d",
			 alsa_play->id);
		return EINVAL;
	}

	frames = snd_pcm_bytes_to_frames(alsa_play->pcm, audio_hdr->size);
	glc_utime_t time = glc_state_time(alsa_play->glc);
	glc_utime_t duration = ((glc_utime_t) 1000000000 * (glc_utime_t) frames) /
			       (glc_utime_t) alsa_play->rate;

	if (time + alsa_play->silence_threshold + duration < audio_hdr->time) {
		struct timespec ts = {
		.tv_sec = (audio_hdr->time - time - duration - alsa_play->silence_threshold)/1000000000,
		.tv_nsec = (audio_hdr->time - time - duration - alsa_play->silence_threshold)%1000000000 };
		nanosleep(&ts,NULL);
	}
	/*
	 * This condition determine what will be the initial audio packet.
	 * it is preferable to be ahead by < duration/2 than behind
	 * the video by > duration/2
	 */
	else if (time > audio_hdr->time + duration/2) {
		glc_log(alsa_play->glc, GLC_DEBUG, "alsa_play",
			"dropped packet. now %" PRId64 " ts %" PRId64,
			time, audio_hdr->time);
		return 0;
	}

	rem = frames;

	while (rem > 0) {
		/* alsa is horrible... */
		/*snd_pcm_wait(alsa_play->pcm, duration);*/

		if (alsa_play->flags & GLC_AUDIO_INTERLEAVED)
			ret = snd_pcm_writei(alsa_play->pcm,
					    &data[snd_pcm_frames_to_bytes(alsa_play->pcm, frames - rem)],
					    rem);
		else {
			for (c = 0; c < alsa_play->channels; c++)
				alsa_play->bufs[c] =
					&data[snd_pcm_samples_to_bytes(alsa_play->pcm, frames)
					      * c + snd_pcm_samples_to_bytes(alsa_play->pcm, frames - rem)];
			ret = snd_pcm_writen(alsa_play->pcm, alsa_play->bufs, rem);
		}

		if (ret == 0)
			break;

		if ((ret == -EBUSY) || (ret == -EAGAIN))
			break;
		else if (ret < 0) {
			if ((ret = alsa_play_xrun(alsa_play, ret))) {
				glc_log(alsa_play->glc, GLC_ERROR, "alsa_play",
					 "xrun recovery failed: %s", snd_strerror(-ret));
				return ret;
			}
		} else
			rem -= ret;
	}

	return 0;
}

int alsa_play_xrun(alsa_play_t alsa_play, int err)
{
	switch(err) {
	case -EPIPE:
		glc_log(alsa_play->glc, GLC_WARN, "alsa_play", "underrun");
		if (unlikely((err = snd_pcm_prepare(alsa_play->pcm)) < 0))
			break;
//		err = snd_pcm_start(alsa_play->pcm);
		break;
	case -ESTRPIPE:
		glc_log(alsa_play->glc, GLC_DEBUG, "alsa_play", "suspended");
		while ((err = snd_pcm_resume(alsa_play->pcm)) == -EAGAIN)
			sched_yield();
		if (err < 0) {
			if (unlikely((err = snd_pcm_prepare(alsa_play->pcm)) < 0))
				break;
//			err = snd_pcm_start(alsa_play->pcm);
		}
		break;
	default:
		glc_log(alsa_play->glc, GLC_DEBUG, "alsa_play", "%s (%d)", snd_strerror(err), err);
		break;
	}
	return -err;
}

/**  \} */
