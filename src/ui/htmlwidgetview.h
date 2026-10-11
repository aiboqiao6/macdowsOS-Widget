#pragma once
#include <QWidget>
#include <functional>
#include <memory>

class HtmlWidgetView final : public QWidget {
public:
    explicit HtmlWidgetView(QWidget* parent = nullptr);
    ~HtmlWidgetView() override;
    void setDocument(const QString& document);
    void setContentScale(qreal scale);
    bool isReady() const;
    bool hasLoaded() const;
    QString errorText() const;
    // 视图销毁后取消待完成回调，避免预览关闭后访问调用方。
    void evaluateScript(const QString& script, std::function<void(QString)> callback);
    std::function<void(const QString&)> statusChanged;
protected:
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void focusInEvent(QFocusEvent* event) override;
private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
    void initialize();
    void updateBounds();
    void fail(const QString& message);
};
