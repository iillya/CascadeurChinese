// Cascadeur Chinese Localizer - pure display-layer Qt Quick hook.
// QML properties and data models remain untouched. Translation is applied to
// a temporary QTextLayout copy only while scene-graph glyphs are generated.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include "hook_lifecycle.h"
#include <shlobj.h>
#include <QtCore/QCoreApplication>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QMutex>
#include <QtCore/QObject>
#include <QtCore/QSet>
#include <QtCore/QStringConverter>
#include <QtCore/QTextStream>
#include <QtCore/QTimer>
#include <QtGui/QGuiApplication>
#include <QtGui/QFontMetrics>
#include <QtGui/QKeyEvent>
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
std::unordered_map<std::string, std::string> g_exact;
std::unordered_map<std::string, std::string> g_folded;
std::mutex g_dictionaryMutex;
QSet<QString> g_untranslated;
QMutex g_untranslatedMutex;

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
    text.remove(QChar(0x200b)); text.remove(QChar(0x200d)); text.remove(QChar(0xfeff));
    text = text.simplified();
    while (text.endsWith(QChar(0x2026))) text.chop(1);
    while (text.endsWith(QLatin1String("..."))) text.chop(3);
    if (text.endsWith(QLatin1String(" *"))) text.chop(2);
    text.replace(QLatin1Char('_'), QLatin1Char(' '));
    text.remove(QLatin1Char('&'));
    text = text.normalized(QString::NormalizationForm_C).toCaseFolded();
    text.remove(QLatin1Char(' '));
    return text;
}

bool looksTranslatable(const QString& source)
{
    const QString text = source.trimmed();
    if (text.size() < 2 || text.size() > 500 || containsCjk(text)) return false;
    bool letter = false;
    for (QChar ch : text) if (ch.isLetter()) { letter = true; break; }
    if (!letter || text.contains(QLatin1String(":\\"))) return false;
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
    std::lock_guard<std::mutex> lock(g_dictionaryMutex);
    g_exact = std::move(exact);
    g_folded = std::move(folded);
}

QString translateText(const QString& raw)
{
    if (!g_enabled.load(std::memory_order_relaxed)) return {};
    QString key = raw.trimmed();
    if (key.isEmpty() || containsCjk(key)) return {};
    std::lock_guard<std::mutex> lock(g_dictionaryMutex);
    auto it = g_exact.find(key.toUtf8().toStdString());
    if (it != g_exact.end()) return QString::fromUtf8(it->second);
    QString suffix;
    if (key.endsWith(QLatin1Char('*'))) { key.chop(1); key = key.trimmed(); suffix = QLatin1Char('*'); }
    it = g_exact.find(key.toUtf8().toStdString());
    if (it != g_exact.end()) return QString::fromUtf8(it->second) + suffix;
    it = g_folded.find(normalize(key).toUtf8().toStdString());
    return it == g_folded.end() ? QString() : QString::fromUtf8(it->second) + suffix;
}

int __fastcall hookedFontAdvance(const QFontMetrics* metrics, const QString& text, int length)
{
    const QString translated = translateText(text);
    if (translated.isEmpty()) return g_fontAdvance(metrics, text, length);
    if (length >= 0) length = std::min(length, int(translated.size()));
    const int pad = std::max(0, g_fontAdvance(metrics, QStringLiteral("中"), -1));
    return g_fontAdvance(metrics, translated, length) + pad;
}

int __fastcall hookedFontAdvanceOpt(const QFontMetrics* metrics, const QString& text,
                                    const QTextOption& option)
{
    const QString translated = translateText(text);
    if (translated.isEmpty()) return g_fontAdvanceOpt(metrics, text, option);
    const int pad = std::max(0, g_fontAdvance(metrics, QStringLiteral("中"), -1));
    return g_fontAdvanceOpt(metrics, translated, option) + pad;
}

qreal __fastcall hookedFontAdvanceF(const QFontMetricsF* metrics, const QString& text, int length)
{
    const QString translated = translateText(text);
    if (translated.isEmpty()) return g_fontAdvanceF(metrics, text, length);
    if (length >= 0) length = std::min(length, int(translated.size()));
    const qreal pad = std::max<qreal>(0.0, g_fontAdvanceF(metrics, QStringLiteral("中"), -1));
    return g_fontAdvanceF(metrics, translated, length) + pad;
}

qreal __fastcall hookedFontAdvanceFOpt(const QFontMetricsF* metrics, const QString& text,
                                       const QTextOption& option)
{
    const QString translated = translateText(text);
    if (translated.isEmpty()) return g_fontAdvanceFOpt(metrics, text, option);
    const qreal pad = std::max<qreal>(0.0, g_fontAdvanceF(metrics, QStringLiteral("中"), -1));
    return g_fontAdvanceFOpt(metrics, translated, option) + pad;
}

void recordUntranslated(const QString& text)
{
    if (!looksTranslatable(text)) return;
    QMutexLocker lock(&g_untranslatedMutex);
    g_untranslated.insert(text.trimmed());
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
        } else {
            recordUntranslated(layout->text());
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

void dumpUntranslated()
{
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
    QList<QString> entries;
    { QMutexLocker lock(&g_untranslatedMutex); entries = g_untranslated.values(); }
    int added = 0;
    for (const QString& entry : entries) {
        if (!merged.contains(entry)) { merged.insert(entry, QString()); ++added; }
    }
    if (added > 0) {
        QStringList keys = merged.keys();
        std::sort(keys.begin(), keys.end(), [](const QString& a, const QString& b) {
            return a.compare(b, Qt::CaseInsensitive) < 0;
        });
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) return;
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
void adjustMenuWidths(QQuickItem* menuBar);
void scheduleAuthorLinksInstall();

class ShortcutFilter final : public QObject {
public:
    bool eventFilter(QObject* watched, QEvent* event) override {
        // Phase 2: UI objects created after initial installation are handled by
        // lifecycle events instead of a permanent polling timer.
        if (event && (event->type() == QEvent::Show ||
                      event->type() == QEvent::Polish ||
                      event->type() == QEvent::ChildAdded))
            scheduleAuthorLinksInstall();
        if (event && event->type() == QEvent::KeyRelease) {
            auto* key = static_cast<QKeyEvent*>(event);
            if (!key->isAutoRepeat() && key->key() == Qt::Key_F3) {
                g_enabled.store(!g_enabled.load());
                adjustMenuWidths(g_mainMenuBar);
                repaintAll(); return true;
            }
            const bool tildeKey = key->key() == Qt::Key_AsciiTilde ||
                                  key->key() == Qt::Key_QuoteLeft;
            if (!key->isAutoRepeat() && tildeKey &&
                key->modifiers().testFlag(Qt::ShiftModifier)) {
                dumpUntranslated(); return true;
            }
        }
        return QObject::eventFilter(watched, event);
    }
};
ShortcutFilter* g_filter = nullptr;
QObject* g_authorLink = nullptr;
QObject* g_githubLink = nullptr;

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
    property string targetUrl
    palette.buttonText: "#66aaff"
    palette.windowText: "#66aaff"
    palette.text: "#66aaff"
    onClicked: Qt.openUrlExternally(targetUrl)
}
)QML";
    QQmlComponent component(engine);
    component.setData(qml, QUrl(QStringLiteral("cascadeur-chinese-author-link.qml")));
    QQmlContext* context = qmlContext(helpObject);
    QObject* object = component.create(context ? context : engine->rootContext());
    auto* item = qobject_cast<QQuickItem*>(object);
    if (!item) { delete object; return nullptr; }
    object->setObjectName(objectName);
    object->setProperty("text", label);
    object->setProperty("targetUrl", url);
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

bool installAuthorLinks()
{
    if (g_authorLink && g_githubLink) return true;
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
            g_authorLink = createMenuLink(parent, object,
                QStringLiteral("Bilibili神说要凑数汉化"),
                QStringLiteral("https://space.bilibili.com/281243426?spm_id_from=333.1007.0.0"),
                QStringLiteral("cascadeur_chinese_author"));
            g_githubLink = createMenuLink(parent, object,
                QStringLiteral("Github仓库"),
                QStringLiteral("https://github.com/iillya/CascadeurChinese"),
                QStringLiteral("cascadeur_chinese_github"));
            g_mainMenuBar = parent;
            adjustMenuWidths(parent);
            QTimer::singleShot(0, parent, [parent] { adjustMenuWidths(parent); });
            QTimer::singleShot(500, parent, [parent] { adjustMenuWidths(parent); });
            QTimer::singleShot(2000, parent, [parent] { adjustMenuWidths(parent); });
            return g_authorLink && g_githubLink;
        }
    }
    return false;
}

void scheduleAuthorLinksInstall() {
    static bool scheduled = false;
    if (scheduled || (g_authorLink && g_githubLink)) return;
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

bool installHook()
{
    HMODULE quick = GetModuleHandleW(L"Qt6Quick.dll");
    if (!quick) quick = LoadLibraryW(L"Qt6Quick.dll");
    if (!quick) return false;
    constexpr const char* symbol = "?addTextLayout@QQuickTextNode@@QEAAXAEBVQPointF@@PEAVQTextLayout@@AEBVQColor@@W4TextStyle@QQuickText@@2222HHHH@Z";
    g_originalAddTextLayout = reinterpret_cast<AddTextLayoutFn>(GetProcAddress(quick, symbol));
    HMODULE gui = GetModuleHandleW(L"Qt6Gui.dll");
    if (!gui) gui = LoadLibraryW(L"Qt6Gui.dll");
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
    if (!g_originalAddTextLayout || DetourTransactionBegin() != NO_ERROR) return false;
    if (DetourUpdateThread(GetCurrentThread()) != NO_ERROR ||
        DetourAttach(reinterpret_cast<PVOID*>(&g_originalAddTextLayout),
                     reinterpret_cast<PVOID>(hookedAddTextLayout)) != NO_ERROR) {
        DetourTransactionAbort(); return false;
    }
    if (g_fontAdvance) DetourAttach(reinterpret_cast<PVOID*>(&g_fontAdvance), reinterpret_cast<PVOID>(hookedFontAdvance));
    if (g_fontAdvanceOpt) DetourAttach(reinterpret_cast<PVOID*>(&g_fontAdvanceOpt), reinterpret_cast<PVOID>(hookedFontAdvanceOpt));
    if (g_fontAdvanceF) DetourAttach(reinterpret_cast<PVOID*>(&g_fontAdvanceF), reinterpret_cast<PVOID>(hookedFontAdvanceF));
    if (g_fontAdvanceFOpt) DetourAttach(reinterpret_cast<PVOID*>(&g_fontAdvanceFOpt), reinterpret_cast<PVOID>(hookedFontAdvanceFOpt));
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
    if (installHook())
        installLifecycleFilter(app);
}

DWORD WINAPI initialize(void*)
{
    QCoreApplication* app = nullptr;
    for (int attempt = 0;
         attempt < CascadeurHookLifecycle::kApplicationWaitAttempts; ++attempt) {
        app = QCoreApplication::instance();
        if (app) break;
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
    } else if (reason == DLL_PROCESS_DETACH) {
        uninstallHook();
    }
    return TRUE;
}
