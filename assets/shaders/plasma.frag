// Soft plasma in the theme's two effect colours.
void main() {
    vec2  p = vP / 720.0;
    float t = uTime * (0.05 + uFx2.x * 0.03);
    float v = sin(p.x * 3.1 + t * 1.7) + sin(p.y * 2.3 - t * 1.3)
            + sin((p.x + p.y) * 2.7 + t) + sin(length(p - vec2(0.9, 0.5)) * 5.0 - t * 2.1);
    vec3 bg = mix(uTop.rgb, uBot.rgb, vP.y / 720.0);
    vec3 c  = mix(uColA.rgb, uColB.rgb, 0.5 + 0.5 * sin(v * 1.6));
    gl_FragColor = vec4(mix(bg, c, 0.35), 1.0);
}
