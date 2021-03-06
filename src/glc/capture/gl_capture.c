/**
 * \file glc/capture/gl_capture.c
 * \brief OpenGL capture adapted from original work (glc) from Pyry Haulos <pyry.haulos@gmail.com>
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
 * \addtogroup gl_capture
 *  \{
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <X11/X.h>
#include <X11/Xlib.h>
#include <X11/extensions/xf86vmode.h>
#include <GL/gl.h>
#include <GL/glx.h>
#include <GL/glext.h>
#include <unistd.h>
#include <inttypes.h>
#include <packetstream.h>
#include <pthread.h>
#include <dlfcn.h>
#include <errno.h>
#include <time.h>

#include <glc/common/glc.h>
#include <glc/common/core.h>
#include <glc/common/log.h>
#include <glc/common/state.h>
#include <glc/common/util.h>
#include <glc/common/rational.h>
#include <glc/common/optimization.h>

#include "gl_capture.h"

#define GL_CAPTURE_TRY_PBO          0x1
#define GL_CAPTURE_USE_PBO          0x2
#define GL_CAPTURE_CAPTURING        0x4
#define GL_CAPTURE_DRAW_INDICATOR   0x8
#define GL_CAPTURE_CROP            0x10
#define GL_CAPTURE_LOCK_FPS        0x20
#define GL_CAPTURE_IGNORE_TIME     0x40

/*
 * The next functions come from:
 * http://locklessinc.com/articles/locks/
 */

/* Compile read-write barrier */
#define barrier() asm volatile("": : :"memory")

/* Pause instruction to prevent excess processor bus usage */
#define cpu_relax() asm volatile("pause\n": : :"memory")

static inline uint32_t xchg_32(volatile void *ptr, uint32_t x)
{
	__asm__ __volatile__("xchgl %0,%1"
				:"=r" ((uint32_t) x)
				:"m" (*(volatile uint32_t *)ptr), "0" (x)
				:"memory");
	return x;
}

#define SPINBUSY 1
typedef volatile uint32_t spinlock_t;

static void spin_lock(spinlock_t *lock)
{
	do {
		if (likely(!xchg_32(lock, SPINBUSY))) return;
		while (*lock) cpu_relax();
	} while(1);
}

static void spin_unlock(spinlock_t *lock)
{
	barrier();
	*lock = 0;
}

typedef void (*FuncPtr)(void);
typedef FuncPtr (*GLXGetProcAddressProc)(const GLubyte *procName);
typedef void (*glGenBuffersProc)(GLsizei n,
                                 GLuint *buffers);
typedef void (*glDeleteBuffersProc)(GLsizei n,
                                    const GLuint *buffers);
typedef void (*glBufferDataProc)(GLenum target,
                                 GLsizeiptr size,
                                 const GLvoid *data,
                                 GLenum usage);
typedef void (*glBindBufferProc)(GLenum target,
                                 GLuint buffer);
typedef GLvoid *(*glMapBufferProc)(GLenum target,
                                   GLenum access);
typedef GLboolean (*glUnmapBufferProc)(GLenum target);

struct gl_capture_video_stream_s {
	glc_state_video_t state_video;
	glc_stream_id_t id;

	volatile glc_flags_t flags;
	glc_video_format_t format;
	Display *dpy;
	int screen;
	GLXDrawable drawable;
	Window attribWin;
	ps_packet_t packet;
	glc_utime_t last, pbo_time;

	unsigned int w, h;
	unsigned int cw, ch, row, cx, cy;

	float brightness, contrast;
	float gamma_red, gamma_green, gamma_blue;

	int indicator_list;

	struct gl_capture_video_stream_s *next;

	GLuint pbo;
	int pbo_active;

	/* stats related vars */
	unsigned num_frames;
	unsigned num_captured_frames;
	uint64_t capture_time_ns;
	int      gather_stats;
};

struct gl_capture_s {
	glc_t *glc;
	spinlock_t capture_spinlock;
	glc_flags_t flags;

	GLenum capture_buffer;   /* GL_FRONT or GL_BACK */
	glc_utime_t fps_period;  /* time in ns between 2 frames */

	/*
	 * The next 2 variables are used to correct error introduced if fps_period
	 * is rational since the variable type is an integer.
	 */
	glc_utime_t fps_rem;     /* fix to apply every fps_rem_period frames */
	unsigned fps_rem_period; /* period in frames which fps_rem is applied */

	struct gl_capture_video_stream_s *video;

	ps_buffer_t *to;

	pthread_mutex_t mutex;

	unsigned int bpp;
	GLenum format;
	GLint pack_alignment;

	unsigned int crop_x, crop_y;
	unsigned int crop_w, crop_h;

	void *libGL_handle;
	GLXGetProcAddressProc glXGetProcAddress;
	glGenBuffersProc      glGenBuffers;
	glDeleteBuffersProc   glDeleteBuffers;
	glBufferDataProc      glBufferData;
	glBindBufferProc      glBindBuffer;
	glMapBufferProc       glMapBuffer;
	glUnmapBufferProc     glUnmapBuffer;
};

static int gl_capture_get_video_stream(gl_capture_t gl_capture,
				struct gl_capture_video_stream_s **video,
				Display *dpy, GLXDrawable drawable);
static int gl_capture_init_video_format(gl_capture_t gl_capture,
				struct gl_capture_video_stream_s *video);
static int gl_capture_write_video_format_message(gl_capture_t gl_capture,
				struct gl_capture_video_stream_s *video,
				unsigned int w, unsigned int h);
static int gl_capture_update_video_stream(gl_capture_t gl_capture,
				   struct gl_capture_video_stream_s *video);
static int gl_capture_clear_video_streams(gl_capture_t gl_capture);

static void gl_capture_error(gl_capture_t gl_capture, int err);

static int gl_capture_get_geometry(gl_capture_t gl_capture,
			    Display *dpy, Window win, unsigned int *w, unsigned int *h);
static int gl_capture_calc_geometry(gl_capture_t gl_capture,
				struct gl_capture_video_stream_s *video,
				unsigned int w, unsigned int h);
static int gl_capture_update_screen(gl_capture_t gl_capture,
				struct gl_capture_video_stream_s *video);
static int gl_capture_update_color(gl_capture_t gl_capture,
				struct gl_capture_video_stream_s *video,
				ps_packet_t *packet);

static int gl_capture_get_pixels(gl_capture_t gl_capture,
				struct gl_capture_video_stream_s *video, char *to);
static int gl_capture_gen_indicator_list(gl_capture_t gl_capture,
				struct gl_capture_video_stream_s *video);

static int gl_capture_init_pbo(gl_capture_t gl);
static int gl_capture_create_pbo(gl_capture_t gl_capture,
				struct gl_capture_video_stream_s *video);
static int gl_capture_destroy_pbo(gl_capture_t gl_capture,
				struct gl_capture_video_stream_s *video);
static int gl_capture_start_pbo(gl_capture_t gl_capture,
				struct gl_capture_video_stream_s *video);
static int gl_capture_read_pbo(gl_capture_t gl_capture,
				struct gl_capture_video_stream_s *video);

int gl_capture_init(gl_capture_t *gl_capture, glc_t *glc)
{
	*gl_capture = (gl_capture_t) calloc(1, sizeof(struct gl_capture_s));

	(*gl_capture)->glc = glc;
	(*gl_capture)->fps_period = 1000000000 / 30;	/* default fps is 30 */
	(*gl_capture)->fps_rem    = 1;
	(*gl_capture)->fps_rem_period = 3;
	(*gl_capture)->pack_alignment = 8;		/* read as dword aligned by default */
	(*gl_capture)->format = GL_BGRA;		/* capture as BGRA data by default */
	(*gl_capture)->bpp = 4;				/* since we use BGRA */
	(*gl_capture)->capture_buffer = GL_FRONT;	/* front buffer is default */

	pthread_mutex_init(&(*gl_capture)->mutex, NULL);

	return 0;
}

int gl_capture_set_buffer(gl_capture_t gl_capture, ps_buffer_t *buffer)
{
	if (unlikely(gl_capture->to))
		return EALREADY;

	gl_capture->to = buffer;
	return 0;
}

int gl_capture_set_read_buffer(gl_capture_t gl_capture, GLenum buffer)
{
	if (buffer == GL_FRONT)
		glc_log(gl_capture->glc, GLC_INFO, "gl_capture",
			 "reading frames from GL_FRONT");
	else if (buffer == GL_BACK)
		glc_log(gl_capture->glc, GLC_INFO, "gl_capture",
			 "reading frames from GL_BACK");
	else {
		glc_log(gl_capture->glc, GLC_ERROR, "gl_capture",
			 "unknown read buffer 0x%02x", buffer);
		return ENOTSUP;
	}

	gl_capture->capture_buffer = buffer;
	return 0;
}

int gl_capture_set_fps(gl_capture_t gl_capture, double fps)
{
	glcs_Rational_t a, b = { 1000000000, 1}, c;
	if (unlikely(fps <= 0.0))
		return EINVAL;

	a = glcs_d2q(fps, 1001000);
	c = glcs_div_q(b, a);
	gl_capture->fps_period = c.num/c.den;
	gl_capture->fps_rem    = c.num%c.den;
	gl_capture->fps_rem_period = c.den;
	glc_log(gl_capture->glc, GLC_INFO, "gl_capture",
		"capturing at %f fps, interval %" PRIu64
		" with a rational fix of %" PRIu64 " every %u frames",
		fps, gl_capture->fps_period, gl_capture->fps_rem,
		gl_capture->fps_rem_period);

	return 0;
}

int gl_capture_set_pack_alignment(gl_capture_t gl_capture, GLint pack_alignment)
{
	if (pack_alignment == 1)
		glc_log(gl_capture->glc, GLC_INFO, "gl_capture",
			 "reading data as byte aligned");
	else if (pack_alignment == 8)
		glc_log(gl_capture->glc, GLC_INFO, "gl_capture",
			 "reading data as dword aligned");
	else {
		glc_log(gl_capture->glc, GLC_ERROR, "gl_capture",
			 "unknown GL_PACK_ALIGNMENT %d", pack_alignment);
		return ENOTSUP;
	}

	gl_capture->pack_alignment = pack_alignment;
	return 0;
}

int gl_capture_try_pbo(gl_capture_t gl_capture, int try_pbo)
{
	if (try_pbo) {
		gl_capture->flags |= GL_CAPTURE_TRY_PBO;
	} else {
		if (unlikely(gl_capture->flags & GL_CAPTURE_USE_PBO)) {
			glc_log(gl_capture->glc, GLC_WARN, "gl_capture",
				 "can't disable PBO; it is in use");
			return EAGAIN;
		}

		glc_log(gl_capture->glc, GLC_DEBUG, "gl_capture",
			 "PBO disabled");
		gl_capture->flags &= ~GL_CAPTURE_TRY_PBO;
	}

	return 0;
}

int gl_capture_set_pixel_format(gl_capture_t gl_capture, GLenum format)
{
	if (format == GL_BGRA) {
		glc_log(gl_capture->glc, GLC_INFO, "gl_capture",
			 "reading frames in GL_BGRA format");
		gl_capture->bpp = 4;
	} else if (format == GL_BGR) {
		glc_log(gl_capture->glc, GLC_INFO, "gl_capture",
			 "reading frames in GL_BGR format");
		gl_capture->bpp = 3;
	} else {
		glc_log(gl_capture->glc, GLC_ERROR, "gl_capture",
			 "unsupported pixel format 0x%02x", format);
		return ENOTSUP;
	}

	gl_capture->format = format;
	return 0;
}

int gl_capture_draw_indicator(gl_capture_t gl_capture, int draw_indicator)
{
	if (draw_indicator) {
		gl_capture->flags |= GL_CAPTURE_DRAW_INDICATOR;

		if (gl_capture->capture_buffer == GL_FRONT)
			glc_log(gl_capture->glc, GLC_WARN, "gl_capture",
				"indicator doesn't work well when capturing from GL_FRONT");
	} else
		gl_capture->flags &= ~GL_CAPTURE_DRAW_INDICATOR;

	return 0;
}

int gl_capture_ignore_time(gl_capture_t gl_capture, int ignore_time)
{
	if (ignore_time)
		gl_capture->flags |= GL_CAPTURE_IGNORE_TIME;
	else
		gl_capture->flags &= ~GL_CAPTURE_IGNORE_TIME;
	return 0;
}

int gl_capture_crop(gl_capture_t gl_capture, unsigned int x, unsigned int y,
			     unsigned int width, unsigned int height)
{
	if ((!x) && (!y) && (!width) && (!height)) {
		gl_capture->flags &= ~GL_CAPTURE_CROP;
		return 0;
	}

	gl_capture->crop_x = x;
	gl_capture->crop_y = y;
	gl_capture->crop_w = width;
	gl_capture->crop_h = height;
	gl_capture->flags |= GL_CAPTURE_CROP;

	return 0;
}

int gl_capture_lock_fps(gl_capture_t gl_capture, int lock_fps)
{
	if (lock_fps)
		gl_capture->flags |= GL_CAPTURE_LOCK_FPS;
	else
		gl_capture->flags &= ~GL_CAPTURE_LOCK_FPS;

	return 0;
}

int gl_capture_start(gl_capture_t gl_capture)
{
	if (unlikely(!gl_capture->to)) {
		glc_log(gl_capture->glc, GLC_ERROR, "gl_capture",
			 "no target buffer specified");
		return EAGAIN;
	}

	if (gl_capture->flags & GL_CAPTURE_CAPTURING)
		glc_log(gl_capture->glc, GLC_WARN, "gl_capture",
			 "capturing is already active");
	else
		glc_log(gl_capture->glc, GLC_INFO, "gl_capture",
			 "starting capturing");

	gl_capture->flags |= GL_CAPTURE_CAPTURING;
	return 0;
}

int gl_capture_stop(gl_capture_t gl_capture)
{
	if (gl_capture->flags & GL_CAPTURE_CAPTURING) {
		spin_lock(&gl_capture->capture_spinlock);
		gl_capture->flags &= ~GL_CAPTURE_CAPTURING;
		spin_unlock(&gl_capture->capture_spinlock);
		glc_log(gl_capture->glc, GLC_INFO, "gl_capture",
			 "stopping capturing");
		gl_capture_clear_video_streams(gl_capture);
	} else
		glc_log(gl_capture->glc, GLC_WARN, "gl_capture",
			 "capturing is already stopped");

	return 0;
}

void gl_capture_error(gl_capture_t gl_capture, int err)
{
	glc_log(gl_capture->glc, GLC_ERROR, "gl_capture",
		"%s (%d)", strerror(err), err);

	/* stop capturing */
	if (gl_capture->flags & GL_CAPTURE_CAPTURING)
		gl_capture_stop(gl_capture);

	/* cancel glc */
	glc_state_set(gl_capture->glc, GLC_STATE_CANCEL);
	if (gl_capture->to)
		ps_buffer_cancel(gl_capture->to);
}

int gl_capture_destroy(gl_capture_t gl_capture)
{
	struct gl_capture_video_stream_s *del;

	while (gl_capture->video != NULL) {
		del = gl_capture->video;
		gl_capture->video = gl_capture->video->next;

		glc_log(gl_capture->glc, GLC_PERF, "gl_capture",
			"captured %u frames in %" PRIu64 " nsec",
			del->num_captured_frames, del->capture_time_ns);

		/* we might be in wrong thread */
		if (del->indicator_list)
			glDeleteLists(del->indicator_list, 1);

		if (del->pbo)
			gl_capture_destroy_pbo(gl_capture, del);

		ps_packet_destroy(&del->packet);
		free(del);
	}

	pthread_mutex_destroy(&gl_capture->mutex);

	if (gl_capture->libGL_handle)
		dlclose(gl_capture->libGL_handle);
	free(gl_capture);

	return 0;
}

int gl_capture_get_geometry(gl_capture_t gl_capture, Display *dpy, Window win,
                    unsigned int *w, unsigned int *h)
{
	Window rootWindow;
	int unused;

	XGetGeometry(dpy, win, &rootWindow, &unused, &unused, w, h,
	             (unsigned int *) &unused, (unsigned int *) &unused);

	return 0;
}

int gl_capture_update_screen(gl_capture_t gl_capture,
			     struct gl_capture_video_stream_s *video)
{
	/** \todo figure out real screen */
	video->screen = DefaultScreen(video->dpy);
	return 0;
}

int gl_capture_calc_geometry(gl_capture_t gl_capture,
			     struct gl_capture_video_stream_s *video,
			     unsigned int w, unsigned int h)
{
	video->w = w;
	video->h = h;

	/* calculate image area when cropping */
	if (gl_capture->flags & GL_CAPTURE_CROP) {
		if (gl_capture->crop_x > video->w)
			video->cx = 0;
		else
			video->cx = gl_capture->crop_x;

		if (gl_capture->crop_y > video->h)
			video->cy = 0;
		else
			video->cy = gl_capture->crop_y;

		if (gl_capture->crop_w + video->cx > video->w)
			video->cw = video->w - video->cx;
		else
			video->cw = gl_capture->crop_w;

		if (gl_capture->crop_h + video->cy > video->h)
			video->ch = video->h - video->cy;
		else
			video->ch = gl_capture->crop_h;

		/* we need to recalc y coord for OpenGL */
		video->cy = video->h - video->ch - video->cy;
	} else {
		video->cw = video->w;
		video->ch = video->h;
		video->cx = video->cy = 0;
	}

	glc_log(gl_capture->glc, GLC_DEBUG, "gl_capture",
		 "calculated capture area for video %d is %ux%u+%u+%u",
		 video->id, video->cw, video->ch, video->cx, video->cy);

	video->row = video->cw * gl_capture->bpp;
	if (unlikely(video->row % gl_capture->pack_alignment != 0))
		video->row += gl_capture->pack_alignment -
			      video->row % gl_capture->pack_alignment;
	return 0;
}

int gl_capture_get_pixels(gl_capture_t gl_capture,
			  struct gl_capture_video_stream_s *video, char *to)
{
	glPushAttrib(GL_PIXEL_MODE_BIT);
	glPushClientAttrib(GL_CLIENT_PIXEL_STORE_BIT);

	glReadBuffer(gl_capture->capture_buffer);
	glPixelStorei(GL_PACK_ALIGNMENT, gl_capture->pack_alignment);
	glReadPixels(video->cx, video->cy, video->cw, video->ch,
		gl_capture->format, GL_UNSIGNED_BYTE, to);

	glPopClientAttrib();
	glPopAttrib();

	return 0;
}

int gl_capture_gen_indicator_list(gl_capture_t gl_capture,
				struct gl_capture_video_stream_s *video)
{
	int size;
	if (!video->indicator_list)
		video->indicator_list = glGenLists(1);

	glNewList(video->indicator_list, GL_COMPILE);

	size = video->h / 75;
	if (size < 10)
		size = 10;

	glPushAttrib(GL_ALL_ATTRIB_BITS);

	glViewport(0, 0, video->w, video->h);
	glEnable(GL_SCISSOR_TEST);
	glScissor(size / 2 - 1, video->h - size - size / 2 - 1, size + 2, size + 2);
	glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
	glClear(GL_COLOR_BUFFER_BIT);
	glScissor(size / 2, video->h - size - size / 2, size, size);
	glClearColor(1.0f, 0.0f, 0.0f, 0.0f);
	glClear(GL_COLOR_BUFFER_BIT);
	glDisable(GL_SCISSOR_TEST);

	glPopAttrib();

	glEndList();
	return 0;
}

int gl_capture_init_pbo(gl_capture_t gl_capture)
{
	const char *gl_extensions = (const char *) glGetString(GL_EXTENSIONS);

	if (unlikely(gl_extensions == NULL))
		return EINVAL;

	if (unlikely(!strstr(gl_extensions, "GL_ARB_pixel_buffer_object")))
		return ENOTSUP;

	gl_capture->libGL_handle = dlopen("libGL.so.1", RTLD_LAZY);
	if (unlikely(!gl_capture->libGL_handle))
		return ENOTSUP;
	gl_capture->glXGetProcAddress =
		(GLXGetProcAddressProc)
		dlsym(gl_capture->libGL_handle, "glXGetProcAddressARB");
	if (unlikely(!gl_capture->glXGetProcAddress))
		return ENOTSUP;
	gl_capture->glGenBuffers =
		(glGenBuffersProc)
		gl_capture->glXGetProcAddress((const GLubyte *) "glGenBuffersARB");
	if (unlikely(!gl_capture->glGenBuffers))
		return ENOTSUP;
	gl_capture->glDeleteBuffers =
		(glDeleteBuffersProc)
		gl_capture->glXGetProcAddress((const GLubyte *) "glDeleteBuffersARB");
	if (unlikely(!gl_capture->glDeleteBuffers))
		return ENOTSUP;
	gl_capture->glBufferData =
		(glBufferDataProc)
		gl_capture->glXGetProcAddress((const GLubyte *) "glBufferDataARB");
	if (unlikely(!gl_capture->glBufferData))
		return ENOTSUP;
	gl_capture->glBindBuffer =
		(glBindBufferProc)
		gl_capture->glXGetProcAddress((const GLubyte *) "glBindBufferARB");
	if (unlikely(!gl_capture->glBindBuffer))
		return ENOTSUP;
	gl_capture->glMapBuffer =
		(glMapBufferProc)
		gl_capture->glXGetProcAddress((const GLubyte *) "glMapBufferARB");
	if (unlikely(!gl_capture->glMapBuffer))
		return ENOTSUP;
	gl_capture->glUnmapBuffer =
		(glUnmapBufferProc)
		gl_capture->glXGetProcAddress((const GLubyte *) "glUnmapBufferARB");
	if (unlikely(!gl_capture->glUnmapBuffer))
		return ENOTSUP;

	glc_log(gl_capture->glc, GLC_INFO, "gl_capture",
		 "using GL_ARB_pixel_buffer_object");

	return 0;
}

int gl_capture_create_pbo(gl_capture_t gl_capture,
			  struct gl_capture_video_stream_s *video)
{
	GLint binding;

	glc_log(gl_capture->glc, GLC_DEBUG, "gl_capture", "creating PBO");

	glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING_ARB, &binding);
	glPushAttrib(GL_ALL_ATTRIB_BITS);

	gl_capture->glGenBuffers(1, &video->pbo);
	gl_capture->glBindBuffer(GL_PIXEL_PACK_BUFFER_ARB, video->pbo);
	gl_capture->glBufferData(GL_PIXEL_PACK_BUFFER_ARB, video->row * video->ch,
				NULL, GL_STREAM_READ);

	glPopAttrib();
	gl_capture->glBindBuffer(GL_PIXEL_PACK_BUFFER_ARB, binding);
	return 0;
}

int gl_capture_destroy_pbo(gl_capture_t gl_capture,
			   struct gl_capture_video_stream_s *video)
{
	glc_log(gl_capture->glc, GLC_DEBUG, "gl_capture", "destroying PBO");
	gl_capture->glDeleteBuffers(1, &video->pbo);
	return 0;
}

int gl_capture_start_pbo(gl_capture_t gl_capture, struct gl_capture_video_stream_s *video)
{
	GLint binding;

	glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING_ARB, &binding);
	glPushAttrib(GL_PIXEL_MODE_BIT);
	glPushClientAttrib(GL_CLIENT_PIXEL_STORE_BIT);

	gl_capture->glBindBuffer(GL_PIXEL_PACK_BUFFER_ARB, video->pbo);

	glReadBuffer(gl_capture->capture_buffer);
	glPixelStorei(GL_PACK_ALIGNMENT, gl_capture->pack_alignment);
	/* to = ((char *)NULL + (offset)) */
	glReadPixels(video->cx, video->cy, video->cw, video->ch,
		gl_capture->format, GL_UNSIGNED_BYTE, NULL);

	glPopClientAttrib();
	glPopAttrib();
	gl_capture->glBindBuffer(GL_PIXEL_PACK_BUFFER_ARB, binding);
	return 0;
}

int gl_capture_read_pbo(gl_capture_t gl_capture, struct gl_capture_video_stream_s *video)
{
	GLvoid *buf;
	GLint binding;

	glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING_ARB, &binding);

	gl_capture->glBindBuffer(GL_PIXEL_PACK_BUFFER_ARB, video->pbo);
	buf = gl_capture->glMapBuffer(GL_PIXEL_PACK_BUFFER_ARB, GL_READ_ONLY);
	if (unlikely(!buf))
		return EINVAL;

	ps_packet_write(&video->packet, buf, video->row * video->ch);

	gl_capture->glUnmapBuffer(GL_PIXEL_PACK_BUFFER_ARB);

	gl_capture->glBindBuffer(GL_PIXEL_PACK_BUFFER_ARB, binding);
	return 0;
}

int gl_capture_get_video_stream(gl_capture_t gl_capture,
				struct gl_capture_video_stream_s **video,
				Display *dpy, GLXDrawable drawable)
{
	struct gl_capture_video_stream_s *fvideo;

	fvideo = gl_capture->video;
	while (fvideo != NULL) {
		if ((fvideo->drawable == drawable) && (fvideo->dpy == dpy))
			break;

		fvideo = fvideo->next;
	}

	if (fvideo == NULL) {
		fvideo = (struct gl_capture_video_stream_s *)
			calloc(1, sizeof(struct gl_capture_video_stream_s));

		fvideo->dpy          = dpy;
		fvideo->drawable     = drawable;
		fvideo->flags        = GLC_VIDEO_NEED_COLOR_UPDATE;
		fvideo->gather_stats = glc_log_get_level(gl_capture->glc) >= GLC_PERF;
		ps_packet_init(&fvideo->packet, gl_capture->to);

		glc_state_video_new(gl_capture->glc, &fvideo->id, &fvideo->state_video);

		fvideo->next      = gl_capture->video;

		/* linked list multithread sync RCU style! */
		gl_capture->video = fvideo;
	}
	__sync_or_and_fetch(&fvideo->flags, GLC_VIDEO_CAPTURING);
	*video = fvideo;
	return 0;
}

static inline void gl_capture_release_video_stream(struct gl_capture_video_stream_s *video)
{
	__sync_and_and_fetch(&video->flags, ~GLC_VIDEO_CAPTURING);
}

int gl_capture_clear_video_streams(gl_capture_t gl_capture)
{
	struct gl_capture_video_stream_s *fvideo;
	fvideo = gl_capture->video;
	while (fvideo != NULL) {
		while (unlikely(fvideo->flags & GLC_VIDEO_CAPTURING)) {
			struct timespec one_ms = { .tv_sec = 0, .tv_nsec = 1000000 };
			clock_nanosleep(CLOCK_MONOTONIC, 0, &one_ms, NULL);
		}
		fvideo->last = 0;
		fvideo = fvideo->next;
	}
	return 0;
}

int gl_capture_init_video_format(gl_capture_t gl_capture,
				struct gl_capture_video_stream_s *video)
{
	/* initialize screen information */
	gl_capture_update_screen(gl_capture, video);

	/* reset gamma values */
	video->gamma_red = video->gamma_green = video->gamma_blue = 1.0;

	if (gl_capture->format == GL_BGRA)
		video->format = GLC_VIDEO_BGRA;
	else
		video->format = GLC_VIDEO_BGR;

	if (gl_capture->pack_alignment == 8)
		__sync_or_and_fetch(&video->flags, GLC_VIDEO_DWORD_ALIGNED);

	return 0;
}

int gl_capture_write_video_format_message(gl_capture_t gl_capture,
			struct gl_capture_video_stream_s *video,
			unsigned int w, unsigned int h)
{
	glc_message_header_t msg;
	glc_video_format_message_t format_msg;

	gl_capture_calc_geometry(gl_capture, video, w, h);

	glc_log(gl_capture->glc, GLC_INFO, "gl_capture",
		 "creating/updating configuration for video %d", video->id);

	msg.type = GLC_MESSAGE_VIDEO_FORMAT;
	format_msg.flags  = video->flags &
			    ~(GLC_VIDEO_CAPTURING|GLC_VIDEO_NEED_COLOR_UPDATE);
	format_msg.format = video->format;
	format_msg.id     = video->id;
	format_msg.width  = video->cw;
	format_msg.height = video->ch;

	ps_packet_open(&video->packet, PS_PACKET_WRITE);
	ps_packet_write(&video->packet, &msg, sizeof(glc_message_header_t));
	ps_packet_write(&video->packet, &format_msg, sizeof(glc_video_format_message_t));
	ps_packet_close(&video->packet);

	glc_log(gl_capture->glc, GLC_DEBUG, "gl_capture",
		 "video %d: %ux%u (%ux%u), 0x%02x flags", video->id,
		 video->cw, video->ch, video->w, video->h, video->flags);

	if (gl_capture->flags & GL_CAPTURE_USE_PBO) {
		if (video->pbo)
			gl_capture_destroy_pbo(gl_capture, video);

		if (gl_capture_create_pbo(gl_capture, video)) {
			gl_capture->flags &= ~(GL_CAPTURE_TRY_PBO | GL_CAPTURE_USE_PBO);
			/** \todo destroy pbo stuff? */
			/** \todo race condition? */
		}
	}

	return 0;
}

int gl_capture_update_video_stream(gl_capture_t gl_capture,
			  struct gl_capture_video_stream_s *video)
{
	unsigned int w, h;

	/* initialize PBO if not already done */
	if (unlikely((!(gl_capture->flags & GL_CAPTURE_USE_PBO)) &&
	    (gl_capture->flags & GL_CAPTURE_TRY_PBO))) {
		pthread_mutex_lock(&gl_capture->mutex);

		/* retest after acquiring the lock  */
		if (unlikely((!(gl_capture->flags & GL_CAPTURE_USE_PBO)) &&
			     (gl_capture->flags & GL_CAPTURE_TRY_PBO))) {

			if (!gl_capture_init_pbo(gl_capture))
				gl_capture->flags |= GL_CAPTURE_USE_PBO;
			else
				gl_capture->flags &= ~GL_CAPTURE_TRY_PBO;
		}

		pthread_mutex_unlock(&gl_capture->mutex);
	}

	gl_capture_get_geometry(gl_capture, video->dpy,
				video->attribWin ? video->attribWin : video->drawable,
				&w, &h);

	if (unlikely(!video->format)) {
		gl_capture_init_video_format(gl_capture,video);
	}

	if (unlikely((w != video->w) || (h != video->h))) {
		gl_capture_write_video_format_message(gl_capture, video, w, h);
	}

	/* how about color correction? */
	if (unlikely(video->flags & GLC_VIDEO_NEED_COLOR_UPDATE))
		gl_capture_update_color(gl_capture, video, &video->packet);

	if (unlikely((gl_capture->flags & GL_CAPTURE_DRAW_INDICATOR) &&
	    (!video->indicator_list)))
		gl_capture_gen_indicator_list(gl_capture, video);

	return 0;
}

/*
 * multithreading notes:
 *
 * This function could be accessed concurrently for different video streams, with
 * the pair dpy,drawable identifying each stream.
 */
int gl_capture_frame(gl_capture_t gl_capture, Display *dpy, GLXDrawable drawable)
{
	struct gl_capture_video_stream_s *video;
	glc_message_header_t msg;
	glc_video_frame_header_t pic;
	glc_utime_t now;
	glc_utime_t before_capture = 0, after_capture = 0;
	char *dma;
	int ret = 0;

	spin_lock(&gl_capture->capture_spinlock);

	if (!(gl_capture->flags & GL_CAPTURE_CAPTURING)) {
		spin_unlock(&gl_capture->capture_spinlock);
		return 0; /* capturing not active */
	}

	gl_capture_get_video_stream(gl_capture, &video, dpy, drawable);
	spin_unlock(&gl_capture->capture_spinlock);

	/* get current time */
	if (unlikely(gl_capture->flags & GL_CAPTURE_IGNORE_TIME))
		now = video->last + gl_capture->fps_period;
	else
		now = glc_state_time(gl_capture->glc);

	/* has gl_capture->fps nanoseconds elapsed since last capture */
	if ((now - video->last < gl_capture->fps_period) &&
	    !(gl_capture->flags & GL_CAPTURE_LOCK_FPS) &&
	    !(gl_capture->flags & GL_CAPTURE_IGNORE_TIME))
		goto finish;

	if (unlikely(video->last && now - video->last > 8*gl_capture->fps_period))
		glc_log(gl_capture->glc, GLC_WARN, "gl_capture",
			"first frame after %" PRIu64 " nsec",
			now - video->last);

	/* not really needed until now */
	gl_capture_update_video_stream(gl_capture, video);
	video->num_frames++;

	/* if PBO is not active, just start transfer and finish */
	if (unlikely((gl_capture->flags & GL_CAPTURE_USE_PBO) &&
	     __sync_bool_compare_and_swap(&video->pbo_active,0,1))) {
		ret = gl_capture_start_pbo(gl_capture, video);
		video->pbo_time = now;
		goto finish;
	}

	if (unlikely(ps_packet_open(&video->packet,
				((gl_capture->flags & GL_CAPTURE_LOCK_FPS) ||
				(gl_capture->flags & GL_CAPTURE_IGNORE_TIME)) ?
				(PS_PACKET_WRITE) :
				(PS_PACKET_WRITE | PS_PACKET_TRY))))
		goto finish;

	if (unlikely((ret = ps_packet_setsize(&video->packet, video->row * video->ch
						+ sizeof(glc_message_header_t)
						+ sizeof(glc_video_frame_header_t)))))
		goto cancel;

	msg.type = GLC_MESSAGE_VIDEO_FRAME;
	if (unlikely((ret = ps_packet_write(&video->packet,
					    &msg, sizeof(glc_message_header_t)))))
		goto cancel;

	/*
	 * if we are using PBO we will actually write previous picture to buffer.
	 * Also, make sure that pbo_time is not in the future. This could happen if
	 * the state time is reset by reloading the capture between a pbo start
	 * and a pbo read.
	 */
	pic.time = (gl_capture->flags & GL_CAPTURE_USE_PBO &&
		    video->pbo_time < now)?video->pbo_time:now;
	pic.id   = video->id;
	if (unlikely((ret = ps_packet_write(&video->packet,
					    &pic, sizeof(glc_video_frame_header_t)))))
		goto cancel;

	if (video->gather_stats)
		before_capture = glc_state_time(gl_capture->glc);
	if (gl_capture->flags & GL_CAPTURE_USE_PBO) {
		if (unlikely((ret = gl_capture_read_pbo(gl_capture, video))))
			goto cancel;

		ret = gl_capture_start_pbo(gl_capture, video);
		video->pbo_time = now;
	} else {
		if (unlikely((ret = ps_packet_dma(&video->packet, (void *) &dma,
					video->row * video->ch, PS_ACCEPT_FAKE_DMA))))
			goto cancel;

		ret = gl_capture_get_pixels(gl_capture, video, dma);
	}
	if (video->gather_stats) {
		after_capture = glc_state_time(gl_capture->glc);
		video->capture_time_ns += after_capture - before_capture;
	}

	ps_packet_close(&video->packet);
	video->num_captured_frames++;
	now = glc_state_time(gl_capture->glc);

	if (unlikely((gl_capture->flags & GL_CAPTURE_LOCK_FPS) &&
		    !(gl_capture->flags & GL_CAPTURE_IGNORE_TIME))) {
		if (now - video->last < gl_capture->fps_period) {
			struct timespec ts = { .tv_sec  = (gl_capture->fps_period + video->last - now)/1000000000,
					       .tv_nsec = (gl_capture->fps_period + video->last - now)%1000000000 };
			clock_nanosleep(CLOCK_MONOTONIC, 0, &ts,NULL);
		}
	}

	/* increment by 1/fps seconds */
	video->last += gl_capture->fps_period;
	if (video->num_captured_frames%gl_capture->fps_rem_period == 0)
		video->last += gl_capture->fps_rem;

finish:
	gl_capture_release_video_stream(video);
	if (unlikely(ret != 0))
		gl_capture_error(gl_capture, ret);

	if (gl_capture->flags & GL_CAPTURE_DRAW_INDICATOR)
		glCallList(video->indicator_list);

	return ret;
cancel:
	if (ret == EBUSY) {
		ret = 0;
		glc_log(gl_capture->glc, GLC_INFO, "gl_capture",
			"dropped frame #%u, buffer not ready",
			video->num_frames);
	}
	ps_packet_cancel(&video->packet);
	goto finish;
}

int gl_capture_refresh_color_correction(gl_capture_t gl_capture)
{
	struct gl_capture_video_stream_s *video;

	glc_log(gl_capture->glc, GLC_INFO, "gl_capture",
		"refreshing color correction");

	video = gl_capture->video;
	while (video != NULL) {
		__sync_or_and_fetch(&video->flags, GLC_VIDEO_NEED_COLOR_UPDATE);
		video = video->next;
	}

	return 0;
}

/** \todo support GammaRamp */
int gl_capture_update_color(gl_capture_t gl_capture,
			    struct gl_capture_video_stream_s *video,
			    ps_packet_t *packet)
{
	glc_message_header_t msg_hdr;
	glc_color_message_t msg;
	XF86VidModeGamma gamma;
	int ret = 0;

	__sync_and_and_fetch(&video->flags, ~GLC_VIDEO_NEED_COLOR_UPDATE);

	XF86VidModeGetGamma(video->dpy, video->screen, &gamma);

	if ((gamma.red == video->gamma_red) &&
	    (gamma.green == video->gamma_green) &&
	    (gamma.blue == video->gamma_blue))
		return 0; /* nothing to update */

	msg_hdr.type = GLC_MESSAGE_COLOR;
	msg.id = video->id;
	video->gamma_red   = msg.red   = gamma.red;
	video->gamma_green = msg.green = gamma.green;
	video->gamma_blue  = msg.blue  = gamma.blue;

	/** \todo figure out brightness and contrast */
	msg.brightness = msg.contrast = 0;

	glc_log(gl_capture->glc, GLC_INFO, "gl_capture",
		"color correction: brightness=%f, contrast=%f, red=%f, green=%f, blue=%f",
		 msg.brightness, msg.contrast, msg.red, msg.green, msg.blue);

	if (unlikely((ret = ps_packet_open(packet, PS_PACKET_WRITE))))
		goto err;
	if (unlikely((ret = ps_packet_write(packet,
				&msg_hdr, sizeof(glc_message_header_t)))))
		goto err;
	if (unlikely((ret = ps_packet_write(packet,
				&msg, sizeof(glc_color_message_t)))))
		goto err;
	if (unlikely((ret = ps_packet_close(packet))))
		goto err;

	return 0;

err:
	ps_packet_cancel(packet);

	glc_log(gl_capture->glc, GLC_ERROR, "gl_capture",
		 "can't write gamma correction information to buffer: %s (%d)",
		 strerror(ret), ret);
	return ret;
}

int gl_capture_set_attribute_window(gl_capture_t gl_capture, Display *dpy,
				    GLXDrawable drawable, Window window)
{
	struct gl_capture_video_stream_s *video;
	gl_capture_get_video_stream(gl_capture, &video, dpy, drawable);

	glc_log(gl_capture->glc, GLC_INFO, "gl_capture",
		"setting attribute window %p for drawable %p",
		(void *) window, (void *) drawable);
	video->attribWin = window;
	return 0;
}

/**  \} */

