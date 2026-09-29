#pragma once

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QSettings>
#include <QStandardPaths>
#include <QVariant>
#include <QHash>

// Application preferences live alongside the layout in widgets.json. Each
// instance reads the latest file, and sync merges only changed keys so a
// layout save cannot overwrite settings (or vice versa).
class AppSettings final
{
public:
    AppSettings()
    {
        QFile file(filePath());
        const bool fileExists = file.exists();
        if (file.open(QIODevice::ReadOnly))
            m_root = QJsonDocument::fromJson(file.readAll()).object();
        if (!fileExists)
            return; // A read must not create the first-run marker.

        const QJsonObject saved = m_root.value(QStringLiteral("settings")).toObject();
        m_removeLegacyBlur = saved.value(QStringLiteral("appearance"))
                                 .toObject().contains(QStringLiteral("systemBlur"));
        m_removeLegacyBackend = saved.value(QStringLiteral("performance"))
                                    .toObject().contains(QStringLiteral("renderBackend"));
        QSettings legacy;
        for (auto it = defaults().cbegin(); it != defaults().cend(); ++it) {
            if (hasKey(saved, it.key()))
                continue;
            QVariant migrated = legacy.contains(it.key()) ? legacy.value(it.key()) : it.value();
            if (it.key() == QStringLiteral("appearance/scale")
                && !legacy.contains(it.key()) && m_root.contains(QStringLiteral("scale")))
                migrated = m_root.value(QStringLiteral("scale")).toDouble(1.0);
#ifdef Q_OS_WIN
            if (it.key() == QStringLiteral("startup/enabled")) {
                QSettings runKey(QStringLiteral("HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Run"),
                                 QSettings::NativeFormat);
                migrated = runKey.contains(QStringLiteral("macdowsOS Widget"))
                           || runKey.contains(QStringLiteral("macdowsOSBattery"));
            }
#endif
            m_updates.insert(it.key(), migrated);
        }
    }

    ~AppSettings() { sync(); }
    AppSettings(const AppSettings&) = delete;
    AppSettings& operator=(const AppSettings&) = delete;

    static QString filePath()
    {
        return QDir(QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation))
            .filePath(QStringLiteral("widgets.json"));
    }

    QVariant value(const QString& key, const QVariant& fallback = {}) const
    {
        if (m_updates.contains(key))
            return m_updates.value(key);
        const QJsonObject saved = m_root.value(QStringLiteral("settings")).toObject();
        const QJsonValue result = findValue(saved, key);
        if (!result.isUndefined())
            return result.toVariant();
        return defaults().value(key, fallback);
    }

    bool contains(const QString& key) const
    {
        return m_updates.contains(key)
               || hasKey(m_root.value(QStringLiteral("settings")).toObject(), key);
    }

    void setValue(const QString& key, const QVariant& value)
    {
        if (contains(key) && this->value(key) == value)
            return;
        m_updates.insert(key, value);
    }

    void ensureDefaults()
    {
        for (auto it = defaults().cbegin(); it != defaults().cend(); ++it)
            if (!contains(it.key()))
                m_updates.insert(it.key(), it.value());
    }

    bool sync()
    {
        if (m_updates.isEmpty() && !m_removeLegacyBlur && !m_removeLegacyBackend)
            return true;
        const QString path = filePath();
        if (!QDir().mkpath(QFileInfo(path).absolutePath()))
            return false;
        QFile current(path);
        QJsonObject root = m_root;
        if (current.open(QIODevice::ReadOnly)) {
            const QJsonDocument document = QJsonDocument::fromJson(current.readAll());
            if (document.isObject())
                root = document.object();
            current.close();
        }
        QJsonObject saved = root.value(QStringLiteral("settings")).toObject();
        for (auto it = m_updates.cbegin(); it != m_updates.cend(); ++it)
            putValue(saved, it.key(), QJsonValue::fromVariant(it.value()));
        if (m_removeLegacyBlur) {
            QJsonObject appearance = saved.value(QStringLiteral("appearance")).toObject();
            appearance.remove(QStringLiteral("systemBlur"));
            saved.insert(QStringLiteral("appearance"), appearance);
        }
        if (m_removeLegacyBackend) {
            QJsonObject performance = saved.value(QStringLiteral("performance")).toObject();
            performance.remove(QStringLiteral("renderBackend"));
            saved.insert(QStringLiteral("performance"), performance);
        }
        root.insert(QStringLiteral("settings"), saved);
        if (!root.contains(QStringLiteral("version")))
            root.insert(QStringLiteral("version"), 4);
        if (!root.contains(QStringLiteral("widgets")))
            root.insert(QStringLiteral("widgets"), QJsonArray());
        QSaveFile file(path);
        if (!file.open(QIODevice::WriteOnly))
            return false;
        file.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
        if (!file.commit())
            return false;
        m_root = root;
        m_updates.clear();
        m_removeLegacyBlur = false;
        m_removeLegacyBackend = false;
        return true;
    }

private:
    static const QHash<QString, QVariant>& defaults()
    {
        static const QHash<QString, QVariant> values = {
            {QStringLiteral("appearance/scale"), 1.0},
            {QStringLiteral("appearance/blurStrength"), 50},
            {QStringLiteral("appearance/opacity"), 1.0},
            {QStringLiteral("appearance/liveBackdrop"), true},
            {QStringLiteral("appearance/fontSmoothing"), 2},
            {QStringLiteral("interaction/mouseThrough"), false},
            {QStringLiteral("interaction/lowPowerRefresh"), false},
            {QStringLiteral("performance/renderScale"), 0.60},
            {QStringLiteral("weather/location"), QStringLiteral("北京")},
            {QStringLiteral("weather/cityId"), QStringLiteral("101010100")},
            {QStringLiteral("startup/enabled"), false}
        };
        return values;
    }

    static QJsonValue findValue(const QJsonObject& object, const QString& key)
    {
        const int slash = key.indexOf(QLatin1Char('/'));
        if (slash < 0)
            return object.value(key);
        return findValue(object.value(key.left(slash)).toObject(), key.mid(slash + 1));
    }

    static bool hasKey(const QJsonObject& object, const QString& key)
    {
        return !findValue(object, key).isUndefined();
    }

    static void putValue(QJsonObject& object, const QString& key, const QJsonValue& value)
    {
        const int slash = key.indexOf(QLatin1Char('/'));
        if (slash < 0) {
            object.insert(key, value);
            return;
        }
        const QString group = key.left(slash);
        QJsonObject nested = object.value(group).toObject();
        putValue(nested, key.mid(slash + 1), value);
        object.insert(group, nested);
    }

    QJsonObject m_root;
    QHash<QString, QVariant> m_updates;
    bool m_removeLegacyBlur = false;
    bool m_removeLegacyBackend = false;
};
