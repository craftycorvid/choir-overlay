#version 450
layout(location = 0) out vec4 fColor;
layout(set = 0, binding = 0) uniform sampler2D sTexture;
layout(location = 0) in struct { vec4 Color; vec2 UV; } In;

layout(constant_id = 0) const int  uMode = 0;     // 0 None,1 Srgb,2 ScRgb,3 Pq,4 Hlg
layout(constant_id = 1) const float uNits = 200.0;

float s2l(float c) { return c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4); }
vec3  s2l(vec3 c)  { return vec3(s2l(c.r), s2l(c.g), s2l(c.b)); }
vec3  to2020(vec3 c) {
    return vec3(0.627392*c.r + 0.329030*c.g + 0.0432691*c.b,
                0.069123*c.r + 0.919523*c.g + 0.0113204*c.b,
                0.016423*c.r + 0.088042*c.g + 0.8956166*c.b);
}
float l2pq(float v) {
    const float m1=0.1593017578125, m2=78.84375, c1=0.8359375, c2=18.8515625, c3=18.6875;
    v = pow(v * (uNits / 10000.0), m1);
    v = (c1 + c2*v) / (1.0 + c3*v);
    return pow(v, m2);
}
float l2hlg(float v) {
    const float a=0.17883277, b=0.28466892, c=0.55991073;
    return v <= 1.0/12.0 ? sqrt(3.0*v) : a*log(12.0*v - b) + c;
}
vec3 hdr_encode(vec3 c) {
    if (uMode == 0) return c;
    c = s2l(c);
    if (uMode == 1) return c;
    if (uMode == 2) return c * (uNits / 80.0);
    c = to2020(c);
    if (uMode == 3) return vec3(l2pq(c.r), l2pq(c.g), l2pq(c.b));
    return vec3(l2hlg(c.r), l2hlg(c.g), l2hlg(c.b));
}
void main() {
    vec4 c = In.Color * texture(sTexture, In.UV.st);
    c.rgb = hdr_encode(c.rgb);
    fColor = c;
}
