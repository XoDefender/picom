// SPDX-License-Identifier: MPL-2.0
// Copyright (c) Yuxuan Shui <yshuiv7@gmail.com>
#include <GL/gl.h>
#include <GL/glext.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <xcb/render.h>        // for xcb_render_fixed_t, XXX

#include "backend/backend.h"
#include "common.h"
#include "compiler.h"
#include "config.h"
#include "kernel.h"
#include "log.h"
#include "region.h"
#include "string_utils.h"
#include "types.h"
#include "utils.h"

#include "backend/backend_common.h"
#include "backend/gl/gl_common.h"

GLuint gl_create_shader(GLenum shader_type, const char *shader_str) {
	log_trace("===\n%s\n===", shader_str);

	bool success = false;
	GLuint shader = glCreateShader(shader_type);
	if (!shader) {
		log_error("Failed to create shader with type %#x.", shader_type);
		goto end;
	}
	glShaderSource(shader, 1, &shader_str, NULL);
	glCompileShader(shader);

	// Get shader status
	{
		GLint status = GL_FALSE;
		glGetShaderiv(shader, GL_COMPILE_STATUS, &status);
		if (status == GL_FALSE) {
			GLint log_len = 0;
			glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &log_len);
			if (log_len) {
				char log[log_len + 1];
				glGetShaderInfoLog(shader, log_len, NULL, log);
				log_error("Failed to compile shader with type %d: %s",
				          shader_type, log);
			}
			goto end;
		}
	}

	success = true;

end:
	if (shader && !success) {
		glDeleteShader(shader);
		shader = 0;
	}
	gl_check_err();

	return shader;
}

GLuint gl_create_program(const GLuint *const shaders, int nshaders) {
	bool success = false;
	GLuint program = glCreateProgram();
	if (!program) {
		log_error("Failed to create program.");
		goto end;
	}

	for (int i = 0; i < nshaders; ++i) {
		glAttachShader(program, shaders[i]);
	}
	glLinkProgram(program);

	// Get program status
	{
		GLint status = GL_FALSE;
		glGetProgramiv(program, GL_LINK_STATUS, &status);
		if (status == GL_FALSE) {
			GLint log_len = 0;
			glGetProgramiv(program, GL_INFO_LOG_LENGTH, &log_len);
			if (log_len) {
				char log[log_len + 1];
				glGetProgramInfoLog(program, log_len, NULL, log);
				log_error("Failed to link program: %s", log);
			}
			goto end;
		}
	}
	success = true;

end:
	if (program) {
		for (int i = 0; i < nshaders; ++i) {
			glDetachShader(program, shaders[i]);
		}
	}
	if (program && !success) {
		glDeleteProgram(program);
		program = 0;
	}
	gl_check_err();

	return program;
}

/**
 * @brief Create a program from NULL-terminated arrays of vertex and fragment shader
 * strings.
 */
GLuint gl_create_program_from_strv(const char **vert_shaders, const char **frag_shaders) {
	int vert_count, frag_count;
	for (vert_count = 0; vert_shaders && vert_shaders[vert_count]; ++vert_count) {
	}
	for (frag_count = 0; frag_shaders && frag_shaders[frag_count]; ++frag_count) {
	}

	GLuint prog = 0;
	auto shaders = (GLuint *)ccalloc(vert_count + frag_count, GLuint);
	for (int i = 0; i < vert_count; ++i) {
		shaders[i] = gl_create_shader(GL_VERTEX_SHADER, vert_shaders[i]);
		if (shaders[i] == 0) {
			goto out;
		}
	}
	for (int i = 0; i < frag_count; ++i) {
		shaders[vert_count + i] =
		    gl_create_shader(GL_FRAGMENT_SHADER, frag_shaders[i]);
		if (shaders[vert_count + i] == 0) {
			goto out;
		}
	}

	prog = gl_create_program(shaders, vert_count + frag_count);

out:
	for (int i = 0; i < vert_count + frag_count; ++i) {
		if (shaders[i] != 0) {
			glDeleteShader(shaders[i]);
		}
	}
	free(shaders);
	gl_check_err();

	return prog;
}

/**
 * @brief Create a program from vertex and fragment shader strings.
 */
GLuint gl_create_program_from_str(const char *vert_shader_str, const char *frag_shader_str) {
	const char *vert_shaders[2] = {vert_shader_str, NULL};
	const char *frag_shaders[2] = {frag_shader_str, NULL};

	return gl_create_program_from_strv(vert_shaders, frag_shaders);
}

void gl_destroy_window_shader(backend_t *backend_data attr_unused, void *shader) {
	if (!shader) {
		return;
	}

	auto pprogram = (gl_win_shader_t *)shader;
	if (pprogram->prog) {
		glDeleteProgram(pprogram->prog);
		pprogram->prog = 0;
	}
	gl_check_err();

	free(shader);
}

/*
 * @brief Implements recursive part of gl_average_texture_color.
 *
 * @note In order to reduce number of textures which needs to be
 * allocated and deleted during this recursive render
 * we reuse the same two textures for render source and
 * destination simply by alterating between them.
 * Unfortunately on first iteration source_texture might
 * be read-only. In this case we will select auxiliary_texture as
 * destination_texture in order not to touch that read-only source
 * texture in following render iteration.
 * Otherwise we simply will switch source and destination textures
 * between each other on each render iteration.
 */
static GLuint
gl_average_texture_color_inner(backend_t *base, GLuint source_texture, GLuint destination_texture,
                          GLuint auxiliary_texture, GLuint fbo, int width, int height) {
	const int max_width = 1;
	const int max_height = 1;
	const int from_width = next_power_of_two(width);
	const int from_height = next_power_of_two(height);
	const int to_width = from_width > max_width ? from_width / 2 : from_width;
	const int to_height = from_height > max_height ? from_height / 2 : from_height;

	// Prepare coordinates
	GLint coord[] = {
	    // top left
	    0, 0,        // vertex coord
	    0, 0,        // texture coord

	    // top right
	    to_width, 0,        // vertex coord
	    width, 0,           // texture coord

	    // bottom right
	    to_width, to_height,        // vertex coord
	    width, height,              // texture coord

	    // bottom left
	    0, to_height,        // vertex coord
	    0, height,           // texture coord
	};
	glBufferSubData(GL_ARRAY_BUFFER, 0, (long)sizeof(*coord) * 16, coord);

	// Prepare framebuffer for new render iteration
	glBindTexture(GL_TEXTURE_2D, destination_texture);
	glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
	                       destination_texture, 0);
	gl_check_fb_complete(GL_FRAMEBUFFER);

	// Bind source texture as downscaling shader uniform input
	glBindTexture(GL_TEXTURE_2D, source_texture);

	// Render into framebuffer
	glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT, NULL);

	// Have we downscaled enough?
	GLuint result;
	if (to_width > max_width || to_height > max_height) {
		GLuint new_source_texture = destination_texture;
		GLuint new_destination_texture = auxiliary_texture != 0 ? auxiliary_texture : source_texture;
		result = gl_average_texture_color_inner(base, new_source_texture, new_destination_texture, 0, fbo, to_width, to_height);
	} else {
		result = destination_texture;
	}

	return result;
}

/*
 * @brief Builds a 1x1 texture which has color corresponding to the average of all
 * pixels of img by recursively rendering into texture of quorter the size (half
 * width and half height).
 * Returned texture must not be deleted, since it's owned by the gl_image. It will be
 * deleted when the gl_image is released.
 */
static GLuint gl_average_texture_color(backend_t *base, struct gl_texture *img) 
{
	auto gd = (struct gl_data *)base;

	// Prepare textures which will be used for destination and source of rendering
	// during downscaling.
	const int texture_count = ARR_SIZE(img->auxiliary_texture);
	if (!img->auxiliary_texture[0]) {
		assert(!img->auxiliary_texture[1]);
		glGenTextures(texture_count, img->auxiliary_texture);
		glActiveTexture(GL_TEXTURE0);
		for (int i = 0; i < texture_count; i++) {
			glBindTexture(GL_TEXTURE_2D, img->auxiliary_texture[i]);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_BORDER);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_BORDER);
			glTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_BORDER_COLOR,
			                 (GLint[]){0, 0, 0, 0});
			glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB8, img->width,
			             img->height, 0, GL_BGR, GL_UNSIGNED_BYTE, NULL);
		}
	}

	// Prepare framebuffer used for rendering and bind it
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, gd->temp_fbo);
	glDrawBuffer(GL_COLOR_ATTACHMENT0);

	// Enable shaders
	glUseProgram(gd->brightness_shader.prog);
	glUniform2f(glGetUniformLocationChecked(gd->brightness_shader.prog, "texsize"),
	            (GLfloat)img->width, (GLfloat)img->height);

	// Prepare vertex attributes
	GLuint vao;
	glGenVertexArrays(1, &vao);
	glBindVertexArray(vao);
	GLuint bo[2];
	glGenBuffers(2, bo);
	glBindBuffer(GL_ARRAY_BUFFER, bo[0]);
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, bo[1]);
	glEnableVertexAttribArray(vert_coord_loc);
	glEnableVertexAttribArray(vert_in_texcoord_loc);
	glVertexAttribPointer(vert_coord_loc, 2, GL_INT, GL_FALSE, sizeof(GLint) * 4, NULL);
	glVertexAttribPointer(vert_in_texcoord_loc, 2, GL_INT, GL_FALSE,
	                      sizeof(GLint) * 4, (void *)(sizeof(GLint) * 2));

	// Allocate buffers for render input
	GLint coord[16] = {0};
	GLuint indices[] = {0, 1, 2, 2, 3, 0};
	glBufferData(GL_ARRAY_BUFFER, (long)sizeof(*coord) * 16, coord, GL_DYNAMIC_DRAW);
	glBufferData(GL_ELEMENT_ARRAY_BUFFER, (long)sizeof(*indices) * 6, indices,
	             GL_STATIC_DRAW);

	// Do actual recursive render to 1x1 texture
	GLuint result_texture = gl_average_texture_color_inner(base, img->texture, img->auxiliary_texture[0], 
													  img->auxiliary_texture[1], gd->temp_fbo, 
													  img->width, img->height);

	// Cleanup vertex attributes
	glDisableVertexAttribArray(vert_coord_loc);
	glDisableVertexAttribArray(vert_in_texcoord_loc);
	glBindBuffer(GL_ARRAY_BUFFER, 0);
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
	glDeleteBuffers(2, bo);
	glBindVertexArray(0);
	glDeleteVertexArrays(1, &vao);

	// Cleanup shaders
	glUseProgram(0);

	// Cleanup framebuffers
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
	glDrawBuffer(GL_BACK);

	// Cleanup render textures
	glBindTexture(GL_TEXTURE_2D, 0);

	gl_check_err();

	return result_texture;
}

static void
gl_set_win_shader_uniforms(struct backend_blit_args *blit_args, struct gl_texture *mask_image)
{
	auto win_shader = (gl_win_shader_t *)blit_args->shader;
	assert(win_shader);
	assert(win_shader->prog);

	glUseProgram(win_shader->prog);
	if (win_shader->uniform_opacity >= 0) {
		glUniform1f(win_shader->uniform_opacity, (float)blit_args->opacity);
	}
	if (win_shader->uniform_invert_color >= 0) {
		glUniform1i(win_shader->uniform_invert_color, blit_args->color_inverted);
	}
	if (win_shader->uniform_tex >= 0) {
		glUniform1i(win_shader->uniform_tex, 0);
	}
	if (win_shader->uniform_dim >= 0) {
		glUniform1f(win_shader->uniform_dim, (float)blit_args->dim);
	}
	if (win_shader->uniform_brightness >= 0) {
		glUniform1i(win_shader->uniform_brightness, 1);
	}
	if (win_shader->uniform_max_brightness >= 0) {
		glUniform1f(win_shader->uniform_max_brightness,
		            (float)blit_args->max_brightness);
	}
	if (win_shader->uniform_corner_radius >= 0) {
		glUniform1f(win_shader->uniform_corner_radius, (float)blit_args->corner_radius);
	}
	if (win_shader->uniform_border_width >= 0) {
		auto border_width = blit_args->border_width;
		if (border_width > blit_args->corner_radius) {
			border_width = 0;
		}
		glUniform1f(win_shader->uniform_border_width, (float)border_width);
	}
	if (win_shader->uniform_time >= 0) {
		struct timespec ts;
		clock_gettime(CLOCK_MONOTONIC, &ts);
		glUniform1f(win_shader->uniform_time,
		            (float)ts.tv_sec * 1000.0F + (float)ts.tv_nsec / 1.0e6F);
	}

	glUniform1i(win_shader->uniform_mask_tex, 2);
	if (blit_args->mask != NULL) 
	{
		glUniform2f(win_shader->uniform_mask_offset,
		            (float)blit_args->mask->origin.x,
		            (float)blit_args->mask->origin.y);

		if (mask_image != NULL) 
		{
			glUniform1i(win_shader->uniform_mask_inverted, blit_args->mask->inverted);
			glUniform1f(win_shader->uniform_mask_corner_radius, (GLfloat)blit_args->mask->corner_radius);
		}
	} 
	else 
	{
		glUniform1i(win_shader->uniform_mask_inverted, 0);
		glUniform1f(win_shader->uniform_mask_corner_radius, 0);
	}
}

/**
 * Render a region with texture data.
 *
 * @param target_fbo   the FBO to render into
 * @param blit_args arguments for the blit
 * @param coord GL vertices
 * @param indices GL indices
 * @param nrects number of rectangles to render
 */
static void
gl_blit_inner(backend_t *base, GLuint target_fbo, struct backend_blit_args *blit_args,
              GLint *coord, GLuint *indices, int nrects) {
	// FIXME(yshui) breaks when `mask` and `img` doesn't have the same y_inverted
	//              value. but we don't ever hit this problem because all of our
	//              images and masks are y_inverted.
	auto gd = (struct gl_data *)base;
	auto img = (struct gl_texture *)blit_args->source_image;
	auto mask_image = blit_args->mask ? (struct gl_texture *)blit_args->mask->image : NULL;
	auto mask_texture = mask_image ? mask_image->texture : gd->default_mask_texture;
	GLuint brightness = blit_args->max_brightness < 1.0 ? gl_average_texture_color(base, img) : 0;

	gl_set_win_shader_uniforms(blit_args, mask_image);

	// Bind texture
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, img->texture);
	glActiveTexture(GL_TEXTURE1);
	glBindTexture(GL_TEXTURE_2D, brightness);
	glActiveTexture(GL_TEXTURE2);
	glBindTexture(GL_TEXTURE_2D, mask_texture);

	GLuint vao;
	glGenVertexArrays(1, &vao);
	glBindVertexArray(vao);

	GLuint bo[2];
	glGenBuffers(2, bo);
	glBindBuffer(GL_ARRAY_BUFFER, bo[0]);
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, bo[1]);
	glBufferData(GL_ARRAY_BUFFER, (long)sizeof(*coord) * nrects * 16, coord, GL_STREAM_DRAW);
	glBufferData(GL_ELEMENT_ARRAY_BUFFER, (long)sizeof(*indices) * nrects * 6,
	             indices, GL_STREAM_DRAW);

	glEnableVertexAttribArray(vert_coord_loc);
	glEnableVertexAttribArray(vert_in_texcoord_loc);
	glVertexAttribPointer(vert_coord_loc, 2, GL_INT, GL_FALSE, sizeof(GLint) * 4, NULL);
	glVertexAttribPointer(vert_in_texcoord_loc, 2, GL_INT, GL_FALSE,
	                      sizeof(GLint) * 4, (void *)(sizeof(GLint) * 2));
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, target_fbo);
	glDrawElements(GL_TRIANGLES, nrects * 6, GL_UNSIGNED_INT, NULL);

	glDisableVertexAttribArray(vert_coord_loc);
	glDisableVertexAttribArray(vert_in_texcoord_loc);
	glBindVertexArray(0);
	glDeleteVertexArrays(1, &vao);

	// Cleanup
	glActiveTexture(GL_TEXTURE2);
	glBindTexture(GL_TEXTURE_2D, 0);
	glActiveTexture(GL_TEXTURE1);
	glBindTexture(GL_TEXTURE_2D, 0);
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, 0);
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
	glDrawBuffer(GL_BACK);

	glBindBuffer(GL_ARRAY_BUFFER, 0);
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
	glDeleteBuffers(2, bo);

	glUseProgram(0);

	gl_check_err();
}

/// Convert rectangles in X coordinates to OpenGL vertex and texture coordinates
/// @param[in] nrects, rects   rectangles
/// @param[in] image_dst       origin of the OpenGL texture, affect the calculated texture
///                            coordinates
/// @param[in] extend_height   height of the drawing extent
/// @param[in] texture_height  height of the OpenGL texture
/// @param[in] root_height     height of the back buffer
/// @param[in] y_inverted      whether the texture is y inverted
/// @param[out] coord, indices output
void x_rect_to_coords(int nrects, const rect_t *rects, coord_t image_dst,
                      int extent_height, int texture_height, int root_height,
                      bool y_inverted, GLint *coord, GLuint *indices) {
	image_dst.y = root_height - image_dst.y;
	image_dst.y -= extent_height;

	for (int i = 0; i < nrects; i++) {
		// Y-flip. Note after this, crect.y1 > crect.y2
		rect_t crect = rects[i];
		crect.y1 = root_height - crect.y1;
		crect.y2 = root_height - crect.y2;

		// Calculate texture coordinates
		// (texture_x1, texture_y1), texture coord for the _bottom left_ corner
		GLint texture_x1 = crect.x1 - image_dst.x,
		      texture_y1 = crect.y2 - image_dst.y,
		      texture_x2 = texture_x1 + (crect.x2 - crect.x1),
		      texture_y2 = texture_y1 + (crect.y1 - crect.y2);

		// X pixmaps might be Y inverted, invert the texture coordinates
		if (y_inverted) {
			texture_y1 = texture_height - texture_y1;
			texture_y2 = texture_height - texture_y2;
		}

		// Vertex coordinates
		auto vx1 = crect.x1;
		auto vy1 = crect.y2;
		auto vx2 = crect.x2;
		auto vy2 = crect.y1;

		// log_trace("Rect %d: %f, %f, %f, %f -> %d, %d, %d, %d",
		//          ri, rx, ry, rxe, rye, rdx, rdy, rdxe, rdye);

		memcpy(&coord[i * 16],
		       ((GLint[][2]){
		           {vx1, vy1},
		           {texture_x1, texture_y1},
		           {vx2, vy1},
		           {texture_x2, texture_y1},
		           {vx2, vy2},
		           {texture_x2, texture_y2},
		           {vx1, vy2},
		           {texture_x1, texture_y2},
		       }),
		       sizeof(GLint[2]) * 8);

		GLuint u = (GLuint)(i * 4);
		memcpy(&indices[i * 6],
		       ((GLuint[]){u + 0, u + 1, u + 2, u + 2, u + 3, u + 0}),
		       sizeof(GLuint) * 6);
	}
}

void gl_mask_rects_to_coords(struct coord origin, struct coord mask_origin, int nrects,
                             const rect_t *rects, GLint *coord, GLuint *indices) {
	for (ptrdiff_t i = 0; i < nrects; i++) {
		// Rectangle in source image coordinates
		rect_t rect_src = region_translate_rect(rects[i], mask_origin);
		// Rectangle in target image coordinates
		rect_t rect_dst = region_translate_rect(rect_src, origin);

		memcpy(&coord[i * 16],
		       ((GLint[][2]){
		           {rect_dst.x1, rect_dst.y1},        // Vertex, bottom-left
		           {rect_src.x1, rect_src.y1},        // Texture
		           {rect_dst.x2, rect_dst.y1},        // Vertex, bottom-right
		           {rect_src.x2, rect_src.y1},        // Texture
		           {rect_dst.x2, rect_dst.y2},        // Vertex, top-right
		           {rect_src.x2, rect_src.y2},        // Texture
		           {rect_dst.x1, rect_dst.y2},        // Vertex, top-left
		           {rect_src.x1, rect_src.y2},        // Texture
		       }),
		       sizeof(GLint[2]) * 8);

		GLuint u = (GLuint)(i * 4);
		memcpy(&indices[i * 6],
		       ((GLuint[]){u + 0, u + 1, u + 2, u + 2, u + 3, u + 0}),
		       sizeof(GLuint) * 6);
	}
}

// TODO(yshui) make use of reg_visible
void gl_compose(backend_t *base, void *image_data, coord_t image_dst, void *mask_data,
                coord_t mask_dst, const region_t *reg_tgt,
                const region_t *reg_visible attr_unused, bool lerp) {
	auto gd    = (struct gl_data *)base;
	auto img   = (struct backend_image *) image_data;
	auto mask  = (struct backend_image *)mask_data;
	auto inner = (struct gl_texture *)img->inner;

	// Painting
	int nrects;
	const rect_t *rects;
	rects = pixman_region32_rectangles((region_t *)reg_tgt, &nrects);
	if (!nrects) {
		// Nothing to paint
		return;
	}

	// Until we start to use glClipControl, reg_tgt, dst_x and dst_y and
	// in a different coordinate system than the one OpenGL uses.
	// OpenGL window coordinate (or NDC) has the origin at the lower left of the
	// screen, with y axis pointing up; Xorg has the origin at the upper left of the
	// screen, with y axis pointing down. We have to do some coordinate conversion in
	// this function

	auto coord = ccalloc(nrects * 16, GLint);
	auto indices = ccalloc(nrects * 6, GLuint);
	coord_t mask_offset = {.x = mask_dst.x - image_dst.x,
	                       .y = mask_dst.y - image_dst.y};
								
	x_rect_to_coords(nrects, rects, image_dst, inner->height, inner->height,
	                 gd->height, inner->y_inverted, coord, indices);

	// Kirill
	if (lerp) {
		for (unsigned int i = 2; i < 16; i+=4) {
			coord[i+0] = lerp_range(0, mask_offset.x, 0, inner->width, coord[i+0]);
			coord[i+1] = lerp_range(0, mask_offset.y, 0, inner->height, coord[i+1]);
		}
	}

	struct backend_mask mask_args = 
	{
	    .image = mask ? (image_handle)mask->inner : NULL,
	    .origin = mask_offset,
	    .corner_radius = mask ? mask->corner_radius : 0,
	    .inverted = mask ? mask->color_inverted : false,
	};

	pixman_region32_init(&mask_args.region);

	struct backend_blit_args blit_args = 
	{
	    .source_image = (image_handle)img->inner,
	    .mask = mask ? &mask_args : NULL,
	    .shader = (void *)img->shader ?: gd->default_shader,
	    .opacity = img->opacity,
	    .color_inverted = img->color_inverted,
	    .ewidth = img->ewidth,
	    .eheight = img->eheight,
	    .dim = img->dim,
	    .corner_radius = img->corner_radius,
	    .border_width = img->border_width,
	    .max_brightness = img->max_brightness,
	};

	gl_blit_inner(base, gd->back_fbo, &blit_args, coord, indices, nrects);

	free(indices);
	free(coord);
}

/**
 * Load a GLSL main program from shader strings.
 */
static bool gl_win_shader_from_stringv(const char **vshader_strv,
                                       const char **fshader_strv, gl_win_shader_t *ret) {
	// Build program
	ret->prog = gl_create_program_from_strv(vshader_strv, fshader_strv);
	if (!ret->prog) {
		log_error("Failed to create GLSL program.");
		gl_check_err();
		return false;
	}

	// Get uniform addresses
	bind_uniform(ret, opacity);
	bind_uniform(ret, invert_color);
	bind_uniform(ret, tex);
	bind_uniform(ret, dim);
	bind_uniform(ret, brightness);
	bind_uniform(ret, max_brightness);
	bind_uniform(ret, corner_radius);
	bind_uniform(ret, border_width);
	bind_uniform(ret, time);

	bind_uniform(ret, mask_tex);
	bind_uniform(ret, mask_offset);
	bind_uniform(ret, mask_inverted);
	bind_uniform(ret, mask_corner_radius);

	gl_check_err();

	return true;
}

/**
 * Callback to run on root window size change.
 */
void gl_resize(struct gl_data *gd, int width, int height) {
	GLint viewport_dimensions[2];
	glGetIntegerv(GL_MAX_VIEWPORT_DIMS, viewport_dimensions);

	gd->height = height;
	gd->width = width;

	assert(viewport_dimensions[0] >= gd->width);
	assert(viewport_dimensions[1] >= gd->height);

	glBindTexture(GL_TEXTURE_2D, gd->back_texture);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB8, width, height, 0, GL_BGR,
	             GL_UNSIGNED_BYTE, NULL);

	gl_check_err();
}

/// Fill a given region in bound framebuffer.
/// @param[in] y_inverted whether the y coordinates in `clip` should be inverted
static void gl_fill_inner(backend_t *base, struct color c, const region_t *clip, GLuint target) 
{
	static const GLuint fill_vert_in_coord_loc = 0;
	int nrects;
	const rect_t *rect = pixman_region32_rectangles((region_t *)clip, &nrects);
	auto gd = (struct gl_data *)base;

	glUseProgram(gd->fill_shader.prog);
	glUniform4f(gd->fill_shader.color_loc, (GLfloat)c.red, (GLfloat)c.green, (GLfloat)c.blue, (GLfloat)c.alpha);

	GLuint vao;
	glGenVertexArrays(1, &vao);
	glBindVertexArray(vao);

	GLuint bo[2];
	glGenBuffers(2, bo);
	glBindBuffer(GL_ARRAY_BUFFER, bo[0]);
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, bo[1]);

	auto coord = ccalloc(nrects * 16, GLint);
	auto indices = ccalloc(nrects * 6, GLuint);
	gl_mask_rects_to_coords((struct coord){0, 0}, (struct coord){0, 0}, 
							nrects, rect, coord, indices);

	glBufferData(GL_ARRAY_BUFFER, nrects * 16 * (long)sizeof(*coord), coord, GL_STREAM_DRAW);
	glBufferData(GL_ELEMENT_ARRAY_BUFFER, nrects * 6 * (long)sizeof(*indices), indices, GL_STREAM_DRAW);

	glEnableVertexAttribArray(fill_vert_in_coord_loc);
	glVertexAttribPointer(fill_vert_in_coord_loc, 2, GL_INT, GL_FALSE, sizeof(GLint) * 4, NULL);
	
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, target);
	glDrawElements(GL_TRIANGLES, nrects * 6, GL_UNSIGNED_INT, NULL);

	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
	glBindBuffer(GL_ARRAY_BUFFER, 0);
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
	glDisableVertexAttribArray(fill_vert_in_coord_loc);
	glBindVertexArray(0);
	glDeleteVertexArrays(1, &vao);

	glDeleteBuffers(2, bo);
	free(indices);
	free(coord);

	gl_check_err();
}

void gl_fill(backend_t *base, struct color c, const region_t *clip) {
	auto gd = (struct gl_data *)base;
	return gl_fill_inner(base, c, clip, gd->back_fbo);
}

void *gl_make_mask(backend_t *base, geometry_t size, const region_t *reg) {
	auto tex = ccalloc(1, struct gl_texture);
	auto gd = (struct gl_data *)base;
	auto img = ccalloc(1, struct backend_image);
	default_init_backend_image(img, size.width, size.height);

	tex->width = size.width;
	tex->height = size.height;
	tex->texture = gl_new_texture(GL_TEXTURE_2D);
	tex->has_alpha = false;
	tex->y_inverted = true;
	img->inner = (struct backend_image_inner_base *)tex;
	img->inner->refcount = 1;

	glBindTexture(GL_TEXTURE_2D, tex->texture);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_BORDER);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_BORDER);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RED, size.width, size.height, 0, GL_RED,
	             GL_UNSIGNED_BYTE, NULL);
	glBindTexture(GL_TEXTURE_2D, 0);

	glBlendFunc(GL_ONE, GL_ZERO);
	glBindFramebuffer(GL_FRAMEBUFFER, gd->temp_fbo);
	glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
	                       tex->texture, 0);
	glDrawBuffer(GL_COLOR_ATTACHMENT0);
	glClearColor(0, 0, 0, 1);
	glClear(GL_COLOR_BUFFER_BIT);

	gl_fill_inner(base, (struct color){1, 1, 1, 1}, reg, gd->temp_fbo);

	glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);

	return img;
}

static void gl_release_image_inner(backend_t *base, struct gl_texture *inner) {
	auto gd = (struct gl_data *)base;
	if (inner->user_data) {
		gd->release_user_data(base, inner);
	}
	assert(inner->user_data == NULL);

	glDeleteTextures(1, &inner->texture);
	glDeleteTextures(2, inner->auxiliary_texture);
	free(inner);
	gl_check_err();
}

void gl_release_image(backend_t *base, void *image_data) {
	struct backend_image *wd = image_data;
	auto inner = (struct gl_texture *)wd->inner;
	inner->refcount--;
	assert(inner->refcount >= 0);
	if (inner->refcount == 0) {
		gl_release_image_inner(base, inner);
	}
	free(wd);
}

void *gl_create_window_shader(backend_t *backend_data attr_unused, const char *source) {
	auto win_shader = (gl_win_shader_t *)ccalloc(1, gl_win_shader_t);

	const char *vert_shaders[2] = {vertex_shader, NULL};
	const char *frag_shaders[4] = {win_shader_glsl, masking_glsl, source, NULL};

	if (!gl_win_shader_from_stringv(vert_shaders, frag_shaders, win_shader)) {
		free(win_shader);
		return NULL;
	}

	GLint viewport_dimensions[2];
	glGetIntegerv(GL_MAX_VIEWPORT_DIMS, viewport_dimensions);

	// Set projection matrix to gl viewport dimensions so we can use screen
	// coordinates for all vertices
	// Note: OpenGL matrices are column major
	GLfloat projection_matrix[4][4] = {{2.0F / (GLfloat)viewport_dimensions[0], 0, 0, 0},
	                                   {0, 2.0F / (GLfloat)viewport_dimensions[1], 0, 0},
	                                   {0, 0, 0, 0},
	                                   {-1, -1, 0, 1}};

	int pml = glGetUniformLocationChecked(win_shader->prog, "projection");
	glUseProgram(win_shader->prog);
	glUniformMatrix4fv(pml, 1, false, projection_matrix[0]);
	glUseProgram(0);

	return win_shader;
}

uint64_t gl_get_shader_attributes(backend_t *backend_data attr_unused, void *shader) {
	auto win_shader = (gl_win_shader_t *)shader;
	uint64_t ret = 0;
	if (glGetUniformLocation(win_shader->prog, "time") >= 0) {
		ret |= SHADER_ATTRIBUTE_ANIMATED;
	}
	return ret;
}

bool gl_init(struct gl_data *gd, session_t *ps) {
	// Initialize GLX data structure
	glDisable(GL_DEPTH_TEST);
	glDepthMask(GL_FALSE);

	glEnable(GL_BLEND);
	// X pixmap is in premultiplied alpha, so we might just as well use it too.
	// Thanks to derhass for help.
	glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);

	// Initialize stencil buffer
	glDisable(GL_STENCIL_TEST);
	glStencilMask(0x1);
	glStencilFunc(GL_EQUAL, 0x1, 0x1);

	// Set gl viewport to the maximum supported size so we won't have to worry about
	// it later on when the screen is resized. The corresponding projection matrix can
	// be set now and won't have to be updated. Since fragments outside the target
	// buffer are skipped anyways, this should have no impact on performance.
	GLint viewport_dimensions[2];
	glGetIntegerv(GL_MAX_VIEWPORT_DIMS, viewport_dimensions);
	glViewport(0, 0, viewport_dimensions[0], viewport_dimensions[1]);

	// Clear screen
	glClearColor(0.0F, 0.0F, 0.0F, 1.0F);
	glClear(GL_COLOR_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);

	glGenFramebuffers(1, &gd->temp_fbo);
	glGenFramebuffers(1, &gd->back_fbo);
	glGenTextures(1, &gd->back_texture);
	if (!gd->back_fbo || !gd->back_texture) {
		log_error("Failed to generate a framebuffer object");
		return false;
	}

	glBindTexture(GL_TEXTURE_2D, gd->back_texture);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glBindTexture(GL_TEXTURE_2D, 0);

	gd->default_mask_texture = gl_new_texture(GL_TEXTURE_2D);
	if (!gd->default_mask_texture) {
		log_error("Failed to generate a default mask texture");
		return false;
	}

	glBindTexture(GL_TEXTURE_2D, gd->default_mask_texture);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RED, 1, 1, 0, GL_RED, GL_UNSIGNED_BYTE,
	             (GLbyte[]){'\xff'});
	glBindTexture(GL_TEXTURE_2D, 0);

	// Initialize shaders
	gd->default_shader = gl_create_window_shader(NULL, win_shader_default);
	if (!gd->default_shader) {
		log_error("Failed to create window shaders");
		return false;
	}

	// Set projection matrix to gl viewport dimensions so we can use screen
	// coordinates for all vertices
	// Note: OpenGL matrices are column major
	GLfloat projection_matrix[4][4] = {{2.0F / (GLfloat)viewport_dimensions[0], 0, 0, 0},
	                                   {0, 2.0F / (GLfloat)viewport_dimensions[1], 0, 0},
	                                   {0, 0, 0, 0},
	                                   {-1, -1, 0, 1}};

	gd->fill_shader.prog = gl_create_program_from_str(fill_vert, fill_frag);
	gd->fill_shader.color_loc = glGetUniformLocation(gd->fill_shader.prog, "color");
	int pml = glGetUniformLocationChecked(gd->fill_shader.prog, "projection");
	glUseProgram(gd->fill_shader.prog);
	glUniformMatrix4fv(pml, 1, false, projection_matrix[0]);
	glUseProgram(0);

	gd->present_prog = gl_create_program_from_str(present_vertex_shader, dummy_frag);
	if (!gd->present_prog) {
		log_error("Failed to create the present shader");
		return false;
	}
	pml = glGetUniformLocationChecked(gd->present_prog, "projection");
	glUseProgram(gd->present_prog);
	glUniform1i(glGetUniformLocationChecked(gd->present_prog, "tex"), 0);
	glUniformMatrix4fv(pml, 1, false, projection_matrix[0]);
	glUseProgram(0);

	gd->shadow_shader.prog =
	    gl_create_program_from_str(present_vertex_shader, shadow_colorization_frag);
	gd->shadow_shader.uniform_color =
	    glGetUniformLocationChecked(gd->shadow_shader.prog, "color");
	pml = glGetUniformLocationChecked(gd->shadow_shader.prog, "projection");
	glUseProgram(gd->shadow_shader.prog);
	glUniform1i(glGetUniformLocationChecked(gd->shadow_shader.prog, "tex"), 0);
	glUniformMatrix4fv(pml, 1, false, projection_matrix[0]);
	glUseProgram(0);
	glBindFragDataLocation(gd->shadow_shader.prog, 0, "out_color");

	gd->brightness_shader.prog =
	    gl_create_program_from_str(interpolating_vert, interpolating_frag);
	if (!gd->brightness_shader.prog) {
		log_error("Failed to create the brightness shader");
		return false;
	}
	pml = glGetUniformLocationChecked(gd->brightness_shader.prog, "projection");
	glUseProgram(gd->brightness_shader.prog);
	glUniform1i(glGetUniformLocationChecked(gd->brightness_shader.prog, "tex"), 0);
	glUniformMatrix4fv(pml, 1, false, projection_matrix[0]);
	glUseProgram(0);

	// Set up the size of the back texture
	gl_resize(gd, ps->root_width, ps->root_height);

	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, gd->back_fbo);
	glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
	                       gd->back_texture, 0);
	glDrawBuffer(GL_COLOR_ATTACHMENT0);
	if (!gl_check_fb_complete(GL_FRAMEBUFFER)) {
		return false;
	}
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);

	gd->logger = gl_string_marker_logger_new();
	if (gd->logger) {
		log_add_target_tls(gd->logger);
	}

	const char *vendor = (const char *)glGetString(GL_VENDOR);
	log_debug("GL_VENDOR = %s", vendor);
	if (strcmp(vendor, "NVIDIA Corporation") == 0) {
		log_info("GL vendor is NVIDIA, don't use glFinish");
		gd->is_nvidia = true;
	} else {
		gd->is_nvidia = false;
	}
	gd->has_robustness = gl_has_extension("GL_ARB_robustness");
	gd->has_egl_image_storage = gl_has_extension("GL_EXT_EGL_image_storage");
	gl_check_err();

	return true;
}

void gl_deinit(struct gl_data *gd) {
	if (gd->logger) {
		log_remove_target_tls(gd->logger);
		gd->logger = NULL;
	}

	if (gd->default_shader) {
		gl_destroy_window_shader(&gd->base, gd->default_shader);
		gd->default_shader = NULL;
	}

	glDeleteProgram(gd->present_prog);
	gd->present_prog = 0;

	glDeleteProgram(gd->fill_shader.prog);
	glDeleteProgram(gd->brightness_shader.prog);
	glDeleteProgram(gd->shadow_shader.prog);
	gd->fill_shader.prog = 0;
	gd->brightness_shader.prog = 0;
	gd->shadow_shader.prog = 0;

	glDeleteTextures(1, &gd->default_mask_texture);
 	glDeleteTextures(1, &gd->back_texture);

	glDeleteFramebuffers(1, &gd->temp_fbo);
	glDeleteFramebuffers(1, &gd->back_fbo);

	gl_check_err();
}

GLuint gl_new_texture(GLenum target) {
	GLuint texture;
	glGenTextures(1, &texture);
	if (!texture) {
		log_error("Failed to generate texture");
		return 0;
	}

	glBindTexture(target, texture);
	glTexParameteri(target, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(target, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameteri(target, GL_TEXTURE_WRAP_S, GL_REPEAT);
	glTexParameteri(target, GL_TEXTURE_WRAP_T, GL_REPEAT);
	glBindTexture(target, 0);

	return texture;
}

/// Actually duplicate a texture into a new one, if this texture is shared
static inline void gl_image_decouple(backend_t *base, struct backend_image *img) 
{
	if (img->inner->refcount == 1) {
		return;
	}
	auto gd = (struct gl_data *)base;
	auto inner = (struct gl_texture *)img->inner;
	auto new_tex = ccalloc(1, struct gl_texture);

	new_tex->texture = gl_new_texture(GL_TEXTURE_2D);
	new_tex->y_inverted = true;
	new_tex->height = inner->height;
	new_tex->width = inner->width;
	new_tex->refcount = 1;
	new_tex->user_data = gd->decouple_texture_user_data(base, inner->user_data);

	glBindTexture(GL_TEXTURE_2D, new_tex->texture);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, new_tex->width, new_tex->height, 0,
	             GL_BGRA, GL_UNSIGNED_BYTE, NULL);
	glBindTexture(GL_TEXTURE_2D, 0);

	assert(gd->present_prog);
	glUseProgram(gd->present_prog);
	glBindTexture(GL_TEXTURE_2D, inner->texture);

	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, gd->temp_fbo);
	glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
	                       new_tex->texture, 0);
	glDrawBuffer(GL_COLOR_ATTACHMENT0);
	gl_check_fb_complete(GL_DRAW_FRAMEBUFFER);

	glClearColor(0, 0, 0, 0);
	glClear(GL_COLOR_BUFFER_BIT);

	// clang-format off
	GLint coord[] = {
		// top left
		0, 0,                 // vertex coord
		0, 0,                 // texture coord

		// top right
		new_tex->width, 0, // vertex coord
		new_tex->width, 0, // texture coord

		// bottom right
		new_tex->width, new_tex->height,
		new_tex->width, new_tex->height,

		// bottom left
		0, new_tex->height,
		0, new_tex->height,
	};
	// clang-format on
	GLuint indices[] = {0, 1, 2, 2, 3, 0};

	GLuint vao;
	glGenVertexArrays(1, &vao);
	glBindVertexArray(vao);

	GLuint bo[2];
	glGenBuffers(2, bo);
	glBindBuffer(GL_ARRAY_BUFFER, bo[0]);
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, bo[1]);
	glBufferData(GL_ARRAY_BUFFER, (long)sizeof(*coord) * 16, coord, GL_STATIC_DRAW);
	glBufferData(GL_ELEMENT_ARRAY_BUFFER, (long)sizeof(*indices) * 6, indices,
	             GL_STATIC_DRAW);

	glEnableVertexAttribArray(vert_coord_loc);
	glEnableVertexAttribArray(vert_in_texcoord_loc);
	glVertexAttribPointer(vert_coord_loc, 2, GL_INT, GL_FALSE, sizeof(GLint) * 4, NULL);
	glVertexAttribPointer(vert_in_texcoord_loc, 2, GL_INT, GL_FALSE,
	                      sizeof(GLint) * 4, (void *)(sizeof(GLint) * 2));

	glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT, NULL);

	glDisableVertexAttribArray(vert_coord_loc);
	glDisableVertexAttribArray(vert_in_texcoord_loc);
	glBindVertexArray(0);
	glDeleteVertexArrays(1, &vao);

	glBindBuffer(GL_ARRAY_BUFFER, 0);
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
	glDeleteBuffers(2, bo);

	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);

	glBindTexture(GL_TEXTURE_2D, 0);
	glUseProgram(0);

	gl_check_err();

	img->inner = (struct backend_image_inner_base *)new_tex;
	inner->refcount--;
}

static void gl_image_apply_alpha(backend_t *base, struct backend_image *img,
                                 const region_t *reg_op, double alpha) {
	// Result color = 0 (GL_ZERO) + alpha (GL_CONSTANT_ALPHA) * original color
	auto inner = (struct gl_texture *)img->inner;
	auto gd = (struct gl_data *)base;

	glBlendFunc(GL_ZERO, GL_CONSTANT_ALPHA);
	glBlendColor(0, 0, 0, (GLclampf)alpha);

	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, gd->temp_fbo);
	glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, inner->texture, 0);
	glDrawBuffer(GL_COLOR_ATTACHMENT0);

	gl_fill_inner(base, (struct color){0, 0, 0, 0}, reg_op, gd->temp_fbo);

	glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
}

void gl_present(backend_t *base, const region_t *region) 
{
	auto gd = (struct gl_data *)base;

	int nrects;
	const rect_t *rects = pixman_region32_rectangles((region_t *)region, &nrects);
	auto coord = ccalloc(nrects * 16, GLint);
	auto indices = ccalloc(nrects * 6, GLuint);
	gl_mask_rects_to_coords((struct coord){0, 0}, (struct coord){0, 0}, 
							nrects, rects, coord, indices);
							
	glUseProgram(gd->present_prog);
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, gd->back_texture);

	GLuint vao;
	glGenVertexArrays(1, &vao);
	glBindVertexArray(vao);

	GLuint bo[2];
	glGenBuffers(2, bo);
	glBindBuffer(GL_ARRAY_BUFFER, bo[0]);
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, bo[1]);
	glBufferData(GL_ARRAY_BUFFER, (long)sizeof(GLint) * nrects * 16, coord, GL_STREAM_DRAW);
	glBufferData(GL_ELEMENT_ARRAY_BUFFER, (long)sizeof(GLuint) * nrects * 6, indices, GL_STREAM_DRAW);

	glEnableVertexAttribArray(vert_coord_loc);
	glEnableVertexAttribArray(vert_in_texcoord_loc);
	glVertexAttribPointer(vert_coord_loc, 2, GL_INT, GL_FALSE, sizeof(GLint) * 4, NULL);
	glVertexAttribPointer(vert_in_texcoord_loc, 2, GL_INT, GL_FALSE, sizeof(GLint) * 4, (void *)(sizeof(GLint) * 2));

	glDrawElements(GL_TRIANGLES, nrects * 6, GL_UNSIGNED_INT, NULL);

	glBindBuffer(GL_ARRAY_BUFFER, 0);
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
	glBindVertexArray(0);
	glDeleteBuffers(2, bo);
	glDeleteVertexArrays(1, &vao);

	free(coord);
	free(indices);
}

bool gl_image_op(backend_t *base, enum image_operations op, void *image_data,
                 const region_t *reg_op, const region_t *reg_visible attr_unused, void *arg) {
	struct backend_image *tex = image_data;
	switch (op) {
	case IMAGE_OP_APPLY_ALPHA:
		gl_image_decouple(base, tex);
		assert(tex->inner->refcount == 1);
		gl_image_apply_alpha(base, tex, reg_op, *(double *)arg);
		break;
	}

	return true;
}

struct gl_shadow_context {
	double radius;
	void *blur_context;
};

struct backend_shadow_context *gl_create_shadow_context(backend_t *base, double radius) {
	auto ctx = ccalloc(1, struct gl_shadow_context);
	ctx->radius = radius;
	ctx->blur_context = NULL;

	if (radius > 0) {
		struct gaussian_blur_args args = {
		    .size = (int)radius,
		    .deviation = gaussian_kernel_std_for_size(radius, 0.5 / 256.0),
		};
		ctx->blur_context = gl_create_blur_context(base, BLUR_METHOD_GAUSSIAN, &args);
		if (!ctx->blur_context) {
			log_error("Failed to create shadow context");
			free(ctx);
			return NULL;
		}
	}
	return (struct backend_shadow_context *)ctx;
}

void gl_destroy_shadow_context(backend_t *base attr_unused, struct backend_shadow_context *ctx) 
{
	auto ctx_ = (struct gl_shadow_context *)ctx;
	if (ctx_->blur_context) {
		gl_destroy_blur_context(base, (struct backend_blur_context *)ctx_->blur_context);
	}
	free(ctx_);
}

void *gl_shadow_from_mask(backend_t *base, void *mask_data, struct backend_shadow_context *sctx, struct color color) 
{
	log_debug("Create shadow from mask");
	auto gd = (struct gl_data *)base;
	auto mask = (struct backend_image *)mask_data;
	auto img = (struct backend_image *)mask;
	auto inner = (struct gl_texture *)img->inner;
	auto gsctx = (struct gl_shadow_context *)sctx;
	int radius = (int)gsctx->radius;

	auto new_inner = ccalloc(1, struct gl_texture);
	new_inner->width = inner->width + radius * 2;
	new_inner->height = inner->height + radius * 2;
	new_inner->texture = gl_new_texture(GL_TEXTURE_2D);
	new_inner->has_alpha = inner->has_alpha;
	new_inner->y_inverted = true;

	auto new_img = ccalloc(1, struct backend_image);
	default_init_backend_image(new_img, new_inner->width, new_inner->height);
	new_img->inner = (struct backend_image_inner_base *)new_inner;
	new_img->inner->refcount = 1;

	// Render the mask to a texture, so inversion and corner radius can be
	// applied.
	auto source_texture = gl_new_texture(GL_TEXTURE_2D);
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, source_texture);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RED, new_inner->width, new_inner->height, 0,
	             GL_RED, GL_UNSIGNED_BYTE, NULL);
	glBindTexture(GL_TEXTURE_2D, 0);

	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, gd->temp_fbo);
	glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, source_texture, 0);
	glDrawBuffer(GL_COLOR_ATTACHMENT0);

	if (img->color_inverted) {
		// If the mask is inverted, clear the source_texture to white, so the
		// "outside" of the mask would be correct
		glClearColor(1, 1, 1, 1);
	} else {
		glClearColor(0, 0, 0, 1);
	}

	glClear(GL_COLOR_BUFFER_BIT);

	{
		// clang-format off
		// interleaved vertex coordinates and texture coordinates
		GLint coords[] = {radius               , radius                , 0           ,             0,
				  radius + inner->width, radius                , inner->width,             0,
				  radius + inner->width, radius + inner->height, inner->width, inner->height,
				  radius               , radius + inner->height, 0           , inner->height,};
		// clang-format on
		GLuint indices[] = {0, 1, 2, 2, 3, 0};

		struct backend_blit_args blit_args = 
		{
		    .source_image = (image_handle)mask->inner,
		    .mask = NULL,
		    .shader = (void *)mask->shader ?: gd->default_shader,
		    .opacity = mask->opacity,
		    .color_inverted = mask->color_inverted,
		    .ewidth = mask->ewidth,
		    .eheight = mask->eheight,
		    .dim = mask->dim,
		    .corner_radius = mask->corner_radius,
		    .border_width = mask->border_width,
		    .max_brightness = mask->max_brightness,
		};

		gl_blit_inner(base, gd->temp_fbo, &blit_args, coords, indices, 1);
	}

	gl_check_err();

	auto tmp_texture = source_texture;
	if (gsctx->blur_context != NULL) {
		glActiveTexture(GL_TEXTURE0);
		tmp_texture = gl_new_texture(GL_TEXTURE_2D);
		glBindTexture(GL_TEXTURE_2D, tmp_texture);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RED, new_inner->width,
		             new_inner->height, 0, GL_RED, GL_UNSIGNED_BYTE, NULL);
		glBindTexture(GL_TEXTURE_2D, 0);

		glBindFramebuffer(GL_DRAW_FRAMEBUFFER, gd->temp_fbo);
		glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tmp_texture, 0);

		region_t reg_blur;
		pixman_region32_init_rect(&reg_blur, 0, 0, (unsigned int)new_inner->width,
		                          (unsigned int)new_inner->height);

		// gl_blur expects reg_blur to be in X coordinate system (i.e. y flipped),
		// but we are covering the whole texture so we don't need to worry about
		// that.
		gl_blur_inner(
		    1.0, gsctx->blur_context, NULL, (coord_t){0}, &reg_blur, NULL,
		    source_texture,
		    (geometry_t){.width = new_inner->width, .height = new_inner->height},
		    gd->temp_fbo, gd->default_mask_texture);

		pixman_region32_fini(&reg_blur);
	}

	// Colorize the shadow with color.
	log_debug("Colorize shadow");
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, new_inner->texture);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, new_inner->width, new_inner->height, 0,
	             GL_RGBA, GL_UNSIGNED_BYTE, NULL);

	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, gd->temp_fbo);
	glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
	                       new_inner->texture, 0);

	glClearColor(0, 0, 0, 0);
	glClear(GL_COLOR_BUFFER_BIT);

	glBindTexture(GL_TEXTURE_2D, tmp_texture);
	glUseProgram(gd->shadow_shader.prog);
	glUniform4f(gd->shadow_shader.uniform_color, (GLfloat)(color.red * color.alpha),
	            (GLfloat)(color.green * color.alpha),
	            (GLfloat)(color.blue * color.alpha), (GLfloat)color.alpha);

	// clang-format off
	GLuint indices[] = {0, 1, 2, 2, 3, 0};
	GLint coord[] = {0                , 0                ,
	                 new_inner->width , 0                ,
	                 new_inner->width , new_inner->height,
	                 0                , new_inner->height,};
	// clang-format on

	GLuint vao;
	glGenVertexArrays(1, &vao);
	glBindVertexArray(vao);

	GLuint bo[2];
	glGenBuffers(2, bo);
	glBindBuffer(GL_ARRAY_BUFFER, bo[0]);
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, bo[1]);
	glBufferData(GL_ARRAY_BUFFER, (long)sizeof(*coord) * 8, coord, GL_STATIC_DRAW);
	glBufferData(GL_ELEMENT_ARRAY_BUFFER, (long)sizeof(*indices) * 6, indices,
	             GL_STATIC_DRAW);

	glEnableVertexAttribArray(vert_coord_loc);
	glVertexAttribPointer(vert_coord_loc, 2, GL_INT, GL_FALSE, sizeof(GLint) * 2, NULL);

	glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT, NULL);

	glDisableVertexAttribArray(vert_coord_loc);
	glBindVertexArray(0);
	glDeleteVertexArrays(1, &vao);

	glBindBuffer(GL_ARRAY_BUFFER, 0);
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
	glDeleteBuffers(2, bo);

	glDeleteTextures(1, (GLuint[]){source_texture});
	if (tmp_texture != source_texture) {
		glDeleteTextures(1, (GLuint[]){tmp_texture});
	}

	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
	gl_check_err();

	return new_img;
}

enum device_status gl_device_status(backend_t *base) {
	auto gd = (struct gl_data *)base;
	if (!gd->has_robustness) {
		return DEVICE_STATUS_NORMAL;
	}
	if (glGetGraphicsResetStatusARB() == GL_NO_ERROR) {
		return DEVICE_STATUS_NORMAL;
	}
	return DEVICE_STATUS_RESETTING;
}
