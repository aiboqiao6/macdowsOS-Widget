// Modified for macdowsOS Widget on 2026-09-13.
// Changes include reusable background input, adaptive frame scheduling,
// reduced-resolution rendering, and desktop-widget integration.
// Distributed under GNU GPL v3.0; see ../LICENSE.

#ifndef QTGLASSFLOWSCENE_H
#define QTGLASSFLOWSCENE_H

#include <QOpenGLWidget>
#include <QOpenGLFunctions>
#include <QOpenGLShaderProgram>
#include <QOpenGLBuffer>
#include <QOpenGLFramebufferObject>
#include <QVector>
#include <QPointF>
#include <QSizeF>
#include <QString>
#include <QElapsedTimer>
#include <QImage>

class QPainter;
class QTimer;
class QShowEvent;
class QHideEvent;

class QtGlassFlowScene : public QOpenGLWidget, protected QOpenGLFunctions
{
    Q_OBJECT
public:
    enum State { Normal, Hovered, Pressed };

    struct GlassObject {
        QPointF position;
        QSizeF size;
        float powerFactor;
        float cornerRadius;
        QString text;
        State state;
        bool dragging;
        QPointF dragOffset;
        bool visible;

        GlassObject()
            : powerFactor(3.0f), cornerRadius(0.0f), state(Normal),
              dragging(false), visible(true) {}
    };

    struct Connection {
        int objA;
        int objB;
        float strength;
    };

    explicit QtGlassFlowScene(QWidget *parent = nullptr);
    ~QtGlassFlowScene() override;

    int addGlassObject(const QPointF &pos, const QSizeF &size,
                       float power = 3.0f,
                       const QString &text = QString());
    void setGlassObjectGeometry(int index, const QPointF &pos,
                                const QSizeF &size);
    void setGlassObjectCornerRadius(int index, float radius);
    void setGlassObjectVisible(int index, bool visible);
    void setBackgroundImage(const QString &path);
    void setBackgroundImage(const QImage &image);
    // Same as setBackgroundImage(), but the image is already vertically
    // oriented for OpenGL texture coordinates. This avoids a second copy when
    // a live desktop crop is captured every frame during a drag.
    void setBackgroundImageFlipped(const QImage &image);

    void setGlassRenderingEnabled(bool enabled);
    bool glassRenderingEnabled() const { return m_glassRenderingEnabled; }
    void setGlassOpacity(float opacity);
    float glassOpacity() const { return m_glassOpacity; }
    void setRefreshInterval(int intervalMs);
    int refreshInterval() const { return m_refreshInterval; }
    // Hosts that handle dragging outside QtGlassFlowScene can keep the render
    // timer at its interactive cadence while the pointer is down.
    void setInteractionActive(bool active);
    // Opt-in for widgets with a continuously changing payload (for example a
    // sweeping second hand). Static cards can keep the lower idle cadence.
    void setAnimationEnabled(bool enabled);
    void setContinuousRenderingEnabled(bool enabled);
    void setRenderingSuspended(bool suspended);
    quint64 frameRevision() const { return m_frameRevision; }
    // Read the last completed frame without asking QOpenGLWidget to render
    // again. Cache one reduced, premultiplied image per visual revision.
    QImage surfaceSnapshot();
    quint64 snapshotReadbacks() const { return m_snapshotReadbacks; }

    // Internal render resolution multiplier. The final widget is still
    // composited at the native device-pixel size, while the backdrop/FBO
    // passes can run at a lower resolution to reduce fill-rate and bandwidth.
    // A value of 1.0 uses the native resolution.
    void setRenderScale(float scale);
    float renderScale() const { return m_renderScale; }

    void setPowerFactor(float v);
    void setRefractionPower(float v);
    void setBlurRadius(float v);
    void setBlurIterations(int v);
    void setNoiseAmount(float v);
    void setAttractionDistance(float v);

signals:
    void objectClicked(int index);
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
    void leaveEvent(QEvent *event) override;

private:
    friend class WidgetRegression;
    bool compileProgram(QOpenGLShaderProgram *prog,
                        const QString &vertPath,
                        const QString &fragPath);
    void initQuad();
    void cleanupGL();
    void drawFullscreenQuad(QOpenGLShaderProgram *prog);
    void createFBOs(int w, int h);
    void destroyFBOs();
    void loadBackgroundTexture();
    void renderBackgroundToFbo();
    void runBlurPass();
    void blitTextureToScreen(GLuint tex);
    void renderGlassObject(int index);
    void updateConnections();
    void syncTimerCadence();
    void setBackgroundImageInternal(const QImage &image, bool alreadyFlipped);
    bool hitTest(int objIndex, const QPointF &widgetPos) const;

    QPointF widgetToNdc(const QPointF &p) const;
    QSizeF widgetSizeToNdc(const QSizeF &s) const;

    QOpenGLFramebufferObject *m_sceneFbo;
    QOpenGLFramebufferObject *m_blurPingFbo;
    QOpenGLFramebufferObject *m_blurPongFbo;
    QOpenGLShaderProgram *m_blitShader;
    QOpenGLShaderProgram *m_blurShader;
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
    QString m_bgPath;
    QImage m_bgImage;
    bool m_bgImageAlreadyFlipped;
    bool m_bgDirty;
    bool m_glassRenderingEnabled;
    float m_glassOpacity;
    bool m_blurCacheDirty;
    float m_renderScale;

    QVector<GlassObject> m_objects;
    QVector<Connection> m_connections;
    bool m_connectionsDirty;

    float m_refractionA;
    float m_refractionB;
    float m_refractionC;
    float m_refractionD;
    float m_fPower;
    float m_blurRadius;
    int m_blurIterations;
    float m_noiseAmount;
    float m_attractionDist;
    float m_globalPower;

    // The Gaussian offsets only depend on the blur settings and render scale.
    // Keep them across backdrop updates; live capture can otherwise rebuild
    // the same exp() table once per frame.
    float m_blurKernelSigma = -1.0f;
    int m_blurKernelStride = 0;
    QVector<QVector2D> m_blurKernelSamples;
    float m_blurKernelCenterWeight = 1.0f;

    QTimer *m_timer;
    int m_refreshInterval;
    bool m_externalInteraction;
    bool m_animationEnabled;
    bool m_continuousRendering = false;
    bool m_renderingSuspended = false;
    QElapsedTimer m_clock;
    int m_hoveredIndex;
    int m_dragIndex;
};

#endif // QTGLASSFLOWSCENE_H
