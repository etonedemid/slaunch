// Rolling dunes under the theme's gradient; Y in the editor sets the horizon.
void main() {
    vec3  col = mix(uTop.rgb, uBot.rgb, vP.y / 720.0);
    float t   = uTime * (0.04 + uFx2.x * 0.02);
    for (int i = 0; i < 4; i++) {
        float k = float(i);
        float h = uFx2.y + k * 60.0
                + sin(vP.x * (0.004 + k * 0.002) + t * (1.0 + k * 0.4) + k * 2.0) * (uFx.z * 1.5 + 10.0 * k);
        float a = smoothstep(h - 1.5, h + 1.5, vP.y);
        col = mix(col, mix(uColA.rgb, uBot.rgb, k / 3.5) * (0.9 - 0.15 * k), a);
    }
    gl_FragColor = vec4(col, 1.0);
}
