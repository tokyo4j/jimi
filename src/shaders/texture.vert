#version 450

layout(location = 0) out vec2 v_uv;

// The fields that texture.frag does not use
layout(push_constant, row_major) uniform Push {
	vec4 output_size;
	vec4 ndc_transform; // The rows of a 2x2 matrix
	vec4 dst;
	mat4 uv_transform;
} pc;

const vec2 pos[6] = vec2[](vec2(0.0, 0.0), vec2(1.0, 0.0), vec2(0.0, 1.0),
	vec2(0.0, 1.0), vec2(1.0, 0.0), vec2(1.0, 1.0));

void main()
{
	vec2 p = pos[gl_VertexIndex];
	vec2 pixel = pc.dst.xy + p * pc.dst.zw;
	vec2 ndc = pixel / pc.output_size.xy * 2.0 - 1.0;
	ndc = vec2(dot(pc.ndc_transform.xy, ndc), dot(pc.ndc_transform.zw, ndc));
	v_uv = (pc.uv_transform * vec4(p, 0.0, 1.0)).xy;
	gl_Position = vec4(ndc, 0.0, 1.0);
}
