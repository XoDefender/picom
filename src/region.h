// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2018 Yuxuan Shui <yshuiv7@gmail.com>
#pragma once
#include <pixman.h>
#include <stdio.h>
#include <stdlib.h>
#include <xcb/xcb.h>

#include "log.h"
#include "utils.h"

typedef struct pixman_region32 pixman_region32_t;
typedef struct pixman_box32 pixman_box32_t;
typedef pixman_region32_t region_t;
typedef pixman_box32_t rect_t;

RC_TYPE(region_t, rc_region, pixman_region32_init, pixman_region32_fini, static inline)

static inline void dump_region(const region_t *x) {
	if (log_get_level_tls() > LOG_LEVEL_TRACE) {
		return;
	}
	int nrects;
	const rect_t *rects = pixman_region32_rectangles((region_t *)x, &nrects);
	log_trace("nrects: %d", nrects);
	for (int i = 0; i < nrects; i++)
		log_trace("(%d, %d) - (%d, %d)", rects[i].x1, rects[i].y1, rects[i].x2,
		          rects[i].y2);
}

/// Convert one xcb rectangle to our rectangle type
static inline rect_t from_x_rect(const xcb_rectangle_t *rect) {
	return (rect_t){
	    .x1 = rect->x,
	    .y1 = rect->y,
	    .x2 = rect->x + rect->width,
	    .y2 = rect->y + rect->height,
	};
}

/// Convert an array of xcb rectangles to our rectangle type
/// Returning an array that needs to be freed
static inline rect_t *from_x_rects(int nrects, const xcb_rectangle_t *rects) {
	rect_t *ret = ccalloc(nrects, rect_t);
	for (int i = 0; i < nrects; i++) {
		ret[i] = from_x_rect(rects + i);
	}
	return ret;
}

/**
 * Resize a region.
 */
static inline void _resize_region(const region_t *region, region_t *output, int dx,
		int dy) {
	if (!region || !output) {
		return;
	}
	if (!dx && !dy) {
		if (region != output) {
			pixman_region32_copy(output, (region_t *)region);
		}
		return;
	}
	// Loop through all rectangles
	int nrects;
	int nnewrects = 0;
	const rect_t *rects = pixman_region32_rectangles((region_t *)region, &nrects);
	auto newrects = ccalloc(nrects, rect_t);
	for (int i = 0; i < nrects; i++) {
		int x1 = rects[i].x1 - dx;
		int y1 = rects[i].y1 - dy;
		int x2 = rects[i].x2 + dx;
		int y2 = rects[i].y2 + dy;
		int wid = x2 - x1;
		int hei = y2 - y1;
		if (wid <= 0 || hei <= 0) {
			continue;
		}
		newrects[nnewrects] =
		    (rect_t){.x1 = x1, .x2 = x2, .y1 = y1, .y2 = y2};
		++nnewrects;
	}

	pixman_region32_fini(output);
	pixman_region32_init_rects(output, newrects, nnewrects);

	free(newrects);
}

static inline region_t resize_region(const region_t *region, int dx, int dy) {
	region_t ret;
	pixman_region32_init(&ret);
	_resize_region(region, &ret, dx, dy);
	return ret;
}

static inline void resize_region_in_place(region_t *region, int dx, int dy) {
	return _resize_region(region, region, dx, dy);
}

static inline rect_t region_translate_rect(rect_t rect, struct coord origin) {
	return (rect_t){
	    .x1 = rect.x1 + origin.x,
	    .y1 = rect.y1 + origin.y,
	    .x2 = rect.x2 + origin.x,
	    .y2 = rect.y2 + origin.y,
	};
}

static inline void log_region_(enum log_level level, const char *func, const region_t *x) {
	if (level < log_get_level_tls()) {
		return;
	}

	int nrects;
	const rect_t *rects = pixman_region32_rectangles((region_t *)x, &nrects);
	if (nrects == 0) {
		log_printf(tls_logger, level, func, "\t(empty)");
		return;
	}
	for (int i = 0; i < min2(nrects, 3); i++) {
		log_printf(tls_logger, level, func, "\t(%d, %d) - (%d, %d)", rects[i].x1,
		           rects[i].y1, rects[i].x2, rects[i].y2);
	}
	if (nrects > 3) {
		auto extent = pixman_region32_extents(x);
		log_printf(tls_logger, level, func, "\t...");
		log_printf(tls_logger, level, func, "\ttotal: (%d, %d) - (%d, %d)",
		           extent->x1, extent->y1, extent->x2, extent->y2);
	}
}

#define log_region(level, x) log_region_(LOG_LEVEL_##level, __func__, x)

/// Subtract `other`, placed at `origin`, from `region`.
static inline void
region_subtract(region_t *region, struct coord origin, const region_t *other) {
	pixman_region32_translate(region, -origin.x, -origin.y);
	pixman_region32_subtract(region, region, other);
	pixman_region32_translate(region, origin.x, origin.y);
}

/// Union `region` with `other` placed at `origin`.
static inline void region_union(region_t *region, struct coord origin, const region_t *other) {
	pixman_region32_translate(region, -origin.x, -origin.y);
	pixman_region32_union(region, region, other);
	pixman_region32_translate(region, origin.x, origin.y);
}

/// Intersect `region` with `other` placed at `origin`.
static inline void
region_intersect(region_t *region, struct coord origin, const region_t *other) {
	pixman_region32_translate(region, -origin.x, -origin.y);
	pixman_region32_intersect(region, region, other);
	pixman_region32_translate(region, origin.x, origin.y);
}

/// Calculate the symmetric difference of `region1`, and `region2` placed at
/// `origin2`, and union the result into `result`.
///
/// @param scratch a region to store temporary results
static inline void
region_symmetric_difference(region_t *result, region_t *scratch, struct coord origin1,
                            const region_t *region1, struct coord origin2,
                            const region_t *region2) {
	pixman_region32_copy(scratch, region1);
	region_subtract(scratch, coord_sub(origin2, origin1), region2);
	region_union(result, origin1, scratch);

	pixman_region32_copy(scratch, region2);
	region_subtract(scratch, coord_sub(origin1, origin2), region1);
	region_union(result, origin2, scratch);
}