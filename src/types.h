// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2018 Yuxuan Shui <yshuiv7@gmail.com>

#pragma once

/// Some common types

#include <stdint.h>
#include <stdbool.h>

/// Enumeration type to represent switches.
typedef enum {
	OFF = 0,        // false
	ON,             // true
	UNSET
} switch_t;

/// A structure representing margins around a rectangle.
typedef struct {
	int top;
	int left;
	int bottom;
	int right;
} margin_t;

struct color {
	double red, green, blue, alpha;
};

typedef uint32_t opacity_t;

typedef struct geometry {
	int width;
	int height;
} geometry_t;

typedef struct coord {
	int x, y;
} coord_t;

#define MARGIN_INIT                                                                      \
	{ 0, 0, 0, 0 }

static inline struct coord coord_add(struct coord a, struct coord b) {
	return (struct coord){
	    .x = a.x + b.x,
	    .y = a.y + b.y,
	};
}

static inline struct coord coord_sub(struct coord a, struct coord b) {
	return (struct coord){
	    .x = a.x - b.x,
	    .y = a.y - b.y,
	};
}

static inline bool coord_eq(struct coord a, struct coord b) {
	return a.x == b.x && a.y == b.y;
}

static inline struct coord coord_neg(struct coord a) {
	return (struct coord){.x = -a.x, .y = -a.y};
}