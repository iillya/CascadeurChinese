#pragma once
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QSaveFile>
#include <QtCore/QStandardPaths>
#include <QtGui/QKeySequence>

namespace CascadeurHotkeyConfig {
constexpr int defaultKey = 0x72; // VK_F3
inline int toVirtualKey(int key)
{
    if (key >= Qt::Key_F1 && key <= Qt::Key_F24) return 0x70 + key - Qt::Key_F1;
    if ((key >= Qt::Key_A && key <= Qt::Key_Z) || (key >= Qt::Key_0 && key <= Qt::Key_9)) return key;
    switch (key) {
    case Qt::Key_Space: return 0x20;
    case Qt::Key_Tab: return 0x09;
    case Qt::Key_Backspace: return 0x08;
    case Qt::Key_Insert: return 0x2d;
    case Qt::Key_Delete: return 0x2e;
    case Qt::Key_Home: return 0x24;
    case Qt::Key_End: return 0x23;
    case Qt::Key_PageUp: return 0x21;
    case Qt::Key_PageDown: return 0x22;
    case Qt::Key_Left: return 0x25;
    case Qt::Key_Up: return 0x26;
    case Qt::Key_Right: return 0x27;
    case Qt::Key_Down: return 0x28;
    default: return 0;
    }
}
inline int toQtKey(int vk)
{
    if (vk >= 0x70 && vk <= 0x87) return Qt::Key_F1 + vk - 0x70;
    if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) return vk;
    const int keys[] = {Qt::Key_Space, Qt::Key_Tab, Qt::Key_Backspace, Qt::Key_Insert,
        Qt::Key_Delete, Qt::Key_Home, Qt::Key_End, Qt::Key_PageUp, Qt::Key_PageDown,
        Qt::Key_Left, Qt::Key_Up, Qt::Key_Right, Qt::Key_Down};
    for (int key : keys) if (toVirtualKey(key) == vk) return key;
    return 0;
}
inline bool valid(int vk) { return toQtKey(vk) != 0; }
inline QString name(int vk) { return QKeySequence(toQtKey(vk)).toString(QKeySequence::NativeText); }
inline QString path()
{
    const QString base = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);
    return base.isEmpty() ? QString() : base + QStringLiteral("/CascadeurChinese/settings.json");
}
inline int load(const QString& filePath, QString* error = nullptr)
{
    if (error) error->clear();
    if (filePath.isEmpty()) {
        if (error) *error = QStringLiteral("无法定位快捷键配置目录，已使用 F3。");
        return defaultKey;
    }
    QFile file(filePath);
    if (!file.exists()) return defaultKey;
    QJsonParseError parse;
    if (!file.open(QIODevice::ReadOnly)) { if (error) *error = file.errorString(); return defaultKey; }
    const auto doc = QJsonDocument::fromJson(file.read(4097), &parse);
    const auto value = doc.object().value(QStringLiteral("toggleVirtualKey"));
    const int vk = value.toInt(-1);
    if (file.size() > 4096 || parse.error != QJsonParseError::NoError || !doc.isObject() ||
        doc.object().value(QStringLiteral("version")).toDouble(-1) != 1 ||
        !value.isDouble() || value.toDouble() != vk || !valid(vk)) {
        if (error) *error = QStringLiteral("快捷键配置无效，已使用 F3。");
        return defaultKey;
    }
    return vk;
}
inline bool save(const QString& filePath, int vk, QString* error = nullptr)
{
    if (error) error->clear();
    if (filePath.isEmpty() || !valid(vk)) { if (error) *error = QStringLiteral("无效的配置路径或按键。"); return false; }
    const QFileInfo info(filePath);
    if (!QDir().mkpath(info.absolutePath())) { if (error) *error = QStringLiteral("无法创建配置目录。"); return false; }
    const QJsonObject object{{QStringLiteral("version"), 1}, {QStringLiteral("toggleVirtualKey"), vk}};
    const QByteArray data = QJsonDocument(object).toJson();
    QSaveFile file(filePath);
    if (!file.open(QIODevice::WriteOnly) || file.write(data) != data.size() || !file.commit()) {
        if (error) *error = file.errorString();
        return false;
    }
    return true;
}
}
