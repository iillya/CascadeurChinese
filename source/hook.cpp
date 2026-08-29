// Cascadeur Chinese Localizer - pure display-layer Qt Quick hook.
// QML properties and data models remain untouched. Translation is applied to
// a temporary QTextLayout copy only while scene-graph glyphs are generated.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
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
#include <QtGui/QKeyEvent>
#include <QtGui/QTextLayout>
#include <QtGui/QWindow>
#include "detours/detours.h"
#include <algorithm>
#include <atomic>
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

class ShortcutFilter final : public QObject {
public:
    bool eventFilter(QObject* watched, QEvent* event) override {
        if (event && event->type() == QEvent::KeyRelease) {
            auto* key = static_cast<QKeyEvent*>(event);
            if (!key->isAutoRepeat() && key->key() == Qt::Key_F3) {
                g_enabled.store(!g_enabled.load()); repaintAll(); return true;
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

void installEventFilterWhenReady()
{
    for (int i = 0; i < 600; ++i) {
        if (QCoreApplication* app = QCoreApplication::instance()) {
            QMetaObject::invokeMethod(app, [app]() {
                if (!g_filter) { g_filter = new ShortcutFilter; g_filter->moveToThread(app->thread()); app->installEventFilter(g_filter); }
            }, Qt::QueuedConnection);
            return;
        }
        Sleep(50);
    }
}

bool installHook()
{
    HMODULE quick = GetModuleHandleW(L"Qt6Quick.dll");
    if (!quick) quick = LoadLibraryW(L"Qt6Quick.dll");
    if (!quick) return false;
    constexpr const char* symbol = "?addTextLayout@QQuickTextNode@@QEAAXAEBVQPointF@@PEAVQTextLayout@@AEBVQColor@@W4TextStyle@QQuickText@@2222HHHH@Z";
    g_originalAddTextLayout = reinterpret_cast<AddTextLayoutFn>(GetProcAddress(quick, symbol));
    if (!g_originalAddTextLayout || DetourTransactionBegin() != NO_ERROR) return false;
    if (DetourUpdateThread(GetCurrentThread()) != NO_ERROR ||
        DetourAttach(reinterpret_cast<PVOID*>(&g_originalAddTextLayout),
                     reinterpret_cast<PVOID>(hookedAddTextLayout)) != NO_ERROR) {
        DetourTransactionAbort(); return false;
    }
    return DetourTransactionCommit() == NO_ERROR;
}

void uninstallHook()
{
    if (!g_originalAddTextLayout || DetourTransactionBegin() != NO_ERROR) return;
    DetourUpdateThread(GetCurrentThread());
    DetourDetach(reinterpret_cast<PVOID*>(&g_originalAddTextLayout), reinterpret_cast<PVOID>(hookedAddTextLayout));
    DetourTransactionCommit();
}

DWORD WINAPI initialize(void*)
{
    loadDictionaries();
    if (installHook()) installEventFilterWhenReady();
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
