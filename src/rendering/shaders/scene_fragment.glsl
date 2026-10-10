#version 120
// 基于 QtGlassFlow 修改，增加透明度、原生像素边缘和组件合成。采用 GPL v3，见项目根目录 LICENSE。

varying vec2 v_texcoord;

// === 背景与几何 ===
uniform sampler2D u_blurredTex;
uniform vec2 u_resolution;
uniform vec2 u_objCenter;     // 标准化设备坐标，范围为负一到一。
uniform vec2 u_objHalfSize;   // 标准化设备坐标下的半尺寸。
uniform vec2 u_objHalfSizePx; // 原生像素半尺寸。
uniform float u_cornerRadiusPx;
uniform float u_powerFactor;

// === 折射参数 ===
uniform float u_a;
uniform float u_b;
uniform float u_c;
uniform float u_d;
uniform float u_fPower;

// === 材质参数 ===
uniform float u_noise;        // 按画质限制在一个颜色量化单位内，最高档关闭。
uniform float u_edgeAaScale;  // 随画质变化的解析边缘覆盖率。

// === 平滑并集桥接 ===
uniform int u_numConnections;
uniform vec2 u_connCenterB[8];
uniform vec2 u_connHalfSizeB[8];
uniform float u_connPowerB[8];
uniform float u_connStrength[8];

uniform float u_opacity;

const float M_E = 2.718281828459045;

// 超椭圆 SDF —— 形状基础
float sdSuperellipse(vec2 p, float n, float r) {
    vec2 p_abs = abs(p);
    float numerator = pow(p_abs.x, n) + pow(p_abs.y, n) - pow(r, n);
    float den_x = pow(p_abs.x, 2.0 * n - 2.0);
    float den_y = pow(p_abs.y, 2.0 * n - 2.0);
    float denominator = n * sqrt(den_x + den_y) + 0.00001;
    return numerator / denominator;
}

// 使用像素精确的圆角矩形，保持宽卡片的圆角与 QPainter 裁剪一致。
float sdRoundedBox(vec2 p, vec2 halfSize, float radius) {
    float r = clamp(radius, 0.0, min(halfSize.x, halfSize.y));
    vec2 q = abs(p) - halfSize + vec2(r);
    return min(max(q.x, q.y), 0.0) + length(max(q, vec2(0.0))) - r;
}

// 折射强度曲线
float refractionF(float x) {
    return 1.0 - u_b * pow(u_c * M_E, -u_d * x - u_a);
}

// 交错梯度抖动避免低频正弦哈希产生可见的方格或摩尔纹。
float rand(vec2 co) {
    vec2 pixel = floor(co);
    return fract(52.9829189 * fract(dot(pixel, vec2(0.06711056, 0.00583715))));
}

// 平滑并集 —— 让相邻形状融合成连续液面
float smin(float a, float b, float k) {
    float h = clamp(0.5 + 0.5 * (b - a) / k, 0.0, 1.0);
    return mix(b, a, h) - k * h * (1.0 - h);
}

void main() {
    vec2 fragNDC = v_texcoord * 2.0 - 1.0;

    // 局部坐标 [-1, +1]
    vec2 localP = (fragNDC - u_objCenter) / u_objHalfSize;

    float d;
    if (u_cornerRadiusPx > 0.0) {
        float scalePx = max(min(u_objHalfSizePx.x, u_objHalfSizePx.y), 1.0);
        d = sdRoundedBox(localP * u_objHalfSizePx,
                         u_objHalfSizePx, u_cornerRadiusPx) / scalePx;
    } else {
        d = sdSuperellipse(localP, u_powerFactor, 1.0);
    }

    // 没有连接时跳过平滑并集循环，避免逐片元计算不必要的超椭圆与幂函数。
    float combinedD = d;
    if (u_numConnections > 0) {
        for (int i = 0; i < 8; i++) {
            if (i >= u_numConnections) break;
            vec2 otherLocalP = (fragNDC - u_connCenterB[i]) / u_connHalfSizeB[i];
            float otherD = sdSuperellipse(otherLocalP, u_connPowerB[i], 1.0);
            float k = 0.35 * u_connStrength[i] + 0.001;
            combinedD = smin(combinedD, otherD, k);
        }
    }

    // 数学边缘外保留一像素过渡，避免小数 DPI 下二值裁剪导致锯齿。
    float edgeWidth = max(fwidth(combinedD) * u_edgeAaScale, 0.0003);
    if (combinedD > edgeWidth)
        discard;

    // 仅绘制归属于当前对象的像素。
    if (u_numConnections > 0) {
        for (int i = 0; i < 8; i++) {
            if (i >= u_numConnections) break;
            vec2 otherLocalP = (fragNDC - u_connCenterB[i]) / u_connHalfSizeB[i];
            float otherD = sdSuperellipse(otherLocalP, u_connPowerB[i], 1.0);
            if (otherD < d) {
                discard;
            }
        }
    }

    float dist = -combinedD;

    // 仅模糊模式的折射强度为零，直接复用局部坐标，跳过幂函数。
    vec2 sampleP = u_fPower == 0.0
                 ? localP
                 : localP * pow(refractionF(dist), u_fPower);
    vec2 targetNDC = sampleP * u_objHalfSize + u_objCenter;
    vec2 texCoord = clamp(targetNDC * 0.5 + 0.5, 0.0, 1.0);

    // === 苹果液态玻璃材质 ===

    // 1. 干净的单次模糊采样 + 极微噪声（消色带）
    vec4 color = texture2D(u_blurredTex, texCoord);
    if (u_noise > 0.0) {
        vec4 noiseVal = vec4(vec3(rand(gl_FragCoord.xy) - 0.5), 0.0);
        color += noiseVal * u_noise;
    }

    // 2. 凸面玻璃渐变（穹顶光照：上略亮，下略暗）
    float convex = 0.5 + 0.5 * localP.y;
    float domeLight = mix(0.93, 1.07, convex);
    color.rgb *= domeLight;

    // 3. 极细白色边框线（约 0.5–1 px 边缘反光）
    float fw = max(edgeWidth, 0.0003);
    // 边缘覆盖率随画质调整，但光学描边维持约一个原生像素。
    float borderFw = clamp(fwidth(combinedD), 0.0003, 0.003);
    float borderLine = smoothstep(borderFw * 2.5, borderFw * 0.5, dist);
    // 方向性高光由共享覆盖层提供，此处仅保留柔和提亮，避免形成重复亮边。
    color.rgb += vec3(1.0) * borderLine * 0.12;

    // 4. 干净锐利的 alpha 抗锯齿
    float alpha = smoothstep(-fw, fw, dist);
    color.a = alpha * u_opacity;

    gl_FragColor = color;
}
