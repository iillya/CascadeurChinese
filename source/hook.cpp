// Cascadeur Chinese Localizer - pure display-layer Qt Quick hook.
// Translation leaves QML properties and data models untouched and is applied to
// a temporary QTextLayout copy only while scene-graph glyphs are generated.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <tlhelp32.h>

#include "hook_lifecycle.h"
#include "qt_compatibility.h"
#include "translation_policy.h"
#include "scene_text_policy.h"
#include "numeric_templates.h"
#include "hotkey_settings.h"
#include <shlobj.h>
#include <QtCore/QAbstractEventDispatcher>
#include <QtCore/QAbstractItemModel>
#include <QtCore/QAbstractNativeEventFilter>
#include <QtCore/QCoreApplication>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonObject>
#include <QtCore/QObject>
#include <QtCore/QPointer>
#include <QtCore/QSaveFile>
#include <QtCore/QSet>
#include <QtCore/QSignalMapper>
#include <QtCore/QTimer>
#include <QtCore/QThread>
#include <QtGui/QGuiApplication>
#include <QtGui/QFontMetrics>
#include <QtGui/QTextLayout>
#include <QtGui/QWindow>
#include <QtQml/QQmlComponent>
#include <QtQml/QQmlContext>
#include <QtQml/QQmlEngine>
#include <QtQml/qqml.h>
#include <QtQuick/QQuickItem>
#include <QtQuick/QQuickWindow>
#include "detours/detours.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>
#include "deep_capture.h"

extern "C" __declspec(dllexport) int __cdecl cascadeur_localizer_api_version() { return 1; }
extern "C" __declspec(dllexport) const char* __cdecl cascadeur_localizer_version() { return "0.2.1"; }

namespace {
HMODULE g_self = nullptr;
std::atomic_bool g_enabled{true};
std::atomic_uint g_toggleVk{VK_F3};
bool g_settingsOpen = false; // GUI thread only.
QPointer<QQuickWindow> g_hotkeySettingsWindow;
QPointer<DeepCapture> g_deepCapture;
thread_local bool g_inDisplayHook = false;
struct DisplayHookGuard {
    const bool previous = g_inDisplayHook;
    DisplayHookGuard() { g_inDisplayHook = true; }
    ~DisplayHookGuard() { g_inDisplayHook = previous; }
};
struct DictionarySnapshot {
    QVector<NumericTemplates::Rule> numeric;
    std::unordered_map<std::string, std::string> exact;
    std::unordered_map<std::string, std::string> folded;
};
std::shared_ptr<const DictionarySnapshot> g_dictionary =
    std::make_shared<const DictionarySnapshot>();
std::atomic_uint64_t g_dictionaryGeneration{1};
void publishDictionary(std::shared_ptr<const DictionarySnapshot> snapshot)
{
    std::atomic_store_explicit(&g_dictionary, std::move(snapshot), std::memory_order_release);
    g_dictionaryGeneration.fetch_add(1, std::memory_order_release);
}

const DictionarySnapshot& currentDictionary()
{
    // shared_ptr's atomic load may acquire an internal lock. Acquire ownership
    // only on publication changes, never once per glyph/measurement query.
    thread_local uint64_t generation = 0;
    thread_local std::shared_ptr<const DictionarySnapshot> snapshot;
    const uint64_t current = g_dictionaryGeneration.load(std::memory_order_acquire);
    if (generation != current) {
        snapshot = std::atomic_load_explicit(&g_dictionary, std::memory_order_acquire);
        generation = current;
    }
    return *snapshot;
}
QStringList g_dictionaryNotes;
// Fixed diagnostic samples, not an untranslated-text capture. Rendering only
// updates atomic fields; JSON is written later on the GUI thread.
struct MenuPaintProbe {
    const char* source;
    std::atomic_int requestedLines{-2};
    std::atomic_int layoutLines{0};
    std::atomic_bool translated{false};
};
MenuPaintProbe g_menuPaintProbes[] = {{"File"}, {"Commands"}, {"Synchronization"}};
std::atomic_bool g_menuProbesActive{true};

using AddTextLayoutFn = void (__fastcall *)(
    void*, const QPointF&, QTextLayout*, const QColor&, int,
    const QColor&, const QColor&, const QColor&, const QColor&,
    int, int, int, int);
AddTextLayoutFn g_originalAddTextLayout = nullptr;
using TextNodeCtorFn = void* (__fastcall *)(void*, QQuickItem*);
using TextNodeDtorFn = void (__fastcall *)(void*);
TextNodeCtorFn g_textNodeCtor = nullptr;
TextNodeDtorFn g_textNodeDtor = nullptr;
std::mutex g_textNodeMutex;
std::unordered_map<void*, bool> g_textNodePreserve;
std::atomic_bool g_textNodeTrackingReady{false};

void* __fastcall hookedTextNodeCtor(void* node, QQuickItem* owner)
{
    void* result = g_textNodeCtor(node, owner);
    try {
        const bool preserve = !owner || CascadeurSceneTextPolicy::preserve(owner);
        std::lock_guard<std::mutex> lock(g_textNodeMutex);
        g_textNodePreserve[node] = preserve;
    } catch (...) {} // unregistered nodes are always preserved
    return result;
}

void __fastcall hookedTextNodeDtor(void* node)
{
    {
        std::lock_guard<std::mutex> lock(g_textNodeMutex);
        g_textNodePreserve.erase(node);
    }
    g_textNodeDtor(node);
}

bool preserveTextNode(void* node)
{
    if (!g_textNodeTrackingReady.load(std::memory_order_acquire)) return true;
    std::lock_guard<std::mutex> lock(g_textNodeMutex);
    const auto found = g_textNodePreserve.find(node);
    return found == g_textNodePreserve.end() || found->second;
}
using FontAdvanceFn = int (__fastcall *)(const QFontMetrics*, const QString&, int);
using FontAdvanceOptFn = int (__fastcall *)(const QFontMetrics*, const QString&, const QTextOption&);
using FontAdvanceFFn = qreal (__fastcall *)(const QFontMetricsF*, const QString&, int);
using FontAdvanceFOptFn = qreal (__fastcall *)(const QFontMetricsF*, const QString&, const QTextOption&);
FontAdvanceFn g_fontAdvance = nullptr;
FontAdvanceOptFn g_fontAdvanceOpt = nullptr;
FontAdvanceFFn g_fontAdvanceF = nullptr;
FontAdvanceFOptFn g_fontAdvanceFOpt = nullptr;

std::wstring selfDirectory()
{
    std::vector<wchar_t> buffer(1024);
    while (buffer.size() <= 32768) {
        const DWORD n = GetModuleFileNameW(g_self, buffer.data(), DWORD(buffer.size()));
        if (!n) return {};
        if (n < buffer.size() - 1) {
            std::wstring path(buffer.data(), n);
            const size_t slash = path.find_last_of(L"\\/");
            return slash == std::wstring::npos ? std::wstring() : path.substr(0, slash);
        }
        buffer.resize(buffer.size() * 2);
    }
    return {};
}

using CascadeurTranslationPolicy::normalize;
using CascadeurTranslationPolicy::looksTranslatable;

void loadDictionaries()
{
    std::unordered_map<std::string, std::string> exact, folded;
    g_dictionaryNotes.clear();
    QDir dir(QString::fromStdWString(selfDirectory()) + QLatin1String("/translations"));
    for (const QString& name : dir.entryList({QStringLiteral("*_zh.json")}, QDir::Files, QDir::Name)) {
        QFile file(dir.filePath(name));
        if (!file.open(QIODevice::ReadOnly)) {
            g_dictionaryNotes.append(name + QStringLiteral(": cannot open"));
            continue;
        }
        if (file.size() > 16 * 1024 * 1024) {
            g_dictionaryNotes.append(name + QStringLiteral(": exceeds 16 MiB limit"));
            continue;
        }
        QJsonParseError error;
        const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &error);
        if (error.error != QJsonParseError::NoError || !document.isObject() ||
            !document.object().value(QLatin1String("translations")).isObject()) {
            g_dictionaryNotes.append(name + QStringLiteral(": invalid dictionary"));
            continue;
        }
        const QJsonObject map = document.object().value(QLatin1String("translations")).toObject();
        for (auto it = map.constBegin(); it != map.constEnd(); ++it) {
            if (it.key().trimmed().isEmpty() || it.key().size() > 500 ||
                !it.value().isString() || it.value().toString().trimmed().isEmpty() ||
                it.value().toString().size() > 10000) continue;
            const std::string value = it.value().toString().toUtf8().toStdString();
            exact[it.key().toUtf8().toStdString()] = value;
            const std::string key = normalize(it.key()).toUtf8().toStdString();
            if (key.empty()) continue;
            const auto existing = folded.find(key);
            if (existing != folded.end() && existing->second != value)
                g_dictionaryNotes.append(name + QStringLiteral(": normalized conflict: ") + it.key());
            folded[key] = value;
        }
    }
    auto snapshot = std::make_shared<DictionarySnapshot>();
    snapshot->exact = std::move(exact);
    snapshot->folded = std::move(folded);
    QFile templates(dir.filePath(QStringLiteral("numeric_templates.json")));
    if (templates.open(QIODevice::ReadOnly)) {
        QJsonParseError error;
        const auto doc = QJsonDocument::fromJson(templates.read(65537), &error);
        const auto sources = doc.object().value(QStringLiteral("sources"));
        if (templates.size() > 65536 || error.error != QJsonParseError::NoError ||
            !sources.isArray() || sources.toArray().size() > 64) {
            g_dictionaryNotes.append(QStringLiteral("numeric_templates.json: invalid configuration or limit exceeded"));
        } else {
            QSet<QString> seen;
            for (const auto& entry : sources.toArray()) {
                const QString source = entry.toString();
                const auto found = snapshot->exact.find(source.toUtf8().toStdString());
                NumericTemplates::Rule rule;
                if (seen.contains(source)) continue;
                seen.insert(source);
                if (!entry.isString() || found == snapshot->exact.end() ||
                    !NumericTemplates::compile(source, QString::fromUtf8(found->second), rule)) {
                    g_dictionaryNotes.append(QStringLiteral("numeric template rejected: ") + source);
                    continue;
                }
                snapshot->numeric.append(rule);
            }
        }
    }
    publishDictionary(std::move(snapshot));
}

QString lookupDictionary(const QString& raw, bool allowNumeric = false)
{
    QString key = raw.trimmed();
    if (!looksTranslatable(key)) return {};
    const auto& dictionary = currentDictionary();
    auto it = dictionary.exact.find(key.toUtf8().toStdString());
    if (it != dictionary.exact.end()) return QString::fromUtf8(it->second);
    QString suffix;
    if (key.endsWith(QLatin1Char('*'))) { key.chop(1); key = key.trimmed(); suffix = QLatin1Char('*'); }
    key.remove(QLatin1Char('&'));
    it = dictionary.exact.find(key.toUtf8().toStdString());
    if (it != dictionary.exact.end()) return QString::fromUtf8(it->second) + suffix;
    auto folded = dictionary.folded.find(normalize(key).toUtf8().toStdString());
    if (folded != dictionary.folded.end()) return QString::fromUtf8(folded->second) + suffix;
    // Templates match the original complete text, never normalized/elided fragments.
    return allowNumeric ? NumericTemplates::lookup(dictionary.numeric, raw) : QString();
}

QString translateText(const QString& raw, bool allowNumeric = false)
{
    if (!g_enabled.load(std::memory_order_acquire)) return {};
    return lookupDictionary(raw, allowNumeric);
}

int __fastcall hookedFontAdvance(const QFontMetrics* metrics, const QString& text, int length)
{
    if (g_inDisplayHook || (length >= 0 && length < text.size()))
        return g_fontAdvance(metrics, text, length);
    DisplayHookGuard guard;
    const QString translated = translateText(text);
    if (translated.isEmpty()) return g_fontAdvance(metrics, text, length);
    const int pad = std::max(0, g_fontAdvance(metrics, QStringLiteral("中"), -1)) / 2;
    return g_fontAdvance(metrics, translated, -1) + pad;
}

int __fastcall hookedFontAdvanceOpt(const QFontMetrics* metrics, const QString& text,
                                    const QTextOption& option)
{
    if (g_inDisplayHook) return g_fontAdvanceOpt(metrics, text, option);
    DisplayHookGuard guard;
    const QString translated = translateText(text);
    if (translated.isEmpty()) return g_fontAdvanceOpt(metrics, text, option);
    const int pad = std::max(0, g_fontAdvanceOpt(metrics, QStringLiteral("中"), option)) / 2;
    return g_fontAdvanceOpt(metrics, translated, option) + pad;
}

qreal __fastcall hookedFontAdvanceF(const QFontMetricsF* metrics, const QString& text, int length)
{
    if (g_inDisplayHook || (length >= 0 && length < text.size()))
        return g_fontAdvanceF(metrics, text, length);
    DisplayHookGuard guard;
    const QString translated = translateText(text);
    if (translated.isEmpty()) return g_fontAdvanceF(metrics, text, length);
    const qreal pad = std::max<qreal>(0.0, g_fontAdvanceF(metrics, QStringLiteral("中"), -1)) * 0.5;
    return g_fontAdvanceF(metrics, translated, -1) + pad;
}

qreal __fastcall hookedFontAdvanceFOpt(const QFontMetricsF* metrics, const QString& text,
                                       const QTextOption& option)
{
    if (g_inDisplayHook) return g_fontAdvanceFOpt(metrics, text, option);
    DisplayHookGuard guard;
    const QString translated = translateText(text);
    if (translated.isEmpty()) return g_fontAdvanceFOpt(metrics, text, option);
    const qreal pad = std::max<qreal>(0.0, g_fontAdvanceFOpt(metrics, QStringLiteral("中"), option)) * 0.5;
    return g_fontAdvanceFOpt(metrics, translated, option) + pad;
}

std::unique_ptr<QTextLayout> makeDisplayLayout(const QTextLayout* source, const QString& text)
{
    if (!source || text.isEmpty()) return {};
    auto result = std::make_unique<QTextLayout>(text, source->font());
    result->setTextOption(source->textOption());
    result->setCacheEnabled(source->cacheEnabled());
    result->beginLayout();
    const int sourceLines = std::max(1, source->lineCount());
    for (int i = 0; i < sourceLines; ++i) {
        QTextLine line = result->createLine();
        if (!line.isValid()) break;
        if (source->lineCount() > 0) {
            const QTextLine original = source->lineAt(std::min(i, source->lineCount() - 1));
            const qreal width = original.width() > 0.0
                ? original.width() : std::max<qreal>(1.0, original.naturalTextWidth());
            line.setLineWidth(width);
            line.setPosition(original.position());
        }
    }
    result->endLayout();
    // Never silently drop the tail when the translation needs more lines.
    if (result->lineCount() == 0) return {};
    const QTextLine last = result->lineAt(result->lineCount() - 1);
    if (last.textStart() + last.textLength() < text.size()) return {};
    return result;
}

void __fastcall hookedAddTextLayout(
    void* node, const QPointF& position, QTextLayout* layout, const QColor& color,
    int style, const QColor& styleColor, const QColor& anchorColor,
    const QColor& selectionColor, const QColor& selectedTextColor,
    int selectionStart, int selectionEnd, int lineStart, int lineCount)
{
    if (!g_originalAddTextLayout) return;
    // Disabled translation must be an immediate pass-through: no scene-table
    // mutex, layout inspection, normalization or diagnostic writes.
    if (!g_enabled.load(std::memory_order_acquire) || g_inDisplayHook) {
        g_originalAddTextLayout(node, position, layout, color, style, styleColor,
            anchorColor, selectionColor, selectedTextColor, selectionStart,
            selectionEnd, lineStart, lineCount);
        return;
    }
    MenuPaintProbe* probe = nullptr;
    if (layout && g_menuProbesActive.load(std::memory_order_relaxed)) {
        for (auto& candidate : g_menuPaintProbes) {
            if (layout->text() == QLatin1String(candidate.source)) {
                probe = &candidate;
                probe->requestedLines.store(lineCount, std::memory_order_relaxed);
                probe->layoutLines.store(layout->lineCount(), std::memory_order_relaxed);
                probe->translated.store(false, std::memory_order_relaxed);
                break;
            }
        }
    }
    // Selection/format ranges refer to the original characters; there is no
    // safe one-to-one mapping to a translated layout. Preserve them verbatim.
    // QQuickText passes 0, unelidedLineCount even for an ordinary complete
    // single-line label. Only an actual subrange is unsafe to remap.
    const bool wholeLayout = layout && lineStart == 0 &&
        (lineCount == -1 || lineCount == layout->lineCount());
    if (!layout || selectionStart != -1 || selectionEnd != -1 ||
        !wholeLayout || !layout->formats().isEmpty() ||
        !layout->preeditAreaText().isEmpty() || preserveTextNode(node)) {
        g_originalAddTextLayout(node, position, layout, color, style, styleColor,
            anchorColor, selectionColor, selectedTextColor, selectionStart,
            selectionEnd, lineStart, lineCount);
        return;
    }
    DisplayHookGuard guard;
    try {
        const QString replacement = translateText(layout->text(), true);
        if (!replacement.isEmpty()) {
            auto display = makeDisplayLayout(layout, replacement);
            if (display && display->lineCount() > 0) {
                if (probe) probe->translated.store(true, std::memory_order_relaxed);
                g_originalAddTextLayout(node, position, display.get(), color, style,
                    styleColor, anchorColor, selectionColor, selectedTextColor,
                    selectionStart, selectionEnd, lineStart,
                    lineCount == -1 ? -1 : display->lineCount());
                return;
            }
        }
    } catch (...) {}
    g_originalAddTextLayout(node, position, layout, color, style, styleColor,
        anchorColor, selectionColor, selectedTextColor, selectionStart,
        selectionEnd, lineStart, lineCount);
}

void repaintAll()
{
    for (QWindow* window : QGuiApplication::allWindows()) if (window) window->requestUpdate();
}

void collectModelDeep(QAbstractItemModel* model, const QModelIndex& parent,
                      int depth, int& budget, QSet<QString>& entries)
{
    if (!model || model->thread() != QThread::currentThread() || depth > 64 || budget <= 0) return;
    const int rows = model->rowCount(parent);
    const int columns = model->columnCount(parent);
    if (columns <= 0) return;
    for (int row = 0; row < rows && budget > 0; ++row) {
        const QModelIndex branch = model->index(row, 0, parent);
        if (!branch.isValid()) { --budget; continue; }
        for (int column = 0; column < columns && budget > 0; ++column) {
            const QModelIndex index = model->index(row, column, parent);
            --budget;
            if (!index.isValid()) continue;
            const QString text = model->data(index, Qt::DisplayRole).toString();
            if (looksTranslatable(text) && lookupDictionary(text).isEmpty())
                entries.insert(text.trimmed());
        }
        collectModelDeep(model, branch, depth + 1, budget, entries);
    }
}

QSet<QString> collectQuickUiUntranslated()
{
    static const char* displayProperties[] = {
        "text", "title", "label", "toolTip", "placeholderText",
        "currentText", "displayText", "accessibleName"
    };
    QSet<QObject*> visited;
    QSet<QAbstractItemModel*> visitedModels;
    QSet<QString> entries;
    int modelBudget = 200000;
    std::function<void(QObject*, int)> visit = [&](QObject* object, int depth) {
        if (!object || object->thread() != QThread::currentThread() || depth > 64 ||
            visited.size() >= 200000 || visited.contains(object)) return;
        visited.insert(object);

        if (auto* item = qobject_cast<QQuickItem*>(object))
            if (CascadeurSceneTextPolicy::preserve(item)) return;

        const bool isEditor = object->inherits("QQuickTextInput") ||
                              object->inherits("QQuickTextEdit") || object->inherits("QLineEdit");
        // Also exclude editor children (cursor, selection and echo text).
        if (isEditor) return;
        for (const char* property : displayProperties) {
            const QVariant value = object->property(property);
            if (!value.isValid() || !value.canConvert<QString>()) continue;
            const QString text = value.toString();
            if (looksTranslatable(text) && lookupDictionary(text).isEmpty())
                entries.insert(text.trimmed());
        }

        const QVariant modelValue = object->property("model");
        if (modelValue.isValid()) {
            if (auto* model = qobject_cast<QAbstractItemModel*>(modelValue.value<QObject*>())) {
                if (!visitedModels.contains(model)) {
                    visitedModels.insert(model);
                    collectModelDeep(model, QModelIndex(), 0, modelBudget, entries);
                }
            }
        }
        for (QObject* child : object->children()) visit(child, depth + 1);
        if (auto* item = qobject_cast<QQuickItem*>(object))
            for (QQuickItem* child : item->childItems()) visit(child, depth + 1);
    };

    for (QWindow* window : QGuiApplication::allWindows()) visit(window, 0);
    return entries;
}

void saveUntranslated(const QSet<QString>& captured)
{
    wchar_t desktop[MAX_PATH] = {};
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_DESKTOPDIRECTORY, nullptr, SHGFP_TYPE_CURRENT, desktop))) return;
    const QString path = QString::fromWCharArray(desktop) +
                         QLatin1String("/Cascadeur_untranslated_zh.json");
    QJsonObject merged;
    QFile previous(path);
    if (previous.exists()) {
        if (!previous.open(QIODevice::ReadOnly)) return;
        QJsonParseError error;
        const QJsonDocument old = QJsonDocument::fromJson(previous.readAll(), &error);
        if (error.error != QJsonParseError::NoError || !old.isObject() ||
            !old.object().value(QLatin1String("translations")).isObject()) {
            MessageBoxW(nullptr, L"已有嗅探文件格式损坏，本次未覆盖。请先备份并修复文件。",
                        L"未翻译词条", MB_OK | MB_ICONERROR);
            return;
        }
        merged = old.object().value(QLatin1String("translations")).toObject();
        previous.close();
    }
    const QList<QString> entries = captured.values();
    int added = 0;
    for (const QString& entry : entries) {
        if (!merged.contains(entry)) { merged.insert(entry, QString()); ++added; }
    }
    if (added > 0 || !previous.exists()) {
        QSaveFile file(path);
        QJsonObject document{
            {QStringLiteral("$schema"), QStringLiteral("sp-translation-v1")},
            {QStringLiteral("id"), QStringLiteral("cascadeur-untranslated")},
            {QStringLiteral("language"), QStringLiteral("zh-CN")},
            {QStringLiteral("translations"), merged}
        };
        const QByteArray bytes = QJsonDocument(document).toJson(QJsonDocument::Indented);
        if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() ||
            !file.commit()) return;
    }
    const int total = merged.size();
    QTimer::singleShot(0, QCoreApplication::instance(), [added, total, path]() {
        const QString message = QStringLiteral("已捕获 %1 条（共 %2 条）。\n已保存到桌面：\n%3")
                                    .arg(added).arg(total).arg(path);
        MessageBoxW(nullptr, reinterpret_cast<const wchar_t*>(message.utf16()),
                    L"未翻译词条", MB_OK | MB_ICONINFORMATION);
    });
}

QPointer<QQuickItem> g_mainMenuBar;
QPointer<QObject> g_languageLink;
QPointer<QObject> g_authorLink;
QPointer<QObject> g_githubLink;
void adjustMenuWidths(QQuickItem* menuBar);
void scheduleAuthorLinksInstall();

void toggleTranslation()
{
    const bool previous = g_enabled.load(std::memory_order_acquire);
    const bool enabled = !previous;
    g_enabled.store(enabled, std::memory_order_release);

    // Mari first invalidates font metrics and widget geometry before repainting.
    // Do the Qt Quick equivalent so cached text nodes and implicit sizes are
    // rebuilt for the newly selected language instead of merely repainted.
    QSet<QObject*> visited;
    std::function<void(QObject*, int)> refresh = [&](QObject* object, int depth) {
        if (!object || object->thread() != QThread::currentThread() || depth > 128 ||
            visited.size() >= 200000 || visited.contains(object)) return;
        visited.insert(object);
        const QPointer<QObject> guard(object);

        QEvent fontChange(QEvent::FontChange);
        QCoreApplication::sendEvent(object, &fontChange);
        if (!guard) return;
        QEvent layoutRequest(QEvent::LayoutRequest);
        QCoreApplication::sendEvent(object, &layoutRequest);
        if (!guard) return;

        // Qt Quick Text caches its QTextLayout and scene-graph glyph node.
        // QQuickText::forceLayout() is the host-exported, QML-visible way to
        // invalidate both without touching the source text property. Item views
        // expose the same method, so invoking it opportunistically also refreshes
        // their delegates and collapsed model content.
        if (object->metaObject()->indexOfMethod("forceLayout()") >= 0)
            QMetaObject::invokeMethod(object, "forceLayout", Qt::DirectConnection);
        if (!guard) return;

        QList<QPointer<QObject>> children;
        if (auto* item = qobject_cast<QQuickItem*>(object)) {
            item->polish();
            item->update();
            for (QQuickItem* child : item->childItems()) children.append(child);
        }
        for (QObject* child : object->children()) children.append(child);
        // A child's forceLayout/FontChange handler can destroy a sibling or
        // even its parent. Snapshot guarded pointers before dispatching events.
        for (const auto& child : children) if (child) refresh(child, depth + 1);
    };
    QList<QPointer<QWindow>> windows;
    for (QWindow* window : QGuiApplication::allWindows()) windows.append(window);
    for (const auto& window : windows) if (window) refresh(window, 0);

    adjustMenuWidths(g_mainMenuBar);
    if (g_mainMenuBar) {
        QEvent styleChange(QEvent::StyleChange);
        QCoreApplication::sendEvent(g_mainMenuBar, &styleChange);
        if (g_mainMenuBar) {
            g_mainMenuBar->polish();
            g_mainMenuBar->update();
        }
    }
    repaintAll();
    QTimer::singleShot(0, QCoreApplication::instance(), [] {
        if (g_mainMenuBar) adjustMenuWidths(g_mainMenuBar);
        repaintAll();
    });
}

void openHotkeySettings()
{
    if (g_hotkeySettingsWindow && g_hotkeySettingsWindow->isVisible()) {
        g_hotkeySettingsWindow->raise();
        g_hotkeySettingsWindow->requestActivate();
        return;
    }
    auto* item = qobject_cast<QQuickItem*>(g_languageLink.data());
    QQmlEngine* engine = item ? qmlEngine(item) : nullptr;
    QString error;
    auto* window = CascadeurHotkeySettings::create(engine, item ? item->window() : nullptr,
        int(g_toggleVk.load(std::memory_order_acquire)),
        [](int vk, QString* error) {
            if (!CascadeurHotkeyConfig::save(CascadeurHotkeyConfig::path(), vk, error)) return false;
            g_toggleVk.store(UINT(vk), std::memory_order_release);
            return true;
        },
        [] { g_settingsOpen = false; }, &error);
    if (!window) {
        const QString message = QStringLiteral("快捷键设置窗口创建失败：\n") + error;
        MessageBoxW(nullptr, reinterpret_cast<LPCWSTR>(message.utf16()),
                    L"Cascadeur 中文补丁", MB_OK | MB_ICONERROR);
        return;
    }
    g_hotkeySettingsWindow = window;
    g_settingsOpen = true;
    window->show();
    window->requestActivate();
}

// Cascadeur's native/3D viewport does not always forward keyboard input as a
// QKeyEvent. Intercept the Windows message stream as Mari does, so the
// shortcuts work regardless of which viewport or control currently has focus.
class NativeKeyFilter final : public QAbstractNativeEventFilter {
public:
    UINT toggleHandled_ = 0;
    bool tildeHandled_ = false;

    bool nativeEventFilter(const QByteArray& eventType, void* message,
                           qintptr* result) override {
        if (!message || (eventType != "windows_generic_MSG" &&
                         eventType != "windows_dispatcher_MSG")) return false;

        auto* msg = static_cast<MSG*>(message);
        if ((msg->message == WM_ACTIVATEAPP && !msg->wParam) ||
            msg->message == WM_KILLFOCUS || g_settingsOpen) {
            toggleHandled_ = 0;
            tildeHandled_ = false;
            return false;
        }
        const bool isDown = msg->message == WM_KEYDOWN ||
                            msg->message == WM_SYSKEYDOWN;
        const bool isUp = msg->message == WM_KEYUP ||
                          msg->message == WM_SYSKEYUP;
        if (!isDown && !isUp) return false;

        const UINT vk = static_cast<UINT>(msg->wParam);
        auto consume = [result] { if (result) *result = 0; return true; };
        if (g_deepCapture && g_deepCapture->active()) {
            if (isDown && vk == VK_ESCAPE) g_deepCapture->cancel();
            return consume();
        }
        if (isUp && toggleHandled_ && vk == toggleHandled_) {
            toggleHandled_ = 0;
            QTimer::singleShot(0, QCoreApplication::instance(), [] { toggleTranslation(); });
            return consume();
        }
        if (isUp && vk == VK_OEM_3 && tildeHandled_) {
            tildeHandled_ = false;
            QTimer::singleShot(0, QCoreApplication::instance(), [] {
                if (g_deepCapture) g_deepCapture->start();
            });
            return consume();
        }
        // Swallow repeats as well as the matched down/up pair. They must not
        // leak into Cascadeur's own shortcuts while our key is held.
        if (isDown && ((toggleHandled_ && vk == toggleHandled_) ||
                       (tildeHandled_ && vk == VK_OEM_3))) return consume();
        if (!isDown || (msg->lParam & 0x40000000) != 0)
            return false;

        const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
        const bool otherModifier = (GetKeyState(VK_CONTROL) & 0x8000) != 0 ||
            (GetKeyState(VK_MENU) & 0x8000) != 0 ||
            (GetKeyState(VK_LWIN) & 0x8000) != 0 || (GetKeyState(VK_RWIN) & 0x8000) != 0;
        if (otherModifier) return false;
        if (shift && vk == VK_OEM_3) {
            tildeHandled_ = true;
            return consume();
        }
        const UINT toggleVk = g_toggleVk.load(std::memory_order_acquire);
        if (!shift && vk == toggleVk) {
            toggleHandled_ = vk;
            return consume();
        }
        return false;
    }
};

class ShortcutFilter final : public QObject {
public:
    bool eventFilter(QObject* watched, QEvent* event) override {
        // Phase 2: UI objects created after initial installation are handled by
        // lifecycle events instead of a permanent polling timer.
        if (event && (event->type() == QEvent::Show ||
                      event->type() == QEvent::Polish ||
                      event->type() == QEvent::ActionAdded ||
                      event->type() == QEvent::LayoutRequest ||
                      event->type() == QEvent::ChildAdded))
            scheduleAuthorLinksInstall();
        return QObject::eventFilter(watched, event);
    }
};
ShortcutFilter* g_filter = nullptr;

void adjustMenuWidths(QQuickItem* menuBar)
{
    if (!menuBar) return;
    const int itemCount = menuBar->property("count").toInt();
    for (int index = 0; index < itemCount; ++index) {
        QQuickItem* item = nullptr;
        if (!QMetaObject::invokeMethod(menuBar, "itemAt", Qt::DirectConnection,
                                       Q_RETURN_ARG(QQuickItem*, item), Q_ARG(int, index)) || !item)
            continue;
        const QString source = item->property("text").toString();
        if (source.isEmpty()) continue;
        const char* originalName = "_cascadeurChineseOriginalImplicitWidth";
        const char* originalWidthName = "_cascadeurChineseOriginalWidth";
        if (!item->property(originalName).isValid())
            item->setProperty(originalName, item->implicitWidth());
        if (!item->property(originalWidthName).isValid())
            item->setProperty(originalWidthName, item->width());
        const QFont font = item->property("font").value<QFont>();
        const QFontMetricsF metrics(font);
        const qreal left = item->property("leftPadding").toReal();
        const qreal right = item->property("rightPadding").toReal();
        const QString translated = translateText(source);
        if (!g_enabled.load() || translated.isEmpty()) {
            // The first saved geometry may already have been measured by our
            // Chinese font hook. Never restore an English label into that
            // smaller width. Measure with hooks bypassed, preserving host text.
            DisplayHookGuard guard;
            const qreal englishWidth = std::ceil(metrics.horizontalAdvance(source) + left + right);
            item->setImplicitWidth(std::max(item->property(originalName).toReal(), englishWidth));
            item->setWidth(std::max(item->property(originalWidthName).toReal(), englishWidth));
            continue;
        }
        const qreal cjkSlack = metrics.horizontalAdvance(QStringLiteral("中")) * 0.5;
        const qreal targetWidth = std::ceil(metrics.horizontalAdvance(translated) +
                                            left + right + cjkSlack);
        item->setImplicitWidth(targetWidth);
        item->setWidth(targetWidth);
    }
}

QObject* createMenuLink(QQuickItem* parent, QObject* helpObject,
                        const QString& label, const QString& url,
                        const QString& objectName)
{
    QQmlEngine* engine = qmlEngine(helpObject);
    if (!engine || !parent) return nullptr;
    const QByteArray qml = R"QML(
import QtQuick 2.15
import QtQuick.Controls 2.15
MenuBarItem {
    id: control
    property string targetUrl
    onClicked: Qt.openUrlExternally(targetUrl)
    HoverHandler { cursorShape: Qt.PointingHandCursor }
    contentItem: Text {
        text: control.text
        font: control.font
        color: "#66aaff"
        horizontalAlignment: Text.AlignHCenter
        verticalAlignment: Text.AlignVCenter
    }
    background: Rectangle {
        // MenuBarItem::highlighted can remain selected after a link is clicked.
        // Links should look active only while the pointer is actually over them.
        color: control.hovered ? "#212121" : "transparent"
    }
}
)QML";
    QQmlComponent component(engine);
    component.setData(qml, QUrl(QStringLiteral("cascadeur-chinese-author-link.qml")));
    QQmlContext* context = qmlContext(helpObject);
    QObject* object = component.create(context ? context : engine->rootContext());
    auto* item = qobject_cast<QQuickItem*>(object);
    if (!item) { delete object; return nullptr; }
    object->setObjectName(objectName);
    const QUrl target(url);
    if (!target.isValid() || target.scheme() != QLatin1String("https") ||
        target.host().isEmpty()) {
        delete object;
        return nullptr;
    }
    object->setProperty("text", label);
    object->setProperty("targetUrl", target.toString());
    // Use the exact geometry/font metrics of Cascadeur's own Help item. The
    // stock Controls fallback has a taller vertical padding, which otherwise
    // puts the link text several pixels below the native menu baseline.
    static const char* inheritedProperties[] = {
        "font", "height", "padding", "topPadding", "bottomPadding",
        "leftPadding", "rightPadding", "spacing", "topInset", "bottomInset"
    };
    for (const char* name : inheritedProperties) {
        const QVariant value = helpObject->property(name);
        if (value.isValid()) object->setProperty(name, value);
    }
    const bool added = QMetaObject::invokeMethod(parent, "addItem", Qt::DirectConnection,
                                                 Q_ARG(QQuickItem*, item));
    if (!added) { delete object; return nullptr; }
    object->setParent(parent);
    return object;
}

QObject* createLanguageMenuItem(QQuickItem* parent, QObject* helpObject)
{
    QQmlEngine* engine = qmlEngine(helpObject);
    if (!engine || !parent) return nullptr;
    const QByteArray qml = R"QML(
import QtQuick 2.15
import QtQuick.Controls 2.15
MenuBarItem {
    id: control
    text: "中/英"
    signal hotkeySettingsRequested()
    // Qt 6.5.1's MenuBarItem may also emit clicked for a passive right tap.
    // This child exclusively handles right clicks; left clicks still reach
    // the native MenuBarItem, including its keyboard/pressed-state behaviour.
    MouseArea {
        anchors.fill: parent
        acceptedButtons: Qt.RightButton
        preventStealing: true
        onClicked: control.hotkeySettingsRequested()
    }
    contentItem: Text {
        text: control.text
        font: control.font
        color: "#cdcdcd"
        horizontalAlignment: Text.AlignHCenter
        verticalAlignment: Text.AlignVCenter
    }
    background: Rectangle {
        color: control.hovered || control.highlighted ? "#212121" : "transparent"
    }
}
)QML";
    QQmlComponent component(engine);
    component.setData(qml, QUrl(QStringLiteral("cascadeur-chinese-language.qml")));
    QQmlContext* context = qmlContext(helpObject);
    QObject* object = component.create(context ? context : engine->rootContext());
    auto* item = qobject_cast<QQuickItem*>(object);
    if (!item) { delete object; return nullptr; }
    object->setObjectName(QStringLiteral("qt_chinese_language"));
    // Let the native control consume its release and clear pressed state.
    // SignalMapper bridges the runtime QML signal without a private Qt class
    // or swallowing mouse events; keyboard activation works through clicked too.
    auto* mapper = new QSignalMapper(object);
    mapper->setMapping(object, 0);
    QObject::connect(object, SIGNAL(clicked()), mapper, SLOT(map()));
    QObject::connect(mapper, &QSignalMapper::mappedInt, object, [](int) { toggleTranslation(); });
    auto* settingsMapper = new QSignalMapper(object);
    settingsMapper->setMapping(object, 0);
    QObject::connect(object, SIGNAL(hotkeySettingsRequested()), settingsMapper, SLOT(map()));
    QObject::connect(settingsMapper, &QSignalMapper::mappedInt, object, [](int) {
        // Leave pointer dispatch before showing the plugin-owned Qt window.
        QTimer::singleShot(0, QCoreApplication::instance(), [] { openHotkeySettings(); });
    });
    static const char* inheritedProperties[] = {
        "font", "height", "padding", "topPadding", "bottomPadding",
        "leftPadding", "rightPadding", "spacing", "topInset", "bottomInset"
    };
    for (const char* name : inheritedProperties) {
        const QVariant value = helpObject->property(name);
        if (value.isValid()) object->setProperty(name, value);
    }
    const bool added = QMetaObject::invokeMethod(parent, "addItem", Qt::DirectConnection,
                                                 Q_ARG(QQuickItem*, item));
    if (!added) { delete object; return nullptr; }
    object->setParent(parent);
    return object;
}

bool installAuthorLinks()
{
    if (g_languageLink && g_authorLink && g_githubLink) return true;
    for (QWindow* window : QGuiApplication::allWindows()) {
        if (!window) continue;
        QList<QObject*> objects = window->findChildren<QObject*>();
        objects.prepend(window);
        if (auto* quickWindow = qobject_cast<QQuickWindow*>(window)) {
            if (QQuickItem* content = quickWindow->contentItem()) {
                std::function<void(QQuickItem*)> appendVisualTree =
                    [&](QQuickItem* item) {
                        if (!item) return;
                        objects.append(item);
                        for (QQuickItem* child : item->childItems()) appendVisualTree(child);
                    };
                appendVisualTree(content);
            }
        }
        for (QObject* object : objects) {
            QString text = object->property("text").toString();
            text.remove(QLatin1Char('&'));
            if (text.trimmed().compare(QLatin1String("Help"), Qt::CaseInsensitive) != 0 &&
                text.trimmed() != QStringLiteral("帮助")) continue;
            if (!object->inherits("QQuickMenuBarItem")) continue;
            QQuickItem* parent = qobject_cast<QQuickItem*>(object->parent());
            if (!parent || !parent->inherits("QQuickMenuBar") ||
                (g_mainMenuBar && g_mainMenuBar != parent)) continue;
            if (!g_languageLink) g_languageLink = createLanguageMenuItem(parent, object);
            if (!g_authorLink) g_authorLink = createMenuLink(parent, object,
                QStringLiteral("Bilibili 神说要凑数汉化"),
                QStringLiteral("https://space.bilibili.com/281243426?spm_id_from=333.1007.0.0"),
                QStringLiteral("qt_chinese_author"));
            if (!g_githubLink) g_githubLink = createMenuLink(parent, object,
                QStringLiteral("GitHub 仓库"),
                QStringLiteral("https://github.com/iillya/CascadeurChinese"),
                QStringLiteral("qt_chinese_github"));
            g_mainMenuBar = parent;
            adjustMenuWidths(parent);
            QTimer::singleShot(0, parent, [parent] { adjustMenuWidths(parent); });
            QTimer::singleShot(500, parent, [parent] { adjustMenuWidths(parent); });
            QTimer::singleShot(2000, parent, [parent] { adjustMenuWidths(parent); });
            return g_languageLink && g_authorLink && g_githubLink;
        }
    }
    return false;
}

void scheduleAuthorLinksInstall() {
    static bool scheduled = false;
    if (scheduled || (g_languageLink && g_authorLink && g_githubLink)) return;
    scheduled = true;
    QTimer::singleShot(200, QCoreApplication::instance(), [] {
        scheduled = false;
        installAuthorLinks();
    });
}

void installLifecycleFilter(QCoreApplication* app) {
    if (!app || g_filter) return;
    g_deepCapture = new DeepCapture(app, collectQuickUiUntranslated, saveUntranslated);
    app->installEventFilter(g_deepCapture);
    g_filter = new ShortcutFilter;
    g_filter->setParent(app);
    app->installEventFilter(g_filter);
    if (auto* dispatcher = QAbstractEventDispatcher::instance(app->thread())) {
        auto* nativeFilter = new NativeKeyFilter;
        dispatcher->installNativeEventFilter(nativeFilter);
        QObject::connect(app, &QCoreApplication::aboutToQuit, dispatcher,
                         [dispatcher, nativeFilter] {
            dispatcher->removeNativeEventFilter(nativeFilter);
            delete nativeFilter;
            g_enabled.store(false, std::memory_order_release);
        });
    }
    installAuthorLinks();

    // Phase 3: optional bounded fallback for UI that appears without a useful
    // Qt lifecycle event. Disabled by default; implementation is retained.
    if (!CascadeurHookLifecycle::kEnableDeferredPolling)
        return;
    auto* timer = new QTimer(app);
    timer->setInterval(CascadeurHookLifecycle::kDeferredScanIntervalMs);
    QObject::connect(timer, &QTimer::timeout, timer, [timer] {
        if (installAuthorLinks()) {
            timer->stop();
            timer->deleteLater();
        }
    });
    timer->start();
    const QPointer<QTimer> timerGuard(timer);
    QTimer::singleShot(CascadeurHookLifecycle::kDeferredScanWindowMs, app,
                       [timerGuard] {
                           if (timerGuard) {
                               timerGuard->stop();
                               timerGuard->deleteLater();
                           }
                       });
}

void writeWindowDiagnostics()
{
    QJsonArray windows;
    for (QWindow* window : QGuiApplication::allWindows()) {
        if (!window) continue;
        QJsonObject item;
        item.insert(QStringLiteral("class"),
                    QString::fromLatin1(window->metaObject()->className()));
        item.insert(QStringLiteral("objectName"), window->objectName());
        item.insert(QStringLiteral("title"), window->title());
        item.insert(QStringLiteral("width"), window->width());
        item.insert(QStringLiteral("height"), window->height());
        item.insert(QStringLiteral("devicePixelRatio"), window->devicePixelRatio());
        item.insert(QStringLiteral("visible"), window->isVisible());
        item.insert(QStringLiteral("surfaceType"), int(window->surfaceType()));
        windows.append(item);
    }
    QJsonObject capabilities;
    capabilities.insert(QStringLiteral("SceneNameProtection"),
        g_textNodeTrackingReady.load(std::memory_order_acquire)
            ? QStringLiteral("OK") : QStringLiteral("FAILED"));
    {
        std::lock_guard<std::mutex> lock(g_textNodeMutex);
        int protectedNodes = 0;
        for (const auto& entry : g_textNodePreserve) if (entry.second) ++protectedNodes;
        capabilities.insert(QStringLiteral("ProtectedTextNodes"), protectedNodes);
        capabilities.insert(QStringLiteral("TrackedTextNodes"), int(g_textNodePreserve.size()));
    }
    capabilities.insert(QStringLiteral("Dictionary"),
        std::atomic_load_explicit(&g_dictionary, std::memory_order_acquire)->exact.empty()
            ? QStringLiteral("FAILED") : QStringLiteral("OK"));
    capabilities.insert(QStringLiteral("TextLayoutHook"),
        g_originalAddTextLayout ? QStringLiteral("OK") : QStringLiteral("DISABLED"));
    capabilities.insert(QStringLiteral("FontMetricsHook"),
        g_fontAdvance || g_fontAdvanceOpt ? QStringLiteral("PARTIAL") : QStringLiteral("DISABLED"));
    capabilities.insert(QStringLiteral("FontMetricsFHook"),
        g_fontAdvanceF || g_fontAdvanceFOpt ? QStringLiteral("PARTIAL") : QStringLiteral("DISABLED"));
    QJsonObject root;
    root.insert(QStringLiteral("product"), QStringLiteral("Cascadeur"));
    root.insert(QStringLiteral("qtVersion"), QString::fromLatin1(qVersion()));
    root.insert(QStringLiteral("engineVersion"), QStringLiteral("0.2.1"));
    root.insert(QStringLiteral("translationEnabled"), g_enabled.load(std::memory_order_acquire));
    QJsonArray menuPaint;
    for (const auto& probe : g_menuPaintProbes) {
        menuPaint.append(QJsonObject{
            {QStringLiteral("source"), QString::fromLatin1(probe.source)},
            {QStringLiteral("requestedLines"), probe.requestedLines.load(std::memory_order_relaxed)},
            {QStringLiteral("layoutLines"), probe.layoutLines.load(std::memory_order_relaxed)},
            {QStringLiteral("translated"), probe.translated.load(std::memory_order_relaxed)}
        });
    }
    root.insert(QStringLiteral("menuPaint"), menuPaint);
    root.insert(QStringLiteral("capabilities"), capabilities);
    root.insert(QStringLiteral("dictionaryNotes"), QJsonArray::fromStringList(g_dictionaryNotes));
    root.insert(QStringLiteral("windows"), windows);
    QSaveFile file(QDir::tempPath() +
                   QLatin1String("/Cascadeur_window_diagnostics.json"));
    if (file.open(QIODevice::WriteOnly)) {
        file.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
        file.commit();
    }
}

bool isExecutableAddress(const void* address)
{
    if (!address) return false;
    MEMORY_BASIC_INFORMATION info{};
    if (!VirtualQuery(address, &info, sizeof(info)) || info.State != MEM_COMMIT ||
        (info.Protect & (PAGE_GUARD | PAGE_NOACCESS))) return false;
    const DWORD protection = info.Protect & 0xff;
    return protection == PAGE_EXECUTE || protection == PAGE_EXECUTE_READ ||
           protection == PAGE_EXECUTE_READWRITE || protection == PAGE_EXECUTE_WRITECOPY;
}

template<typename Function>
bool attachHook(Function& original, Function replacement)
{
    if (!isExecutableAddress(reinterpret_cast<void*>(original))) {
        original = nullptr;
        return false;
    }
    // Enlist existing render/worker threads too, so Detours can relocate an
    // instruction pointer that happens to be inside a patched function.
    std::vector<HANDLE> threads;
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    bool ready = snapshot != INVALID_HANDLE_VALUE;
    THREADENTRY32 entry{};
    entry.dwSize = sizeof(entry);
    if (ready && Thread32First(snapshot, &entry)) {
        do {
            if (entry.th32OwnerProcessID != GetCurrentProcessId() ||
                entry.th32ThreadID == GetCurrentThreadId()) continue;
            HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT |
                                       THREAD_SET_CONTEXT | THREAD_QUERY_INFORMATION,
                                       FALSE, entry.th32ThreadID);
            if (thread) threads.push_back(thread);
            else if (GetLastError() != ERROR_INVALID_PARAMETER) ready = false;
        } while (Thread32Next(snapshot, &entry));
    } else {
        ready = false;
    }
    if (snapshot != INVALID_HANDLE_VALUE) CloseHandle(snapshot);
    bool installed = false;
    if (ready && DetourTransactionBegin() == NO_ERROR) {
        ready = DetourUpdateThread(GetCurrentThread()) == NO_ERROR;
        for (HANDLE thread : threads)
            if (DetourUpdateThread(thread) != NO_ERROR) ready = false;
        if (ready && DetourAttach(reinterpret_cast<PVOID*>(&original),
                                  reinterpret_cast<PVOID>(replacement)) == NO_ERROR)
            installed = DetourTransactionCommit() == NO_ERROR;
        else
            DetourTransactionAbort();
    }
    for (HANDLE thread : threads) CloseHandle(thread);
    if (!installed) original = nullptr;
    return installed;
}

bool installHook()
{
    HMODULE quick = GetModuleHandleW(L"Qt6Quick.dll");
    if (!quick) return false;
    constexpr const char* symbol = "?addTextLayout@QQuickTextNode@@QEAAXAEBVQPointF@@PEAVQTextLayout@@AEBVQColor@@W4TextStyle@QQuickText@@2222HHHH@Z";
    g_originalAddTextLayout = reinterpret_cast<AddTextLayoutFn>(GetProcAddress(quick, symbol));
    g_textNodeCtor = reinterpret_cast<TextNodeCtorFn>(GetProcAddress(quick,
        "??0QQuickTextNode@@QEAA@PEAVQQuickItem@@@Z"));
    g_textNodeDtor = reinterpret_cast<TextNodeDtorFn>(GetProcAddress(quick,
        "??1QQuickTextNode@@UEAA@XZ"));
    // No raw private offsets. Track owner policy via exported construction and
    // destruction. Unknown/pre-existing nodes remain untranslated, fail closed.
    const bool destructorInstalled = attachHook(g_textNodeDtor, hookedTextNodeDtor);
    const bool constructorInstalled = destructorInstalled && attachHook(g_textNodeCtor, hookedTextNodeCtor);
    g_textNodeTrackingReady.store(constructorInstalled, std::memory_order_release);
    if (!constructorInstalled) return false;
    HMODULE gui = GetModuleHandleW(L"Qt6Gui.dll");
    if (gui) {
        g_fontAdvance = reinterpret_cast<FontAdvanceFn>(GetProcAddress(gui,
            "?horizontalAdvance@QFontMetrics@@QEBAHAEBVQString@@H@Z"));
        g_fontAdvanceOpt = reinterpret_cast<FontAdvanceOptFn>(GetProcAddress(gui,
            "?horizontalAdvance@QFontMetrics@@QEBAHAEBVQString@@AEBVQTextOption@@@Z"));
        g_fontAdvanceF = reinterpret_cast<FontAdvanceFFn>(GetProcAddress(gui,
            "?horizontalAdvance@QFontMetricsF@@QEBANAEBVQString@@H@Z"));
        g_fontAdvanceFOpt = reinterpret_cast<FontAdvanceFOptFn>(GetProcAddress(gui,
            "?horizontalAdvance@QFontMetricsF@@QEBANAEBVQString@@AEBVQTextOption@@@Z"));
    }
    if (!attachHook(g_originalAddTextLayout, hookedAddTextLayout)) {
        g_fontAdvance = nullptr;
        g_fontAdvanceOpt = nullptr;
        g_fontAdvanceF = nullptr;
        g_fontAdvanceFOpt = nullptr;
        return false;
    }
    // Each optional overload degrades independently; no half-installed state
    // and no dependency on a different overload's trampoline.
    attachHook(g_fontAdvance, hookedFontAdvance);
    attachHook(g_fontAdvanceOpt, hookedFontAdvanceOpt);
    attachHook(g_fontAdvanceF, hookedFontAdvanceF);
    attachHook(g_fontAdvanceFOpt, hookedFontAdvanceFOpt);
    return true;
}

void runOnGuiThread(QCoreApplication* app) {
    // Phase 1: the Qt application exists and this runs on its GUI thread.
    loadDictionaries();
    QString hotkeyError;
    g_toggleVk.store(UINT(CascadeurHotkeyConfig::load(CascadeurHotkeyConfig::path(), &hotkeyError)),
                     std::memory_order_release);
    if (!hotkeyError.isEmpty()) g_dictionaryNotes.append(hotkeyError);
    if (installHook()) {
        installLifecycleFilter(app);
    }
    QTimer::singleShot(2000, app, [] { writeWindowDiagnostics(); });
    QTimer::singleShot(5000, app, [] { writeWindowDiagnostics(); });
    QTimer::singleShot(5000, app, [] { g_menuProbesActive.store(false, std::memory_order_relaxed); });
}

DWORD WINAPI initialize(void*)
{
    // This injection is process-lifetime only, not a hot-unloadable plug-in.
    // Keep callback code mapped until Windows tears down the host process.
    HMODULE pinned = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_PIN,
                           reinterpret_cast<LPCWSTR>(&initialize), &pinned)) return 1;
    if (!CascadeurQtCompatibility::supportedRuntime(qVersion())) return 1;
    QCoreApplication* app = nullptr;
    for (int attempt = 0;
         attempt < CascadeurHookLifecycle::kApplicationWaitAttempts; ++attempt) {
        app = QCoreApplication::instance();
        if (app && qobject_cast<QGuiApplication*>(app)) break;
        app = nullptr;
        Sleep(CascadeurHookLifecycle::kApplicationWaitIntervalMs);
    }
    if (!app) return 1;
    QMetaObject::invokeMethod(app, [app] { runOnGuiThread(app); },
                              Qt::QueuedConnection);
    return 0;
}
} // namespace

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, void*)
{
    if (reason == DLL_PROCESS_ATTACH) {
        g_self = module; DisableThreadLibraryCalls(module);
        if (HANDLE thread = CreateThread(nullptr, 0, initialize, nullptr, 0, nullptr)) CloseHandle(thread);
    }
    return TRUE;
}
