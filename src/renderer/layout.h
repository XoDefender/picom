// SPDX-License-Identifier: MPL-2.0
// Copyright (c) Yuxuan Shui <yshuiv7@gmail.com>
#pragma once
#include <pixman.h>
#include <stdint.h>
#include <xcb/xproto.h>
#include "backend/backend.h"
#include "region.h"
#include "types.h"

struct layer_key {
	/// Window ID
	xcb_window_t window;
	// explicit padding because this will be used as hash table key
	uint32_t pad;       
};

/// A layer to be rendered in a render layout
struct layer {
	/// Window that will be rendered in this layer
	struct layer_key key;
	/// The window, this is only valid for the current layout. Once
	/// a frame has passed, windows could have been freed.
	struct managed_win *win;
	/// Damaged region of this layer, in screen coordinates
	region_t damaged;
	/// Origin (the top left outmost corner) of the window in screen coordinates
	struct coord origin;
	/// Size of the window
	struct geometry size;
	/// Origin of the shadow in screen coordinates
	struct coord shadow_origin;
	/// Size of the shadow
	struct geometry shadow_size;
	/// Opacity of this window
	float opacity;
	/// Opacity of the background blur of this window
	float blur_opacity;

	/// How many commands are needed to render this layer
	unsigned number_of_commands;

	/// Rank of this layer in the previous frame, -1 if this window
	/// appears in this frame for the first time
	int prev_rank;
	/// Rank of this layer in the next frame, -1 if this window is
	/// removed in the next frame
	int next_rank;

	/// Is this window completely opaque?
	bool is_opaque;
	/// Is this window clipping the windows beneath it?
	bool is_clipping;

	// Is this layer intended for painting
	bool to_paint;
};

/// Layout of windows at a specific frame
struct layout {
	struct geometry size;
	/// The root image generation, see `struct session::root_image_generation`
	uint64_t root_image_generation;
	/// Number of layers in `layers`
	unsigned len;
	/// Capacity of `layers`
	unsigned capacity;
	/// Layers as a flat array, from bottom to top in stack order.
	struct layer *layers;
	/// Number of commands in `commands`
	unsigned number_of_commands;
	/// Where does the commands for the bottom most layer start.
	/// Any commands before that is for the desktop background.
	unsigned first_layer_start;
	/// Commands that are needed to render this layout. Commands
	/// are recorded in the same order as the layers they correspond to. Each layer
	/// can have 0 or more commands associated with it.
	struct backend_command *commands;
};

struct layer_index {
	UT_hash_handle hh;
	struct layer_key key;
	unsigned index;
	struct list_node free_list;
};

// ## Layout manager Concepts
//
// - "layer", because windows form a stack, it's easy to think of the final screen as
//   a series of layers stacked on top of each other. Each layer is the same size as
//   the screen, and contains a single window positioned somewhere in the layer. Other
//   parts of the layer are transparent.
//   When talking about "screen at a certain layer", we mean the result you would get
//   if you stack all layers from the bottom up to that certain layer, ignoring any layers above.
struct layout_manager {
	unsigned max_buffer_age;
	/// Index of the most recent layout in `layouts`.
	unsigned current;
	/// Mapping from window to its index in the current layout.
	struct layer_index *layer_indices;
	struct list_node free_indices;

	// internal
	/// Scratch region used for calculations, to avoid repeated allocations.
	region_t scratch_region;
	/// Current and past layouts, at most `max_buffer_age` layouts are stored.
	struct layout layouts[];
};

/// Compute the layout of windows to be rendered in the current frame, and append it to
/// the end of layout manager's ring buffer.  The layout manager has a ring buffer of
/// layouts, with its size chosen at creation time. Calling this will push at new layout
/// at the end of the ring buffer, and remove the oldest layout if the buffer is full.
void layout_manager_append_layout(struct layout_manager *lm, struct list_node* window_stack, uint64_t root_image_generation, struct geometry size);
/// Get the layout `age` frames into the past. Age `0` is the most recently appended layout.
struct layout *layout_manager_layout(struct layout_manager *lm, unsigned age);
void layout_manager_free(struct layout_manager *lm);
/// Create a new render lm with a ring buffer for `max_buffer_age` layouts.
struct layout_manager *layout_manager_new(unsigned max_buffer_age);
// Calculate the layer`s region, decide whether it is visible and mark it with to_paint
// It is not visible if does not intersect with reg_scratch (obscured by some layer higher on stack)
void layout_manager_mark_obscured_layers(struct layout_manager *lm, region_t *reg_scratch);