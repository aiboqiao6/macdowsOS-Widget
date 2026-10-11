#include "widgets/customwidget.h"
#include "app/appsettings.h"
#include <QColor>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QSaveFile>
#include <QSet>
#include <QUuid>

namespace CustomWidgets {
namespace {
bool fail(QString* error, const QString& message) {
    if (error) *error = message;
    return false;
}
bool writeCatalog(const QList<Definition>& definitions, QString* error) {
    QJsonArray array;
    for (const auto& definition : definitions) array.append(definition.toJson());
    QJsonObject root{{"version", 1}, {"definitions", array}};
    QDir().mkpath(QFileInfo(catalogPath()).absolutePath());
    QSaveFile file(catalogPath());
    const auto bytes = QJsonDocument(root).toJson();
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit())
        return fail(error, QStringLiteral("无法保存组件库：") + file.errorString());
    return true;
}
QString color(const QString& value) { return QColor(value).name(QColor::HexRgb); }
}
QJsonObject Definition::toJson() const {
    QJsonArray array;
    for (const auto& block : blocks) array.append(block);
    return {{"id", id}, {"name", name}, {"mode", mode}, {"variant", variant},
        {"background", background}, {"foreground", foreground}, {"accent", accent},
        {"padding", padding}, {"blocks", array}, {"html", html}, {"css", css},
        {"javascript", javascript}, {"allowNetwork", allowNetwork}};
}
QString catalogPath() {
    return QDir(QFileInfo(AppSettings::filePath()).absolutePath()).filePath("custom-widgets.json");
}
bool fromJson(const QJsonObject& object, Definition& result, QString* error) {
    Definition candidate;
    candidate.id = object.value("id").toString();
    candidate.name = object.value("name").toString().trimmed();
    candidate.mode = object.value("mode").toString("blocks");
    candidate.variant = object.value("variant").toInt(1);
    candidate.background = object.value("background").toString(candidate.background);
    candidate.foreground = object.value("foreground").toString(candidate.foreground);
    candidate.accent = object.value("accent").toString(candidate.accent);
    candidate.padding = object.value("padding").toInt(22);
    candidate.html = object.value("html").toString();
    candidate.css = object.value("css").toString();
    candidate.javascript = object.value("javascript").toString();
    candidate.allowNetwork = object.value("allowNetwork").toBool(false);
    if (QUuid(candidate.id).isNull()) return fail(error, QStringLiteral("组件 ID 无效。"));
    if (candidate.name.isEmpty() || candidate.name.size() > 80)
        return fail(error, QStringLiteral("请输入 1–80 个字符的组件名称。"));
    if (candidate.mode != "blocks" && candidate.mode != "html")
        return fail(error, QStringLiteral("不支持的组件模式。"));
    if (candidate.variant < 0 || candidate.variant > 2 || candidate.padding < 0 || candidate.padding > 64)
        return fail(error, QStringLiteral("组件尺寸或边距无效。"));
    for (const QString& value : {candidate.background, candidate.foreground, candidate.accent})
        if (!QColor(value).isValid()) return fail(error, QStringLiteral("颜色格式无效，请使用 #RRGGBB。"));
    if (!object.value("blocks").isArray() || object.value("blocks").toArray().size() > 40)
        return fail(error, QStringLiteral("最多可添加 40 个内容块。"));
    const QSet<QString> types{"text", "metric", "clock", "date", "progress", "divider"};
    for (const auto& entry : object.value("blocks").toArray()) {
        if (!entry.isObject()) return fail(error, QStringLiteral("内容块格式无效。"));
        QJsonObject block = entry.toObject();
        if (!types.contains(block.value("type").toString())
            || block.value("text").toString().size() > 4000
            || block.value("size").toInt(18) < 10 || block.value("size").toInt(18) > 96
            || block.value("value").toInt(50) < 0 || block.value("value").toInt(50) > 100
            || !QStringList{"left", "center", "right"}.contains(block.value("align").toString("left")))
            return fail(error, QStringLiteral("内容块类型、字号、进度或对齐方式无效。"));
        candidate.blocks.append(block);
    }
    if (QJsonDocument(object).toJson(QJsonDocument::Compact).size() > MaxFileBytes)
        return fail(error, QStringLiteral("单个组件不能超过 1 MB。"));
    if (sandboxedDocument(candidate).toUtf8().size() > 1900 * 1024)
        return fail(error, QStringLiteral("HTML 编码后内容过大，请减少文本或内嵌资源。"));
    result = candidate;
    if (error) error->clear();
    return true;
}
QList<Definition> load(QString* error) {
    if (error) error->clear();
    QFile file(catalogPath());
    if (!file.exists()) return {};
    if (!file.open(QIODevice::ReadOnly) || file.size() > 32 * MaxFileBytes) {
        fail(error, QStringLiteral("组件库无法读取或超过 32 MB。")); return {};
    }
    QJsonParseError parseError;
    const auto json = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (!json.isObject() || json.object().value("version").toInt() != 1
        || !json.object().value("definitions").isArray()) {
        fail(error, QStringLiteral("组件库格式无效，请检查 custom-widgets.json。")); return {};
    }
    QList<Definition> result;
    QSet<QString> ids;
    for (const auto& entry : json.object().value("definitions").toArray()) {
        Definition definition;
        if (!entry.isObject() || !fromJson(entry.toObject(), definition, error) || ids.contains(definition.id)) {
            fail(error, QStringLiteral("组件库包含无效或重复的组件，原文件已保留。")); return {};
        }
        ids.insert(definition.id);
        result.append(definition);
    }
    return result;
}
bool save(const Definition& definition, QString* error) {
    Definition checked;
    if (!fromJson(definition.toJson(), checked, error)) return false;
    QString loadError;
    auto definitions = load(&loadError);
    if (!loadError.isEmpty()) return fail(error, loadError);
    bool replaced = false;
    for (auto& entry : definitions) if (entry.id == checked.id) { entry = checked; replaced = true; break; }
    if (!replaced) definitions.append(checked);
    if (definitions.size() > 200) return fail(error, QStringLiteral("组件库最多保存 200 个组件。"));
    QJsonArray total;
    for (const auto& entry : definitions) total.append(entry.toJson());
    if (QJsonDocument(total).toJson().size() > 32 * MaxFileBytes)
        return fail(error, QStringLiteral("组件库不能超过 32 MB。"));
    return writeCatalog(definitions, error);
}
bool remove(const QString& id, QString* error) {
    QString loadError;
    auto definitions = load(&loadError);
    if (!loadError.isEmpty()) return fail(error, loadError);
    for (int index = definitions.size() - 1; index >= 0; --index)
        if (definitions[index].id == id) definitions.removeAt(index);
    return writeCatalog(definitions, error);
}
Definition makeTemplate(int index) {
    Definition result;
    result.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    result.name = QStringLiteral("我的小组件");
    auto block = [](const QString& type, const QString& text, int size) {
        return QJsonObject{{"type", type}, {"text", text}, {"size", size}, {"align", "left"}, {"value", 50}};
    };
    if (index == 1) {
        result.name = QStringLiteral("每日时光");
        result.blocks = {block("text", QStringLiteral("把时间留给重要的事"), 14), block("clock", "", 42), block("date", "", 14)};
    } else if (index == 2) {
        result.name = QStringLiteral("目标进度");
        result.blocks = {block("text", QStringLiteral("本周阅读"), 18), block("metric", QStringLiteral("3 / 5 本"), 32), block("progress", QStringLiteral("完成进度"), 14)};
        result.blocks.last().insert("value", 60);
    } else if (index == 3) {
        result.name = QStringLiteral("HTML 计数器");
        result.mode = "html";
        result.html = QStringLiteral("<main><small>保持专注</small><h1 id=\"count\">0</h1><button id=\"add\">完成一次 +1</button></main>");
        result.css = QStringLiteral("body{margin:0;background:#18243b;color:#f4f7ff;font-family:system-ui}main{padding:24px}h1{font-size:48px;margin:8px 0}button{border:0;border-radius:12px;padding:10px 16px;background:#78aaff;color:#101a2c;cursor:pointer}");
        result.javascript = QStringLiteral("let count=0;document.getElementById('add').onclick=()=>{document.getElementById('count').textContent=++count;};");
    } else {
        result.blocks = {block("text", QStringLiteral("今日提醒"), 18), block("text", QStringLiteral("写下你想在桌面看到的内容。"), 22)};
    }
    return result;
}
QString document(const Definition& definition) {
    if (definition.mode == "html") {
        QString script = definition.javascript;
        script.replace("</script", "<\\/script", Qt::CaseInsensitive);
        return QStringLiteral("<!doctype html><html><head><meta charset=utf-8><meta name=viewport content=\"width=device-width,initial-scale=1\"><style>html,body{min-height:100%;box-sizing:border-box}*{box-sizing:border-box}%1</style></head><body>%2<script>%3</script></body></html>")
            .arg(definition.css, definition.html, script);
    }
    QString body;
    for (const auto& block : definition.blocks) {
        const QString type = block.value("type").toString();
        const QString style = QStringLiteral("font-size:%1px;text-align:%2;font-weight:%3;")
            .arg(block.value("size").toInt(18)).arg(block.value("align").toString("left"))
            .arg(block.value("bold").toBool(type == "metric") ? 650 : 400);
        const QString text = block.value("text").toString().toHtmlEscaped();
        if (type == "divider") body += "<hr>";
        else if (type == "clock" || type == "date")
            body += QStringLiteral("<div data-live='%1' style='%2'></div>").arg(type, style);
        else if (type == "progress")
            body += QStringLiteral("<div style='%1'><div>%2 <span>%3%</span></div><progress max=100 value=%3></progress></div>")
                .arg(style, text).arg(block.value("value").toInt(50));
        else body += QStringLiteral("<div style='%1;white-space:pre-wrap;overflow-wrap:anywhere'>%2</div>").arg(style, text);
    }
    return QStringLiteral("<!doctype html><html lang=zh-CN><meta charset=utf-8><meta name=viewport content='width=device-width,initial-scale=1'><style>"
        "*{box-sizing:border-box}body{margin:0;padding:%1px;background:%2;color:%3;font-family:'Microsoft YaHei',system-ui;display:flex;flex-direction:column;gap:10px}"
        "hr{width:100%;border:0;border-top:1px solid %3;opacity:.2}progress{width:100%;height:9px;accent-color:%4}span{float:right;opacity:.7}"
        "</style><body>%5<script>function tick(){document.querySelectorAll('[data-live]').forEach(e=>{const d=new Date();e.textContent=e.dataset.live==='clock'?d.toLocaleTimeString('zh-CN',{hour12:false}):d.toLocaleDateString('zh-CN',{year:'numeric',month:'long',day:'numeric',weekday:'long'})})}tick();setInterval(tick,1000)</script></body></html>")
        .arg(definition.padding).arg(color(definition.background), color(definition.foreground), color(definition.accent), body);
}
QString sandboxedDocument(const Definition& definition) {
    const QString network = definition.allowNetwork ? QStringLiteral("https: http:") : QStringLiteral("'none'");
    const QString assets = definition.allowNetwork ? QStringLiteral("data: https: http:") : QStringLiteral("data:");
    const QString scripts = definition.allowNetwork ? QStringLiteral("'unsafe-inline' https: http:") : QStringLiteral("'unsafe-inline'");
    const QString policy = QStringLiteral("default-src 'none'; script-src %3; style-src %3; img-src %1; font-src %1; media-src %1; connect-src %2; frame-src about:; object-src 'none'; base-uri 'none'; form-action 'none'").arg(assets, network, scripts);
    return QStringLiteral("<!doctype html><meta charset=utf-8><meta http-equiv='Content-Security-Policy' content=\"%1\"><style>html,body{margin:0;width:100%;height:100%;overflow:hidden;background:transparent}iframe{width:100%;height:100%;border:0;border-radius:20px}</style><iframe title=\"%2\" sandbox=\"allow-scripts\" srcdoc=\"%3\"></iframe>")
        .arg(policy.toHtmlEscaped(), definition.name.toHtmlEscaped(), document(definition).toHtmlEscaped());
}
bool importFile(const QString& path, Definition& result, QString* error) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size() > MaxFileBytes)
        return fail(error, QStringLiteral("无法读取文件，或文件超过 1 MB。"));
    const auto bytes = file.readAll();
    if (QFileInfo(path).suffix().compare("html", Qt::CaseInsensitive) == 0
        || QFileInfo(path).suffix().compare("htm", Qt::CaseInsensitive) == 0) {
        result = makeTemplate(3);
        result.name = QFileInfo(path).completeBaseName().left(80);
        result.html = QString::fromUtf8(bytes);
        result.css.clear(); result.javascript.clear();
        return fromJson(result.toJson(), result, error);
    }
    QJsonParseError parseError;
    const auto json = QJsonDocument::fromJson(bytes, &parseError);
    if (!json.isObject() || json.object().value("format").toString() != "macdows-widget"
        || json.object().value("version").toInt() != 1
        || !fromJson(json.object().value("definition").toObject(), result, error))
        return fail(error, QStringLiteral("组件文件无效或版本不支持。"));
    result.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    return true;
}
bool exportFile(const QString& path, const Definition& definition, QString* error) {
    Definition checked;
    if (!fromJson(definition.toJson(), checked, error)) return false;
    const QByteArray bytes = QJsonDocument(QJsonObject{{"format", "macdows-widget"}, {"version", 1}, {"definition", checked.toJson()}}).toJson();
    if (bytes.size() > MaxFileBytes) return fail(error, QStringLiteral("导出文件超过 1 MB，请减少内容。"));
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit())
        return fail(error, QStringLiteral("导出失败：") + file.errorString());
    return true;
}
}
