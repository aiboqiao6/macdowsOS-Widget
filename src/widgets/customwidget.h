#pragma once

#include <QJsonObject>
#include <QList>
#include <QString>

namespace CustomWidgets {
constexpr qint64 MaxFileBytes = 1024 * 1024;
struct Definition {
    QString id;
    QString name;
    QString mode = QStringLiteral("blocks");
    int variant = 1;
    QString background = QStringLiteral("#18243b");
    QString foreground = QStringLiteral("#f4f7ff");
    QString accent = QStringLiteral("#78aaff");
    int padding = 22;
    QList<QJsonObject> blocks;
    QString html;
    QString css;
    QString javascript;
    bool allowNetwork = false;
    QJsonObject toJson() const;
};
QString catalogPath();
bool fromJson(const QJsonObject& object, Definition& result, QString* error = nullptr);
QList<Definition> load(QString* error = nullptr);
bool save(const Definition& definition, QString* error = nullptr);
bool remove(const QString& id, QString* error = nullptr);
Definition makeTemplate(int index = 0);
QString document(const Definition& definition);
QString sandboxedDocument(const Definition& definition);
bool importFile(const QString& path, Definition& result, QString* error = nullptr);
bool exportFile(const QString& path, const Definition& definition, QString* error = nullptr);
}
