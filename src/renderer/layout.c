// SPDX-License-Identifier: MPL-2.0
// Copyright (c) Yuxuan Shui <yshuiv7@gmail.com>
#include <stddef.h>
#include <uthash.h>

//#include "command_builder.h"
#include "common.h"
#include "list.h"
#include "region.h"
#include "types.h"
#include "utils.h"
#include "win.h"

#include "layout.h"

/// Compute layout of a layer from a window. Returns false if the window is not
/// visible / should not be rendered. `out_layer` is modified either way.
static bool
layer_from_window(struct layer *out_layer, struct managed_win *w, struct geometry size) {
	bool to_paint = false;
	if (!w->ever_damaged || w->paint_excluded) {
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

	out_layer->opacity = (float)w->opacity;
	out_layer->blur_opacity = (float)(w->opacity / w->opacity_target_old);
	if (out_layer->opacity == 0 && out_layer->blur_opacity == 0) {
		goto out;
	}

	pixman_region32_copy(&out_layer->damaged, &w->damaged);
	pixman_region32_translate(&out_layer->damaged, out_layer->origin.x,
	                          out_layer->origin.y);
	// TODO(yshui) Is there a better way to handle shaped windows? Shaped windows can
	// have a very large number of rectangles in their shape, we don't want to handle
	// that and slow ourselves down. so we treat them as transparent and just use
	// their extent rectangle.
	out_layer->is_opaque = !win_has_alpha(w) && out_layer->opacity == 1.0F && !w->bounding_shaped;
	out_layer->is_clipping = w->transparent_clipping;
	out_layer->next_rank = -1;
	out_layer->prev_rank = -1;
	out_layer->key = (struct layer_key){.window = w->base.id};
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
	*layout = (struct layout){};
}

struct layout_manager *layout_manager_new(unsigned max_buffer_age) {
	struct layout_manager *planner = malloc(sizeof(struct layout_manager) + (max_buffer_age + 1) * sizeof(struct layout));
	planner->max_buffer_age = max_buffer_age + 1;
	planner->current = 0;
	planner->layer_indices = NULL;
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
	pixman_region32_fini(&lm->scratch_region);
	free(lm);
}

void layout_manager_append_layout(struct layout_manager *lm, struct list_node* window_stack, uint64_t root_pixmap_generation, struct geometry size) 
{
	auto prev_layout = &lm->layouts[lm->current];
	lm->current = (lm->current + 1) % lm->max_buffer_age;
	auto layout = &lm->layouts[lm->current];
	layout->root_image_generation = root_pixmap_generation;

	unsigned count = 0;
	list_foreach(struct win, w, window_stack, stack_neighbour) {
		if (w->managed) {
			count += 1;
		}
	}

	unsigned nlayers = count;
	if (nlayers > layout->capacity) 
	{
		struct layer *new_layers = realloc(layout->layers, nlayers * sizeof(struct layer));
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
	for (struct list_node *cursor = window_stack->prev; cursor != window_stack; cursor = cursor->prev) 
	{
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

void layout_manager_mark_layers_with_to_paint(struct layout_manager *lm, region_t reg_scratch)
{
	pixman_region32_copy(&lm->scratch_region, &reg_scratch);
	struct layout curr_layout = lm->layouts[lm->current];
	for(auto i = curr_layout.len - 1; i; i--)
	{
		auto curr_layer = curr_layout.layers[i];
		auto reg_bound_curr = win_get_bounding_shape_global_by_val(curr_layer.win);

		pixman_region32_intersect(&reg_bound_curr, &reg_bound_curr, &lm->scratch_region);
		if(!pixman_region32_not_empty(&reg_bound_curr)) {
			curr_layer.win->to_paint = false;
		}

		if(curr_layer.is_opaque) {
			pixman_region32_subtract(&lm->scratch_region, &lm->scratch_region, &reg_bound_curr);
		}
			
		pixman_region32_fini(&reg_bound_curr);
	}
}