#pragma once

#include "rendering/desktopcapture.h"
#include <QMenu>
#include <QTimer>

class LiquidGlassMenuSurface;

// QMenu still owns input, accessibility, action layout and popup dismissal.
// Its transparent child draws the same glass pipeline as desktop cards.
class LiquidGlassMenu final : public QMenu
{
public:
    enum class Icon { Add, Search, Remove };
    explicit LiquidGlassMenu(const QFont& font, QWidget* parent = nullptr);
    ~LiquidGlassMenu() override;
    QAction* addGlassAction(const QString& text, Icon icon);
    QAction* exec(const QPoint& position);
    void popup(const QPoint& position);

protected:
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void moveEvent(QMoveEvent* event) override;
    void paintEvent(QPaintEvent* event) override;
    bool event(QEvent* event) override;

private:
    friend class LiquidGlassMenuSurface;
    void paintContents(QPainter& painter);
    void refreshBackdrop();
    void releaseBackdrop();
    QPoint preparePopup(const QPoint& position);
    void createSurface();
    LiquidGlassMenuSurface* m_surface = nullptr;
    QTimer m_captureTimer;
    DesktopCapture::Cache m_cache;
    QImage m_fallback;
    QImage m_lastBackdrop;
    WId m_registeredWindow = 0;
    quint64 m_generation = 0;
    bool m_capturePending = false;
    bool m_liveBackdrop = false;
    float m_renderScale = 1.0f;
};
