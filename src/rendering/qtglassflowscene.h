// 基于 QtGlassFlow 修改，增加背景输入、按需刷新与桌面集成。采用 GPL v3，见项目根目录 LICENSE。

#ifndef QTGLASSFLOWSCENE_H
#define QTGLASSFLOWSCENE_H

#include <QOpenGLWidget>
#include <QOpenGLFunctions>
#include <QOpenGLShaderProgram>
#include <QOpenGLBuffer>
#include <QOpenGLFramebufferObject>
#include <QVector>
#include <QCache>
#include <QSet>
#include <QPointF>
#include <QSizeF>
#include <QString>
#include <QElapsedTimer>
#include <QImage>
#include "rendering/cleartypetext.h"
#include "rendering/liquidglassstyle.h"

class QPainter;
class QTimer;
class QShowEvent;
class QHideEvent;

class QtGlassFlowScene : public QOpenGLWidget, protected QOpenGLFunctions
{
    Q_OBJECT
    ClearTypeText::Renderer m_textRenderer;
public:
    // 未保存自定义设置时，完全沿用原有液态玻璃材质。
    struct MaterialParameters {
        float opacity = 1.0f;
        float refraction = LiquidGlassStyle::kRefractionPower;
        float blurRadius = LiquidGlassStyle::kBlurRadius;
        int blurIterations = LiquidGlassStyle::kBlurIterations;
        float noise = LiquidGlassStyle::kNoiseAmount;
    };

    struct GlassObject {
        QPointF position;
        QSizeF size;
        float powerFactor;
        float cornerRadius;
        QPointF dragOffset;
        bool visible;

        GlassObject()
            : powerFactor(LiquidGlassStyle::kShapePower), cornerRadius(0.0f), visible(true) {}
    };

    struct Connection {
        int objA;
        int objB;
        float strength;
    };

    explicit QtGlassFlowScene(QWidget *parent = nullptr);
    ~QtGlassFlowScene() override;

    int addGlassObject(const QPointF &pos, const QSizeF &size,
                       float power = LiquidGlassStyle::kShapePower);
    void setGlassObjectGeometry(int index, const QPointF &pos,
                                const QSizeF &size);
    void setGlassObjectCornerRadius(int index, float radius);
    void setGlassObjectVisible(int index, bool visible);
    void setBackgroundImage(const QImage &image);
    // 图像已按 GL 纹理坐标翻转，避免实时裁剪再次复制。
    void setBackgroundImageFlipped(const QImage &image);

    void setGlassRenderingEnabled(bool enabled);
    bool glassRenderingEnabled() const { return m_glassRenderingEnabled; }
    void setGlassOpacity(float opacity);
    float glassOpacity() const { return m_glassOpacity; }
    void setDarkModeEnabled(bool enabled);
    bool darkModeEnabled() const { return m_darkModeEnabled; }
    MaterialParameters materialParameters() const;
    void setMaterialParameters(const MaterialParameters& parameters);
    void setRefreshInterval(int intervalMs);
    int refreshInterval() const { return m_refreshInterval; }
    // 由宿主处理窗口拖动时，仍可启用交互刷新节奏。
    void setInteractionActive(bool active);
    // 为连续变化的内容启用动画，例如平滑秒针；静态卡片按需绘制。
    void setAnimationEnabled(bool enabled);
    void setContinuousRenderingEnabled(bool enabled);
    void setRenderingSuspended(bool suspended);
    quint64 frameRevision() const { return m_frameRevision; }
    // 读取最后完成帧，不触发额外重绘；每个视觉版本缓存一张预乘图像。
    QImage surfaceSnapshot();
    quint64 snapshotReadbacks() const { return m_snapshotReadbacks; }

    // 内部背景与 FBO 分辨率倍率；最终窗口仍按屏幕原生像素合成，1.0 表示原生画质。
    void setRenderScale(float scale);
    float renderScale() const { return m_renderScale; }

    void setPowerFactor(float v);
    void setRefractionPower(float v);
    void setBlurRadius(float v);
    void setBlurIterations(int v);
    void setNoiseAmount(float v);
    void setAttractionDistance(float v);

signals:
    void frameRendered();

protected:
    void initializeGL() override;
    void paintGL() override;
    void resizeGL(int w, int h) override;
    void showEvent(QShowEvent *event) override;
    void hideEvent(QHideEvent *event) override;
    virtual void paintOverlay(QPainter &painter);
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;

private:
    friend class WidgetRegression;
    bool compileProgram(QOpenGLShaderProgram *prog,
                        const QString &vertPath,
                        const QString &fragPath);
    void initQuad();
    void cleanupGL();
    void reportGlErrors(const char* stage);
    void bindFullscreenQuad(QOpenGLShaderProgram* program);
    void releaseFullscreenQuad(QOpenGLShaderProgram* program);
    void drawFullscreenQuad(QOpenGLShaderProgram *prog);
    QOpenGLShaderProgram* blurProgram(int sampleCount);
    void createFBOs(int w, int h);
    void destroyFBOs();
    void loadBackgroundTexture();
    void renderBackgroundToFbo();
    void runBlurPass();
    void renderGlassObject(int index);
    void updateConnections();
    void syncTimerCadence();
    void setBackgroundImageInternal(const QImage &image, bool alreadyFlipped);
    bool hitTest(int objIndex, const QPointF &widgetPos) const;

    QPointF widgetToNdc(const QPointF &p) const;
    QSizeF widgetSizeToNdc(const QSizeF &s) const;

    QOpenGLFramebufferObject *m_sceneFbo;
    QOpenGLFramebufferObject *m_blurPingFbo;
    QOpenGLShaderProgram *m_blitShader;
    QOpenGLShaderProgram *m_blurShader;
    // 按采样数缓存最多四个程序，必须在所属上下文中清空。
    QCache<int, QOpenGLShaderProgram> m_blurPrograms{4};
    QSet<int> m_failedBlurPrograms;
    QOpenGLShaderProgram *m_glassShader;
    QOpenGLBuffer m_quadVbo;
    bool m_quadInitialized;
    bool m_gpuReady = false;
    quint64 m_frameRevision = 0;
    QImage m_surfaceSnapshot;
    quint64 m_snapshotRevision = 0;
    quint64 m_snapshotReadbacks = 0;
    QOpenGLFramebufferObject* m_snapshotFbo = nullptr;

    GLuint m_bgTexture;
    int m_bgWidth;
    int m_bgHeight;
    QImage m_bgImage;
    bool m_bgImageAlreadyFlipped;
    bool m_bgDirty;
    bool m_glassRenderingEnabled;
    float m_glassOpacity;
    bool m_darkModeEnabled = false;
    bool m_blurCacheDirty;
    float m_renderScale;

    QVector<GlassObject> m_objects;
    QVector<Connection> m_connections;
    bool m_connectionsDirty;

    float m_fPower;
    float m_blurRadius;
    int m_blurIterations;
    float m_noiseAmount;
    float m_attractionDist;
    float m_globalPower;

    // 高斯偏移仅随模糊参数和画质变化，背景更新时复用，避免每帧计算指数表。
    float m_blurKernelSigma = -1.0f;
    int m_blurKernelStride = 0;
    QVector<QVector2D> m_blurKernelSamples;
    float m_blurKernelCenterWeight = 1.0f;
    QOpenGLShaderProgram* m_uploadedKernelProgram = nullptr;
    float m_uploadedKernelSigma = -1.0f;
    int m_uploadedKernelStride = 0;

    QTimer *m_timer;
    int m_refreshInterval;
    bool m_externalInteraction;
    bool m_animationEnabled;
    bool m_continuousRendering = false;
    bool m_renderingSuspended = false;
    QElapsedTimer m_diagnosticsClock;
    quint64 m_diagnosticsFrames = 0;
    int m_dragIndex;
};

#endif
