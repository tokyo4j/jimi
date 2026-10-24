#ifndef JIMI_COLOR_H
#define JIMI_COLOR_H

#include "util.h"
#include "color-management-v1-protocol.h"

// CIE 1931 xy chromaticities
struct color_primaries {
	double red[2];
	double green[2];
	double blue[2];
	double white[2];
};

// How the pixels of an image encode colors, and the display that the image
// is meant for, as in color-management-v1. The records shown to clients are
// referenced and immutable. The renderer also describes its own images
// without records.
struct image_description {
	uint32_t refs;
	// Of the record, unique among those ever created. 0 for the others.
	uint64_t identity;
	// The clients may get the parameters, as they do for those of outputs.
	bool allows_information;
	// enum wp_color_manager_v1_primaries, or 0 for unnamed ones
	uint32_t primaries_named;
	struct color_primaries primaries;
	// enum wp_color_manager_v1_transfer_function, or 0 for a power curve of
	// the exponent
	uint32_t tf_named;
	double tf_power;
	// In cd/m². Values of 1.0 of the transfer function are shown at the
	// maximum, except with PQ, which is absolute.
	double min_luminance;
	double max_luminance;
	double reference_luminance;
	// The target color volume, of the mastering display
	struct color_primaries target_primaries;
	double target_min_luminance;
	double target_max_luminance;
	double max_cll; // 0 if unknown
	double max_fall; // 0 if unknown
};

// The transfer functions of the shaders, which match texture.frag.
enum color_tf {
	COLOR_TF_LINEAR,
	COLOR_TF_POWER,
	COLOR_TF_SRGB,
	COLOR_TF_BT1886,
	COLOR_TF_PQ,
};

// Converts the colors of an image description into those of another: decodes
// them by the transfer function of the source, transforms the linear colors
// by the matrix, which also maps the reference white of the source to that of
// the destination, and encodes them by the transfer function of the
// destination. Colors outside of the destination are clipped unless it is
// linear.
struct color_transform {
	bool identity;
	float matrix[3][3];
	enum color_tf src_tf;
	float src_power; // The exponent of COLOR_TF_POWER
	enum color_tf dst_tf;
	float dst_power;
};

// sRGB, shown by the gamma 2.2 of most displays. Surfaces without image
// descriptions, the images that we draw, and SDR outputs have it.
extern const struct image_description srgb_description;

bool color_primaries_from_named(uint32_t named, struct color_primaries *primaries);
void image_description_set_default_luminances(struct image_description *description);
struct image_description *image_description_create(
	const struct image_description *params);
struct image_description *image_description_ref(
	struct image_description *description);
void image_description_unref(struct image_description *description);
bool color_primaries_to_xyz(const struct color_primaries *primaries,
	double matrix[3][3]);
void color_xyz_to_rgb(const struct color_primaries *primaries,
	const double xyz[3], double rgb[3]);
double color_eotf(const struct image_description *description, double value);
double color_inverse_eotf(const struct image_description *description, double value);
void color_transform_init(struct color_transform *transform,
	const struct image_description *src, const struct image_description *dst);
struct color color_convert(struct color color, const struct image_description *dst);
void color_ycbcr_coefficients(const struct image_description *description,
	double *kr, double *kb);

#endif
