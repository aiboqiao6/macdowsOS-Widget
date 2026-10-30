#version 120
// 基于 QtGlassFlow 修改，增加透明度、原生像素边缘和组件合成。采用 GPL v3，见项目根目录 LICENSE。

varying vec2 v_texcoord;

// === 背景与几何 ===
uniform sampler2D u_blurredTex;
uniform float u_curvature;
uniform vec2 u_objCenter;     // 标准化设备坐标，范围为负一到一。
uniform vec2 u_objHalfSize;   // 标准化设备坐标下的半尺寸。
uniform vec2 u_objHalfSizePx; // 原生像素半尺寸。
uniform float u_cornerRadiusPx;
uniform float u_powerFactor;

// === 折射参数 ===
uniform float u_fPower;

// === 材质参数 ===
uniform float u_noise;        // 按画质限制在一个颜色量化单位内，最高档关闭。
uniform float u_edgeAaScale;  // 随画质变化的解析边缘覆盖率。
uniform bool u_darkMode;
uniform float u_devicePixelRatio;

// === 平滑并集桥接 ===
uniform int u_numConnections;
uniform vec2 u_connCenterB[8];
uniform vec2 u_connHalfSizeB[8];
uniform float u_connPowerB[8];
uniform float u_connStrength[8];

uniform float u_opacity;

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
    // 在丢弃片元前计算导数，避免轮廓边缘的非一致分支导致法线不稳定。
    vec2 surfaceGradient = vec2(dFdx(combinedD), dFdy(combinedD));
    float distanceWidth = abs(surfaceGradient.x) + abs(surfaceGradient.y);
    float edgeWidth = max(distanceWidth * u_edgeAaScale, 0.0003);
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

    // 距离场梯度换算为原生像素，宽卡片和小数 DPI 使用同一曲面厚度。
    float gradientLength = max(length(surfaceGradient), 0.000001);
    vec2 normal = surfaceGradient / gradientLength;
    float depthScale = u_cornerRadiusPx > 0.0 && u_numConnections == 0
        ? min(u_objHalfSizePx.x, u_objHalfSizePx.y) : 1.0 / gradientLength;
    float depthPx = max(dist * depthScale, 0.0);
    // 窄边缘径向折射形成凸面透镜；厚度由圆角决定，不随面板面积无界增长。
    float lensDecayPx = max(u_cornerRadiusPx * 0.15,
                            min(u_objHalfSizePx.x, u_objHalfSizePx.y) * 0.035);
    float rimCurve = exp(-depthPx / max(lensDecayPx, 1.0));
    float amplitude = mix(0.018, 0.44, u_curvature) * u_fPower;
    vec2 sampleP = localP * (1.0 - amplitude * rimCurve);
    vec2 targetNDC = sampleP * u_objHalfSize + u_objCenter;
    vec2 texCoord = clamp(targetNDC * 0.5 + 0.5, 0.0, 1.0);

    // 统一的柔化透光避免清晰原图与模糊图混合后出现双重边缘。
    vec4 color = texture2D(u_blurredTex, texCoord);
    if (u_fPower != 0.0) {
        // 固定的环境反射压缩背景明暗，保留色彩差异，形成参考玻璃的灰阶与厚度。
        float luminance = dot(color.rgb, vec3(0.2126, 0.7152, 0.0722));
        if (u_darkMode) {
            // 深色透光压低灰白反射，仍保留壁纸色彩与边缘折射。
            color.rgb = clamp(color.rgb * 0.630 - vec3(luminance * 0.347)
                              + vec3(0.015, 0.017, 0.021), 0.0, 1.0);
        } else {
            color.rgb = clamp(color.rgb * 0.94 - vec3(luminance * 0.35)
                              + vec3(0.129, 0.147, 0.159), 0.0, 1.0);
        }
    }
    if (u_noise > 0.0) {
        vec4 noiseVal = vec4(vec3(rand(gl_FragCoord.xy) - 0.5), 0.0);
        color += noiseVal * u_noise;
    }

    // 方向性反光只占原生像素轮廓，不在整个玻璃表面叠加明暗遮罩。
    float fw = max(edgeWidth, 0.0003);
    float rimDepth = depthPx / max(u_devicePixelRatio, 1.0);
    float outerRim = exp(-pow((rimDepth - 0.70) / 0.80, 2.0));
    float innerRim = exp(-pow((rimDepth - 2.0) / 0.65, 2.0));
    float directionalLight = dot(normal, normalize(vec2(-0.55, 0.84)));
    float keyLight = pow(max(directionalLight, 0.0), 1.6);
    float returnLight = pow(max(-directionalLight, 0.0), 2.0);
    // 外沿反射天空色，内沿细暗边表现厚度；反光沿圆角连续转向。
    float reflection = outerRim * (0.22 + 0.65 * keyLight + 0.25 * returnLight);
    vec3 rimColor = u_darkMode ? vec3(0.56, 0.76, 0.80) : vec3(0.82, 0.94, 0.98);
    color.rgb *= 1.0 - innerRim * (u_darkMode ? 0.28 : 0.19);
    color.rgb = mix(color.rgb, rimColor, reflection * (u_darkMode ? 0.38 : 0.44));

    // 解析覆盖率保持圆角透明边缘平滑。
    float alpha = smoothstep(-fw, fw, dist);
    color.a = alpha * u_opacity;

    gl_FragColor = color;
}
