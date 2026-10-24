#version 450

// The fields that the push constants of the pipelines drawing boxes start with
layout(push_constant) uniform Push {
	vec4 output_size;
	vec4 ndc_transform; // The rows of a 2x2 matrix
	ivec4 dst;
} pc;

// In pixels from the top-left corner of the box
layout(location = 0) out vec2 v_pos;

const vec2 pos[6] = vec2[](vec2(0.0, 0.0), vec2(1.0, 0.0), vec2(0.0, 1.0),
	vec2(0.0, 1.0), vec2(1.0, 0.0), vec2(1.0, 1.0));

void main()
{
	vec2 p = pos[gl_VertexIndex];
	vec2 size = vec2(pc.dst.zw);
	vec2 pixel = vec2(pc.dst.xy) + p * size;
	vec2 ndc = pixel / pc.output_size.xy * 2.0 - 1.0;
	ndc = vec2(dot(pc.ndc_transform.xy, ndc), dot(pc.ndc_transform.zw, ndc));
	v_pos = p * size;
	gl_Position = vec4(ndc, 0.0, 1.0);
}
