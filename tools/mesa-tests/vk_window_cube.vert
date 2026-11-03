#version 450
// vk-window's cube: 36 vertices (6 faces x 2 triangles) from gl_VertexIndex,
// a colour per face, as gl-window draws it.
layout(push_constant) uniform Push {
    mat4 transform;
} push;
layout(location = 0) out vec3 shade;

const vec3 corners[8] = vec3[8](vec3(-1, -1, -1), vec3(1, -1, -1), vec3(1, 1, -1), vec3(-1, 1, -1),
                                vec3(-1, -1, 1), vec3(1, -1, 1), vec3(1, 1, 1), vec3(-1, 1, 1));
const int faces[24] = int[24](0, 1, 2, 3, 5, 4, 7, 6, 4, 0, 3, 7, 1, 5, 6, 2, 3, 2, 6, 7, 4, 5, 1, 0);
const int order[6] = int[6](0, 1, 2, 0, 2, 3);
const vec3 colours[6] = vec3[6](vec3(0.22, 0.55, 0.82), vec3(0.42, 0.71, 0.93), vec3(0.49, 0.88, 0.63),
                                vec3(0.90, 0.28, 0.30), vec3(0.95, 0.78, 0.30), vec3(0.67, 0.71, 0.75));

void main() {
    int face = gl_VertexIndex / 6;
    gl_Position = push.transform * vec4(corners[faces[face * 4 + order[gl_VertexIndex % 6]]], 1.0);
    shade = colours[face];
}
