// SPDX-License-Identifier: MPL-2.0
// Copyright (c) Yuxuan Shui <yshuiv7@gmail.com>
#pragma once
#include <X11/Xlib.h>
#include <GL/glx.h>
#include <stdbool.h>
#include <xcb/render.h>
#include <xcb/xcb.h>

#include "compiler.h"
#include "log.h"
#include "utils.h"
#include "x.h"

struct glx_fbconfig_info {
	GLXFBConfig cfg;
	int texture_tgts;
	int texture_fmt;
	int y_inverted;
};

bool glx_find_fbconfig(Display *dpy, int screen, struct xvisual_info m, struct glx_fbconfig_info *info);

struct glxext_info {
	bool initialized;
	bool has_GLX_SGI_video_sync;
	bool has_GLX_SGI_swap_control;
	bool has_GLX_OML_sync_control;
	bool has_GLX_MESA_swap_control;
	bool has_GLX_EXT_swap_control;
	bool has_GLX_EXT_texture_from_pixmap;
	bool has_GLX_ARB_create_context;
	bool has_GLX_EXT_buffer_age;
	bool has_GLX_MESA_query_renderer;
	bool has_GLX_ARB_create_context_robustness;
};

extern struct glxext_info glxext;

extern PFNGLXGETVIDEOSYNCSGIPROC glXGetVideoSyncSGI;
extern PFNGLXWAITVIDEOSYNCSGIPROC glXWaitVideoSyncSGI;
extern PFNGLXGETSYNCVALUESOMLPROC glXGetSyncValuesOML;
extern PFNGLXWAITFORMSCOMLPROC glXWaitForMscOML;
extern PFNGLXSWAPINTERVALEXTPROC glXSwapIntervalEXT;
extern PFNGLXSWAPINTERVALSGIPROC glXSwapIntervalSGI;
extern PFNGLXSWAPINTERVALMESAPROC glXSwapIntervalMESA;
extern PFNGLXBINDTEXIMAGEEXTPROC glXBindTexImageEXT;
extern PFNGLXRELEASETEXIMAGEEXTPROC glXReleaseTexImageEXT;
extern PFNGLXCREATECONTEXTATTRIBSARBPROC glXCreateContextAttribsARB;

#ifdef GLX_MESA_query_renderer
extern PFNGLXQUERYCURRENTRENDERERINTEGERMESAPROC glXQueryCurrentRendererIntegerMESA;
#endif

void glxext_init(Display *, int screen);
