#ifdef GL_ES
precision mediump float;
#endif

#ifdef GL_ES
varying lowp vec4 v_fragmentColor;
#else
varying vec4 v_fragmentColor;
#endif

uniform float u_strength;
uniform float u_mode;

vec3 clampColor(vec3 c) {
    return clamp(c, 0.0, 1.0);
}

vec3 deuteranopia(vec3 c) {
    return clampColor(vec3(
            dot(c, vec3(0.367322, 0.860646, -0.227968)),
            dot(c, vec3(0.280085, 0.672501,  0.047413)),
            dot(c, vec3(-0.011820, 0.042940,  0.968881))));
}

vec3 protanopia(vec3 c) {
    return clampColor(vec3(
            dot(c, vec3(0.152286, 1.052583, -0.204868)),
            dot(c, vec3(0.114503, 0.786281,  0.099216)),
            dot(c, vec3(-0.003882,-0.048116,  1.051998))));
}

vec3 tritanopia(vec3 c) {
    return clampColor(vec3(
            dot(c, vec3(1.255528,-0.076749, -0.178779)),
            dot(c, vec3(-0.078411, 0.930809,  0.147602)),
            dot(c, vec3(0.004733, 0.691367,  0.303900))));
}

vec3 achromatopsia(vec3 c) {
    float luma = dot(c, vec3(0.2126, 0.7152, 0.0722));
    return vec3(luma);
}

vec3 contrast(vec3 c, float amount) {
    float boost = 1.0 + amount * 0.22;
    return clampColor((c - 0.5) * boost + 0.5);
}

vec3 applyFilter(vec3 c) {
    vec3 target;

    if (u_mode < 0.5) {
        target = deuteranopia(c);
    }
    else if (u_mode < 1.5) {
        target = protanopia(c);
    }
    else if (u_mode < 2.5) {
        target = tritanopia(c);
    }
    else {
        target = achromatopsia(c);
    }

    vec3 mixed = mix(c, target, clamp(u_strength, 0.0, 1.0));
    return contrast(mixed, u_strength);
}

void main() {
    vec4 original = v_fragmentColor;

    if (original.a <= 0.001 || u_strength <= 0.001) {
        gl_FragColor = original;
        return;
    }

    gl_FragColor = vec4(applyFilter(original.rgb), original.a);
}