#version 450

// Or the luma plane of YCbCr
layout(set = 0, binding = 0) uniform sampler2D tex;
layout(set = 0, binding = 1) uniform sampler2D chroma;

// The fields of struct texture_push_constants after those of texture.vert
layout(push_constant, row_major) uniform Push {
	// The texture is YCbCr, which the affine matrix converts into RGB.
	layout(offset = 112) uint ycbcr;
	layout(offset = 128) vec4 ycbcr_matrix[3]; // The rows
	mat3 matrix;
	uint src_tf;
	float src_power;
	uint dst_tf;
	float dst_power;
	uint convert; // Otherwise, the colors are kept.
	uint opaque; // The alpha channel is ignored.
} pc;

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 out_color;

// Matches enum color_tf
const uint TF_LINEAR = 0u;
const uint TF_POWER = 1u;
const uint TF_SRGB = 2u;
const uint TF_BT1886 = 3u;
const uint TF_PQ = 4u;

// Of SMPTE ST 2084
const float PQ_M1 = 0.1593017578125;
const float PQ_M2 = 78.84375;
const float PQ_C1 = 0.8359375;
const float PQ_C2 = 18.8515625;
const float PQ_C3 = 18.6875;
// The display that BT.1886 assumes by default, in cd/m²
const float BT1886_BLACK = 0.01;
const float BT1886_WHITE = 100.0;

// The linear values, 1.0 at the luminance that the transfer function encodes
// as 1.0. The curves are mirrored for negative values, which extended ranges
// have. Like color.c.
vec3 eotf(uint tf, float power, vec3 value)
{
	vec3 magnitude = abs(value);
	if (tf == TF_POWER) {
		return sign(value) * pow(magnitude, vec3(power));
	} else if (tf == TF_SRGB) {
		return sign(value) * mix(magnitude / 12.92,
			pow((magnitude + 0.055) / 1.055, vec3(2.4)),
			greaterThan(magnitude, vec3(0.04045)));
	} else if (tf == TF_BT1886) {
		float black = pow(BT1886_BLACK, 1.0 / 2.4);
		float white = pow(BT1886_WHITE, 1.0 / 2.4);
		float gain = pow(white - black, 2.4);
		float lift = black / (white - black);
		vec3 luminance = gain * pow(max(value + lift, 0.0), vec3(2.4));
		return (luminance - BT1886_BLACK) / (BT1886_WHITE - BT1886_BLACK);
	} else if (tf == TF_PQ) {
		vec3 p = pow(clamp(value, 0.0, 1.0), vec3(1.0 / PQ_M2));
		return pow(max(p - PQ_C1, 0.0) / (PQ_C2 - PQ_C3 * p), vec3(1.0 / PQ_M1));
	}
	return value;
}

vec3 inverse_eotf(uint tf, float power, vec3 value)
{
	vec3 magnitude = abs(value);
	if (tf == TF_POWER) {
		return sign(value) * pow(magnitude, vec3(1.0 / power));
	} else if (tf == TF_SRGB) {
		return sign(value) * mix(magnitude * 12.92,
			1.055 * pow(magnitude, vec3(1.0 / 2.4)) - 0.055,
			greaterThan(magnitude, vec3(0.0031308)));
	} else if (tf == TF_BT1886) {
		float black = pow(BT1886_BLACK, 1.0 / 2.4);
		float white = pow(BT1886_WHITE, 1.0 / 2.4);
		float gain = pow(white - black, 2.4);
		float lift = black / (white - black);
		vec3 luminance = value * (BT1886_WHITE - BT1886_BLACK) + BT1886_BLACK;
		return pow(max(luminance, 0.0) / gain, vec3(1.0 / 2.4)) - lift;
	} else if (tf == TF_PQ) {
		vec3 p = pow(clamp(value, 0.0, 1.0), vec3(PQ_M1));
		return pow((PQ_C1 + PQ_C2 * p) / (1.0 + PQ_C3 * p), vec3(PQ_M2));
	}
	return value;
}

// The colors are premultiplied by the alpha of their encoding, so they are
// divided by it to be converted, and multiplied by it again. Colors outside
// of the destination are clipped unless it is linear.
void main()
{
	vec4 color = texture(tex, v_uv);
	if (pc.ycbcr != 0u) {
		vec4 ycbcr = vec4(color.r, texture(chroma, v_uv).rg, 1.0);
		color = vec4(dot(pc.ycbcr_matrix[0], ycbcr),
			dot(pc.ycbcr_matrix[1], ycbcr), dot(pc.ycbcr_matrix[2], ycbcr), 1.0);
	}
	if (pc.opaque != 0u) {
		color.a = 1.0;
	}
	if (pc.convert != 0u) {
		vec3 rgb = color.a > 0.0 ? color.rgb / color.a : vec3(0.0);
		rgb = pc.matrix * eotf(pc.src_tf, pc.src_power, rgb);
		if (pc.dst_tf != TF_LINEAR) {
			rgb = clamp(rgb, 0.0, 1.0);
		}
		color.rgb = inverse_eotf(pc.dst_tf, pc.dst_power, rgb) * color.a;
	}
	out_color = color;
}
