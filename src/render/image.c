#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "internal.h"

#include <fontconfig/fontconfig.h>
#include <librsvg/rsvg.h>
#include <pango/pangocairo.h>

// Textures drawn by cairo: images, SVG documents and text.

// A transparent canvas of the size in logical pixels multiplied by the
// scale.
static cairo_t *
canvas_create(int width, int height, double scale)
{
	cairo_surface_t *surface = cairo_image_surface_create(
		CAIRO_FORMAT_ARGB32, (int)round(width * scale), (int)round(height * scale));
	assert(cairo_surface_status(surface) == CAIRO_STATUS_SUCCESS);
	cairo_t *cr = cairo_create(surface);
	assert(cairo_status(cr) == CAIRO_STATUS_SUCCESS);
	// The context holds a reference.
	cairo_surface_destroy(surface);
	return cr;
}

// Copies the canvas to a new texture, and destroys it.
static struct texture *
canvas_to_texture(cairo_t *cr)
{
	cairo_surface_t *surface = cairo_get_target(cr);
	cairo_surface_flush(surface);
	int width = cairo_image_surface_get_width(surface);
	int height = cairo_image_surface_get_height(surface);
	uint8_t *pixels = cairo_image_surface_get_data(surface);
	int stride = cairo_image_surface_get_stride(surface);
	assert(pixels && stride >= width * 4);
	struct texture *texture = texture_create((uint32_t)width,
		(uint32_t)height, DRM_FORMAT_ARGB8888);
	texture_write_pixels(texture, pixels, (uint32_t)stride, NULL);
	cairo_destroy(cr);
	return texture;
}

// Scales the SVG document to fit the canvas, keeping its aspect ratio.
static bool
paint_svg(cairo_t *cr, RsvgHandle *handle)
{
	cairo_surface_t *surface = cairo_get_target(cr);
	RsvgRectangle viewport = {
		.width = cairo_image_surface_get_width(surface),
		.height = cairo_image_surface_get_height(surface),
	};
	return rsvg_handle_render_document(handle, cr, &viewport, NULL);
}

// Scales the image to fit the canvas, keeping its aspect ratio, and centers
// it.
static void
paint_image(cairo_t *cr, cairo_surface_t *image)
{
	cairo_surface_t *surface = cairo_get_target(cr);
	double canvas_width = cairo_image_surface_get_width(surface);
	double canvas_height = cairo_image_surface_get_height(surface);
	double image_width = cairo_image_surface_get_width(image);
	double image_height = cairo_image_surface_get_height(image);
	double scale = MIN(canvas_width / image_width, canvas_height / image_height);
	cairo_translate(cr, (canvas_width - image_width * scale) / 2.0,
		(canvas_height - image_height * scale) / 2.0);
	cairo_scale(cr, scale, scale);
	cairo_set_source_surface(cr, image, 0, 0);
	cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_GOOD);
	cairo_paint(cr);
}

// An SVG document, scaled to fill the size, which draws in the color where it
// uses currentColor. The texture has the size in logical pixels multiplied by
// the scale.
struct texture *
texture_create_svg(const char *data, int width, int height, double scale,
	struct color color)
{
	RsvgHandle *handle = rsvg_handle_new_from_data((const guint8 *)data,
		strlen(data), NULL);
	assert(handle);
	char stylesheet[64];
	int length = snprintf(stylesheet, sizeof(stylesheet),
		"svg { color: rgba(%ld, %ld, %ld, %.3f); }", lroundf(color.r * 255.0f),
		lroundf(color.g * 255.0f), lroundf(color.b * 255.0f), color.a);
	assert(length > 0 && (size_t)length < sizeof(stylesheet));
	assert(rsvg_handle_set_stylesheet(handle, (const guint8 *)stylesheet,
		(gsize)length, NULL));
	cairo_t *cr = canvas_create(width, height, scale);
	assert(paint_svg(cr, handle));
	g_object_unref(handle);
	return canvas_to_texture(cr);
}

static bool
has_suffix(const char *string, const char *suffix)
{
	size_t length = strlen(string);
	size_t suffix_length = strlen(suffix);
	return length >= suffix_length
		&& strcmp(string + length - suffix_length, suffix) == 0;
}

// An SVG or PNG file, scaled to fit the size like texture_create_svg().
// Returns NULL if the file cannot be loaded.
// TODO: Support XPM files, which old applications still install.
struct texture *
texture_create_image_file(const char *path, int width, int height, double scale)
{
	if (has_suffix(path, ".svg")) {
		RsvgHandle *handle = rsvg_handle_new_from_file(path, NULL);
		if (!handle) {
			return NULL;
		}
		cairo_t *cr = canvas_create(width, height, scale);
		bool painted = paint_svg(cr, handle);
		g_object_unref(handle);
		if (!painted) {
			cairo_destroy(cr);
			return NULL;
		}
		return canvas_to_texture(cr);
	}
	if (has_suffix(path, ".png")) {
		cairo_surface_t *image = cairo_image_surface_create_from_png(path);
		if (cairo_surface_status(image) != CAIRO_STATUS_SUCCESS) {
			cairo_surface_destroy(image);
			return NULL;
		}
		cairo_t *cr = canvas_create(width, height, scale);
		paint_image(cr, image);
		cairo_surface_destroy(image);
		return canvas_to_texture(cr);
	}
	return NULL;
}

// Premultiplied ARGB8888 pixels without padding, scaled to fit the size like
// texture_create_svg().
struct texture *
texture_create_image_pixels(const uint32_t *pixels, int image_width,
	int image_height, int width, int height, double scale)
{
	// Cairo only reads the pixels.
	cairo_surface_t *image = cairo_image_surface_create_for_data(
		(unsigned char *)pixels, CAIRO_FORMAT_ARGB32, image_width,
		image_height, image_width * 4);
	assert(cairo_surface_status(image) == CAIRO_STATUS_SUCCESS);
	cairo_t *cr = canvas_create(width, height, scale);
	paint_image(cr, image);
	cairo_surface_destroy(image);
	return canvas_to_texture(cr);
}

void
text_fini(void)
{
	pango_cairo_font_map_set_default(NULL);
	FcFini();
}

// A line of text, laid out for the scale of the canvas.
static PangoLayout *
create_text_layout(cairo_t *cr, const char *text, const char *font)
{
	PangoLayout *layout = pango_cairo_create_layout(cr);
	assert(layout);
	PangoFontDescription *description = pango_font_description_from_string(font);
	assert(description);
	pango_layout_set_font_description(layout, description); // Copies it
	pango_font_description_free(description);
	pango_layout_set_text(layout, text, -1);
	pango_layout_set_single_paragraph_mode(layout, TRUE);
	return layout;
}

// The width the text takes at the scale, which differs by the scale with the
// hinting of the font.
int
text_width(const char *text, const char *font, double scale)
{
	cairo_t *cr = canvas_create(1, 1, scale);
	cairo_scale(cr, scale, scale);
	PangoLayout *layout = create_text_layout(cr, text, font);
	int width;
	pango_layout_get_pixel_size(layout, &width, NULL);
	g_object_unref(layout);
	cairo_destroy(cr);
	return width;
}

// A line of text, ellipsized to fit the width and centered vertically, like
// texture_create_svg() in size.
struct texture *
texture_create_text(const char *text, const char *font, struct color color,
	int width, int height, double scale)
{
	cairo_t *cr = canvas_create(width, height, scale);
	cairo_scale(cr, scale, scale);
	PangoLayout *layout = create_text_layout(cr, text, font);
	pango_layout_set_width(layout, width * PANGO_SCALE);
	pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_END);

	// The text starts on a pixel of the canvas. Otherwise its strokes are
	// blurred across two pixels. Pango aligns right-to-left text to the
	// right, at x, which is a fraction of a pixel at fractional scales.
	PangoRectangle logical;
	pango_layout_get_extents(layout, NULL, &logical);
	double x = (double)logical.x / PANGO_SCALE;
	int text_height;
	pango_layout_get_pixel_size(layout, NULL, &text_height);
	double y = round((height - text_height) / 2.0 * scale) / scale;
	cairo_move_to(cr, round(x * scale) / scale - x, y);
	cairo_set_source_rgba(cr, color.r, color.g, color.b, color.a);
	pango_cairo_show_layout(cr, layout);
	g_object_unref(layout);
	return canvas_to_texture(cr);
}
