#include <assert.h>
#include <math.h>
#include <stdlib.h>

#include "color.h"

// The constants of SMPTE ST 2084
#define PQ_M1 0.1593017578125
#define PQ_M2 78.84375
#define PQ_C1 0.8359375
#define PQ_C2 18.8515625
#define PQ_C3 18.6875
// That PQ encodes as 1.0, in cd/m²
#define PQ_MAX_LUMINANCE 10000.0
// The display that BT.1886 assumes by default, in cd/m²
#define BT1886_BLACK 0.01
#define BT1886_WHITE 100.0

// By enum wp_color_manager_v1_primaries, from Recommendation ITU-T H.273
static const struct color_primaries named_primaries[] = {
	[WP_COLOR_MANAGER_V1_PRIMARIES_SRGB] = {
		{0.640, 0.330}, {0.300, 0.600}, {0.150, 0.060}, {0.3127, 0.3290},
	},
	[WP_COLOR_MANAGER_V1_PRIMARIES_PAL_M] = {
		{0.670, 0.330}, {0.210, 0.710}, {0.140, 0.080}, {0.310, 0.316},
	},
	[WP_COLOR_MANAGER_V1_PRIMARIES_PAL] = {
		{0.640, 0.330}, {0.290, 0.600}, {0.150, 0.060}, {0.3127, 0.3290},
	},
	[WP_COLOR_MANAGER_V1_PRIMARIES_NTSC] = {
		{0.630, 0.340}, {0.310, 0.595}, {0.155, 0.070}, {0.3127, 0.3290},
	},
	[WP_COLOR_MANAGER_V1_PRIMARIES_GENERIC_FILM] = {
		{0.681, 0.319}, {0.243, 0.692}, {0.145, 0.049}, {0.310, 0.316},
	},
	[WP_COLOR_MANAGER_V1_PRIMARIES_BT2020] = {
		{0.708, 0.292}, {0.170, 0.797}, {0.131, 0.046}, {0.3127, 0.3290},
	},
	[WP_COLOR_MANAGER_V1_PRIMARIES_CIE1931_XYZ] = {
		{1.0, 0.0}, {0.0, 1.0}, {0.0, 0.0}, {1.0 / 3.0, 1.0 / 3.0},
	},
	[WP_COLOR_MANAGER_V1_PRIMARIES_DCI_P3] = {
		{0.680, 0.320}, {0.265, 0.690}, {0.150, 0.060}, {0.314, 0.351},
	},
	[WP_COLOR_MANAGER_V1_PRIMARIES_DISPLAY_P3] = {
		{0.680, 0.320}, {0.265, 0.690}, {0.150, 0.060}, {0.3127, 0.3290},
	},
	[WP_COLOR_MANAGER_V1_PRIMARIES_ADOBE_RGB] = {
		{0.640, 0.330}, {0.210, 0.710}, {0.150, 0.060}, {0.3127, 0.3290},
	},
};

const struct image_description srgb_description = {
	.primaries_named = WP_COLOR_MANAGER_V1_PRIMARIES_SRGB,
	.primaries = {
		{0.640, 0.330}, {0.300, 0.600}, {0.150, 0.060}, {0.3127, 0.3290},
	},
	.tf_named = WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_GAMMA22,
	.min_luminance = 0.2,
	.max_luminance = 80.0,
	.reference_luminance = 80.0,
	.target_primaries = {
		{0.640, 0.330}, {0.300, 0.600}, {0.150, 0.060}, {0.3127, 0.3290},
	},
	.target_min_luminance = 0.2,
	.target_max_luminance = 80.0,
};

// False if the primaries are not named.
bool
color_primaries_from_named(uint32_t named, struct color_primaries *primaries)
{
	if (named == 0 || named >= ARRAY_LEN(named_primaries)) {
		return false;
	}
	*primaries = named_primaries[named];
	return true;
}

// Those of the transfer function, from the protocol.
void
image_description_set_default_luminances(struct image_description *description)
{
	switch (description->tf_named) {
	case WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_ST2084_PQ:
		description->min_luminance = 0.005;
		description->max_luminance = 0.005 + PQ_MAX_LUMINANCE;
		description->reference_luminance = 203.0;
		return;
	case WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_BT1886:
		description->min_luminance = BT1886_BLACK;
		description->max_luminance = BT1886_WHITE;
		description->reference_luminance = BT1886_WHITE;
		return;
	default:
		description->min_luminance = 0.2;
		description->max_luminance = 80.0;
		description->reference_luminance = 80.0;
		return;
	}
}

static uint64_t next_identity = 1;

// A new record with the parameters.
struct image_description *
image_description_create(const struct image_description *params)
{
	struct image_description *description = malloc(sizeof(*description));
	assert(description);
	*description = *params;
	description->refs = 1;
	description->identity = next_identity++;
	return description;
}

struct image_description *
image_description_ref(struct image_description *description)
{
	description->refs++;
	return description;
}

// NULL is ignored.
void
image_description_unref(struct image_description *description)
{
	if (description && --description->refs == 0) {
		free(description);
	}
}

static void
matrix3_multiply(double dest[3][3], const double a[3][3], const double b[3][3])
{
	double result[3][3];
	for (int i = 0; i < 3; i++) {
		for (int j = 0; j < 3; j++) {
			result[i][j] = a[i][0] * b[0][j] + a[i][1] * b[1][j] + a[i][2] * b[2][j];
		}
	}
	for (int i = 0; i < 3; i++) {
		for (int j = 0; j < 3; j++) {
			dest[i][j] = result[i][j];
		}
	}
}

static void
matrix3_apply(const double m[3][3], const double v[3], double dest[3])
{
	double result[3];
	for (int i = 0; i < 3; i++) {
		result[i] = m[i][0] * v[0] + m[i][1] * v[1] + m[i][2] * v[2];
	}
	for (int i = 0; i < 3; i++) {
		dest[i] = result[i];
	}
}

// False if the matrix is singular.
static bool
matrix3_invert(double dest[3][3], const double m[3][3])
{
	double cofactors[3][3];
	for (int i = 0; i < 3; i++) {
		for (int j = 0; j < 3; j++) {
			int i1 = (i + 1) % 3, i2 = (i + 2) % 3;
			int j1 = (j + 1) % 3, j2 = (j + 2) % 3;
			cofactors[i][j] = m[i1][j1] * m[i2][j2] - m[i1][j2] * m[i2][j1];
		}
	}
	double det = m[0][0] * cofactors[0][0] + m[0][1] * cofactors[0][1]
		+ m[0][2] * cofactors[0][2];
	if (fabs(det) < 1e-9) {
		return false;
	}
	for (int i = 0; i < 3; i++) {
		for (int j = 0; j < 3; j++) {
			dest[i][j] = cofactors[j][i] / det;
		}
	}
	return true;
}

// Of luminance 1
static void
white_xyz(const double white[2], double xyz[3])
{
	xyz[0] = white[0] / white[1];
	xyz[1] = 1.0;
	xyz[2] = (1.0 - white[0] - white[1]) / white[1];
}

// The matrix from linear RGB of the primaries to CIE 1931 XYZ, which shows
// white with luminance 1. False if the primaries do not span a color space.
// See http://www.brucelindbloom.com/index.html?Eqn_RGB_XYZ_Matrix.html
bool
color_primaries_to_xyz(const struct color_primaries *primaries, double matrix[3][3])
{
	const double *xy[3] = {primaries->red, primaries->green, primaries->blue};
	// The columns are the primaries at an arbitrary luminance, which is
	// fixed by the white. Unlike their luminance 1, this allows primaries
	// with y = 0 like those of XYZ.
	double columns[3][3];
	for (int i = 0; i < 3; i++) {
		columns[0][i] = xy[i][0];
		columns[1][i] = xy[i][1];
		columns[2][i] = 1.0 - xy[i][0] - xy[i][1];
	}
	double inverse[3][3];
	if (primaries->white[1] <= 0.0 || !matrix3_invert(inverse, columns)) {
		return false;
	}
	double white[3], scale[3];
	white_xyz(primaries->white, white);
	matrix3_apply(inverse, white, scale);
	for (int i = 0; i < 3; i++) {
		for (int j = 0; j < 3; j++) {
			matrix[i][j] = columns[i][j] * scale[j];
		}
	}
	return true;
}

// The linear RGB of the primaries for the XYZ color.
void
color_xyz_to_rgb(const struct color_primaries *primaries, const double xyz[3],
	double rgb[3])
{
	double to_xyz[3][3], from_xyz[3][3];
	assert(color_primaries_to_xyz(primaries, to_xyz));
	assert(matrix3_invert(from_xyz, to_xyz));
	matrix3_apply(from_xyz, xyz, rgb);
}

// Adapts XYZ colors seen under one white to the other by the Bradford
// transform, so that the viewer adapted to either sees the same colors.
static void
chromatic_adaptation(const double from[2], const double to[2], double matrix[3][3])
{
	static const double bradford[3][3] = {
		{0.8951, 0.2664, -0.1614},
		{-0.7502, 1.7135, 0.0367},
		{0.0389, -0.0685, 1.0296},
	};
	double inverse[3][3];
	assert(matrix3_invert(inverse, bradford));
	double from_xyz[3], to_xyz[3], from_cone[3], to_cone[3];
	white_xyz(from, from_xyz);
	white_xyz(to, to_xyz);
	matrix3_apply(bradford, from_xyz, from_cone);
	matrix3_apply(bradford, to_xyz, to_cone);
	double scale[3][3] = {
		{to_cone[0] / from_cone[0], 0.0, 0.0},
		{0.0, to_cone[1] / from_cone[1], 0.0},
		{0.0, 0.0, to_cone[2] / from_cone[2]},
	};
	matrix3_multiply(matrix, scale, bradford);
	matrix3_multiply(matrix, inverse, matrix);
}

// The transfer function of the shaders for the image description.
static enum color_tf
shader_tf(const struct image_description *description, float *power)
{
	*power = 1.0f;
	switch (description->tf_named) {
	case 0:
		*power = (float)description->tf_power;
		return COLOR_TF_POWER;
	case WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_GAMMA22:
		*power = 2.2f;
		return COLOR_TF_POWER;
	case WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_GAMMA28:
		*power = 2.8f;
		return COLOR_TF_POWER;
	case WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_COMPOUND_POWER_2_4:
		return COLOR_TF_SRGB;
	case WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_BT1886:
		return COLOR_TF_BT1886;
	case WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_ST2084_PQ:
		return COLOR_TF_PQ;
	case WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_EXT_LINEAR:
		return COLOR_TF_LINEAR;
	}
	assert(!"unsupported transfer function");
	return COLOR_TF_LINEAR;
}

// Like texture.frag. The curves are mirrored for negative values, which
// extended ranges have.
static double
eotf(enum color_tf tf, double power, double value)
{
	double magnitude = fabs(value);
	switch (tf) {
	case COLOR_TF_LINEAR:
		return value;
	case COLOR_TF_POWER:
		return copysign(pow(magnitude, power), value);
	case COLOR_TF_SRGB:
		return copysign(magnitude <= 0.04045 ? magnitude / 12.92
			: pow((magnitude + 0.055) / 1.055, 2.4), value);
	case COLOR_TF_BT1886: {
		double black = pow(BT1886_BLACK, 1.0 / 2.4);
		double white = pow(BT1886_WHITE, 1.0 / 2.4);
		double gain = pow(white - black, 2.4);
		double lift = black / (white - black);
		double luminance = gain * pow(fmax(value + lift, 0.0), 2.4);
		return (luminance - BT1886_BLACK) / (BT1886_WHITE - BT1886_BLACK);
	}
	case COLOR_TF_PQ: {
		double p = pow(clamp(value, 0.0, 1.0), 1.0 / PQ_M2);
		return pow(fmax(p - PQ_C1, 0.0) / (PQ_C2 - PQ_C3 * p), 1.0 / PQ_M1);
	}
	}
	assert(!"unknown transfer function");
	return value;
}

static double
inverse_eotf(enum color_tf tf, double power, double value)
{
	double magnitude = fabs(value);
	switch (tf) {
	case COLOR_TF_LINEAR:
		return value;
	case COLOR_TF_POWER:
		return copysign(pow(magnitude, 1.0 / power), value);
	case COLOR_TF_SRGB:
		return copysign(magnitude <= 0.0031308 ? magnitude * 12.92
			: 1.055 * pow(magnitude, 1.0 / 2.4) - 0.055, value);
	case COLOR_TF_BT1886: {
		double black = pow(BT1886_BLACK, 1.0 / 2.4);
		double white = pow(BT1886_WHITE, 1.0 / 2.4);
		double gain = pow(white - black, 2.4);
		double lift = black / (white - black);
		double luminance = value * (BT1886_WHITE - BT1886_BLACK) + BT1886_BLACK;
		return pow(fmax(luminance, 0.0) / gain, 1.0 / 2.4) - lift;
	}
	case COLOR_TF_PQ: {
		double p = pow(clamp(value, 0.0, 1.0), PQ_M1);
		return pow((PQ_C1 + PQ_C2 * p) / (1.0 + PQ_C3 * p), PQ_M2);
	}
	}
	assert(!"unknown transfer function");
	return value;
}

// The linear value of the encoded one, 1.0 at the luminance that the
// transfer function encodes as 1.0.
double
color_eotf(const struct image_description *description, double value)
{
	float power;
	enum color_tf tf = shader_tf(description, &power);
	return eotf(tf, power, value);
}

double
color_inverse_eotf(const struct image_description *description, double value)
{
	float power;
	enum color_tf tf = shader_tf(description, &power);
	return inverse_eotf(tf, power, value);
}

// That the transfer function encodes as 1.0
static double
tf_max_luminance(const struct image_description *description)
{
	return description->tf_named
			== WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_ST2084_PQ
		? PQ_MAX_LUMINANCE : description->max_luminance;
}

// The colors are adapted to the white of the destination, as for the
// perceptual rendering intent, and anchored at the reference whites.
void
color_transform_init(struct color_transform *transform,
	const struct image_description *src, const struct image_description *dst)
{
	transform->src_tf = shader_tf(src, &transform->src_power);
	transform->dst_tf = shader_tf(dst, &transform->dst_power);
	double src_to_xyz[3][3], dst_to_xyz[3][3], xyz_to_dst[3][3];
	double adaptation[3][3], matrix[3][3];
	assert(color_primaries_to_xyz(&src->primaries, src_to_xyz));
	assert(color_primaries_to_xyz(&dst->primaries, dst_to_xyz));
	assert(matrix3_invert(xyz_to_dst, dst_to_xyz));
	chromatic_adaptation(src->primaries.white, dst->primaries.white, adaptation);
	matrix3_multiply(matrix, adaptation, src_to_xyz);
	matrix3_multiply(matrix, xyz_to_dst, matrix);
	double luminance = tf_max_luminance(src) / src->reference_luminance
		* dst->reference_luminance / tf_max_luminance(dst);
	transform->identity = transform->src_tf == transform->dst_tf
		&& transform->src_power == transform->dst_power;
	for (int i = 0; i < 3; i++) {
		for (int j = 0; j < 3; j++) {
			double value = matrix[i][j] * luminance;
			transform->matrix[i][j] = (float)value;
			transform->identity = transform->identity
				&& fabs(value - (i == j ? 1.0 : 0.0)) < 1e-4;
		}
	}
}

// The sRGB color in the image description, like the shaders convert
// textures.
struct color
color_convert(struct color color, const struct image_description *dst)
{
	struct color_transform transform;
	color_transform_init(&transform, &srgb_description, dst);
	if (transform.identity) {
		return color;
	}
	double rgb[3] = {color.r, color.g, color.b};
	for (int i = 0; i < 3; i++) {
		rgb[i] = eotf(transform.src_tf, transform.src_power, rgb[i]);
	}
	double matrix[3][3];
	for (int i = 0; i < 3; i++) {
		for (int j = 0; j < 3; j++) {
			matrix[i][j] = transform.matrix[i][j];
		}
	}
	matrix3_apply(matrix, rgb, rgb);
	for (int i = 0; i < 3; i++) {
		if (transform.dst_tf != COLOR_TF_LINEAR) {
			rgb[i] = clamp(rgb[i], 0.0, 1.0);
		}
		rgb[i] = inverse_eotf(transform.dst_tf, transform.dst_power, rgb[i]);
	}
	return (struct color){(float)rgb[0], (float)rgb[1], (float)rgb[2],
		color.a};
}

// The coefficients of red and blue of the luma of YCbCr in the colors of the
// image description. Without color-representation-v1, they follow the
// primaries, like those of videos do.
void
color_ycbcr_coefficients(const struct image_description *description,
	double *kr, double *kb)
{
	switch (description->primaries_named) {
	case WP_COLOR_MANAGER_V1_PRIMARIES_BT2020:
		*kr = 0.2627;
		*kb = 0.0593;
		return;
	case WP_COLOR_MANAGER_V1_PRIMARIES_PAL_M:
	case WP_COLOR_MANAGER_V1_PRIMARIES_PAL:
	case WP_COLOR_MANAGER_V1_PRIMARIES_NTSC:
		*kr = 0.299;
		*kb = 0.114;
		return;
	default:
		*kr = 0.2126;
		*kb = 0.0722;
		return;
	}
}
