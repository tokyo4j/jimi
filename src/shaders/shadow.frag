#version 450

layout(push_constant) uniform Push {
	vec4 output_size;
	vec4 ndc_transform; // The rows of a 2x2 matrix
	ivec4 dst;
	vec4 color;
	ivec4 box; // Casting the shadow, relative to dst
	ivec2 offset; // Of the shadow from the box
	float radius;
	uint corner_mask;
	float sigma; // Of the gaussian blur
} pc;

layout(location = 0) in vec2 v_pos;
layout(location = 0) out vec4 out_color;

const uint CORNER_TOP_LEFT = 1u;
const uint CORNER_TOP_RIGHT = 2u;
const uint CORNER_BOTTOM_RIGHT = 4u;
const uint CORNER_BOTTOM_LEFT = 8u;

const float PI = 3.141592653589793;
const int NR_SAMPLES = 4;

// The radius of the corner of the box, as rounded by the mask.
float corner_radius(uint corner, float radius)
{
	return (pc.corner_mask & corner) != 0u ? radius : 0.0;
}

// The signed distance from the edge of the box of the size at pos, negative
// inside.
float box_distance(vec2 pos, vec2 size, float radius)
{
	vec2 half_size = size * 0.5;
	pos -= half_size;
	uint corner = pos.y < 0.0
		? (pos.x < 0.0 ? CORNER_TOP_LEFT : CORNER_TOP_RIGHT)
		: (pos.x < 0.0 ? CORNER_BOTTOM_LEFT : CORNER_BOTTOM_RIGHT);
	float r = corner_radius(corner, radius);
	vec2 q = abs(pos) - half_size + r;
	return min(max(q.x, q.y), 0.0) + length(max(q, 0.0)) - r;
}

float gaussian(float x)
{
	return exp(-(x * x) / (2.0 * pc.sigma * pc.sigma)) / (sqrt(2.0 * PI) * pc.sigma);
}

// An approximation of the error function, which GLSL lacks
vec2 erf(vec2 x)
{
	vec2 s = sign(x);
	vec2 a = abs(x);
	x = 1.0 + (0.278393 + (0.230389 + 0.078108 * (a * a)) * a) * a;
	x *= x;
	return s - s / (x * x);
}

// How far the row of the box at y from its center extends from the center
// towards the side whose corner has the radius.
float row_extent(float y, vec2 half_size, float radius)
{
	float delta = min(half_size.y - radius - abs(y), 0.0);
	return half_size.x - radius + sqrt(max(radius * radius - delta * delta, 0.0));
}

// The row of the box at y from its center, blurred horizontally at x: the
// integral of the gaussian around x over the row.
float blurred_row(float x, float y, vec2 half_size, float radius)
{
	bool top = y < 0.0;
	float left = row_extent(y, half_size,
		corner_radius(top ? CORNER_TOP_LEFT : CORNER_BOTTOM_LEFT, radius));
	float right = row_extent(y, half_size,
		corner_radius(top ? CORNER_TOP_RIGHT : CORNER_BOTTOM_RIGHT, radius));
	vec2 integral = 0.5 + 0.5 * erf(vec2(x + left, x - right)
		* (sqrt(0.5) / pc.sigma));
	return integral.x - integral.y;
}

// The box of the size blurred at pos, relative to its center. The blurred
// rows are summed vertically by a few samples of the gaussian around pos,
// where it is not negligible. See
// https://madebyevan.com/shaders/fast-rounded-rectangle-shadows/
float blurred_box(vec2 pos, vec2 size, float radius)
{
	vec2 half_size = size * 0.5;
	pos -= half_size;
	float start = clamp(pos.y - 3.0 * pc.sigma, -half_size.y, half_size.y);
	float end = clamp(pos.y + 3.0 * pc.sigma, -half_size.y, half_size.y);
	float step = (end - start) / float(NR_SAMPLES);
	float value = 0.0;
	for (int i = 0; i < NR_SAMPLES; i++) {
		float y = start + step * (float(i) + 0.5);
		value += blurred_row(pos.x, y, half_size, radius)
			* gaussian(pos.y - y) * step;
	}
	return value;
}

void main()
{
	vec2 size = vec2(pc.box.zw);
	float radius = min(pc.radius, min(size.x, size.y) * 0.5);
	vec2 pos = v_pos - vec2(pc.box.xy);
	// The pixels that the box covers entirely are left out, so that the
	// shadow does not show through what fills the box.
	if (box_distance(pos, size, radius) <= -0.5) {
		discard;
	}

	float alpha = blurred_box(pos - vec2(pc.offset), size, radius)
		* pc.color.a;
	if (alpha <= 0.0) {
		discard;
	}
	out_color = vec4(pc.color.rgb * alpha, alpha);
}
