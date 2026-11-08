#version 460
#extension GL_EXT_fragment_shader_barycentric : require
layout(location=0) out vec4 o;
void main(){ o=vec4(gl_BaryCoordEXT, 1); }
