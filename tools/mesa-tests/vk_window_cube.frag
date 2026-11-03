#version 450
layout(location = 0) in vec3 shade;
layout(location = 0) out vec4 colour;

void main() {
    colour = vec4(shade, 1.0);
}
