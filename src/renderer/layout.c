// SPDX-License-Identifier: MPL-2.0
// Copyright (c) Yuxuan Shui <yshuiv7@gmail.com>
#include <stddef.h>
#include <uthash.h>

#include "command_builder.h"
#include "common.h"

#include "list.h"
#include "region.h"
#include "types.h"
#include "utils.h"
#include "win.h"
#include "layout.h"

struct layer_index {
	UT_hash_handle hh;
	struct layer_key key;
	unsigned index;
	struct list_node free_list;
};
struct layout_manager {
	unsigned max_buffer_age;
	/// Index of the most recent layout in `layouts`.
	unsigned current;
	/// Mapping from window to its index in the current layout.
	struct layer_index *layer_indices;
	/// Output render plan.
	struct render_plan *plan;
	unsigned plan_capacity;
	struct list_node free_indices;

	// internal
	/// Scratch region used for calculations, to avoid repeated allocations.
	region_t scratch_region;
	/// Current and past layouts, at most `max_buffer_age` layouts are stored.
	struct layout layouts[];
};

/// Compute layout of a layer from a window. Returns false if the window is not
/// visible / should not be rendered. `out_layer` is modified either way.
static bool
layer_from_window(struct layer *out_layer, struct managed_win *w, struct geometry size) {
	bool to_paint = false;
	if (!w->ever_damaged) {
		goto out;
	}

	out_layer->origin = (struct coord){.x = w->g.x, .y = w->g.y};
	out_layer->size = (struct geometry){.width = w->widthb, .height = w->heightb};
	if (w->shadow) {
		out_layer->shadow_origin =
		    (struct coord){.x = w->g.x + w->shadow_dx, .y = w->g.y + w->shadow_dy};
		out_layer->shadow_size =
		    (struct geometry){.width = w->shadow_width, .height = w->shadow_height};
	} else {
		out_layer->shadow_origin = (struct coord){};
		out_layer->shadow_size = (struct geometry){};
	}
	if (out_layer->size.width <= 0 || out_layer->size.height <= 0) {
		goto out;
	}
	if (out_layer->size.height + out_layer->origin.y <= 0 ||
	    out_layer->size.width + out_layer->origin.x <= 0 ||
	    out_layer->origin.y >= size.height || out_layer->origin.x >= size.width) {
		goto out;
	}

	out_layer->opacity = w->opacity;
	out_layer->blur_opacity = w->opacity / w->opacity_target;

	if (out_layer->opacity == 0) {
		goto out;
	}

	pixman_region32_copy(&out_layer->damaged, &w->damaged);
	pixman_region32_translate(&out_layer->damaged, out_layer->origin.x,
	                          out_layer->origin.y);
	// TODO(yshui) Is there a better way to handle shaped windows? Shaped windows can
	// have a very large number of rectangles in their shape, we don't want to handle
	// that and slow ourselves down. so we treat them as transparent and just use
	// their extent rectangle.
	out_layer->is_opaque =
	    !win_has_alpha(w) && out_layer->opacity == 1.0F && !w->bounding_shaped;
	out_layer->is_clipping = w->transparent_clipping;
	out_layer->next_rank = -1;
	out_layer->prev_rank = -1;
	out_layer->key =
	    (struct layer_key){.window = w->base.id};
	out_layer->win = w;
	to_paint = true;

out:
	pixman_region32_clear(&w->damaged);
	return to_paint;
}

static void layout_deinit(struct layout *layout) {
	for (unsigned i = 0; i < layout->len; i++) {
		pixman_region32_fini(&layout->layers[i].damaged);
	}
	free(layout->layers);
	command_builder_command_list_free(layout->commands);
	*layout = (struct layout){};
}

struct layout_manager *layout_manager_new(unsigned max_buffer_age) {
	struct layout_manager *planner = malloc(
	    sizeof(struct layout_manager) + (max_buffer_age + 1) * sizeof(struct layout));
	planner->max_buffer_age = max_buffer_age + 1;
	planner->current = 0;
	planner->layer_indices = NULL;
	planner->plan_capacity = 0;
	planner->plan = NULL;
	list_init_head(&planner->free_indices);
	pixman_region32_init(&planner->scratch_region);
	for (unsigned i = 0; i <= max_buffer_age; i++) {
		planner->layouts[i] = (struct layout){};
	}
	return planner;
}

void layout_manager_free(struct layout_manager *lm) {
	for (unsigned i = 0; i < lm->max_buffer_age; i++) {
		layout_deinit(&lm->layouts[i]);
	}
	struct layer_index *index, *tmp;
	HASH_ITER(hh, lm->layer_indices, index, tmp) {
		HASH_DEL(lm->layer_indices, index);
		free(index);
	}
	list_foreach_safe(struct layer_index, i, &lm->free_indices, free_list) {
		list_remove(&i->free_list);
		free(i);
	}
	for (unsigned i = 0; i < lm->plan_capacity; i++) {
		pixman_region32_fini(&lm->plan[i].render);
	}
	pixman_region32_fini(&lm->scratch_region);
	free(lm->plan);
	free(lm);
}

// ## Layout manager Concepts
//
// - "layer", because windows form a stack, it's easy to think of the final screen as
//   a series of layers stacked on top of each other. Each layer is the same size as
//   the screen, and contains a single window positioned somewhere in the layer. Other
//   parts of the layer are transparent.
//   When talking about "screen at a certain layer", we mean the result you would get
//   if you stack all layers from the bottom up to that certain layer, ignoring any layers
//   above.

void layout_manager_append_layout(struct layout_manager *lm, struct list_node* window_stack, struct geometry size) {
	auto prev_layout = &lm->layouts[lm->current];
	lm->current = (lm->current + 1) % lm->max_buffer_age;
	auto layout = &lm->layouts[lm->current];
	command_builder_command_list_free(layout->commands);

	unsigned count = 0;
	list_foreach(struct win, w, window_stack, stack_neighbour) {
		if (w->managed) {
			count += 1;
		}
	}

	unsigned nlayers = count;

	if (nlayers > layout->capacity) {
		struct layer *new_layers =
		    realloc(layout->layers, nlayers * sizeof(struct layer));
		BUG_ON(new_layers == NULL);
		for (unsigned i = layout->capacity; i < nlayers; i++) {
			pixman_region32_init(&new_layers[i].damaged);
		}
		layout->capacity = nlayers;
		layout->layers = new_layers;
	}

	layout->size = size;

	unsigned rank = 0;
	struct layer_index *index, *next_index;
	for (struct list_node *cursor = window_stack->prev;
	     cursor != window_stack; cursor = cursor->prev) {
		auto w = list_entry(cursor, struct win, stack_neighbour);
		if (!w->managed) {
			continue;
		}
		if (!layer_from_window(&layout->layers[rank], (struct managed_win *)w, size)) {
			continue;
		}

		HASH_FIND(hh, lm->layer_indices, &layout->layers[rank].key,
		          sizeof(layout->layers[rank].key), index);
		if (index) {
			prev_layout->layers[index->index].next_rank = (int)rank;
			layout->layers[rank].prev_rank = (int)index->index;
		}
		rank++;
		assert(rank <= nlayers);
	}
	layout->len = rank;

	// Update indices. If a layer exist in both prev_layout and current layout,
	// we could update the index using next_rank; if a layer no longer exist in
	// current layout, we remove it from the indices.
	HASH_ITER(hh, lm->layer_indices, index, next_index) {
		if (prev_layout->layers[index->index].next_rank == -1) {
			HASH_DEL(lm->layer_indices, index);
			list_insert_after(&lm->free_indices, &index->free_list);
		} else {
			index->index = (unsigned)prev_layout->layers[index->index].next_rank;
		}
	}
	// And finally, if a layer in current layout didn't exist in prev_layout, add a
	// new index for it.
	for (unsigned i = 0; i < layout->len; i++) {
		if (layout->layers[i].prev_rank != -1) {
			continue;
		}
		if (!list_is_empty(&lm->free_indices)) {
			index =
			    list_entry(lm->free_indices.next, struct layer_index, free_list);
			list_remove(&index->free_list);
		} else {
			index = cmalloc(struct layer_index);
		}
		index->key = layout->layers[i].key;
		index->index = i;
		HASH_ADD(hh, lm->layer_indices, key, sizeof(index->key), index);
	}
}

struct layout *layout_manager_layout(struct layout_manager *lm, unsigned age) {
	if (age >= lm->max_buffer_age) {
		assert(false);
		return NULL;
	}
	return &lm->layouts[(lm->current + lm->max_buffer_age - age) % lm->max_buffer_age];
}

void layout_manager_collect_window_damage(const struct layout_manager *lm, unsigned index,
                                          unsigned buffer_age, region_t *damage) {
	unsigned int curr = lm->current;
	struct layer* layer = &lm->layouts[curr].layers[index];
	for (unsigned i = 0; i < buffer_age; i++) {
		pixman_region32_union(damage, damage, &layer->damaged);
		curr = (curr + lm->max_buffer_age - 1) % lm->max_buffer_age;
		assert(layer->prev_rank >= 0);
		layer = &lm->layouts[curr].layers[layer->prev_rank];
	}
}

unsigned layout_manager_max_buffer_age(const struct layout_manager *lm) {
	return lm->max_buffer_age - 1;
}

int layer_prev_rank(struct layout_manager *lm, unsigned buffer_age, unsigned index_) {
	int index = to_int_checked(index_);
	unsigned layout = lm->current;
	while (buffer_age--) {
		index = lm->layouts[layout].layers[index].prev_rank;
		if (index < 0) {
			break;
		}
		layout = (layout + lm->max_buffer_age - 1) % lm->max_buffer_age;
	}
	return index;
}

int layer_next_rank(struct layout_manager *lm, unsigned buffer_age, unsigned index_) {
	int index = to_int_checked(index_);
	unsigned layout = (lm->current + lm->max_buffer_age - buffer_age) % lm->max_buffer_age;
	while (buffer_age--) {
		index = lm->layouts[layout].layers[index].next_rank;
		if (index < 0) {
			break;
		}
		layout = (layout + 1) % lm->max_buffer_age;
	}
	return index;
}