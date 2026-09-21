// Modified for macdowsOS Widget on 2026-09-13.
// Changes include reusable background input, adaptive frame scheduling,
// reduced-resolution rendering, and desktop-widget integration.
// Distributed under GNU GPL v3.0; see ../LICENSE.

#include "qtglassflowscene.h"

#include <QFile>
#include <QImage>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QShowEvent>
#include <QHideEvent>
#include <QTimer>
#include <QVector2D>
#include <QVector3D>
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
    "void main() { gl_FragColor = texture2D(u_texture, v_texcoord); }\n";

static const float kQuadVerts[] = {
    -1.0f, -1.0f, 0.0f, 0.0f,
     1.0f, -1.0f, 1.0f, 0.0f,
     1.0f,  1.0f, 1.0f, 1.0f,
    -1.0f, -1.0f, 0.0f, 0.0f,
     1.0f,  1.0f, 1.0f, 1.0f,
    -1.0f,  1.0f, 0.0f, 1.0f
};

QImage boxBlurPass(const QImage &source, int radius, bool horizontal)
{
    if (source.isNull() || radius <= 0)
        return source;
    QImage result(source.size(), QImage::Format_ARGB32_Premultiplied);
    const int width = source.width();
    const int height = source.height();
    const int window = radius * 2 + 1;
    if (horizontal) {
        for (int y = 0; y < height; ++y) {
            const uchar *input = source.constScanLine(y);
            uchar *output = result.scanLine(y);
            int sums[4] = {};
            for (int offset = -radius; offset <= radius; ++offset) {
                const uchar *pixel = input + qBound(0, offset, width - 1) * 4;
                for (int channel = 0; channel < 4; ++channel)
                    sums[channel] += pixel[channel];
            }
            for (int x = 0; x < width; ++x) {
                for (int channel = 0; channel < 4; ++channel)
                    output[x * 4 + channel] = uchar((sums[channel] + window / 2) / window);
                const uchar *removed = input + qBound(0, x - radius, width - 1) * 4;
                const uchar *added = input + qBound(0, x + radius + 1, width - 1) * 4;
                for (int channel = 0; channel < 4; ++channel)
                    sums[channel] += int(added[channel]) - int(removed[channel]);
            }
        }
    } else {
        for (int x = 0; x < width; ++x) {
            int sums[4] = {};
            for (int offset = -radius; offset <= radius; ++offset) {
                const uchar *pixel = source.constScanLine(qBound(0, offset, height - 1)) + x * 4;
                for (int channel = 0; channel < 4; ++channel)
                    sums[channel] += pixel[channel];
            }
            for (int y = 0; y < height; ++y) {
                uchar *output = result.scanLine(y) + x * 4;
                for (int channel = 0; channel < 4; ++channel)
                    output[channel] = uchar((sums[channel] + window / 2) / window);
                const uchar *removed = source.constScanLine(
                    qBound(0, y - radius, height - 1)) + x * 4;
                const uchar *added = source.constScanLine(
                    qBound(0, y + radius + 1, height - 1)) + x * 4;
                for (int channel = 0; channel < 4; ++channel)
                    sums[channel] += int(added[channel]) - int(removed[channel]);
            }
        }
    }
    return result;
}

QImage boxBlur(QImage image, int radius, int passes)
{
    for (int pass = 0; pass < passes; ++pass) {
        image = boxBlurPass(image, radius, true);
        image = boxBlurPass(image, radius, false);
    }
    return image;
}

// Keep fractional coverage between passes. Quantizing each axis to 8 bits
// creates aligned plateaus around highlights, even at native resolution.
// Several smaller boxes approximate a Gaussian without the rectangular lobes
// of one/two large boxes; running sums keep the work linear in image size.
QImage highPrecisionBlur(const QImage &source, int radius, int originalPasses, int passes)
{
    QImage image = source.convertToFormat(QImage::Format_RGBA64_Premultiplied);
    QImage scratch(image.size(), image.format());
    if (image.isNull() || scratch.isNull())
        return source;
    const double variance = originalPasses * radius * (radius + 1.0) / 3.0;
    int lowerWidth = qFloor(std::sqrt(12.0 * variance / passes + 1.0));
    if (!(lowerWidth & 1))
        --lowerWidth;
    lowerWidth = qMax(1, lowerWidth);
    const double lowerVariance = (lowerWidth * lowerWidth - 1.0) / 12.0;
    const int upperCount = qBound(0, qRound((variance - passes * lowerVariance)
                                          / ((lowerWidth + 1.0) / 3.0)), passes);
    struct Sum {
        int r = 0, g = 0, b = 0, a = 0;
        void add(QRgba64 p) { r += p.red(); g += p.green(); b += p.blue(); a += p.alpha(); }
        void remove(QRgba64 p) { r -= p.red(); g -= p.green(); b -= p.blue(); a -= p.alpha(); }
        QRgba64 pixel(int n) const {
            return QRgba64::fromRgba64((r + n / 2) / n, (g + n / 2) / n,
                                       (b + n / 2) / n, (a + n / 2) / n);
        }
    };
    const int w = image.width(), h = image.height();
    for (int pass = 0; pass < passes; ++pass) {
        const int r = lowerWidth / 2 + (pass < upperCount ? 1 : 0);
        const int n = r * 2 + 1;
        for (int y = 0; y < h; ++y) {
            const auto *input = reinterpret_cast<const QRgba64 *>(image.constScanLine(y));
            auto *output = reinterpret_cast<QRgba64 *>(scratch.scanLine(y));
            Sum sum;
            for (int k = -r; k <= r; ++k)
                sum.add(input[qBound(0, k, w - 1)]);
            for (int x = 0; x < w; ++x) {
                output[x] = sum.pixel(n);
                sum.remove(input[qMax(0, x - r)]);
                sum.add(input[qMin(w - 1, x + r + 1)]);
            }
        }
        QVector<Sum> columns(w);
        for (int k = -r; k <= r; ++k) {
            const auto *row = reinterpret_cast<const QRgba64 *>(scratch.constScanLine(qBound(0, k, h - 1)));
            for (int x = 0; x < w; ++x)
                columns[x].add(row[x]);
        }
        for (int y = 0; y < h; ++y) {
            auto *output = reinterpret_cast<QRgba64 *>(image.scanLine(y));
            const auto *removed = reinterpret_cast<const QRgba64 *>(scratch.constScanLine(qMax(0, y - r)));
            const auto *added = reinterpret_cast<const QRgba64 *>(scratch.constScanLine(qMin(h - 1, y + r + 1)));
            for (int x = 0; x < w; ++x) {
                output[x] = columns[x].pixel(n);
                columns[x].remove(removed[x]);
                columns[x].add(added[x]);
            }
        }
    }
    return image.convertToFormat(QImage::Format_ARGB32_Premultiplied);
}
}

QtGlassFlowScene::QtGlassFlowScene(QWidget *parent)
    : QOpenGLWidget(parent),
      m_sceneFbo(nullptr), m_blurPingFbo(nullptr), m_blurPongFbo(nullptr),
      m_blitShader(nullptr), m_blurShader(nullptr), m_glassShader(nullptr),
      m_quadVbo(QOpenGLBuffer::VertexBuffer), m_quadInitialized(false),
      m_bgTexture(0), m_bgWidth(0), m_bgHeight(0), m_bgDirty(false),
      m_bgImageAlreadyFlipped(false),
      m_glassRenderingEnabled(true), m_blurOnly(false), m_glassOpacity(1.0f),
      m_blurCacheDirty(true), m_cpuBackdropDirty(true),
      m_renderBackend(AutoBackend), m_effectiveRenderBackend(GpuBackend),
      // Native resolution is the quality default. Users can explicitly pick
      // a reduced scale on slower GPUs, but fractional-DPI screens should not
      // silently start from an upscaled 60% glass buffer.
      m_renderScale(1.0f), m_connectionsDirty(true),
      m_refractionA(0.7f), m_refractionB(2.3f), m_refractionC(5.2f),
      m_refractionD(6.9f), m_fPower(1.0f), m_blurRadius(2.0f),
      m_blurIterations(2), m_noiseAmount(0.06f), m_attractionDist(160.0f),
      m_globalPower(3.0f), m_timer(nullptr), m_refreshInterval(16),
      m_externalInteraction(false),
      m_animationEnabled(false),
      m_hoveredIndex(-1), m_dragIndex(-1)
{
    setMouseTracking(true);
    setAttribute(Qt::WA_OpaquePaintEvent, true);

    QSurfaceFormat fmt = format();
    fmt.setRenderableType(QSurfaceFormat::OpenGL);
    fmt.setProfile(QSurfaceFormat::CompatibilityProfile);
    fmt.setVersion(2, 1);
    fmt.setSwapBehavior(QSurfaceFormat::DoubleBuffer);
    // QOpenGLWidget renders into an offscreen texture which is subsequently
    // composed by Qt and DWM. Waiting for another swap interval here creates
    // a second, serial vsync and can halve live-backdrop cadence.
    fmt.setSwapInterval(0);
    setFormat(fmt);
    // Repaint the full transparent framebuffer on every scheduled frame.
    // PartialUpdate can expose Qt's cleared backing texture after a framebuffer
    // read or a DWM composition change, which appears as a one-frame white card.
    // Other glass windows use m_surfaceSnapshot, so they do not rely on FBO
    // preservation between paintGL calls.
    setUpdateBehavior(QOpenGLWidget::NoPartialUpdate);
}

QtGlassFlowScene::~QtGlassFlowScene()
{
    cleanupGL();
}

void QtGlassFlowScene::cleanupGL()
{
    if (!context())
        return;
    disconnect(context(), &QOpenGLContext::aboutToBeDestroyed,
               this, &QtGlassFlowScene::cleanupGL);
    makeCurrent();
    destroyFBOs();
    delete m_snapshotFbo;
    m_snapshotFbo = nullptr;
    m_surfaceSnapshot = {};
    m_snapshotRevision = 0;
    if (m_bgTexture) {
        glDeleteTextures(1, &m_bgTexture);
        m_bgTexture = 0;
    }
    delete m_blitShader;
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
                                     float power, const QString &text)
{
    GlassObject object;
    object.position = pos;
    object.size = size;
    object.powerFactor = power;
    object.text = text;
    m_objects.append(object);
    m_connectionsDirty = true;
    update();
    return m_objects.size() - 1;
}

void QtGlassFlowScene::setGlassObjectGeometry(int index, const QPointF &pos,
                                              const QSizeF &size)
{
    if (index < 0 || index >= m_objects.size())
        return;
    m_objects[index].position = pos;
    m_objects[index].size = size;
    m_connectionsDirty = true;
    update();
}

void QtGlassFlowScene::setGlassObjectCornerRadius(int index, float radius)
{
    if (index < 0 || index >= m_objects.size())
        return;
    m_objects[index].cornerRadius = qMax(0.0f, radius);
    update();
}

void QtGlassFlowScene::setGlassObjectVisible(int index, bool visible)
{
    if (index < 0 || index >= m_objects.size())
        return;
    if (m_objects[index].visible == visible)
        return;
    m_objects[index].visible = visible;
    if (!visible) {
        if (m_hoveredIndex == index)
            m_hoveredIndex = -1;
        if (m_dragIndex == index)
            m_dragIndex = -1;
    }
    m_connectionsDirty = true;
    update();
}

void QtGlassFlowScene::setBackgroundImage(const QString &path)
{
    m_bgPath = path;
    m_bgImage = QImage();
    m_bgImageAlreadyFlipped = false;
    m_bgDirty = true;
    m_blurCacheDirty = true;
    m_cpuBackdropDirty = true;
    if (isValid()) {
        makeCurrent();
        loadBackgroundTexture();
        doneCurrent();
    }
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
    m_bgPath.clear();
    m_bgImage = image;
    m_bgImageAlreadyFlipped = alreadyFlipped;
    m_bgDirty = true;
    m_blurCacheDirty = true;
    m_cpuBackdropDirty = true;
    update();
}

void QtGlassFlowScene::setGlassRenderingEnabled(bool enabled)
{
    if (m_glassRenderingEnabled == enabled)
        return;
    m_glassRenderingEnabled = enabled;
    update();
}

void QtGlassFlowScene::setBlurOnlyEnabled(bool enabled)
{
    if (m_blurOnly == enabled)
        return;
    m_blurOnly = enabled;
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

void QtGlassFlowScene::setRefreshInterval(int intervalMs)
{
    // High-refresh displays need intervals below 16 ms (120 Hz = 8 ms,
    // 144 Hz = 6-7 ms, 240 Hz = 4 ms). Presentation is still vsync-bound by
    // the surface format, so extra timer wakes are coalesced by Qt/DWM.
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
    // A static card has no time-varying pixels: all visual changes arrive via
    // update() from an input/data event. Stop the timer completely in that
    // state instead of waking the GUI and GPU five times per second. Animated
    // cards (the clock) keep their low idle cadence, and interaction always
    // restores the active display's requested cadence immediately.
    if (!isVisible() || m_renderingSuspended || (!interacting && !m_animationEnabled)) {
        m_timer->stop();
        return;
    }

    const int interval = interacting ? m_refreshInterval
                                     : qMax(m_refreshInterval, 64);
    // Coarse wakeups are materially cheaper for an idle animation and do not
    // affect pointer-driven rendering, which remains on a precise timer.
    m_timer->setTimerType(interacting ? Qt::PreciseTimer : Qt::CoarseTimer);
    if (m_timer->interval() != interval)
        m_timer->setInterval(interval);
    if (!m_timer->isActive())
        m_timer->start();
}

void QtGlassFlowScene::setRenderBackend(RenderBackend backend)
{
    if (m_renderBackend == backend)
        return;
    m_renderBackend = backend;
    if (isValid())
        makeCurrent();
    updateEffectiveBackend();
    if (isValid())
        doneCurrent();
    m_blurCacheDirty = true;
    m_cpuBackdropDirty = true;
    update();
}

void QtGlassFlowScene::setRenderScale(float scale)
{
    const float next = qBound(0.5f, scale, 1.5f);
    if (qFuzzyCompare(m_renderScale, next))
        return;
    m_renderScale = next;
    m_blurCacheDirty = true;
    m_cpuBackdropDirty = true;
    update();
}

void QtGlassFlowScene::setPowerFactor(float value) { m_globalPower = value; update(); }
void QtGlassFlowScene::setRefractionPower(float value) { m_fPower = value; update(); }

void QtGlassFlowScene::setBlurRadius(float value)
{
    const float next = qMax(0.0f, value);
    if (qFuzzyCompare(m_blurRadius, next))
        return;
    m_blurRadius = next;
    m_blurCacheDirty = true;
    m_cpuBackdropDirty = true;
    update();
}

void QtGlassFlowScene::setBlurIterations(int value)
{
    const int next = qMax(1, value);
    if (m_blurIterations == next)
        return;
    m_blurIterations = next;
    m_blurCacheDirty = true;
    m_cpuBackdropDirty = true;
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

void QtGlassFlowScene::drawFullscreenQuad(QOpenGLShaderProgram *program)
{
    m_quadVbo.bind();
    constexpr int stride = 4 * sizeof(float);
    program->enableAttributeArray(0);
    program->enableAttributeArray(1);
    program->setAttributeBuffer(0, GL_FLOAT, 0, 2, stride);
    program->setAttributeBuffer(1, GL_FLOAT, 2 * sizeof(float), 2, stride);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    program->disableAttributeArray(0);
    program->disableAttributeArray(1);
    m_quadVbo.release();
}

void QtGlassFlowScene::initializeGL()
{
    initializeOpenGLFunctions();
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
    updateEffectiveBackend();
    initQuad();
    if (!m_bgPath.isEmpty() || !m_bgImage.isNull())
        loadBackgroundTexture();

    if (!m_timer) {
        m_timer = new QTimer(this);
        m_timer->setTimerType(Qt::PreciseTimer);
        connect(m_timer, &QTimer::timeout, this, [this]() {
            if (isVisible())
                update();
        });
    }
    // syncTimerCadence() owns the timer lifecycle. In particular, static
    // cards deliberately leave it stopped. Starting it unconditionally here
    // would restart a newly-created QTimer at its default 0 ms interval and
    // cause a tight repaint loop on every non-animated widget.
    syncTimerCadence();
    m_clock.start();
}

void QtGlassFlowScene::resizeGL(int w, int h)
{
    Q_UNUSED(w);
    Q_UNUSED(h);
    const qreal dpr = devicePixelRatioF();
    const int nativePixelWidth = qMax(1, qRound(width() * dpr));
    const int nativePixelHeight = qMax(1, qRound(height() * dpr));
    if (m_effectiveRenderBackend == CpuBackend) {
        destroyFBOs();
        glViewport(0, 0, nativePixelWidth, nativePixelHeight);
        m_cpuBackdropDirty = true;
        return;
    }
    const int pixelWidth = qMax(1, qRound(nativePixelWidth * m_renderScale));
    const int pixelHeight = qMax(1, qRound(nativePixelHeight * m_renderScale));
    createFBOs(pixelWidth, pixelHeight);
    m_blurCacheDirty = true;
    m_cpuBackdropDirty = true;
    glViewport(0, 0, nativePixelWidth, nativePixelHeight);
}

void QtGlassFlowScene::showEvent(QShowEvent *event)
{
    QOpenGLWidget::showEvent(event);
    // A hidden QOpenGLWidget stops its scene timer completely. Re-evaluate the
    // cadence when it becomes visible so animated cards resume at their normal
    // rate while static cards remain event-driven and asleep.
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
    m_blurPongFbo = new QOpenGLFramebufferObject(w, h, format);
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
    setLinear(m_blurPongFbo);
    m_blurCacheDirty = true;
}

void QtGlassFlowScene::destroyFBOs()
{
    delete m_sceneFbo; m_sceneFbo = nullptr;
    delete m_blurPingFbo; m_blurPingFbo = nullptr;
    delete m_blurPongFbo; m_blurPongFbo = nullptr;
}

void QtGlassFlowScene::loadBackgroundTexture()
{
    QImage image = m_bgImage.isNull() && !m_bgPath.isEmpty() ? QImage(m_bgPath) : m_bgImage;
    if (image.isNull()) {
        qWarning() << "failed to load background image:" << m_bgPath;
        return;
    }
#if QT_VERSION >= QT_VERSION_CHECK(6, 9, 0)
    if (!m_bgImageAlreadyFlipped)
        image = image.flipped(Qt::Vertical);
#else
    if (!m_bgImageAlreadyFlipped)
        image = image.mirrored(false, true);
#endif
    // DesktopCapture and wallpaperBackdrop are RGB32 images on Windows.
    // Upload those buffers in their native BGRA layout instead of converting
    // and copying the entire crop to RGBA on every update.  Keep conversion
    // only for uncommon image formats that the OpenGL upload cannot consume
    // directly.
    GLenum uploadFormat = GL_RGBA;
    GLenum uploadType = GL_UNSIGNED_BYTE;
    if (image.format() == QImage::Format_RGB32
        || image.format() == QImage::Format_ARGB32
        || image.format() == QImage::Format_ARGB32_Premultiplied) {
        uploadFormat = GL_BGRA;
        // QImage's 32-bit formats are laid out as B,G,R,A bytes on the
        // Windows little-endian targets supported here, so no per-pixel
        // packing conversion is needed.
        uploadType = GL_UNSIGNED_BYTE;
    } else if (image.format() != QImage::Format_RGBA8888) {
        image = image.convertToFormat(QImage::Format_RGBA8888);
    }
    if (!m_bgTexture)
        glGenTextures(1, &m_bgTexture);
    glBindTexture(GL_TEXTURE_2D, m_bgTexture);
    const bool sameSize = m_bgWidth == image.width() && m_bgHeight == image.height();
    if (sameSize) {
        // Reuse the existing allocation for live backdrop updates. Reallocating
        // a full texture on every drag frame causes avoidable driver stalls.
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, image.width(), image.height(),
                        uploadFormat, uploadType, image.constBits());
    } else {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, image.width(), image.height(), 0,
                     uploadFormat, uploadType, image.constBits());
    }
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);
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
    if (!m_sceneFbo || !m_blurPingFbo || !m_blurPongFbo)
        return;
    m_blurShader->bind();
    m_blurShader->setUniformValue("u_resolution", QVector2D(float(m_sceneFbo->width()), float(m_sceneFbo->height())));
    // Dilating a fixed seven-tap kernel leaves unsampled gaps and produces a
    // lattice around highlights. High quality covers every texel, combining
    // adjacent Gaussian taps with hardware linear filtering. Lower qualities
    // explicitly trade sampling density for speed as well as FBO resolution.
    const float sigma = qBound(0.01f, m_blurRadius * m_renderScale * 1.95f, 42.0f);
    const int stride = m_renderScale < .7f ? 4 : m_renderScale < .9f ? 3
                     : m_renderScale < 1.2f ? 2 : 1;
    const int radius = qMin(127, qCeil(sigma * 3.0f));
    QVector<QVector2D> samples;
    float total = 1.0f;
    for (int first = 1; first <= radius; first += 2 * stride) {
        const int second = first + stride;
        const float a = std::exp(-float(first * first) / (2 * sigma * sigma));
        const float b = second <= radius
            ? std::exp(-float(second * second) / (2 * sigma * sigma)) : 0.0f;
        const float weight = a + b;
        if (weight < 1e-8f)
            break;
        samples.append(QVector2D((first * a + second * b) / weight, weight));
        total += 2 * weight;
    }
    for (auto& sample : samples)
        sample.setY(sample.y() / total);
    m_blurShader->setUniformValue("u_centerWeight", 1.0f / total);
    m_blurShader->setUniformValue("u_sampleCount", int(samples.size()));
    if (!samples.isEmpty())
        m_blurShader->setUniformValueArray("u_samples", samples.constData(), int(samples.size()));
    m_blurShader->setUniformValue("u_texture", 0);
    glActiveTexture(GL_TEXTURE0);
    glDisable(GL_BLEND);
    GLuint source = m_sceneFbo->texture();
    for (int i = 0; i < m_blurIterations; ++i) {
        m_blurPingFbo->bind();
        glViewport(0, 0, m_blurPingFbo->width(), m_blurPingFbo->height());
        glBindTexture(GL_TEXTURE_2D, source);
        m_blurShader->setUniformValue("u_direction", QVector2D(1, 0));
        drawFullscreenQuad(m_blurShader);
        m_blurPingFbo->release();

        m_blurPongFbo->bind();
        glViewport(0, 0, m_blurPongFbo->width(), m_blurPongFbo->height());
        glBindTexture(GL_TEXTURE_2D, m_blurPingFbo->texture());
        m_blurShader->setUniformValue("u_direction", QVector2D(0, 1));
        drawFullscreenQuad(m_blurShader);
        m_blurPongFbo->release();
        source = m_blurPongFbo->texture();
    }
    glBindTexture(GL_TEXTURE_2D, 0);
    m_blurShader->release();
}

void QtGlassFlowScene::updateEffectiveBackend()
{
    if (m_renderBackend == CpuBackend || (context() && !m_gpuReady)) {
        m_effectiveRenderBackend = CpuBackend;
        return;
    }
    m_effectiveRenderBackend = GpuBackend;
    if (m_renderBackend != AutoBackend || !QOpenGLContext::currentContext())
        return;
    const GLubyte *renderer = glGetString(GL_RENDERER);
    if (!renderer)
        return;
    const QString name = QString::fromLatin1(reinterpret_cast<const char *>(renderer)).toLower();
    if (name.contains("gdi generic") || name.contains("llvmpipe")
        || name.contains("softpipe") || name.contains("software")
        || name.contains("microsoft basic render"))
        m_effectiveRenderBackend = CpuBackend;
}

void QtGlassFlowScene::ensureCpuBackdrop()
{
    if (!m_cpuBackdropDirty)
        return;
    QImage source = m_bgImage.isNull() && !m_bgPath.isEmpty() ? QImage(m_bgPath) : m_bgImage;
    if (source.isNull()) {
        m_cpuBlurredImage = QImage();
        m_cpuBackdropDirty = false;
        return;
    }
    // Live desktop crops are kept in OpenGL orientation to avoid an extra
    // upload-time copy. The software fallback still works in image (top-left)
    // coordinates, so restore that orientation before CPU scaling/blur.
#if QT_VERSION >= QT_VERSION_CHECK(6, 9, 0)
    if (m_bgImageAlreadyFlipped)
        source = source.flipped(Qt::Vertical);
#else
    if (m_bgImageAlreadyFlipped)
        source = source.mirrored(false, true);
#endif
    const qreal dpr = devicePixelRatioF();
    // CPU quality used to downsample every backdrop by a blur-dependent 1/7
    // (or worse) and enlarge it again. That shortcut produced the regular
    // square lattice visible over bright, strongly blurred regions. Economical
    // modes still follow their selected scale, but native/high/maximum quality
    // now blur at full device-pixel resolution. Supersampling a CPU image above
    // native would add no detail to QPainter's native-sized destination.
    const qreal quality = qBound<qreal>(0.5, m_renderScale, 1.5);
    const qreal workingScale = qMin<qreal>(1.0, quality);
    const QSize working(qMax(1, qRound(width() * dpr * workingScale)),
                        qMax(1, qRound(height() * dpr * workingScale)));
    source = source.convertToFormat(QImage::Format_ARGB32_Premultiplied)
                 .scaled(working, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    const int radius = qBound(1, qRound(m_blurRadius * workingScale * 1.8), 36);
    const int passes = qBound(1, m_blurIterations, 3);
    m_cpuBlurredImage = quality >= 1.25
        ? highPrecisionBlur(source, radius, passes, quality >= 1.5 ? 5 : 3)
        : boxBlur(std::move(source), radius, passes);
    m_cpuBackdropDirty = false;
}

void QtGlassFlowScene::renderCpuGlass(QPainter &painter)
{
    ensureCpuBackdrop();
    if (m_cpuBlurredImage.isNull())
        return;
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
    for (const GlassObject &object : std::as_const(m_objects)) {
        if (!object.visible)
            continue;
        const QRectF objectRect(object.position, object.size);
        const qreal radius = qBound<qreal>(0, object.cornerRadius,
                                           qMin(objectRect.width(), objectRect.height()) * .5);
        QPainterPath shape;
        shape.addRoundedRect(objectRect, radius, radius);
        painter.save();
        painter.setClipPath(shape);
        painter.setOpacity(m_glassOpacity);
        painter.drawImage(QRectF(rect()), m_cpuBlurredImage);
        painter.setOpacity(1.0);
        QLinearGradient sheen(objectRect.topLeft(), objectRect.bottomLeft());
        sheen.setColorAt(0, QColor(255, 255, 255, 36));
        sheen.setColorAt(.35, QColor(255, 255, 255, 8));
        sheen.setColorAt(1, QColor(20, 28, 40, 18));
        painter.fillPath(shape, sheen);
        painter.restore();
        QLinearGradient rim(objectRect.topLeft(), objectRect.bottomRight());
        rim.setColorAt(0, QColor(255, 255, 255, 150));
        rim.setColorAt(.5, QColor(255, 255, 255, 28));
        rim.setColorAt(1, QColor(255, 255, 255, 95));
        painter.setBrush(Qt::NoBrush);
        painter.setPen(QPen(QBrush(rim), 1.0));
        painter.drawRoundedRect(objectRect.adjusted(.6, .6, -.6, -.6),
                                qMax<qreal>(0, radius - .6), qMax<qreal>(0, radius - .6));
    }
    painter.restore();
}

void QtGlassFlowScene::blitTextureToScreen(GLuint texture)
{
    glDisable(GL_BLEND);
    m_blitShader->bind();
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, texture);
    m_blitShader->setUniformValue("u_texture", 0);
    drawFullscreenQuad(m_blitShader);
    glBindTexture(GL_TEXTURE_2D, 0);
    m_blitShader->release();
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
    const QPointF centerWidget(object.position.x() + object.size.width() * .5,
                               object.position.y() + object.size.height() * .5);
    const QPointF centerNdc = widgetToNdc(centerWidget);
    const QSizeF fullNdc = widgetSizeToNdc(object.size);
    const QSizeF halfNdc(fullNdc.width() * .5, fullNdc.height() * .5);
    const float power = object.powerFactor > 0 ? object.powerFactor : m_globalPower;
    m_glassShader->bind();
    m_glassShader->setUniformValue("u_blurredTex", 0);
    m_glassShader->setUniformValue("u_resolution", QVector2D(m_sceneFbo ? m_sceneFbo->width() : width(),
                                                               m_sceneFbo ? m_sceneFbo->height() : height()));
    m_glassShader->setUniformValue("u_objCenter", QVector2D(centerNdc.x(), centerNdc.y()));
    m_glassShader->setUniformValue("u_objHalfSize", QVector2D(halfNdc.width(), halfNdc.height()));
    const float dpr = float(devicePixelRatioF());
    m_glassShader->setUniformValue("u_objHalfSizePx", QVector2D(float(object.size.width()) * dpr * .5f,
                                                                   float(object.size.height()) * dpr * .5f));
    m_glassShader->setUniformValue("u_cornerRadiusPx", object.cornerRadius * dpr);
    m_glassShader->setUniformValue("u_powerFactor", power);
    m_glassShader->setUniformValue("u_a", m_refractionA);
    m_glassShader->setUniformValue("u_b", m_refractionB);
    m_glassShader->setUniformValue("u_c", m_refractionC);
    m_glassShader->setUniformValue("u_d", m_refractionD);
    m_glassShader->setUniformValue("u_fPower", m_blurOnly ? 0.0f : m_fPower);
    // Dither is useful at reduced resolution but its deterministic pixel
    // pattern can itself read as a fine grid. Fade it out with quality and
    // disable it completely at maximum quality, where the higher precision
    // sampling makes the trade-off unnecessary.
    const float ditherWeight = qBound(0.0f, (1.5f - m_renderScale) / 0.9f, 1.0f);
    const float dither = qMin(m_noiseAmount, 1.0f / 255.0f) * ditherWeight;
    m_glassShader->setUniformValue("u_noise", m_blurOnly ? 0.0f : dither);
    m_glassShader->setUniformValue("u_edgeAaScale",
                                   qBound(0.85f, 0.65f + m_renderScale * 0.45f, 1.30f));
    m_glassShader->setUniformValue("u_time", float(m_clock.elapsed()) * .001f);
    m_glassShader->setUniformValue("u_tintColor", QVector3D(0, 0, 0));
    m_glassShader->setUniformValue("u_tintStrength", 0.0f);
    m_glassShader->setUniformValue("u_opacity", m_glassOpacity);
    m_glassShader->setUniformValue("u_rippleTime", -1.0f);
    m_glassShader->setUniformValue("u_rippleCenter", QVector2D(.5f, .5f));
    m_glassShader->setUniformValue("u_flowSpeed", 0.0f);
    m_glassShader->setUniformValue("u_deformAmount", 0.0f);

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
    glBindTexture(GL_TEXTURE_2D, m_blurPongFbo ? m_blurPongFbo->texture() : 0);
    glEnable(GL_BLEND);
    glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    drawFullscreenQuad(m_glassShader);
    glBindTexture(GL_TEXTURE_2D, 0);
    m_glassShader->release();
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
    // Qt can deliver a paint queued before occlusion even after our timer
    // stops. Retain the last completed snapshot/revision until resumed.
    if (m_renderingSuspended)
        return;
    const qreal dpr = devicePixelRatioF();
    const int nativePixelWidth = qMax(1, qRound(width() * dpr));
    const int nativePixelHeight = qMax(1, qRound(height() * dpr));
    const int pixelWidth = qMax(1, qRound(nativePixelWidth * m_renderScale));
    const int pixelHeight = qMax(1, qRound(nativePixelHeight * m_renderScale));
    const bool useCpu = m_effectiveRenderBackend == CpuBackend;
    if (useCpu && m_sceneFbo)
        destroyFBOs();
    if (!useCpu && (!m_sceneFbo || m_sceneFbo->width() != pixelWidth
                    || m_sceneFbo->height() != pixelHeight))
        createFBOs(pixelWidth, pixelHeight);

    if (m_glassRenderingEnabled && !useCpu) {
        if (m_bgDirty)
            loadBackgroundTexture();
        updateConnections();
        if (m_blurCacheDirty) {
            renderBackgroundToFbo();
            runBlurPass();
            m_blurCacheDirty = false;
        }
    } else if (useCpu) {
        ensureCpuBackdrop();
    }

    glBindFramebuffer(GL_FRAMEBUFFER, defaultFramebufferObject());
    // The widget's default framebuffer remains native resolution. Only the
    // cached backdrop and blur passes use the reduced render target above.
    glViewport(0, 0, nativePixelWidth, nativePixelHeight);
    glClearColor(0, 0, 0, 0);
    glClear(GL_COLOR_BUFFER_BIT);
    if (m_glassRenderingEnabled && useCpu) {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        renderCpuGlass(painter);
        painter.end();
    } else if (m_glassRenderingEnabled) {
        for (int i = 0; i < m_objects.size(); ++i)
            renderGlassObject(i);
    }
    glDisable(GL_BLEND);

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::TextAntialiasing, true);
    QFont font = painter.font();
    font.setPointSizeF(12.0);
    font.setBold(true);
    painter.setFont(font);
    for (int i = 0; i < m_objects.size(); ++i) {
        const GlassObject &object = m_objects[i];
        if (!object.visible || object.text.isEmpty())
            continue;
        const QRectF objectRect(object.position, object.size);
        painter.setPen(QColor(0, 0, 0, 120));
        painter.drawText(objectRect.translated(1, 1), Qt::AlignCenter, object.text);
        painter.setPen(i == m_hoveredIndex ? QColor(255, 255, 255) : QColor(240, 240, 248));
        painter.drawText(objectRect, Qt::AlignCenter, object.text);
    }
    paintOverlay(painter);
    painter.end();
    ++m_frameRevision;
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
    makeCurrent();
    const QSize native(qRound(width() * devicePixelRatioF()), qRound(height() * devicePixelRatioF()));
    // The visible framebuffer is already at native device resolution. Never
    // enlarge this snapshot: doing so adds no detail and forces overlapping
    // cards through a needless upsample/downsample cycle. RenderScale > 1 is
    // applied to the glass/background FBOs before their native resolve.
    const QSize reduced(qMax(1, qRound(native.width() * qMin(1.0f, m_renderScale))),
                        qMax(1, qRound(native.height() * qMin(1.0f, m_renderScale))));
    QSize readSize = native;
    glBindFramebuffer(GL_FRAMEBUFFER, defaultFramebufferObject());
    if (QOpenGLFramebufferObject::hasOpenGLFramebufferBlit()) {
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
    glReadPixels(0, 0, readSize.width(), readSize.height(), GL_RGBA, GL_UNSIGNED_BYTE, pixels.bits());
    glBindFramebuffer(GL_FRAMEBUFFER, defaultFramebufferObject());
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
                m_objects[i].state = Pressed;
                m_objects[i].dragging = true;
                m_objects[i].dragOffset = event->position() - m_objects[i].position;
                emit objectClicked(i);
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
    if (m_dragIndex >= 0 && m_dragIndex < m_objects.size() && m_objects[m_dragIndex].dragging) {
        GlassObject &object = m_objects[m_dragIndex];
        QPointF position = event->position() - object.dragOffset;
        position.setX(qBound<qreal>(0, position.x(), width() - object.size.width()));
        position.setY(qBound<qreal>(0, position.y(), height() - object.size.height()));
        object.position = position;
        m_connectionsDirty = true;
        update();
        return;
    }
    int hover = -1;
    for (int i = m_objects.size() - 1; i >= 0; --i) {
        if (hitTest(i, event->position())) { hover = i; break; }
    }
    if (hover != m_hoveredIndex) {
        if (m_hoveredIndex >= 0 && m_hoveredIndex < m_objects.size()
            && m_objects[m_hoveredIndex].state != Pressed)
            m_objects[m_hoveredIndex].state = Normal;
        m_hoveredIndex = hover;
        if (hover >= 0)
            m_objects[hover].state = Hovered;
        syncTimerCadence();
        update();
    }
    QOpenGLWidget::mouseMoveEvent(event);
}

void QtGlassFlowScene::mouseReleaseEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton && m_dragIndex >= 0
        && m_dragIndex < m_objects.size()) {
        GlassObject &object = m_objects[m_dragIndex];
        object.dragging = false;
        object.state = hitTest(m_dragIndex, event->position()) ? Hovered : Normal;
        m_dragIndex = -1;
        syncTimerCadence();
        update();
        return;
    }
    QOpenGLWidget::mouseReleaseEvent(event);
}

void QtGlassFlowScene::leaveEvent(QEvent *event)
{
    if (m_hoveredIndex >= 0 && m_hoveredIndex < m_objects.size()
        && m_objects[m_hoveredIndex].state != Pressed)
        m_objects[m_hoveredIndex].state = Normal;
    m_hoveredIndex = -1;
    syncTimerCadence();
    update();
    QOpenGLWidget::leaveEvent(event);
}
