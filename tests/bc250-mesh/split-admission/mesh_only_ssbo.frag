#version 460
layout(location=0) flat in vec4 attr[10];
layout(location=0) out vec4 o;
void main(){ vec4 s=vec4(0); for(int k=0;k<10;k++) s+=attr[k]; o=s; }
