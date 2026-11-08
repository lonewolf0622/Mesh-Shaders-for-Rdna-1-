#version 460
layout(location=0) out vec4 o;
void main(){ o=vec4(float(gl_PrimitiveID & 255)/255.0, float((gl_PrimitiveID>>8) & 255)/255.0, 0, 1); }
