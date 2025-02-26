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

/// Apply driver specified global workarounds. It's safe to call this multiple times.
void apply_driver_workarounds(struct session *ps) 
{
	bool force_xrender = false;
	bool force_legacy_glx = false;
	bool force_vsync = false;

	#ifdef ELBRUS
		force_xrender = true;
	#endif

	if (getenv("FLY_VM_NAME") || 
	   (ps->drivers & DRIVER_SOFTWARE) ||
	   (ps->drivers & DRIVER_NOUVEAU)) {
		force_xrender = true;
	}
	if (ps->drivers & DRIVER_LEGACY_GL) {
		force_legacy_glx = true;
	}
	if (ps->drivers & DRIVER_NVIDIA) {
		setenv("__GL_MaxFramesAllowed", "1", true);
		ps->o.xrender_sync_fence = true;
	}
	if (ps->drivers & DRIVER_INTEL) {
		force_vsync = true;
	}

	if (!is_bkend_ready(ps) && !ps->o.force_glx && force_xrender) {
		ps->o.backend = BKEND_XRENDER;
		log_warn("Artifacts may happen with your drivers, xrender is forced");
	}	

	if (bkend_use_glx(ps)) 
	{
		if (force_legacy_glx) {
			ps->o.legacy_backends = true;
			log_warn("The OpenGL version is < 3.3, legacy glx is forced");
		}
		if (!ps->o.force_no_vsync && force_vsync) {
			ps->o.vsync = true;
			log_warn("Artifacts may happen with your drivers, vsync is forced");
		}
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
	int nitems = 0;
	XVisualInfo vreq = {.visualid = ps->vis};
    XVisualInfo *visual_info = XGetVisualInfo(ps->dpy, VisualIDMask, &vreq, &nitems);
	if (!visual_info) {
		log_error("Failed to acquire XVisualInfo for current visual.");
		return;
	}
	
    GLXContext gl_context = glXCreateContext(ps->dpy, visual_info, NULL, GL_TRUE);
	if (!glXMakeCurrent(ps->dpy, ps->root, gl_context)) {
		log_error("Failed to attach GLX context.");
		XFree(visual_info);
		return;
	}
    
	const char *renderer = (const char *) glGetString(GL_RENDERER);
	const char *vendor = (const char *) glGetString(GL_VENDOR);
	const char *software_renderers[] = {"llvmpipe", "Software Rasterizer", "softpipe"};

	if(renderer && vendor) 
	{
		// Gallium drivers
    	// ====================================================
		for (size_t i = 0; i < ARR_SIZE(software_renderers); i++) 
		{
			if (strstr(renderer, software_renderers[i])) {
				*ret |= DRIVER_SOFTWARE;
				break;
			}
		}

		if(strstr(vendor, "nouveau") || (strstr(vendor, "Mesa") && 
		  (strstr(renderer, "NV")    || strstr(renderer, "GeForce")))) {
			*ret |= DRIVER_NOUVEAU;
		}

		// Mesa classic drivers
		// ====================================================
		else if(strstr(vendor, "Intel") || strstr(renderer, "Intel")) {
			*ret |= DRIVER_INTEL;
		}

		// GL version
		// ====================================================
		GLint major = 0, minor = 0; 
		glGetIntegerv(GL_MAJOR_VERSION, &major); 
		glGetIntegerv(GL_MINOR_VERSION, &minor);
		if (major < 3 || (major == 3 && minor < 3)) {
			*ret |= DRIVER_LEGACY_GL;
    	}
	}
	else {
		log_warn("Failed to get OpenGL info.");
	}

	glXMakeCurrent(ps->dpy, None, NULL);
	glXDestroyContext(ps->dpy, gl_context);
	XFree(visual_info);
}

enum driver detect_driver(struct session *ps) 
{
	enum driver ret = 0;
	
	detect_driver_ddx(ps->c, ps->root, &ret);

	if(!is_bkend_ready(ps) && !getenv("FLY_VM_NAME")) {
		detect_driver_opengl(ps, &ret);
	}

	if (ps->backend_data && ps->backend_data->ops->detect_driver) {
		ret |= ps->backend_data->ops->detect_driver(ps->backend_data);
	}

	return ret;
}