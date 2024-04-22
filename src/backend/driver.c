// SPDX-License-Identifier: MPL-2.0
// Copyright (c) Yuxuan Shui <yshuiv7@gmail.com>
#include <stdlib.h>
#include <string.h>

#include <xcb/randr.h>
#include <xcb/xcb.h>

#include "backend/backend.h"
#include "backend/driver.h"
#include "common.h"
#include "compiler.h"
#include "log.h"

bool is_software_render(enum driver driver)
{
	if(driver & DRIVER_LLVMPIPE || driver & DRIVER_SOFTPIPE || driver & DRIVER_SWRAST)
		return true;
	else 
		return false;
}

/// Apply driver specified global workarounds. It's safe to call this multiple times.
void apply_driver_workarounds(struct session *ps) {
	if (bkend_use_glx(ps) && !ps->o.force_glx && is_software_render(ps->drivers))
		ps->o.backend = BKEND_XRENDER;

	if (ps->drivers & DRIVER_NVIDIA) {
		setenv("__GL_MaxFramesAllowed", "1", true);
		ps->o.xrender_sync_fence = true;
	}
}

void detect_driver_ddx(xcb_connection_t *c, xcb_window_t window, enum driver* ret) {
	// First we try doing backend agnostic detection using RANDR
	// There's no way to query the X server about what driver is loaded, so RANDR is
	// our best shot.
	auto randr_version = xcb_randr_query_version_reply(
	    c, xcb_randr_query_version(c, XCB_RANDR_MAJOR_VERSION, XCB_RANDR_MINOR_VERSION),
	    NULL);
	if (randr_version &&
	    (randr_version->major_version > 1 || randr_version->minor_version >= 4)) {
		auto r = xcb_randr_get_providers_reply(
		    c, xcb_randr_get_providers(c, window), NULL);
		if (r == NULL) {
			log_warn("Failed to get RANDR providers");
			free(randr_version);
			return;
		}

		auto providers = xcb_randr_get_providers_providers(r);
		for (auto i = 0; i < xcb_randr_get_providers_providers_length(r); i++) 
		{
			auto r2 = xcb_randr_get_provider_info_reply(
			    c, xcb_randr_get_provider_info(c, providers[i], r->timestamp), NULL);
			if (r2 == NULL) {
				continue;
			}
			if (r2->num_outputs == 0) {
				free(r2);
				continue;
			}

			auto name_len = xcb_randr_get_provider_info_name_length(r2);
			assert(name_len >= 0);
			auto name = strndup(xcb_randr_get_provider_info_name(r2), (size_t)name_len);

			if (strcasestr(name, "modesetting") != NULL) {
				*ret |= DRIVER_MODESETTING;
			} else if (strcasestr(name, "Radeon") != NULL) {
				// Be conservative, add both radeon drivers
				*ret |= DRIVER_AMDGPU | DRIVER_RADEON;
			} else if (strcasestr(name, "NVIDIA") != NULL) {
				*ret |= DRIVER_NVIDIA;
			} else if (strcasestr(name, "nouveau") != NULL) {
				*ret |= DRIVER_NOUVEAU;
			} else if (strcasestr(name, "Intel") != NULL) {
				*ret |= DRIVER_INTEL;
			}
			free(name);
			free(r2);
		}
		free(r);
	}
	free(randr_version);
}

void detect_driver_opengl(session_t *ps, enum driver* ret) 
{
	// Do not check the context, assume it exists as there are no errors
	if(ps->backend_data || ps->psglx)
		return;

	int nitems = 0;
	XVisualInfo vreq = {.visualid = ps->vis};
    XVisualInfo *visual_info = XGetVisualInfo(ps->dpy, VisualIDMask, &vreq, &nitems);
	
    GLXContext gl_context = glXCreateContext(ps->dpy, visual_info, NULL, GL_TRUE);
    glXMakeCurrent(ps->dpy, ps->root, gl_context);

	const char *renderer = (const char *)glGetString(GL_RENDERER);
	if(!renderer) return;
	if(!strncmp(renderer, "llvmpipe", strlen("llvmpipe")))
		*ret |= DRIVER_LLVMPIPE;
	else if(!strncmp(renderer, "softpipe", strlen("softpipe")))
		*ret |= DRIVER_SOFTPIPE;
	else if(!strncmp(renderer, "Software Rasterizer", strlen("Software Rasterizer")))
		*ret |= DRIVER_SWRAST;

	XFree(visual_info);
	glXMakeCurrent(ps->dpy, None, NULL);
    glXDestroyContext(ps->dpy, gl_context);
}

enum driver detect_driver(struct session *ps) 
{
	enum driver ret = 0;
	
	detect_driver_ddx(ps->c, ps->root, &ret);
	detect_driver_opengl(ps, &ret);

	if (ps->backend_data && ps->backend_data->ops->detect_driver) {
		ret |= ps->backend_data->ops->detect_driver(ps->backend_data);
	}

	return ret;
}