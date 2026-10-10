#pragma once

#include <QWidget>
class QLabel;
class QSlider;
class ScalePreview;

// 向导和设置共用滑块、百分比与原尺寸预览，拖动时不重建玻璃窗口。
class ScaleSelector final : public QWidget
{
public:
    explicit ScaleSelector(int percent, QWidget* parent = nullptr);
    int percent() const;
    QSlider* slider() const { return m_slider; }
    QSize previewSize() const;
    void setPreviewEnabled(bool enabled);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;

private:
    void updateValue(int percent);
    QSlider* m_slider;
    QLabel* m_value;
    QLabel* m_dimensions;
    ScalePreview* m_preview;
    bool m_previewEnabled = true;
};
