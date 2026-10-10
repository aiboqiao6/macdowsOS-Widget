#include "widgets/desktopwidget.h"
#include "widgets/widgetgeometry.h"
#include "widgets/cardmetrics.h"
#include "app/appsettings.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QFile>
#include <QSaveFile>
#include <QDir>
#include <QFileInfo>
#include <QGuiApplication>
#include <QScreen>
#include <QtMath>

using namespace CardMetrics;
using namespace WidgetGeometry;

QString DesktopWidget::kindName(CardKind kind)
{
    switch (kind) {
    case CardKind::Battery: return QStringLiteral("battery");
    case CardKind::Weather: return QStringLiteral("weather");
    case CardKind::Clock: return QStringLiteral("clock");
    case CardKind::Dictionary: return QStringLiteral("dictionary");
    case CardKind::Music: return QStringLiteral("music");
    }
    return QStringLiteral("battery");
}

DesktopWidget::CardKind DesktopWidget::kindFromName(const QString& value)
{
    if (value == QStringLiteral("weather")) return CardKind::Weather;
    if (value == QStringLiteral("clock")) return CardKind::Clock;
    if (value == QStringLiteral("dictionary")) return CardKind::Dictionary;
    if (value == QStringLiteral("music")) return CardKind::Music;
    return CardKind::Battery;
}

QString DesktopWidget::configurationPath()
{
    const QString path = AppSettings::filePath();
    QDir().mkpath(QFileInfo(path).absolutePath());
    return path;
}

void DesktopWidget::saveConfiguration() const
{
    saveAllConfigurations();
}

void DesktopWidget::saveAllConfigurations()
{
    QJsonArray widgets;
    for (DesktopWidget* widget : s_instances) {
        if (!widget)
            continue;
        QJsonObject item;
        item.insert(QStringLiteral("id"), widget->m_instanceId);
        item.insert(QStringLiteral("type"), kindName(widget->m_cardKind));
        item.insert(QStringLiteral("variant"), widget->m_cardVariant);
        item.insert(QStringLiteral("x"), widget->x());
        item.insert(QStringLiteral("y"), widget->y());
        item.insert(QStringLiteral("visible"), widget->isVisible());
        widgets.append(item);
    }
    const QString path = configurationPath();
    QJsonObject root;
    QFile existing(path);
    if (existing.open(QIODevice::ReadOnly)) {
        root = QJsonDocument::fromJson(existing.readAll()).object();
        existing.close();
    }
    root.insert(QStringLiteral("version"), 4);
    root.insert(QStringLiteral("scale"), s_instances.isEmpty() ? 1.0 : s_instances.first()->m_widgetScale);
    root.insert(QStringLiteral("widgets"), widgets);
    QSaveFile file(path);
    if (file.open(QIODevice::WriteOnly)) {
        file.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
        file.commit();
    }
}

DesktopWidget* DesktopWidget::addWidget(CardKind kind, const QPoint& desktopPoint, int variant)
{
    auto* widget = new DesktopWidget(nullptr, kind, false, QString(), variant);
    widget->setAttribute(Qt::WA_DeleteOnClose, true);
    if (!widget->placeAtDesktopPoint(desktopPoint)) {
        delete widget;
        return nullptr;
    }
    widget->show();
    saveAllConfigurations();
    return widget;
}

QList<DesktopWidget*> DesktopWidget::restoreWidgets()
{
    QList<DesktopWidget*> result;
    QFile file(configurationPath());
    if (!file.open(QIODevice::ReadOnly))
        return result;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll());
    const QJsonObject root = document.object();
    const int version = root.value(QStringLiteral("version")).toInt();
    const QJsonArray widgets = root.value(QStringLiteral("widgets")).toArray();
    bool primaryAssigned = false;
    for (const QJsonValue& value : widgets) {
        const QJsonObject item = value.toObject();
        const QString type = item.value(QStringLiteral("type")).toString();
        if (type != QStringLiteral("battery") && type != QStringLiteral("weather")
            && type != QStringLiteral("clock") && type != QStringLiteral("dictionary")
            && type != QStringLiteral("music"))
            continue;
        const CardKind kind = kindFromName(type);
        // 优先由电量组件持有托盘，配置次序变化不改变此规则。
        const bool primary = kind == CardKind::Battery && !primaryAssigned;
        const int storedVariant = item.contains(QStringLiteral("variant"))
            ? item.value(QStringLiteral("variant")).toInt() : defaultVariant(kind);
        auto* widget = new DesktopWidget(nullptr,
                                         kind,
                                         primary,
                                         item.value(QStringLiteral("id")).toString(),
                                         storedVariant);
        QPoint restoredPosition(item.value(QStringLiteral("x")).toInt(),
                                item.value(QStringLiteral("y")).toInt());
        if (version < 4 && item.contains(QStringLiteral("gridColumn"))
            && item.contains(QStringLiteral("gridRow"))) {
            QScreen* targetScreen = nullptr;
            const QString screenName = item.value(QStringLiteral("screen")).toString();
            for (QScreen* candidate : QGuiApplication::screens()) {
                if (candidate && candidate->name() == screenName) {
                    targetScreen = candidate;
                    break;
                }
            }
            if (!targetScreen)
                targetScreen = QGuiApplication::screenAt(restoredPosition);
            if (!targetScreen)
                targetScreen = QGuiApplication::primaryScreen();
            if (targetScreen) {
                const int cell = qMax(1, qRound(kGridBaseCell * widget->m_widgetScale));
                const QRect targetArea = targetScreen->availableGeometry();
                const int gap = gridGapForArea(targetArea, widget->m_widgetScale);
                const int pitch = cell + gap;
                restoredPosition = gridOrigin(targetArea, gap)
                    + QPoint(item.value(QStringLiteral("gridColumn")).toInt() * pitch,
                             item.value(QStringLiteral("gridRow")).toInt() * pitch);
            }
        }
        QRect restoredTarget;
        if (version < 4) {
            restoredTarget = nearestGridRect(kind, widget->m_cardVariant,
                                             restoredPosition, widget->m_widgetScale, widget);
        } else {
            const int cell = qMax(1, qRound(kGridBaseCell * widget->m_widgetScale));
            QScreen* screen = nullptr;
            int bestOverlap = 0;
            // 显示器变更后保存中心可能位于所有屏幕之外，需要重新选择目标屏幕。
            for (QScreen* candidate : QGuiApplication::screens()) {
                const QRect candidateArea = candidate->availableGeometry();
                const int candidateGap = gridGapForArea(candidateArea, widget->m_widgetScale);
                const QSize candidateSize = gridWidgetSize(
                    kind, widget->m_cardVariant, cell, candidateGap);
                const QRect overlap = candidateArea.intersected(
                    QRect(restoredPosition, candidateSize));
                const int pixels = overlap.isValid() ? overlap.width() * overlap.height() : 0;
                if (pixels > bestOverlap) {
                    bestOverlap = pixels;
                    screen = candidate;
                }
            }
            if (!screen)
                screen = QGuiApplication::primaryScreen();
            const QRect area = screen ? screen->availableGeometry() : QRect();
            const int gap = gridGapForArea(area, widget->m_widgetScale);
            const QSize size = gridWidgetSize(kind, widget->m_cardVariant, cell, gap);
            if (screen) {
                restoredPosition.setX(qBound(area.left(), restoredPosition.x(),
                    qMax(area.left(), area.right() - size.width() + 1)));
                restoredPosition.setY(qBound(area.top(), restoredPosition.y(),
                    qMax(area.top(), area.bottom() - size.height() + 1)));
            }
            restoredTarget = QRect(restoredPosition, size);
        }
        if (!restoredTarget.isValid()) {
            delete widget;
            continue;
        }
        primaryAssigned = primaryAssigned || primary;
        widget->m_restoringPosition = true;
        widget->setFixedSize(restoredTarget.size());
        widget->move(restoredTarget.topLeft());
        widget->m_restoringPosition = false;
        widget->m_hasSavedPosition = true;
        if (item.value(QStringLiteral("visible")).toBool(true))
            widget->show();
        result.append(widget);
    }
    if (!primaryAssigned && !result.isEmpty()) {
        // 旧配置即使没有电量卡片，也要指定一个托盘管理者。
        result.first()->m_ownsTray = true;
        result.first()->setupTrayIcon();
    }
    // 恢复旧网格配置后迁移为精确位置。
    if (!result.isEmpty())
        saveAllConfigurations();
    return result;
}

