// 基于 QtGlassFlow 修改，增加背景输入、按需刷新与桌面集成。采用 GPL v3，见项目根目录 LICENSE。

#include "qtglassflowscene.h"
#include "renderdiagnostics.h"
#include "rendering/glpixeltransfer.h"
#include "app/appsettings.h"

#include <QFile>
#include <QImage>
#include <QMouseEvent>
#include <QPainter>
#include <QShowEvent>
#include <QHideEvent>
#include <QTimer>
#include <QVector2D>
#include <QtMath>
#include <QDebug>
#include <QOpenGLContext>
#include <QOpenGLExtraFunctions>
#include <QSurfaceFormat>
#include <cmath>

namespace {
static const char *kBlitVertSrc =
    "#version 120\n"
    "attribute vec2 a_position;\n"
    "attribute vec2 a_texcoord;\n"
    "varying vec2 v_texcoord;\n"
    "void main() { gl_Position = vec4(a_position, 0.0, 1.0); v_texcoord = a_texcoord; }\n";

static const char *kBlitFragSrc =
    "#version 120\n"
    "varying vec2 v_texcoord;\n"
    "uniform sampler2D u_texture;\n"
    "uniform bool u_flipY;\n"
    "void main() { vec2 uv = v_texcoord; if (u_flipY) uv.y = 1.0 - uv.y;"
    "gl_FragColor = texture2D(u_texture, uv); }\n";

static const float kQuadVerts[] = {
    -1.0f, -1.0f, 0.0f, 0.0f,
     1.0f, -1.0f, 1.0f, 0.0f,
     1.0f,  1.0f, 1.0f, 1.0f,
    -1.0f, -1.0f, 0.0f, 0.0f,
     1.0f,  1.0f, 1.0f, 1.0f,
    -1.0f,  1.0f, 0.0f, 1.0f
};

}

QtGlassFlowScene::QtGlassFlowScene(QWidget *parent)
    : QOpenGLWidget(parent),
      m_sceneFbo(nullptr), m_blurPingFbo(nullptr),
      m_blitShader(nullptr), m_blurShader(nullptr), m_glassShader(nullptr),
      m_quadVbo(QOpenGLBuffer::VertexBuffer), m_quadInitialized(false),
      m_bgTexture(0), m_bgWidth(0), m_bgHeight(0), m_bgDirty(false),
      m_bgImageAlreadyFlipped(false),
      m_glassRenderingEnabled(true), m_glassOpacity(1.0f),
      m_blurCacheDirty(true),
      // 渲染器内部默认使用原生分辨率，应用可根据保存的画质设置覆盖。
      m_renderScale(1.0f), m_connectionsDirty(true),
      m_fPower(LiquidGlassStyle::kRefractionPower),
      m_blurRadius(LiquidGlassStyle::kBlurRadius),
      m_blurIterations(LiquidGlassStyle::kBlurIterations),
      m_noiseAmount(LiquidGlassStyle::kNoiseAmount), m_attractionDist(0.0f),
      m_globalPower(LiquidGlassStyle::kShapePower), m_timer(nullptr), m_refreshInterval(16),
      m_externalInteraction(false),
      m_animationEnabled(false),
      m_dragIndex(-1)
{
    AppSettings settings;
    m_darkModeEnabled = settings.value(QStringLiteral("appearance/darkMode"), false).toBool();
    MaterialParameters material;
    material.opacity = settings.value(QStringLiteral("material/opacity"), material.opacity).toFloat();
    material.refraction = settings.value(QStringLiteral("material/refraction"), material.refraction).toFloat();
    material.blurRadius = settings.value(QStringLiteral("material/blurRadius"), material.blurRadius).toFloat();
    material.blurIterations = settings.value(QStringLiteral("material/blurIterations"), material.blurIterations).toInt();
    material.noise = settings.value(QStringLiteral("material/noise"), material.noise).toFloat();
    setMaterialParameters(material);
    setMouseTracking(true);
    setAttribute(Qt::WA_OpaquePaintEvent, true);

    QSurfaceFormat fmt = format();
    fmt.setRenderableType(QSurfaceFormat::OpenGL);
    fmt.setProfile(QSurfaceFormat::CompatibilityProfile);
    fmt.setVersion(2, 1);
    fmt.setAlphaBufferSize(8);
    fmt.setSwapBehavior(QSurfaceFormat::DoubleBuffer);
    // QOpenGLWidget 的结果由 Qt 与 DWM 合成，此处额外等待垂直同步会降低实时背景帧率。
    fmt.setSwapInterval(0);
    setFormat(fmt);
    // 每次完整重绘透明帧缓冲，避免 Qt 重组后暴露已清空纹理；下层读取使用独立快照缓存。
    setUpdateBehavior(QOpenGLWidget::NoPartialUpdate);
    connect(this, &QOpenGLWidget::frameSwapped, this, [this]() {
        if (isVisible() && m_continuousRendering && !m_renderingSuspended
            && m_refreshInterval < 64)
            update();
    });
}

QtGlassFlowScene::~QtGlassFlowScene()
{
    cleanupGL();
}

void QtGlassFlowScene::cleanupGL()
{
    if (!context())
        return;
    RenderDiagnostics::write("gl.context.destroy", QString("surface=0x%1 context=0x%2 frames=%3")
        .arg(quintptr(this), 0, 16).arg(quintptr(context()), 0, 16).arg(m_frameRevision));
    disconnect(context(), &QOpenGLContext::aboutToBeDestroyed,
               this, &QtGlassFlowScene::cleanupGL);
    makeCurrent();
    m_textRenderer.clear();
    destroyFBOs();
    delete m_snapshotFbo;
    m_snapshotFbo = nullptr;
    m_surfaceSnapshot = {};
    m_frameRevision = 0;
    m_snapshotRevision = 0;
    if (m_bgTexture) {
        glDeleteTextures(1, &m_bgTexture);
        m_bgTexture = 0;
    }
    delete m_blitShader;
    m_blurPrograms.clear();
    m_failedBlurPrograms.clear();
    m_uploadedKernelProgram = nullptr;
    delete m_blurShader;
    delete m_glassShader;
    m_blitShader = nullptr;
    m_blurShader = nullptr;
    m_glassShader = nullptr;
    m_bgWidth = m_bgHeight = 0;
    m_bgDirty = true;
    m_gpuReady = false;
    m_quadInitialized = false;
    if (m_quadVbo.isCreated())
        m_quadVbo.destroy();
    doneCurrent();
}

int QtGlassFlowScene::addGlassObject(const QPointF &pos, const QSizeF &size,
                                     float power)
{
    GlassObject object;
    object.position = pos;
    object.size = size;
    object.powerFactor = power;
    m_objects.append(object);
    m_connectionsDirty = true;
    m_blurCacheDirty = true;
    update();
    return m_objects.size() - 1;
}

void QtGlassFlowScene::setGlassObjectGeometry(int index, const QPointF &pos,
                                              const QSizeF &size)
{
    if (index < 0 || index >= m_objects.size())
        return;
    if (m_objects[index].size != size)
        m_blurCacheDirty = true;
    m_objects[index].position = pos;
    m_objects[index].size = size;
    m_connectionsDirty = true;
    update();
}

void QtGlassFlowScene::setGlassObjectCornerRadius(int index, float radius)
{
    if (index < 0 || index >= m_objects.size())
        return;
    const float boundedRadius = qMax(0.0f, radius);
    if (m_objects[index].cornerRadius == boundedRadius)
        return;
    m_objects[index].cornerRadius = boundedRadius;
    m_blurCacheDirty = true;
    update();
}

void QtGlassFlowScene::setGlassObjectVisible(int index, bool visible)
{
    if (index < 0 || index >= m_objects.size())
        return;
    if (m_objects[index].visible == visible)
        return;
    m_objects[index].visible = visible;
    m_blurCacheDirty = true;
    if (!visible && m_dragIndex == index) {
        m_dragIndex = -1;
        syncTimerCadence();
    }
    m_connectionsDirty = true;
    update();
}

void QtGlassFlowScene::setBackgroundImage(const QImage &image)
{
    setBackgroundImageInternal(image, false);
}

void QtGlassFlowScene::setBackgroundImageFlipped(const QImage &image)
{
    setBackgroundImageInternal(image, true);
}

void QtGlassFlowScene::setBackgroundImageInternal(const QImage &image,
                                                   bool alreadyFlipped)
{
    if (image.isNull())
        return;
    if (m_bgImage.cacheKey() == image.cacheKey()
        && m_bgImageAlreadyFlipped == alreadyFlipped)
        return;
    m_bgImage = image;
    m_bgImageAlreadyFlipped = alreadyFlipped;
    m_bgDirty = true;
    m_blurCacheDirty = true;
    update();
}

void QtGlassFlowScene::setGlassRenderingEnabled(bool enabled)
{
    if (m_glassRenderingEnabled == enabled)
        return;
    m_glassRenderingEnabled = enabled;
    update();
}

void QtGlassFlowScene::setGlassOpacity(float opacity)
{
    const float next = qBound(0.05f, opacity, 1.0f);
    if (qFuzzyCompare(m_glassOpacity, next))
        return;
    m_glassOpacity = next;
    update();
}

void QtGlassFlowScene::setDarkModeEnabled(bool enabled)
{
    if (m_darkModeEnabled == enabled)
        return;
    m_darkModeEnabled = enabled;
    // 只更新材质参数，背景纹理与模糊结果继续复用。
    update();
}

void QtGlassFlowScene::setRefreshInterval(int intervalMs)
{
    // 根据高刷新率显示器缩短间隔，最终呈现由 Qt 和 DWM 合并调度。
    m_refreshInterval = qBound(2, intervalMs, 1000);
    syncTimerCadence();
}

void QtGlassFlowScene::setInteractionActive(bool active)
{
    if (m_externalInteraction == active)
        return;
    m_externalInteraction = active;
    syncTimerCadence();
}

void QtGlassFlowScene::setAnimationEnabled(bool enabled)
{
    if (m_animationEnabled == enabled)
        return;
    m_animationEnabled = enabled;
    syncTimerCadence();
}

void QtGlassFlowScene::setRenderingSuspended(bool suspended)
{
    if (m_renderingSuspended == suspended)
        return;
    m_renderingSuspended = suspended;
    RenderDiagnostics::write(QString("render.suspend/0x%1").arg(quintptr(this), 0, 16),
        QString("suspended=%1 visible=%2 frame=%3").arg(suspended).arg(isVisible()).arg(m_frameRevision), 1000);
    syncTimerCadence();
    if (!suspended)
        update();
}

void QtGlassFlowScene::setContinuousRenderingEnabled(bool enabled)
{
    if (m_continuousRendering == enabled)
        return;
    m_continuousRendering = enabled;
    syncTimerCadence();
}

void QtGlassFlowScene::syncTimerCadence()
{
    if (!m_timer)
        return;
    const bool interacting = m_externalInteraction || m_dragIndex >= 0
                             || m_continuousRendering;
    // 静态组件由输入或数据变化触发重绘；动画和交互期间才运行场景定时器。
    if (!isVisible() || m_renderingSuspended || (!interacting && !m_animationEnabled)) {
        m_timer->stop();
        return;
    }

    const int interval = interacting ? m_refreshInterval
                                     : qMax(m_refreshInterval, 64);
    // 空闲动画使用粗略定时器节能，指针交互仍使用精确定时器。
    m_timer->setTimerType(interacting ? Qt::PreciseTimer : Qt::CoarseTimer);
    if (m_timer->interval() != interval)
        m_timer->setInterval(interval);
    if (!m_timer->isActive())
        m_timer->start();
}

void QtGlassFlowScene::setRenderScale(float scale)
{
    const float next = qBound(0.5f, scale, 1.5f);
    if (qFuzzyCompare(m_renderScale, next))
        return;
    m_renderScale = next;
    m_blurKernelSigma = -1.0f;
    m_blurKernelSamples.clear();
    m_blurCacheDirty = true;
    update();
}

QtGlassFlowScene::MaterialParameters QtGlassFlowScene::materialParameters() const
{
    return {m_glassOpacity, m_fPower, m_blurRadius, m_blurIterations, m_noiseAmount};
}

void QtGlassFlowScene::setMaterialParameters(const MaterialParameters& parameters)
{
    const MaterialParameters defaults;
    const auto bounded = [](float value, float minimum, float maximum, float fallback) {
        return std::isfinite(value) ? qBound(minimum, value, maximum) : fallback;
    };
    setGlassOpacity(bounded(parameters.opacity, 0.05f, 1.0f, defaults.opacity));
    setRefractionPower(bounded(parameters.refraction, 0.0f, 3.0f, defaults.refraction));
    setBlurRadius(bounded(parameters.blurRadius, 0.0f, 12.0f, defaults.blurRadius));
    setBlurIterations(qBound(1, parameters.blurIterations, 6));
    setNoiseAmount(bounded(parameters.noise, 0.0f, 0.1f, defaults.noise));
}

void QtGlassFlowScene::setPowerFactor(float value) { m_globalPower = value; update(); }
void QtGlassFlowScene::setRefractionPower(float value)
{
    if ((m_fPower == 0.0f) != (value == 0.0f))
        m_blurCacheDirty = true;
    m_fPower = value;
    update();
}

void QtGlassFlowScene::setBlurRadius(float value)
{
    const float next = qMax(0.0f, value);
    if (qFuzzyCompare(m_blurRadius, next))
        return;
    m_blurRadius = next;
    m_blurKernelSigma = -1.0f;
    m_blurKernelSamples.clear();
    m_blurCacheDirty = true;
    update();
}

void QtGlassFlowScene::setBlurIterations(int value)
{
    const int next = qMax(1, value);
    if (m_blurIterations == next)
        return;
    m_blurIterations = next;
    m_blurCacheDirty = true;
    update();
}

void QtGlassFlowScene::setNoiseAmount(float value) { m_noiseAmount = value; update(); }
void QtGlassFlowScene::setAttractionDistance(float value)
{
    const float next = qMax(0.0f, value);
    if (qFuzzyCompare(m_attractionDist, next))
        return;
    m_attractionDist = next;
    m_connectionsDirty = true;
    update();
}

bool QtGlassFlowScene::compileProgram(QOpenGLShaderProgram *program,
                                      const QString &vertPath,
                                      const QString &fragPath)
{
    if (!program->addShaderFromSourceFile(QOpenGLShader::Vertex, vertPath)) {
        qWarning() << "vertex shader compile failed:" << vertPath << program->log();
        return false;
    }
    if (!program->addShaderFromSourceFile(QOpenGLShader::Fragment, fragPath)) {
        qWarning() << "fragment shader compile failed:" << fragPath << program->log();
        return false;
    }
    program->bindAttributeLocation("a_position", 0);
    program->bindAttributeLocation("a_texcoord", 1);
    if (!program->link()) {
        qWarning() << "shader link failed:" << program->log();
        return false;
    }
    return true;
}

void QtGlassFlowScene::initQuad()
{
    if (m_quadInitialized)
        return;
    m_quadVbo.create();
    m_quadVbo.bind();
    m_quadVbo.setUsagePattern(QOpenGLBuffer::StaticDraw);
    m_quadVbo.allocate(kQuadVerts, sizeof(kQuadVerts));
    m_quadVbo.release();
    m_quadInitialized = true;
}

void QtGlassFlowScene::bindFullscreenQuad(QOpenGLShaderProgram *program)
{
    m_quadVbo.bind();
    constexpr int stride = 4 * sizeof(float);
    program->enableAttributeArray(0);
    program->enableAttributeArray(1);
    program->setAttributeBuffer(0, GL_FLOAT, 0, 2, stride);
    program->setAttributeBuffer(1, GL_FLOAT, 2 * sizeof(float), 2, stride);
}

void QtGlassFlowScene::releaseFullscreenQuad(QOpenGLShaderProgram* program)
{
    program->disableAttributeArray(0);
    program->disableAttributeArray(1);
    m_quadVbo.release();
}

void QtGlassFlowScene::drawFullscreenQuad(QOpenGLShaderProgram* program)
{
    bindFullscreenQuad(program);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    releaseFullscreenQuad(program);
}

QOpenGLShaderProgram* QtGlassFlowScene::blurProgram(int sampleCount)
{
    // 小核沿用通用程序，固定采样变体用于有实测收益的较大高斯核。
    if (sampleCount < 16)
        return m_blurShader;
    if (auto* cached = m_blurPrograms.object(sampleCount))
        return cached;
    if (m_failedBlurPrograms.contains(sampleCount))
        return m_blurShader;
    QFile fragmentFile(QStringLiteral(":/qtglassflow/shaders/blur_fragment.glsl"));
    if (!fragmentFile.open(QIODevice::ReadOnly))
        return m_blurShader;
    QByteArray fragment = fragmentFile.readAll();
    fragment.replace("#version 120", "#version 120\n#define BLUR_SAMPLE_COUNT " + QByteArray::number(sampleCount));
    auto* program = new QOpenGLShaderProgram;
    const bool compiled = program->addShaderFromSourceFile(QOpenGLShader::Vertex,
        QStringLiteral(":/qtglassflow/shaders/blur_vertex.glsl"))
        && program->addShaderFromSourceCode(QOpenGLShader::Fragment, fragment);
    program->bindAttributeLocation("a_position", 0);
    program->bindAttributeLocation("a_texcoord", 1);
    if (!compiled || !program->link()) {
        // 旧驱动不接受固定循环变体时继续使用原始程序，不改变画质参数。
        RenderDiagnostics::write("gl.blur-specialization.failed", program->log(), 10000);
        m_failedBlurPrograms.insert(sampleCount);
        delete program;
        return m_blurShader;
    }
    m_blurPrograms.insert(sampleCount, program);
    return program;
}

void QtGlassFlowScene::initializeGL()
{
    initializeOpenGLFunctions();
    if (RenderDiagnostics::enabled()) {
        const auto string = [this](GLenum name) {
            const auto* bytes = glGetString(name);
            return bytes ? QString::fromUtf8(reinterpret_cast<const char*>(bytes)) : QStringLiteral("unavailable");
        };
        const QSurfaceFormat actual = context()->format();
        RenderDiagnostics::write("gl.context.create", QString("surface=0x%1 hwnd=0x%2 context=0x%3 vendor=%4 renderer=%5 GL=%6 GLSL=%7 format=%8.%9 alpha=%10 samples=%11 DPR=%12 renderScale=%13")
            .arg(quintptr(this), 0, 16).arg(quintptr(effectiveWinId()), 0, 16).arg(quintptr(context()), 0, 16)
            .arg(string(GL_VENDOR), string(GL_RENDERER), string(GL_VERSION), string(GL_SHADING_LANGUAGE_VERSION))
            .arg(actual.majorVersion()).arg(actual.minorVersion()).arg(actual.alphaBufferSize())
            .arg(actual.samples()).arg(devicePixelRatioF()).arg(m_renderScale));
        m_diagnosticsClock.start();
        m_diagnosticsFrames = 0;
    }
    connect(context(), &QOpenGLContext::aboutToBeDestroyed,
            this, &QtGlassFlowScene::cleanupGL, Qt::DirectConnection);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);

    m_blitShader = new QOpenGLShaderProgram(this);
    m_blitShader->addShaderFromSourceCode(QOpenGLShader::Vertex, kBlitVertSrc);
    m_blitShader->addShaderFromSourceCode(QOpenGLShader::Fragment, kBlitFragSrc);
    m_blitShader->bindAttributeLocation("a_position", 0);
    m_blitShader->bindAttributeLocation("a_texcoord", 1);
    if (!m_blitShader->link())
        qWarning() << "blit shader link failed:" << m_blitShader->log();

    m_blurShader = new QOpenGLShaderProgram(this);
    compileProgram(m_blurShader, QStringLiteral(":/qtglassflow/shaders/blur_vertex.glsl"),
                   QStringLiteral(":/qtglassflow/shaders/blur_fragment.glsl"));
    m_glassShader = new QOpenGLShaderProgram(this);
    compileProgram(m_glassShader, QStringLiteral(":/qtglassflow/shaders/scene_vertex.glsl"),
                   QStringLiteral(":/qtglassflow/shaders/scene_fragment.glsl"));
    m_gpuReady = m_blitShader->isLinked() && m_blurShader->isLinked() && m_glassShader->isLinked();
    RenderDiagnostics::write("gl.shaders", QString("surface=0x%1 ready=%2 blit=%3 blur=%4 glass=%5")
        .arg(quintptr(this), 0, 16).arg(m_gpuReady).arg(m_blitShader->isLinked())
        .arg(m_blurShader->isLinked()).arg(m_glassShader->isLinked()));
    initQuad();
    if (m_gpuReady && !m_bgImage.isNull())
        loadBackgroundTexture();

    if (!m_timer) {
        m_timer = new QTimer(this);
        m_timer->setTimerType(Qt::PreciseTimer);
        connect(m_timer, &QTimer::timeout, this, [this]() {
            if (isVisible())
                update();
        });
    }
    // 由 syncTimerCadence 统一管理定时器，避免静态组件意外以零间隔循环重绘。
    syncTimerCadence();
    reportGlErrors("initialize");
}

void QtGlassFlowScene::reportGlErrors(const char* stage)
{
    if (!RenderDiagnostics::enabled())
        return;
    for (int index = 0; index < 16; ++index) {
        const GLenum error = glGetError();
        if (error == GL_NO_ERROR)
            break;
        RenderDiagnostics::write(QString("gl.error/0x%1/%2").arg(quintptr(this), 0, 16).arg(stage),
            QString("code=0x%1 frame=%2 defaultFbo=%3 size=%4x%5 DPR=%6 scale=%7")
                .arg(error, 0, 16).arg(m_frameRevision).arg(defaultFramebufferObject())
                .arg(width()).arg(height()).arg(devicePixelRatioF()).arg(m_renderScale), 1000);
    }
}

void QtGlassFlowScene::resizeGL(int w, int h)
{
    Q_UNUSED(w);
    Q_UNUSED(h);
    const qreal dpr = devicePixelRatioF();
    const int nativePixelWidth = qMax(1, qRound(width() * dpr));
    const int nativePixelHeight = qMax(1, qRound(height() * dpr));
    const int pixelWidth = qMax(1, qRound(nativePixelWidth * m_renderScale));
    const int pixelHeight = qMax(1, qRound(nativePixelHeight * m_renderScale));
    if (m_gpuReady)
        createFBOs(pixelWidth, pixelHeight);
    m_blurCacheDirty = true;
    glViewport(0, 0, nativePixelWidth, nativePixelHeight);
}

void QtGlassFlowScene::showEvent(QShowEvent *event)
{
    QOpenGLWidget::showEvent(event);
    // 窗口重新显示时恢复所需刷新节奏，静态组件继续按需重绘。
    syncTimerCadence();
}

void QtGlassFlowScene::hideEvent(QHideEvent *event)
{
    if (m_timer)
        m_timer->stop();
    QOpenGLWidget::hideEvent(event);
}

void QtGlassFlowScene::createFBOs(int w, int h)
{
    destroyFBOs();
    QOpenGLFramebufferObjectFormat format;
    format.setAttachment(QOpenGLFramebufferObject::NoAttachment);
    format.setInternalTextureFormat(GL_RGBA8);
    m_sceneFbo = new QOpenGLFramebufferObject(w, h, format);
    m_blurPingFbo = new QOpenGLFramebufferObject(w, h, format);
    RenderDiagnostics::write("gl.fbo.create", QString("surface=0x%1 size=%2x%3 scene=%4 ping=%5 valid=%6/%7")
        .arg(quintptr(this), 0, 16).arg(w).arg(h).arg(m_sceneFbo->handle())
        .arg(m_blurPingFbo->handle())
        .arg(m_sceneFbo->isValid()).arg(m_blurPingFbo->isValid()));
    const auto setLinear = [this](QOpenGLFramebufferObject *fbo) {
        glBindTexture(GL_TEXTURE_2D, fbo->texture());
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glBindTexture(GL_TEXTURE_2D, 0);
    };
    setLinear(m_sceneFbo);
    setLinear(m_blurPingFbo);
    m_blurCacheDirty = true;
    reportGlErrors("fbo-create");
}

void QtGlassFlowScene::destroyFBOs()
{
    delete m_sceneFbo; m_sceneFbo = nullptr;
    delete m_blurPingFbo; m_blurPingFbo = nullptr;
}

void QtGlassFlowScene::loadBackgroundTexture()
{
    QImage image = m_bgImage;
    if (image.isNull())
        return;
    // 上传原始共享像素，在背景绘制阶段处理方向；常用 BGRA 格式无需每帧转换或翻转。
    GLenum uploadFormat = GL_RGBA;
    GLenum uploadType = GL_UNSIGNED_BYTE;
    if (image.format() == QImage::Format_RGB32
        || image.format() == QImage::Format_ARGB32
        || image.format() == QImage::Format_ARGB32_Premultiplied) {
        uploadFormat = GL_BGRA;
        // Windows 小端平台的 QImage 32 位格式采用 BGRA 字节布局，可直接上传。
        uploadType = GL_UNSIGNED_BYTE;
    } else if (image.format() != QImage::Format_RGBA8888) {
        image = image.convertToFormat(QImage::Format_RGBA8888);
    }
    // 外部图像的行跨度可能不规则，仅在 GL 像素行长度无法表达时重新打包。
    if (image.bytesPerLine() % 4)
        image = image.copy();
    PixelTransferScope transfer(this, true, int(image.bytesPerLine() / 4));
    GLint activeTexture = 0;
    glGetIntegerv(GL_ACTIVE_TEXTURE, &activeTexture);
    glActiveTexture(GL_TEXTURE0);
    GLint textureBinding = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &textureBinding);
    if (!m_bgTexture)
        glGenTextures(1, &m_bgTexture);
    glBindTexture(GL_TEXTURE_2D, m_bgTexture);
    const bool sameSize = m_bgWidth == image.width() && m_bgHeight == image.height();
    if (sameSize) {
        // 实时背景更新复用纹理存储，避免每帧分配造成驱动停顿。
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, image.width(), image.height(),
                        uploadFormat, uploadType, image.constBits());
    } else {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, image.width(), image.height(), 0,
                     uploadFormat, uploadType, image.constBits());
    }
    if (!sameSize) {
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }
    reportGlErrors("texture-upload");
    glBindTexture(GL_TEXTURE_2D, GLuint(textureBinding));
    glActiveTexture(GLenum(activeTexture));
    m_bgWidth = image.width();
    m_bgHeight = image.height();
    m_bgDirty = false;
    m_blurCacheDirty = true;
}

void QtGlassFlowScene::renderBackgroundToFbo()
{
    if (!m_sceneFbo)
        return;
    m_sceneFbo->bind();
    glViewport(0, 0, m_sceneFbo->width(), m_sceneFbo->height());
    if (m_bgTexture) {
        glDisable(GL_BLEND);
        m_blitShader->bind();
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, m_bgTexture);
        m_blitShader->setUniformValue("u_texture", 0);
        m_blitShader->setUniformValue("u_flipY", !m_bgImageAlreadyFlipped);
        drawFullscreenQuad(m_blitShader);
        glBindTexture(GL_TEXTURE_2D, 0);
        m_blitShader->release();
    } else {
        glClearColor(0, 0, 0, 1);
        glClear(GL_COLOR_BUFFER_BIT);
    }
    m_sceneFbo->release();
}

void QtGlassFlowScene::runBlurPass()
{
    if (!m_sceneFbo || !m_blurPingFbo)
        return;
    // 高画质逐像素覆盖高斯核，并利用线性过滤合并相邻采样；低画质降低密度与 FBO 分辨率。
    float scattering = 1.0f;
    if (m_fPower != 0.0f) {
        for (const GlassObject& object : std::as_const(m_objects)) {
            if (!object.visible)
                continue;
            const float curvature = LiquidGlassStyle::curvature(
                float(qMin(object.size.width(), object.size.height()) * .5), object.cornerRadius);
            scattering = qMax(scattering, 1.0f + (1.0f - curvature) * 2.4f);
        }
    }
    const float sigma = qBound(0.01f, m_blurRadius * scattering * m_renderScale * 1.95f, 42.0f);
    const int stride = m_renderScale < .7f ? 4 : m_renderScale < .9f ? 3
                     : m_renderScale < 1.2f ? 2 : 1;
    if (!qFuzzyCompare(m_blurKernelSigma, sigma) || m_blurKernelStride != stride) {
        m_blurKernelSigma = sigma;
        m_blurKernelStride = stride;
        m_blurKernelSamples.clear();
        const int radius = qMin(127, qCeil(sigma * 3.0f));
        float total = 1.0f;
        for (int first = 1; first <= radius; first += 2 * stride) {
            const int second = first + stride;
            const float a = std::exp(-float(first * first) / (2 * sigma * sigma));
            const float b = second <= radius
                ? std::exp(-float(second * second) / (2 * sigma * sigma)) : 0.0f;
            const float weight = a + b;
            if (weight < 1e-8f)
                break;
            m_blurKernelSamples.append(QVector2D((first * a + second * b) / weight, weight));
            total += 2 * weight;
        }
        for (auto& sample : m_blurKernelSamples)
            sample.setY(sample.y() / total);
        m_blurKernelCenterWeight = 1.0f / total;
    }
    auto* program = blurProgram(int(m_blurKernelSamples.size()));
    program->bind();
    program->setUniformValue("u_resolution", QVector2D(float(m_sceneFbo->width()), float(m_sceneFbo->height())));
    // 连续背景帧只更换纹理内容；核参数未变化时不重复向驱动提交权重表。
    if (m_uploadedKernelProgram != program || m_uploadedKernelSigma != m_blurKernelSigma
        || m_uploadedKernelStride != m_blurKernelStride) {
        program->setUniformValue("u_centerWeight", m_blurKernelCenterWeight);
        program->setUniformValue("u_sampleCount", int(m_blurKernelSamples.size()));
        if (!m_blurKernelSamples.isEmpty())
            program->setUniformValueArray("u_samples", m_blurKernelSamples.constData(),
                                         int(m_blurKernelSamples.size()));
        m_uploadedKernelProgram = program;
        m_uploadedKernelSigma = m_blurKernelSigma;
        m_uploadedKernelStride = m_blurKernelStride;
    }
    program->setUniformValue("u_texture", 0);
    glActiveTexture(GL_TEXTURE0);
    glDisable(GL_BLEND);
    bindFullscreenQuad(program);
    // 同尺寸的水平、垂直通道共用视口和顶点绑定，目标 FBO 直接切换。
    glViewport(0, 0, m_sceneFbo->width(), m_sceneFbo->height());
    GLuint source = m_sceneFbo->texture();
    for (int i = 0; i < m_blurIterations; ++i) {
        m_blurPingFbo->bind();
        glBindTexture(GL_TEXTURE_2D, source);
        program->setUniformValue("u_direction", QVector2D(1, 0));
        glDrawArrays(GL_TRIANGLES, 0, 6);

        // 水平通道已读完原背景，垂直通道写回场景纹理，两个同格式 FBO 即可完成全部迭代。
        m_sceneFbo->bind();
        glBindTexture(GL_TEXTURE_2D, m_blurPingFbo->texture());
        program->setUniformValue("u_direction", QVector2D(0, 1));
        glDrawArrays(GL_TRIANGLES, 0, 6);
        source = m_sceneFbo->texture();
    }
    glBindTexture(GL_TEXTURE_2D, 0);
    releaseFullscreenQuad(program);
    program->release();
}

QPointF QtGlassFlowScene::widgetToNdc(const QPointF &point) const
{
    const float w = float(width());
    const float h = float(height());
    if (w <= 0 || h <= 0)
        return QPointF();
    return QPointF(point.x() / w * 2.0 - 1.0, 1.0 - point.y() / h * 2.0);
}

QSizeF QtGlassFlowScene::widgetSizeToNdc(const QSizeF &size) const
{
    const float w = float(width());
    const float h = float(height());
    if (w <= 0 || h <= 0)
        return QSizeF();
    return QSizeF(size.width() / w * 2.0, size.height() / h * 2.0);
}

void QtGlassFlowScene::renderGlassObject(int index)
{
    if (index < 0 || index >= m_objects.size() || !m_objects[index].visible)
        return;
    const GlassObject &object = m_objects[index];

    // 将片元绘制限制在当前对象及相连对象的并集，额外扩展一像素以保留边缘覆盖率。
    QRectF drawBounds(object.position, object.size);
    for (const Connection &connection : std::as_const(m_connections)) {
        if (connection.objA != index && connection.objB != index)
            continue;
        const int other = connection.objA == index ? connection.objB : connection.objA;
        if (other >= 0 && other < m_objects.size() && m_objects[other].visible)
            drawBounds = drawBounds.united(QRectF(m_objects[other].position,
                                                   m_objects[other].size));
    }
    const qreal deviceScale = devicePixelRatioF();
    const int nativeWidth = qMax(1, qRound(width() * deviceScale));
    const int nativeHeight = qMax(1, qRound(height() * deviceScale));
    const int left = qMax(0, qFloor((drawBounds.left() - 1.5) * deviceScale));
    const int top = qMax(0, qFloor((drawBounds.top() - 1.5) * deviceScale));
    const int right = qMin(nativeWidth, qCeil((drawBounds.right() + 1.5) * deviceScale));
    const int bottom = qMin(nativeHeight, qCeil((drawBounds.bottom() + 1.5) * deviceScale));
    if (right <= left || bottom <= top)
        return;
    glEnable(GL_SCISSOR_TEST);
    glScissor(left, nativeHeight - bottom, right - left, bottom - top);

    const QPointF centerWidget(object.position.x() + object.size.width() * .5,
                               object.position.y() + object.size.height() * .5);
    const QPointF centerNdc = widgetToNdc(centerWidget);
    const QSizeF fullNdc = widgetSizeToNdc(object.size);
    const QSizeF halfNdc(fullNdc.width() * .5, fullNdc.height() * .5);
    const float power = object.powerFactor > 0 ? object.powerFactor : m_globalPower;
    m_glassShader->bind();
    m_glassShader->setUniformValue("u_blurredTex", 0);
    m_glassShader->setUniformValue("u_objCenter", QVector2D(centerNdc.x(), centerNdc.y()));
    m_glassShader->setUniformValue("u_objHalfSize", QVector2D(halfNdc.width(), halfNdc.height()));
    const float dpr = float(devicePixelRatioF());
    m_glassShader->setUniformValue("u_objHalfSizePx", QVector2D(float(object.size.width()) * dpr * .5f,
                                                                   float(object.size.height()) * dpr * .5f));
    m_glassShader->setUniformValue("u_cornerRadiusPx", float(object.cornerRadius * dpr));
    m_glassShader->setUniformValue("u_curvature", LiquidGlassStyle::curvature(
        float(qMin(object.size.width(), object.size.height()) * .5), object.cornerRadius));
    m_glassShader->setUniformValue("u_powerFactor", power);
    m_glassShader->setUniformValue("u_fPower", m_fPower);
    // 降低高画质抖动强度，最高画质关闭抖动，避免确定性像素图案形成细网格。
    const float ditherWeight = qBound(0.0f, (1.5f - m_renderScale) / 0.9f, 1.0f);
    const float dither = qMin(m_noiseAmount, 1.0f / 255.0f) * ditherWeight;
    m_glassShader->setUniformValue("u_noise", dither);
    m_glassShader->setUniformValue("u_edgeAaScale",
                                   qBound(0.85f, 0.65f + m_renderScale * 0.45f, 1.30f));
    m_glassShader->setUniformValue("u_opacity", m_glassOpacity);
    m_glassShader->setUniformValue("u_darkMode", m_darkModeEnabled);

    QVector<QVector2D> centers, halves;
    QVector<float> powers, strengths;
    for (const Connection &connection : std::as_const(m_connections)) {
        if (connection.objA != index && connection.objB != index)
            continue;
        const int other = connection.objA == index ? connection.objB : connection.objA;
        if (other < 0 || other >= m_objects.size() || !m_objects[other].visible)
            continue;
        const GlassObject &otherObject = m_objects[other];
        const QPointF center(otherObject.position.x() + otherObject.size.width() * .5,
                             otherObject.position.y() + otherObject.size.height() * .5);
        const QPointF ndc = widgetToNdc(center);
        const QSizeF hs = widgetSizeToNdc(otherObject.size);
        centers.append(QVector2D(ndc.x(), ndc.y()));
        halves.append(QVector2D(hs.width() * .5f, hs.height() * .5f));
        powers.append(otherObject.powerFactor > 0 ? otherObject.powerFactor : m_globalPower);
        strengths.append(connection.strength);
        if (centers.size() >= 8)
            break;
    }
    const int count = centers.size();
    m_glassShader->setUniformValue("u_numConnections", count);
    if (count > 0) {
        m_glassShader->setUniformValueArray("u_connCenterB", centers.constData(), count);
        m_glassShader->setUniformValueArray("u_connHalfSizeB", halves.constData(), count);
        m_glassShader->setUniformValueArray("u_connPowerB", powers.constData(), count, 1);
        m_glassShader->setUniformValueArray("u_connStrength", strengths.constData(), count, 1);
    }
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, m_sceneFbo ? m_sceneFbo->texture() : 0);
    glEnable(GL_BLEND);
    glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    drawFullscreenQuad(m_glassShader);
    glBindTexture(GL_TEXTURE_2D, 0);
    m_glassShader->release();
    glDisable(GL_SCISSOR_TEST);
}

void QtGlassFlowScene::updateConnections()
{
    if (!m_connectionsDirty)
        return;
    m_connections.clear();
    if (m_attractionDist <= 0) {
        m_connectionsDirty = false;
        return;
    }
    for (int i = 0; i < m_objects.size(); ++i) {
        if (!m_objects[i].visible)
            continue;
        for (int j = i + 1; j < m_objects.size(); ++j) {
            if (!m_objects[j].visible)
                continue;
            const QPointF a(m_objects[i].position.x() + m_objects[i].size.width() * .5,
                            m_objects[i].position.y() + m_objects[i].size.height() * .5);
            const QPointF b(m_objects[j].position.x() + m_objects[j].size.width() * .5,
                            m_objects[j].position.y() + m_objects[j].size.height() * .5);
            const float dx = float(a.x() - b.x());
            const float dy = float(a.y() - b.y());
            const float distance = std::sqrt(dx * dx + dy * dy);
            const float ra = float(qMin(m_objects[i].size.width(), m_objects[i].size.height()) * .5);
            const float rb = float(qMin(m_objects[j].size.width(), m_objects[j].size.height()) * .5);
            const float gap = distance - ra - rb;
            if (gap < m_attractionDist)
                m_connections.append({i, j, qBound(0.0f, 1.0f - gap / m_attractionDist, 1.0f)});
        }
    }
    m_connectionsDirty = false;
}

void QtGlassFlowScene::paintGL()
{
    // 暂停只停止自动调度；Qt 主动请求绘制时仍须完整重建帧，避免呈现失效 FBO 内容。
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_SCISSOR_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glActiveTexture(GL_TEXTURE0);
    glBlendEquation(GL_FUNC_ADD);
    const qreal dpr = devicePixelRatioF();
    const int nativePixelWidth = qMax(1, qRound(width() * dpr));
    const int nativePixelHeight = qMax(1, qRound(height() * dpr));
    const int pixelWidth = qMax(1, qRound(nativePixelWidth * m_renderScale));
    const int pixelHeight = qMax(1, qRound(nativePixelHeight * m_renderScale));
    if (m_gpuReady && (!m_sceneFbo || m_sceneFbo->width() != pixelWidth
                       || m_sceneFbo->height() != pixelHeight))
        createFBOs(pixelWidth, pixelHeight);

    if (m_glassRenderingEnabled && m_gpuReady) {
        if (m_bgDirty)
            loadBackgroundTexture();
        updateConnections();
        if (m_blurCacheDirty) {
            renderBackgroundToFbo();
            runBlurPass();
            m_blurCacheDirty = false;
        }
    }

    glBindFramebuffer(GL_FRAMEBUFFER, defaultFramebufferObject());
    // 最终帧缓冲始终使用屏幕原生分辨率，仅背景和模糊缓存随画质缩放。
    glViewport(0, 0, nativePixelWidth, nativePixelHeight);
    glDisable(GL_SCISSOR_TEST);
    glClearColor(0, 0, 0, 0);
    glClear(GL_COLOR_BUFFER_BIT);
    if (m_glassRenderingEnabled && m_gpuReady) {
        for (int i = 0; i < m_objects.size(); ++i)
            renderGlassObject(i);
    }
    glDisable(GL_BLEND);
    reportGlErrors("glass-paint");

    m_textRenderer.begin(this, QSize(nativePixelWidth, nativePixelHeight));
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::TextAntialiasing,
                          !(painter.font().styleStrategy() & QFont::NoAntialias));
    paintOverlay(painter);
    painter.end();
    m_textRenderer.end();
    reportGlErrors("overlay-paint");
    ++m_frameRevision;
    if (RenderDiagnostics::enabled() && m_diagnosticsClock.elapsed() >= 5000) {
        const qint64 elapsed = m_diagnosticsClock.restart();
        RenderDiagnostics::write("render.heartbeat", QString("surface=0x%1 hwnd=0x%2 frame=%3 fps=%4 suspended=%5 visible=%6 geometry=%7,%8,%9x%10 DPR=%11 scale=%12 defaultFbo=%13 captureCache=%14 snapshotReads=%15")
            .arg(quintptr(this), 0, 16).arg(quintptr(effectiveWinId()), 0, 16).arg(m_frameRevision)
            .arg((m_frameRevision - m_diagnosticsFrames) * 1000.0 / qMax<qint64>(1, elapsed), 0, 'f', 1)
            .arg(m_renderingSuspended).arg(isVisible()).arg(x()).arg(y()).arg(width()).arg(height())
            .arg(devicePixelRatioF()).arg(m_renderScale).arg(defaultFramebufferObject())
            .arg(!m_blurCacheDirty).arg(m_snapshotReadbacks));
        m_diagnosticsFrames = m_frameRevision;
    }
    emit frameRendered();
}

QImage QtGlassFlowScene::surfaceSnapshot()
{
    if (!isValid() || m_frameRevision == 0)
        return {};
    if (m_snapshotRevision == m_frameRevision && !m_surfaceSnapshot.isNull())
        return m_surfaceSnapshot;

    QOpenGLContext* previous = QOpenGLContext::currentContext();
    QSurface* previousSurface = previous ? previous->surface() : nullptr;
    // makeCurrent 会绑定默认 FBO；已处于此上下文时先保留调用者的离屏目标。
    if (previous != context())
        makeCurrent();
    if (QOpenGLContext::currentContext() != context())
    {
        RenderDiagnostics::write("gl.make-current.failed", QString("surface=0x%1 contextValid=%2")
            .arg(quintptr(this), 0, 16).arg(context() && context()->isValid()), 1000);
        return m_surfaceSnapshot;
    }
    const QSize native(qRound(width() * devicePixelRatioF()), qRound(height() * devicePixelRatioF()));
    // 快照不放大原生帧缓冲；超采样只用于玻璃与背景处理，避免重叠卡片重复缩放。
    const QSize reduced(qMax(1, qRound(native.width() * qMin(1.0f, m_renderScale))),
                        qMax(1, qRound(native.height() * qMin(1.0f, m_renderScale))));
    QSize readSize = native;
    const bool canBlit = QOpenGLFramebufferObject::hasOpenGLFramebufferBlit();
    GLint readFramebuffer = 0, drawFramebuffer = 0;
    glGetIntegerv(canBlit ? GL_READ_FRAMEBUFFER_BINDING : GL_FRAMEBUFFER_BINDING, &readFramebuffer);
    if (canBlit)
        glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &drawFramebuffer);
    glBindFramebuffer(GL_FRAMEBUFFER, defaultFramebufferObject());
    if (canBlit) {
        if (!m_snapshotFbo || m_snapshotFbo->size() != reduced) {
            delete m_snapshotFbo;
            m_snapshotFbo = new QOpenGLFramebufferObject(reduced);
        }
        if (m_snapshotFbo->isValid()) {
            auto* gl = context()->extraFunctions();
            gl->glBindFramebuffer(GL_READ_FRAMEBUFFER, defaultFramebufferObject());
            gl->glBindFramebuffer(GL_DRAW_FRAMEBUFFER, m_snapshotFbo->handle());
            gl->glBlitFramebuffer(0, 0, native.width(), native.height(),
                                  0, 0, reduced.width(), reduced.height(), GL_COLOR_BUFFER_BIT, GL_LINEAR);
            glBindFramebuffer(GL_FRAMEBUFFER, m_snapshotFbo->handle());
            readSize = reduced;
        }
    }
    QImage pixels(readSize, QImage::Format_RGBA8888_Premultiplied);
    {
        PixelTransferScope transfer(this, false);
        glReadPixels(0, 0, readSize.width(), readSize.height(), GL_RGBA, GL_UNSIGNED_BYTE, pixels.bits());
        reportGlErrors("snapshot-read");
    }
    if (canBlit) {
        auto* gl = context()->extraFunctions();
        gl->glBindFramebuffer(GL_READ_FRAMEBUFFER, GLuint(readFramebuffer));
        gl->glBindFramebuffer(GL_DRAW_FRAMEBUFFER, GLuint(drawFramebuffer));
    } else {
        glBindFramebuffer(GL_FRAMEBUFFER, GLuint(readFramebuffer));
    }
    doneCurrent();
    if (previous && previousSurface)
        previous->makeCurrent(previousSurface);
#if QT_VERSION >= QT_VERSION_CHECK(6, 9, 0)
    pixels = pixels.flipped(Qt::Vertical);
#else
    pixels = pixels.mirrored(false, true);
#endif
    if (pixels.size() != reduced)
        pixels = pixels.scaled(reduced, Qt::IgnoreAspectRatio, Qt::FastTransformation);
    m_surfaceSnapshot = std::move(pixels);
    m_snapshotRevision = m_frameRevision;
    ++m_snapshotReadbacks;
    return m_surfaceSnapshot;
}

void QtGlassFlowScene::paintOverlay(QPainter &painter)
{
    Q_UNUSED(painter);
}

bool QtGlassFlowScene::hitTest(int index, const QPointF &point) const
{
    if (index < 0 || index >= m_objects.size())
        return false;
    const GlassObject &object = m_objects[index];
    if (!object.visible || object.size.width() <= 0 || object.size.height() <= 0)
        return false;
    const double cx = object.position.x() + object.size.width() * .5;
    const double cy = object.position.y() + object.size.height() * .5;
    const double lx = (point.x() - cx) / (object.size.width() * .5);
    const double ly = (point.y() - cy) / (object.size.height() * .5);
    const double n = object.powerFactor > 0 ? object.powerFactor : m_globalPower;
    return std::pow(std::abs(lx), n) + std::pow(std::abs(ly), n) <= 1.0;
}

void QtGlassFlowScene::mousePressEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton) {
        for (int i = m_objects.size() - 1; i >= 0; --i) {
            if (hitTest(i, event->position())) {
                m_dragIndex = i;
                m_objects[i].dragOffset = event->position() - m_objects[i].position;
                syncTimerCadence();
                update();
                return;
            }
        }
    }
    QOpenGLWidget::mousePressEvent(event);
}

void QtGlassFlowScene::mouseMoveEvent(QMouseEvent *event)
{
    if (m_dragIndex >= 0 && m_dragIndex < m_objects.size()) {
        GlassObject &object = m_objects[m_dragIndex];
        QPointF position = event->position() - object.dragOffset;
        position.setX(qBound<qreal>(0, position.x(), width() - object.size.width()));
        position.setY(qBound<qreal>(0, position.y(), height() - object.size.height()));
        object.position = position;
        m_connectionsDirty = true;
        update();
        return;
    }
    QOpenGLWidget::mouseMoveEvent(event);
}

void QtGlassFlowScene::mouseReleaseEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton && m_dragIndex >= 0
        && m_dragIndex < m_objects.size()) {
        m_dragIndex = -1;
        syncTimerCadence();
        update();
        return;
    }
    QOpenGLWidget::mouseReleaseEvent(event);
}
