#pragma once

#include <QtCore/QPointer>
#include <QtCore/QSignalMapper>
#include <QtQml/QQmlComponent>
#include <QtQml/QQmlEngine>
#include <QtQuick/QQuickWindow>
#include <functional>
#include <memory>
#include <QtGui/QKeyEvent>
#include "hotkey_config.h"

namespace CascadeurHotkeySettings {

class Recorder final : public QObject {
    QPointer<QQuickWindow> window_;
    int heldKey_ = 0;
public:
    explicit Recorder(QQuickWindow* window) : QObject(window), window_(window) {
        window->installEventFilter(this);
    }
    bool eventFilter(QObject*, QEvent* event) override {
        if (!window_) return false;
        if (event->type() == QEvent::FocusOut || event->type() == QEvent::WindowDeactivate) {
            heldKey_ = 0;
            window_->setProperty("listening", false);
        }
        if (event->type() != QEvent::KeyPress && event->type() != QEvent::KeyRelease &&
            event->type() != QEvent::ShortcutOverride) return false;
        auto* key = static_cast<QKeyEvent*>(event);
        const bool listening = window_->property("listening").toBool();
        if (!listening && key->key() != heldKey_) return false;
        if (event->type() == QEvent::ShortcutOverride) { event->accept(); return true; }
        if (event->type() == QEvent::KeyRelease) {
            if (!key->isAutoRepeat() && key->key() == heldKey_) heldKey_ = 0;
            return true;
        }
        if (key->isAutoRepeat()) return true;
        if (!listening) return true;
        heldKey_ = key->key();
        if (key->key() == Qt::Key_Escape) {
            window_->setProperty("listening", false);
            return true;
        }
        const int vk = CascadeurHotkeyConfig::toVirtualKey(key->key());
        if (vk && key->modifiers() == Qt::NoModifier) {
            window_->setProperty("pendingVirtualKey", vk);
            window_->setProperty("pendingKeyName", CascadeurHotkeyConfig::name(vk));
            window_->setProperty("errorText", QString());
            window_->setProperty("listening", false);
        } else {
            window_->setProperty("errorText", QStringLiteral("请按单个字母、数字、功能键或导航键，不使用组合键。"));
        }
        return true;
    }
};

// Plugin-owned Qt Quick window: works with QGuiApplication, no QWidget or
// Comctl32 activation context required. The host's objects remain untouched.
inline QQuickWindow* create(QQmlEngine* engine, QWindow* parentWindow,
                           int initialVirtualKey,
                           std::function<bool(int, QString*)> accepted,
                           std::function<void()> closed,
                           QString* errorMessage = nullptr)
{
    if (errorMessage) errorMessage->clear();
    if (!engine || !accepted || !closed || !CascadeurHotkeyConfig::valid(initialVirtualKey)) {
        if (errorMessage) *errorMessage = QStringLiteral("无有效的 Qt 引擎或快捷键参数。");
        return nullptr;
    }
    static const QByteArray qml = R"QML(
import QtQuick 2.15
import QtQuick.Window 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15

Window {
    id: settings
    objectName: "cascadeur_chinese_hotkey_settings"
    title: "中英切换快捷键"
    width: 360
    height: minimumHeight
    minimumWidth: 360
    minimumHeight: Math.ceil(contentLayout.implicitHeight + 32)
    color: "#303030"
    flags: Qt.Dialog | Qt.WindowTitleHint | Qt.WindowCloseButtonHint
    modality: Qt.WindowModal
    visible: false
    property int pendingVirtualKey: 114
    property string pendingKeyName: "F3"
    property bool listening: false
    property string errorText: ""
    signal acceptRequested()

    component ActionButton: Button {
        id: control
        property bool primary: false
        implicitHeight: 30
        implicitWidth: Math.max(68, contentItem.implicitWidth + 24)
        font.family: "Microsoft YaHei UI"
        font.pixelSize: 13
        hoverEnabled: true
        contentItem: Text {
            text: control.text
            font: control.font
            color: !control.enabled ? "#888888" : "#e6e6e6"
            horizontalAlignment: Text.AlignHCenter
            verticalAlignment: Text.AlignVCenter
        }
        background: Rectangle {
            radius: 4
            color: !control.enabled ? "#353535" : (control.down ? "#333333" : control.hovered ? "#4c4c4c" : "#3f3f3f")
            border.width: 1
            border.color: control.activeFocus ? "#66aaff" : control.primary ? "#66aaff" : "#565656"
        }
    }

    Shortcut { sequence: "Escape"; enabled: !settings.listening; onActivated: settings.close() }

    ColumnLayout {
        id: contentLayout
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.margins: 16
        spacing: 10
        Text {
            text: "点击下方按钮，然后按下新的快捷键。"
            color: "#e6e6e6"
            font.family: "Microsoft YaHei UI"
            font.pixelSize: 13
        }
        ActionButton {
            id: captureButton
            objectName: "hotkeyCapture"
            Layout.fillWidth: true
            Layout.preferredHeight: 30
            font.pixelSize: 13
            text: settings.listening ? "等待按键…" : "当前：" + settings.pendingKeyName
            onClicked: { settings.errorText = ""; settings.listening = true; forceActiveFocus() }
        }
        Text {
            Layout.fillWidth: true
            text: "按 Esc 键取消录入。单击“确定”保存，下次启动时继续使用。"
            color: "#bdbdbd"
            wrapMode: Text.WordWrap
            font.family: "Microsoft YaHei UI"
            font.pixelSize: 12
            lineHeight: 1.4
        }
        Text {
            Layout.fillWidth: true
            text: "按 Shift + ~ 可嗅探未翻译的 UI 文本。"
            color: "#bdbdbd"
            wrapMode: Text.WordWrap
            font.family: "Microsoft YaHei UI"
            font.pixelSize: 12
        }
        Text { Layout.fillWidth: true; text: settings.errorText; color: "#ffb4a8"; font.family: "Microsoft YaHei UI"; font.pixelSize: 12; wrapMode: Text.WordWrap; visible: text.length > 0 }
        RowLayout {
            Layout.fillWidth: true
            spacing: 8
            ActionButton {
                objectName: "hotkeyReset"
                text: "重置"
                onClicked: { settings.listening = false; settings.pendingVirtualKey = 114; settings.pendingKeyName = "F3"; settings.errorText = "" }
            }
            Item { Layout.fillWidth: true }
            ActionButton {
                objectName: "hotkeyCancel"
                text: "取消"
                onClicked: settings.close()
            }
            ActionButton {
                objectName: "hotkeyAccept"
                text: "确定"
                primary: true
                enabled: !settings.listening
                onClicked: settings.acceptRequested()
            }
        }
    }
}
)QML";
    QQmlComponent component(engine);
    component.setData(qml, QUrl(QStringLiteral("qrc:/cascadeur-chinese/hotkey-settings.qml")));
    QObject* object = component.createWithInitialProperties(
        {{QStringLiteral("pendingVirtualKey"), initialVirtualKey},
         {QStringLiteral("pendingKeyName"), CascadeurHotkeyConfig::name(initialVirtualKey)}}, engine->rootContext());
    auto* window = qobject_cast<QQuickWindow*>(object);
    if (!window) {
        if (errorMessage) *errorMessage = component.errorString();
        delete object;
        return nullptr;
    }
    window->QObject::setParent(engine);
    new Recorder(window);
    window->setTransientParent(parentWindow);
    if (parentWindow) {
        window->setPosition(parentWindow->x() + (parentWindow->width() - window->width()) / 2,
                            parentWindow->y() + (parentWindow->height() - window->height()) / 2);
    }
    const QPointer<QQuickWindow> guard(window);
    auto* mapper = new QSignalMapper(window);
    mapper->setMapping(window, 0);
    QObject::connect(window, SIGNAL(acceptRequested()), mapper, SLOT(map()));
    QObject::connect(mapper, &QSignalMapper::mappedInt, window,
                     [guard, accepted = std::move(accepted)](int) {
        if (!guard || !guard->isVisible()) return;
        const int key = guard->property("pendingVirtualKey").toInt();
        if (!CascadeurHotkeyConfig::valid(key) || guard->property("listening").toBool()) return;
        QString error;
        if (!accepted(key, &error)) {
            if (guard) guard->setProperty("errorText", QStringLiteral("保存失败：") + error);
            return;
        }
        if (guard) guard->close();
    });
    // Closing, Cancel, Escape, and engine destruction all release the shortcut
    // suppression exactly once. Defer destruction until event dispatch finishes.
    const auto notified = std::make_shared<bool>(false);
    auto notifyClosed = [notified, closed = std::move(closed)] {
        if (*notified) return;
        *notified = true;
        closed();
    };
    QObject::connect(window, &QWindow::visibleChanged, window,
                     [guard, notifyClosed](bool visible) {
        if (!visible) {
            notifyClosed();
            if (guard) guard->deleteLater();
        }
    });
    QObject::connect(window, &QObject::destroyed, engine, notifyClosed);
    return window;
}
}
