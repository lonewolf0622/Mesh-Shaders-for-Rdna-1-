#version 460
layout(location=0) in vec4 attr[8];
layout(location=0) out vec4 o;
void main(){ vec4 s=vec4(0); for(int k=0;k<8;k++) s+=attr[k]; o=s; }
