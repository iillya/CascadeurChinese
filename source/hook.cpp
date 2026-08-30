// Cascadeur Chinese Localizer - pure display-layer Qt Quick hook.
// QML properties and data models remain untouched. Translation is applied to
// a temporary QTextLayout copy only while scene-graph glyphs are generated.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <commctrl.h>

#include "hook_lifecycle.h"
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
#include <QtCore/QSaveFile>
#include <QtCore/QSet>
#include <QtCore/QStringConverter>
#include <QtCore/QTextStream>
#include <QtCore/QTimer>
#include <QtGui/QGuiApplication>
#include <QtGui/QFontMetrics>
#include <QtGui/QMouseEvent>
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

extern "C" __declspec(dllexport) int __cdecl cascadeur_localizer_api_version() { return 1; }
extern "C" __declspec(dllexport) const char* __cdecl cascadeur_localizer_version() { return "0.1.0"; }

namespace {
HMODULE g_self = nullptr;
std::atomic_bool g_enabled{true};
std::atomic_uint g_toggleVk{VK_F3};
struct DictionarySnapshot {
    std::unordered_map<std::string, std::string> exact;
    std::unordered_map<std::string, std::string> folded;
};
std::shared_ptr<const DictionarySnapshot> g_dictionary =
    std::make_shared<const DictionarySnapshot>();

using AddTextLayoutFn = void (__fastcall *)(
    void*, const QPointF&, QTextLayout*, const QColor&, int,
    const QColor&, const QColor&, const QColor&, const QColor&,
    int, int, int, int);
AddTextLayoutFn g_originalAddTextLayout = nullptr;
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

bool containsCjk(const QString& text)
{
    for (QChar ch : text) {
        const ushort u = ch.unicode();
        if ((u >= 0x3400 && u <= 0x9fff) || (u >= 0xf900 && u <= 0xfaff)) return true;
    }
    return false;
}

QString normalize(QString text)
{
    text.replace(QChar(0x00a0), QLatin1Char(' '));
    text.replace(QChar(0x3000), QLatin1Char(' '));
    text.remove(QChar(0x00ad)); text.remove(QChar(0x200b));
    text.remove(QChar(0x200c)); text.remove(QChar(0x200d)); text.remove(QChar(0xfeff));
    for (QChar& ch : text) {
        const ushort u = ch.unicode();
        if (u >= 0xff01 && u <= 0xff5e) ch = QChar(u - 0xfee0);
    }
    text.replace(QChar(0x2018), QLatin1Char('\''));
    text.replace(QChar(0x2019), QLatin1Char('\''));
    text.replace(QChar(0x201c), QLatin1Char('"'));
    text.replace(QChar(0x201d), QLatin1Char('"'));
    text.replace(QChar(0x2013), QLatin1Char('-'));
    text.replace(QChar(0x2014), QLatin1Char('-'));
    text = text.simplified();
    while (text.endsWith(QChar(0x2026))) text.chop(1);
    while (text.endsWith(QLatin1String("..."))) text.chop(3);
    if (text.endsWith(QLatin1String(" *"))) text.chop(2);
    text.replace(QLatin1Char('_'), QLatin1Char(' '));
    text.remove(QLatin1Char('&'));
    text = text.normalized(QString::NormalizationForm_D);
    QString withoutMarks;
    withoutMarks.reserve(text.size());
    for (QChar ch : text)
        if (ch.category() != QChar::Mark_NonSpacing &&
            ch.category() != QChar::Mark_SpacingCombining &&
            ch.category() != QChar::Mark_Enclosing)
            withoutMarks.append(ch);
    text = withoutMarks.normalized(QString::NormalizationForm_C).toCaseFolded();
    text.remove(QLatin1Char(' '));
    return text;
}

bool looksTranslatable(const QString& source)
{
    const QString text = source.trimmed();
    if (text.size() < 2 || text.size() > 500 || containsCjk(text)) return false;
    bool letter = false;
    for (QChar ch : text) if (ch.isLetter()) { letter = true; break; }
    if (!letter || text.contains(QLatin1String(":\\")) || text.startsWith(QLatin1Char('/')) ||
        text.startsWith(QLatin1String("./")) || text.startsWith(QLatin1String("../")) ||
        (text.startsWith(QLatin1Char('<')) && text.endsWith(QLatin1Char('>'))) ||
        text.contains(QLatin1String("</")) || text.contains(QLatin1String("${")) ||
        text.contains(QLatin1String("{{"))) return false;
    if (text.startsWith(QLatin1String("http"), Qt::CaseInsensitive) ||
        text.startsWith(QLatin1String("file:"), Qt::CaseInsensitive)) return false;
    return true;
}

void loadDictionaries()
{
    std::unordered_map<std::string, std::string> exact, folded;
    QDir dir(QString::fromStdWString(selfDirectory()) + QLatin1String("/translations"));
    for (const QString& name : dir.entryList({QStringLiteral("*_zh.json")}, QDir::Files, QDir::Name)) {
        QFile file(dir.filePath(name));
        if (!file.open(QIODevice::ReadOnly)) continue;
        const QJsonObject map = QJsonDocument::fromJson(file.readAll()).object()
                                    .value(QLatin1String("translations")).toObject();
        for (auto it = map.constBegin(); it != map.constEnd(); ++it) {
            if (it.key().isEmpty() || !it.value().isString() || it.value().toString().isEmpty()) continue;
            const std::string value = it.value().toString().toUtf8().toStdString();
            exact[it.key().toUtf8().toStdString()] = value;
            folded[normalize(it.key()).toUtf8().toStdString()] = value;
        }
    }
    auto snapshot = std::make_shared<DictionarySnapshot>();
    snapshot->exact = std::move(exact);
    snapshot->folded = std::move(folded);
    std::atomic_store_explicit(&g_dictionary,
        std::static_pointer_cast<const DictionarySnapshot>(snapshot),
        std::memory_order_release);
}

QString lookupDictionary(const QString& raw)
{
    QString key = raw.trimmed();
    if (!looksTranslatable(key)) return {};
    const auto dictionary = std::atomic_load_explicit(&g_dictionary,
                                                       std::memory_order_acquire);
    auto it = dictionary->exact.find(key.toUtf8().toStdString());
    if (it != dictionary->exact.end()) return QString::fromUtf8(it->second);
    QString suffix;
    if (key.endsWith(QLatin1Char('*'))) { key.chop(1); key = key.trimmed(); suffix = QLatin1Char('*'); }
    key.remove(QLatin1Char('&'));
    it = dictionary->exact.find(key.toUtf8().toStdString());
    if (it != dictionary->exact.end()) return QString::fromUtf8(it->second) + suffix;
    auto folded = dictionary->folded.find(normalize(key).toUtf8().toStdString());
    return folded == dictionary->folded.end()
        ? QString() : QString::fromUtf8(folded->second) + suffix;
}

QString translateText(const QString& raw)
{
    if (!g_enabled.load(std::memory_order_acquire)) return {};
    return lookupDictionary(raw);
}

int __fastcall hookedFontAdvance(const QFontMetrics* metrics, const QString& text, int length)
{
    const QString translated = translateText(text);
    if (translated.isEmpty()) return g_fontAdvance(metrics, text, length);
    const int pad = std::max(0, g_fontAdvance(metrics, QStringLiteral("中"), -1)) / 2;
    return g_fontAdvance(metrics, translated, -1) + pad;
}

int __fastcall hookedFontAdvanceOpt(const QFontMetrics* metrics, const QString& text,
                                    const QTextOption& option)
{
    const QString translated = translateText(text);
    if (translated.isEmpty()) return g_fontAdvanceOpt(metrics, text, option);
    const int pad = std::max(0, g_fontAdvance(metrics, QStringLiteral("中"), -1)) / 2;
    return g_fontAdvanceOpt(metrics, translated, option) + pad;
}

qreal __fastcall hookedFontAdvanceF(const QFontMetricsF* metrics, const QString& text, int length)
{
    const QString translated = translateText(text);
    if (translated.isEmpty()) return g_fontAdvanceF(metrics, text, length);
    const qreal pad = std::max<qreal>(0.0, g_fontAdvanceF(metrics, QStringLiteral("中"), -1)) * 0.5;
    return g_fontAdvanceF(metrics, translated, -1) + pad;
}

qreal __fastcall hookedFontAdvanceFOpt(const QFontMetricsF* metrics, const QString& text,
                                       const QTextOption& option)
{
    const QString translated = translateText(text);
    if (translated.isEmpty()) return g_fontAdvanceFOpt(metrics, text, option);
    const qreal pad = std::max<qreal>(0.0, g_fontAdvanceF(metrics, QStringLiteral("中"), -1)) * 0.5;
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
    return result;
}

void __fastcall hookedAddTextLayout(
    void* node, const QPointF& position, QTextLayout* layout, const QColor& color,
    int style, const QColor& styleColor, const QColor& anchorColor,
    const QColor& selectionColor, const QColor& selectedTextColor,
    int selectionStart, int selectionEnd, int lineStart, int lineCount)
{
    if (!layout || !g_originalAddTextLayout) return;
    try {
        const QString replacement = translateText(layout->text());
        if (!replacement.isEmpty()) {
            auto display = makeDisplayLayout(layout, replacement);
            if (display && display->lineCount() > 0) {
                g_originalAddTextLayout(node, position, display.get(), color, style,
                    styleColor, anchorColor, selectionColor, selectedTextColor,
                    -1, -1, 0, -1);
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

QString escapeJson(QString text)
{
    text.replace(QLatin1Char('\\'), QLatin1String("\\\\"));
    text.replace(QLatin1Char('"'), QLatin1String("\\\""));
    text.replace(QLatin1Char('\n'), QLatin1String("\\n"));
    text.replace(QLatin1Char('\r'), QLatin1String("\\r"));
    text.replace(QLatin1Char('\t'), QLatin1String("\\t"));
    return text;
}

void collectModelDeep(QAbstractItemModel* model, const QModelIndex& parent,
                      int depth, int& budget, QSet<QString>& entries)
{
    if (!model || depth > 64 || budget <= 0) return;
    const int rows = model->rowCount(parent);
    const int columns = model->columnCount(parent);
    for (int row = 0; row < rows && budget > 0; ++row) {
        const QModelIndex branch = model->index(row, 0, parent);
        for (int column = 0; column < columns && budget > 0; ++column) {
            const QModelIndex index = model->index(row, column, parent);
            const QString text = model->data(index, Qt::DisplayRole).toString();
            if (looksTranslatable(text) && lookupDictionary(text).isEmpty())
                entries.insert(text.trimmed());
            --budget;
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
    QSet<QString> entries;
    int modelBudget = 200000;
    std::function<void(QObject*)> visit = [&](QObject* object) {
        if (!object || visited.contains(object)) return;
        visited.insert(object);

        const QString className = QString::fromLatin1(object->metaObject()->className());
        const bool isEditor = className.contains(QLatin1String("TextInput"), Qt::CaseInsensitive) ||
                              className.contains(QLatin1String("TextEdit"), Qt::CaseInsensitive) ||
                              className.contains(QLatin1String("LineEdit"), Qt::CaseInsensitive);
        if (!isEditor) {
            for (const char* property : displayProperties) {
                const QVariant value = object->property(property);
                if (!value.isValid() || !value.canConvert<QString>()) continue;
                const QString text = value.toString();
                if (looksTranslatable(text) && lookupDictionary(text).isEmpty())
                    entries.insert(text.trimmed());
            }
        }

        const QVariant modelValue = object->property("model");
        if (modelValue.isValid()) {
            if (auto* model = qobject_cast<QAbstractItemModel*>(modelValue.value<QObject*>()))
                collectModelDeep(model, QModelIndex(), 0, modelBudget, entries);
        }
        for (QObject* child : object->children()) visit(child);
        if (auto* item = qobject_cast<QQuickItem*>(object))
            for (QQuickItem* child : item->childItems()) visit(child);
    };

    for (QWindow* window : QGuiApplication::allWindows()) visit(window);
    return entries;
}

void dumpUntranslated()
{
    // Mari-style deep capture: read every current Quick control and recursively
    // descend item models, including nodes hidden under collapsed controls.
    const QSet<QString> captured = collectQuickUiUntranslated();
    wchar_t desktop[MAX_PATH] = {};
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_DESKTOPDIRECTORY, nullptr, SHGFP_TYPE_CURRENT, desktop))) return;
    const QString path = QString::fromWCharArray(desktop) +
                         QLatin1String("/Cascadeur_untranslated_zh.json");
    QJsonObject merged;
    QFile previous(path);
    if (previous.open(QIODevice::ReadOnly)) {
        const QJsonDocument old = QJsonDocument::fromJson(previous.readAll());
        if (old.isObject())
            merged = old.object().value(QLatin1String("translations")).toObject();
    }
    const QList<QString> entries = captured.values();
    int added = 0;
    for (const QString& entry : entries) {
        if (!merged.contains(entry)) { merged.insert(entry, QString()); ++added; }
    }
    if (added > 0) {
        QStringList keys = merged.keys();
        std::sort(keys.begin(), keys.end(), [](const QString& a, const QString& b) {
            return a.compare(b, Qt::CaseInsensitive) < 0;
        });
        QSaveFile file(path);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) return;
        QTextStream out(&file); out.setEncoding(QStringConverter::Utf8);
    out << "{\n  \"$schema\": \"sp-translation-v1\",\n  \"id\": \"cascadeur-untranslated\",\n"
               "  \"language\": \"zh-CN\",\n"
               "  \"description\": \"Shift+~ 增量采集的未翻译词条（待填写）\",\n"
               "  \"translations\": {\n";
        for (qsizetype i = 0; i < keys.size(); ++i) {
            const QString& key = keys.at(i);
            out << "    \"" << escapeJson(key) << "\": \""
                << escapeJson(merged.value(key).toString()) << "\""
                << (i + 1 == keys.size() ? "\n" : ",\n");
        }
        out << "  }\n}\n";
        out.flush();
        if (!file.commit()) return;
    }
    const int total = merged.size();
    QTimer::singleShot(0, QCoreApplication::instance(), [added, total, path]() {
        const QString message = QStringLiteral("已捕获 %1 条（共 %2 条）。\n已保存到桌面：\n%3")
                                    .arg(added).arg(total).arg(path);
        MessageBoxW(nullptr, reinterpret_cast<const wchar_t*>(message.utf16()),
                    L"未翻译词条", MB_OK | MB_ICONINFORMATION);
    });
}

QQuickItem* g_mainMenuBar = nullptr;
QObject* g_languageLink = nullptr;
QObject* g_authorLink = nullptr;
QObject* g_githubLink = nullptr;
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
    std::function<void(QObject*)> refresh = [&](QObject* object) {
        if (!object || visited.contains(object)) return;
        visited.insert(object);

        QEvent fontChange(QEvent::FontChange);
        QCoreApplication::sendEvent(object, &fontChange);
        QEvent layoutRequest(QEvent::LayoutRequest);
        QCoreApplication::sendEvent(object, &layoutRequest);

        // Qt Quick Text caches its QTextLayout and scene-graph glyph node.
        // QQuickText::forceLayout() is the host-exported, QML-visible way to
        // invalidate both without touching the source text property. Item views
        // expose the same method, so invoking it opportunistically also refreshes
        // their delegates and collapsed model content.
        if (object->metaObject()->indexOfMethod("forceLayout()") >= 0)
            QMetaObject::invokeMethod(object, "forceLayout", Qt::DirectConnection);

        if (auto* item = qobject_cast<QQuickItem*>(object)) {
            item->polish();
            item->update();
            for (QQuickItem* child : item->childItems()) refresh(child);
        }
        for (QObject* child : object->children()) refresh(child);
    };
    for (QWindow* window : QGuiApplication::allWindows()) refresh(window);

    adjustMenuWidths(g_mainMenuBar);
    if (g_mainMenuBar) {
        QEvent styleChange(QEvent::StyleChange);
        QCoreApplication::sendEvent(g_mainMenuBar, &styleChange);
        g_mainMenuBar->polish();
        g_mainMenuBar->update();
    }
    repaintAll();
    QTimer::singleShot(0, QCoreApplication::instance(), [] {
        if (g_mainMenuBar) adjustMenuWidths(g_mainMenuBar);
        repaintAll();
    });
}

void openHotkeySettings()
{
    using TaskDialogIndirectFn = HRESULT (WINAPI *)(
        const TASKDIALOGCONFIG*, int*, int*, BOOL*);
    HMODULE controls = GetModuleHandleW(L"comctl32.dll");
    const auto taskDialog = controls
        ? reinterpret_cast<TaskDialogIndirectFn>(
              GetProcAddress(controls, "TaskDialogIndirect"))
        : nullptr;
    if (!taskDialog) {
        MessageBoxW(nullptr, L"当前系统组件不支持快捷键设置窗口，继续使用 F3。",
                    L"Cascadeur 中文补丁", MB_OK | MB_ICONINFORMATION);
        return;
    }
    TASKDIALOG_BUTTON choices[11]{};
    const wchar_t* labels[] = {
        L"F2", L"F3", L"F4", L"F5", L"F6", L"F7",
        L"F8", L"F9", L"F10", L"F11", L"F12"
    };
    for (int i = 0; i < 11; ++i) {
        choices[i].nButtonID = VK_F2 + i;
        choices[i].pszButtonText = labels[i];
    }
    TASKDIALOGCONFIG config{};
    config.cbSize = sizeof(config);
    config.dwFlags = TDF_POSITION_RELATIVE_TO_WINDOW;
    config.dwCommonButtons = TDCBF_OK_BUTTON | TDCBF_CANCEL_BUTTON;
    config.pszWindowTitle = L"Cascadeur 中文补丁";
    config.pszMainInstruction = L"设置中/英切换快捷键";
    config.pszContent = L"选择一个功能键。Shift + ~ 始终用于未翻译词条嗅探。";
    config.cRadioButtons = 11;
    config.pRadioButtons = choices;
    config.nDefaultRadioButton = int(g_toggleVk.load(std::memory_order_acquire));
    int button = IDCANCEL;
    int selected = config.nDefaultRadioButton;
    if (SUCCEEDED(taskDialog(&config, &button, &selected, nullptr)) &&
        button == IDOK && selected >= VK_F2 && selected <= VK_F12)
        g_toggleVk.store(UINT(selected), std::memory_order_release);
}

// Cascadeur's native/3D viewport does not always forward keyboard input as a
// QKeyEvent. Intercept the Windows message stream as Mari does, so the
// shortcuts work regardless of which viewport or control currently has focus.
class NativeKeyFilter final : public QAbstractNativeEventFilter {
public:
    bool shiftDown_ = false;
    UINT toggleHandled_ = 0;
    bool tildeHandled_ = false;

    bool nativeEventFilter(const QByteArray& eventType, void* message,
                           qintptr* result) override {
        Q_UNUSED(eventType);
        Q_UNUSED(result);
        if (!message) return false;

        auto* msg = static_cast<MSG*>(message);
        const bool isDown = msg->message == WM_KEYDOWN ||
                            msg->message == WM_SYSKEYDOWN;
        const bool isUp = msg->message == WM_KEYUP ||
                          msg->message == WM_SYSKEYUP;
        if (!isDown && !isUp) return false;

        const UINT vk = static_cast<UINT>(msg->wParam);
        if (vk == VK_SHIFT || vk == VK_LSHIFT || vk == VK_RSHIFT) {
            shiftDown_ = isDown;
            return false;
        }
        if (isUp && toggleHandled_ && vk == toggleHandled_) {
            toggleHandled_ = 0;
            return true;
        }
        if (isUp && vk == VK_OEM_3 && tildeHandled_) {
            tildeHandled_ = false;
            return true;
        }
        if (!isDown || (msg->lParam & 0x40000000) != 0)
            return false;

        if (shiftDown_ && vk == VK_OEM_3) {
            tildeHandled_ = true;
            dumpUntranslated();
            return true;
        }
        const UINT toggleVk = g_toggleVk.load(std::memory_order_acquire);
        if (!shiftDown_ && vk == toggleVk) {
            toggleHandled_ = vk;
            toggleTranslation();
            return true;
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
        if (watched == g_languageLink && event &&
            event->type() == QEvent::MouseButtonRelease) {
            auto* mouse = static_cast<QMouseEvent*>(event);
            if (mouse->button() == Qt::LeftButton) {
                toggleTranslation();
                return true;
            }
            if (mouse->button() == Qt::RightButton) {
                openHotkeySettings();
                return true;
            }
        }
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
        const QString translated = translateText(source);
        if (!g_enabled.load() || translated.isEmpty()) {
            item->setImplicitWidth(item->property(originalName).toReal());
            item->setWidth(item->property(originalWidthName).toReal());
            continue;
        }
        const QFont font = item->property("font").value<QFont>();
        const QFontMetricsF metrics(font);
        const qreal left = item->property("leftPadding").toReal();
        const qreal right = item->property("rightPadding").toReal();
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
        color: control.hovered || control.highlighted ? "#212121" : "transparent"
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
            auto* helpItem = qobject_cast<QQuickItem*>(object);
            QQuickItem* parent = qobject_cast<QQuickItem*>(object->parent());
            if (!parent) continue;
            g_languageLink = createLanguageMenuItem(parent, object);
            g_authorLink = createMenuLink(parent, object,
                QStringLiteral("Bilibili神说要凑数汉化"),
                QStringLiteral("https://space.bilibili.com/281243426?spm_id_from=333.1007.0.0"),
                QStringLiteral("qt_chinese_author"));
            g_githubLink = createMenuLink(parent, object,
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
    QTimer::singleShot(0, QCoreApplication::instance(), [] {
        scheduled = false;
        installAuthorLinks();
    });
}

void installLifecycleFilter(QCoreApplication* app) {
    if (!app || g_filter) return;
    g_filter = new ShortcutFilter;
    g_filter->moveToThread(app->thread());
    app->installEventFilter(g_filter);
    if (auto* dispatcher = QAbstractEventDispatcher::instance(app->thread()))
        dispatcher->installNativeEventFilter(new NativeKeyFilter);
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
    capabilities.insert(QStringLiteral("Dictionary"),
        std::atomic_load_explicit(&g_dictionary, std::memory_order_acquire)->exact.empty()
            ? QStringLiteral("FAILED") : QStringLiteral("OK"));
    capabilities.insert(QStringLiteral("TextLayoutHook"),
        g_originalAddTextLayout ? QStringLiteral("OK") : QStringLiteral("DISABLED"));
    const int fontCount = int(bool(g_fontAdvance)) + int(bool(g_fontAdvanceOpt)) +
                          int(bool(g_fontAdvanceF)) + int(bool(g_fontAdvanceFOpt));
    capabilities.insert(QStringLiteral("FontMetricsHook"),
        fontCount == 4 ? QStringLiteral("PARTIAL") :
        (fontCount ? QStringLiteral("PARTIAL") : QStringLiteral("DISABLED")));
    QJsonObject root;
    root.insert(QStringLiteral("product"), QStringLiteral("Cascadeur"));
    root.insert(QStringLiteral("qtVersion"), QString::fromLatin1(qVersion()));
    root.insert(QStringLiteral("capabilities"), capabilities);
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

bool installHook()
{
    HMODULE quick = GetModuleHandleW(L"Qt6Quick.dll");
    if (!quick) return false;
    constexpr const char* symbol = "?addTextLayout@QQuickTextNode@@QEAAXAEBVQPointF@@PEAVQTextLayout@@AEBVQColor@@W4TextStyle@QQuickText@@2222HHHH@Z";
    g_originalAddTextLayout = reinterpret_cast<AddTextLayoutFn>(GetProcAddress(quick, symbol));
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
    if (!isExecutableAddress(reinterpret_cast<void*>(g_originalAddTextLayout)) ||
        DetourTransactionBegin() != NO_ERROR) return false;
    if (DetourUpdateThread(GetCurrentThread()) != NO_ERROR ||
        DetourAttach(reinterpret_cast<PVOID*>(&g_originalAddTextLayout),
                     reinterpret_cast<PVOID>(hookedAddTextLayout)) != NO_ERROR) {
        DetourTransactionAbort(); return false;
    }
    auto attachOptional = [](auto& original, auto replacement) {
        if (!isExecutableAddress(reinterpret_cast<void*>(original))) {
            original = nullptr;
            return true;
        }
        return DetourAttach(reinterpret_cast<PVOID*>(&original),
                            reinterpret_cast<PVOID>(replacement)) == NO_ERROR;
    };
    if (!attachOptional(g_fontAdvance, hookedFontAdvance) ||
        !attachOptional(g_fontAdvanceOpt, hookedFontAdvanceOpt) ||
        !attachOptional(g_fontAdvanceF, hookedFontAdvanceF) ||
        !attachOptional(g_fontAdvanceFOpt, hookedFontAdvanceFOpt)) {
        DetourTransactionAbort();
        return false;
    }
    return DetourTransactionCommit() == NO_ERROR;
}

void uninstallHook()
{
    if (!g_originalAddTextLayout || DetourTransactionBegin() != NO_ERROR) return;
    DetourUpdateThread(GetCurrentThread());
    DetourDetach(reinterpret_cast<PVOID*>(&g_originalAddTextLayout), reinterpret_cast<PVOID>(hookedAddTextLayout));
    if (g_fontAdvance) DetourDetach(reinterpret_cast<PVOID*>(&g_fontAdvance), reinterpret_cast<PVOID>(hookedFontAdvance));
    if (g_fontAdvanceOpt) DetourDetach(reinterpret_cast<PVOID*>(&g_fontAdvanceOpt), reinterpret_cast<PVOID>(hookedFontAdvanceOpt));
    if (g_fontAdvanceF) DetourDetach(reinterpret_cast<PVOID*>(&g_fontAdvanceF), reinterpret_cast<PVOID>(hookedFontAdvanceF));
    if (g_fontAdvanceFOpt) DetourDetach(reinterpret_cast<PVOID*>(&g_fontAdvanceFOpt), reinterpret_cast<PVOID>(hookedFontAdvanceFOpt));
    DetourTransactionCommit();
}

void runOnGuiThread(QCoreApplication* app) {
    // Phase 1: the Qt application exists and this runs on its GUI thread.
    loadDictionaries();
    if (installHook()) {
        installLifecycleFilter(app);
        QTimer::singleShot(2000, app, [] { writeWindowDiagnostics(); });
    }
}

DWORD WINAPI initialize(void*)
{
    QCoreApplication* app = nullptr;
    for (int attempt = 0;
         attempt < CascadeurHookLifecycle::kApplicationWaitAttempts; ++attempt) {
        app = QCoreApplication::instance();
        if (app && qobject_cast<QGuiApplication*>(app) &&
            QString::fromLatin1(qVersion()).startsWith(QLatin1String("6."))) break;
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
