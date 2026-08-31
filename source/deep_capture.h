#pragma once
// Explicit UI automation helper, not the read-only translation/capture core.
// Adapter verified against Cascadeur 2026.1.2 embedded PropertyEditor QML.
#include <QtCore/QElapsedTimer>
#include <QtCore/QMetaProperty>
#include <QtCore/QPointer>
#include <QtCore/QSet>
#include <QtCore/QTimer>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QSaveFile>
#include <QtCore/QDir>
#include <QtGui/QGuiApplication>
#include <QtQuick/QQuickItem>
#include <QtQuick/QQuickWindow>
#include <QtQml/QQmlContext>
#include <QtQml/QQmlExpression>
#include <QtQml/qqml.h>
#include <functional>
#include <utility>
#include <windows.h>

class DeepCapture final : public QObject {
    struct Header {
        QPointer<QQuickItem> item;
        QPointer<QObject> editor;
        QString name;
        int component = 0;
        bool expanded = false;
    };
    struct Scroll {
        QPointer<QQuickItem> item;
        QVariant x, y;
    };
    QPointer<QWindow> window_;
    QList<Header> changed_;
    QList<Scroll> scrolls_;
    QSet<QString> entries_;
    QTimer timer_;
    QElapsedTimer elapsed_;
    std::function<QSet<QString>()> collect_;
    std::function<void(const QSet<QString>&)> save_;
    bool active_ = false, restoring_ = false;
    int restored_ = 0, failed_ = 0, opened_ = 0, restoreIndex_ = -1;
    QString reason_;

    static bool matches(const Header& h) {
        return h.item && h.editor && h.item->window() &&
            h.item->property("headerText").toString() == h.name &&
            h.item->property("componentIndex").toInt() == h.component;
    }
    static bool toggle(const Header& h) {
        if (!matches(h)) return false;
        QQmlContext* context = qmlContext(h.item);
        if (!context || context->contextProperty("_propertyEditorView").value<QObject*>() != h.editor)
            return false;
        // Same operation as the verified header MouseArea; no property writes,
        // no switchPin, no forceActiveFocus, no business-model setData calls.
        QQmlExpression expression(context, h.item,
            QStringLiteral("_propertyEditorView.switchExpand(headerText, componentIndex)"));
        expression.evaluate();
        return !expression.hasError();
    }
    QList<Header> headers() {
        QList<Header> result;
        auto* quick = qobject_cast<QQuickWindow*>(window_.data());
        if (!quick) return result;
        QSet<QQuickItem*> visited;
        std::function<void(QQuickItem*, int)> visit = [&](QQuickItem* item, int depth) {
            if (!item || depth > 64 || visited.size() >= 20000 || visited.contains(item)) return;
            visited.insert(item);
            if (item->inherits("QQuickTextInput") || item->inherits("QQuickTextEdit")) return;
            QQmlContext* context = qmlContext(item);
            QObject* editor = context ? context->contextProperty("_propertyEditorView").value<QObject*>() : nullptr;
            if (item->isVisible() && item->isEnabled() && editor &&
                QByteArray(editor->metaObject()->className()) == "view::PropertyEditor" &&
                item->metaObject()->indexOfSignal("headerClicked()") >= 0 &&
                item->property("headerText").metaType().id() == QMetaType::QString &&
                item->property("isExpand").metaType().id() == QMetaType::Bool &&
                item->property("isPin").metaType().id() == QMetaType::Bool &&
                item->property("componentIndex").metaType().id() == QMetaType::Int) {
                result.append({item, editor, item->property("headerText").toString(),
                               item->property("componentIndex").toInt(), item->property("isExpand").toBool()});
                for (auto* parent = item->parentItem(); parent; parent = parent->parentItem()) {
                    if (!parent->inherits("QQuickFlickable")) continue;
                    bool known = false;
                    for (const auto& scroll : scrolls_) if (scroll.item == parent) known = true;
                    if (!known) scrolls_.append({parent, parent->property("contentX"), parent->property("contentY")});
                }
            }
            for (auto* child : item->childItems()) visit(child, depth + 1);
        };
        visit(quick->contentItem(), 0);
        return result;
    }
    void finish() {
        timer_.stop();
        for (const auto& scroll : scrolls_) if (scroll.item) {
            scroll.item->setProperty("contentX", scroll.x);
            scroll.item->setProperty("contentY", scroll.y);
        }
        active_ = false;
        QJsonObject report{{QStringLiteral("reason"), reason_},
            {QStringLiteral("opened"), opened_}, {QStringLiteral("restored"), restored_},
            {QStringLiteral("restoreFailures"), failed_}, {QStringLiteral("captured"), entries_.size()}};
        QSaveFile file(QDir::tempPath() + QStringLiteral("/Cascadeur_deep_capture.json"));
        if (file.open(QIODevice::WriteOnly)) {
            file.write(QJsonDocument(report).toJson()); file.commit();
        }
        // Release the completed snapshot; no candidate pool is retained in
        // the background until the user's next capture request.
        const QSet<QString> captured = std::move(entries_);
        changed_.clear();
        scrolls_.clear();
        window_.clear();
        save_(captured);
        if (failed_ || opened_ == 0) {
            MessageBoxW(nullptr, failed_ ? L"部分分组已销毁或改变，无法全部恢复。请检查属性面板。"
                : L"未找到符合已验证结构的折叠属性分组；仅完成只读采集。请先选中对象。",
                L"深度采集", MB_OK | MB_ICONINFORMATION);
        }
    }
    void step() {
        if (!active_) return;
        if (restoring_) {
            if (restoreIndex_ >= 0) {
                const Header h = changed_[restoreIndex_--];
                if (!matches(h)) { ++failed_; return; }
                if (h.item->property("isExpand").toBool() == h.expanded ||
                    (toggle(h) && h.item && h.item->property("isExpand").toBool() == h.expanded)) ++restored_;
                else ++failed_;
                return;
            }
            finish(); return;
        }
        if (!window_ || !window_->isVisible()) { cancel(QStringLiteral("window unavailable")); return; }
        if (QGuiApplication::applicationState() != Qt::ApplicationActive) {
            cancel(QStringLiteral("application deactivated")); return;
        }
        if (elapsed_.elapsed() > 60000 || opened_ >= 128) { cancel(QStringLiteral("limit reached")); return; }
        // If a header is recycled for a different selection, never touch it again.
        for (const auto& h : changed_) if (!matches(h)) { cancel(QStringLiteral("selection/layout changed")); return; }
        entries_.unite(collect_());
        for (const auto& h : headers()) {
            bool processed = false;
            for (const auto& old : changed_)
                if (old.editor == h.editor && old.name == h.name && old.component == h.component) processed = true;
            if (h.expanded || processed) continue;
            changed_.append(h);
            if (!toggle(h)) { cancel(QStringLiteral("adapter invocation failed")); return; }
            ++opened_;
            return; // Allow event loop and lazy delegates to settle before capture.
        }
        cancel(QStringLiteral("completed"));
    }
public:
    DeepCapture(QCoreApplication* parent, std::function<QSet<QString>()> collect,
                std::function<void(const QSet<QString>&)> save)
        : QObject(parent), collect_(std::move(collect)), save_(std::move(save)) {
        timer_.setInterval(300);
        QObject::connect(&timer_, &QTimer::timeout, this, [this] { step(); });
        QObject::connect(parent, &QCoreApplication::aboutToQuit, this, [this] {
            timer_.stop(); active_ = false; // Never call dying host objects.
        });
    }
    bool active() const { return active_; }
    bool eventFilter(QObject*, QEvent* event) override {
        if (!active_) return false;
        // Temporarily block user edits/selection while restoring UI state.
        // Shortcut handling itself remains in the native Windows filter.
        switch (event->type()) {
        case QEvent::MouseButtonPress: case QEvent::MouseButtonRelease:
        case QEvent::MouseButtonDblClick: case QEvent::Wheel:
        case QEvent::TouchBegin: case QEvent::TouchUpdate: case QEvent::TouchEnd:
        case QEvent::TabletPress: case QEvent::TabletMove: case QEvent::TabletRelease:
        case QEvent::KeyPress: case QEvent::KeyRelease: case QEvent::Shortcut:
        case QEvent::InputMethod: case QEvent::Drop:
            return true;
        default: return false;
        }
    }
    void cancel(const QString& reason = QStringLiteral("cancelled by user")) {
        if (!active_ || restoring_) return;
        reason_ = reason; restoring_ = true; restoreIndex_ = int(changed_.size()) - 1;
    }
    void start() {
        if (active_) return;
        window_ = QGuiApplication::focusWindow();
        if (!qobject_cast<QQuickWindow*>(window_.data())) { save_(collect_()); return; }
        changed_.clear(); scrolls_.clear(); entries_.clear();
        opened_ = restored_ = failed_ = 0; restoring_ = false;
        active_ = true; elapsed_.start(); timer_.start();
    }
};
