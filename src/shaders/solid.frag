#version 450

layout(push_constant) uniform Push {
	vec4 output_size;
	vec4 ndc_transform; // The rows of a 2x2 matrix
	ivec4 dst;
	vec4 color;
	float radius;
	uint corner_mask;
	float border_width;
} pc;

layout(location = 0) in vec2 v_pos;
layout(location = 0) out vec4 out_color;

const uint CORNER_TOP_LEFT = 1u;
const uint CORNER_TOP_RIGHT = 2u;
const uint CORNER_BOTTOM_RIGHT = 4u;
const uint CORNER_BOTTOM_LEFT = 8u;

// The signed distance from the edge of the box, negative inside. The corners
// in the mask are rounded by the radius.
float box_distance(vec2 size, float radius)
{
	vec2 half_size = size * 0.5;
	vec2 pos = v_pos - half_size;
	uint corner = pos.y < 0.0
		? (pos.x < 0.0 ? CORNER_TOP_LEFT : CORNER_TOP_RIGHT)
		: (pos.x < 0.0 ? CORNER_BOTTOM_LEFT : CORNER_BOTTOM_RIGHT);
	float r = (pc.corner_mask & corner) != 0u ? radius : 0.0;
	vec2 q = abs(pos) - half_size + r;
	return min(max(q.x, q.y), 0.0) + length(max(q, 0.0)) - r;
}

void main()
{
	vec2 size = vec2(pc.dst.zw);
	float radius = min(pc.radius, min(size.x, size.y) * 0.5);
	float dist = box_distance(size, radius);
	float alpha = 1.0 - smoothstep(-0.5, 0.5, dist);
	// The border is the box without the box inset by its width.
	if (pc.border_width > 0.0) {
		alpha *= smoothstep(-0.5, 0.5, dist + pc.border_width);
	}

	alpha *= pc.color.a;
	if (alpha <= 0.0) {
		discard;
	}
	out_color = vec4(pc.color.rgb * alpha, alpha);
}
