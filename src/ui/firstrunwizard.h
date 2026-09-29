#pragma once

#include "rendering/liquidglasswidget.h"

class QCheckBox;
class QCloseEvent;
class QComboBox;
class QEventLoop;
class QFrame;
class QLabel;
class QParallelAnimationGroup;
class QShowEvent;
class QSlider;
class QWidget;

class FirstRunWizard final : public LiquidGlassWidget
{
public:
    explicit FirstRunWizard(QWidget* parent = nullptr);
    bool run();

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void showEvent(QShowEvent* event) override;
    void closeEvent(QCloseEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void paintOverlay(QPainter& painter) override;
    bool wallpaperOnlyWhenIdle() const override { return !liveBackdropEnabled(); }

private:
    void finish();
    void showPage(int index);
    void updateScaleValue(int percent);
    void rememberInterfaceMetrics();
    void applyInterfaceScale();

    QWidget* m_pageHost = nullptr;
    QWidget* m_pages[3] = {nullptr, nullptr, nullptr};
    QParallelAnimationGroup* m_transition = nullptr;
    int m_currentPage = 0;
    bool m_introPlayed = false;
    QSlider* m_scaleSlider = nullptr;
    QLabel* m_scaleValue = nullptr;
    QComboBox* m_qualityBox = nullptr;
    QCheckBox* m_liveBackdrop = nullptr;
    QEventLoop* m_loop = nullptr;
    bool m_completed = false;
    qreal m_interfaceScale = 1.0;
};
