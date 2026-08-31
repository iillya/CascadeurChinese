// Standalone reconstruction of four TextUtils routines from disassembly.
// Default: QtCore reconstruction. --original: isolated, hash-locked native calls.
// Never attaches to the user's Cascadeur process or opens a scene.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <cstdio>
#include <QtCore/QCryptographicHash>
#include <QtCore/QFile>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QRegularExpression>
#include <QtCore/QSaveFile>
#include <QtCore/QString>

static QString camel(QString text) {
    if (text.isEmpty()) return text;
    static const QRegularExpression first(QStringLiteral("(.)([A-Z][a-z]+)"));
    static const QRegularExpression second(QStringLiteral("([a-z])([A-Z])"));
    text.replace(first, QStringLiteral("\\1 \\2"));
    text.replace(second, QStringLiteral("\\1 \\2"));
    text = text.toLower();
    text.replace(0, 1, text.at(0).toUpper());
    return text;
}

static QString snake(QString text) {
    if (text.isEmpty()) return text;
    text.replace(QStringLiteral("_"), QStringLiteral(" "), Qt::CaseSensitive);
    text.replace(0, 1, text.at(0).toUpper());
    return text;
}

static QString any(QString text) { return camel(snake(std::move(text))); }

static QString domain(const QString& text) {
    const auto pieces = text.split(QRegularExpression(QStringLiteral("([a-zA-Z]+)(::)")), Qt::KeepEmptyParts);
    return camel(pieces.isEmpty() ? QString() : pieces.last());
}

int main(int argc, char** argv) {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    if (argc != 3 && argc != 4) return 2;
    const bool original = argc == 4 && QByteArray(argv[3]) == "--original";
    if (argc == 4 && !original) return 2;
    using MetaCall = void (__cdecl*)(void*, int, int, void**);
    MetaCall metaCall = nullptr;
    if (original) {
        const QString path = QStringLiteral("C:/Program Files/Cascadeur/presenter_lib.dll");
        QFile dll(path);
        if (!dll.open(QIODevice::ReadOnly)) return 7;
        QCryptographicHash hash(QCryptographicHash::Sha256);
        if (!hash.addData(&dll) || hash.result().toHex().toUpper() !=
            "2B2FD4043F1F83B92338DC4FE7E0F0866F3C7C554A3BF5D92387A112FA1470F1") return 8;
        dll.close();
        AddDllDirectory(L"C:\\Program Files\\Cascadeur");
        const auto module = LoadLibraryExW(reinterpret_cast<const wchar_t*>(path.utf16()), nullptr,
            LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        if (!module) {
            std::fprintf(stderr, "presenter_lib.dll LoadLibrary failed: Win32 error %lu\n", GetLastError());
            return 9;
        }
        const auto base = reinterpret_cast<const char*>(module);
        if (QByteArray(base + 0xba4c18) != "utils::TextUtils") return 10;
        metaCall = reinterpret_cast<MetaCall>(const_cast<char*>(base) + 0x50810);
        if (*reinterpret_cast<MetaCall const*>(base + 0xba4860) != metaCall) return 11;
    }
    QFile input(QString::fromLocal8Bit(argv[1]));
    if (!input.open(QIODevice::ReadOnly)) return 3;
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(input.readAll(), &error);
    if (error.error != QJsonParseError::NoError || !document.isArray()) return 4;
    QJsonArray rows;
    for (const auto& entry : document.array()) {
        if (!entry.isString()) return 5;
        const QString text = entry.toString();
        auto invoke = [&](int method) {
            QString result;
            void* arguments[] = {&result, const_cast<QString*>(&text)};
            metaCall(nullptr, 0 /* QMetaObject::InvokeMetaMethod */, method, arguments);
            return result;
        };
        rows.append(QJsonObject{{QStringLiteral("input"), text},
            {QStringLiteral("camel"), original ? invoke(0) : camel(text)},
            {QStringLiteral("snake"), original ? invoke(1) : snake(text)},
            {QStringLiteral("any"), original ? invoke(2) : any(text)},
            {QStringLiteral("domain"), original ? invoke(3) : domain(text)}});
    }
    wchar_t modulePath[32768] = {};
    GetModuleFileNameW(GetModuleHandleW(L"Qt6Core.dll"), modulePath, 32768);
    QJsonObject report{{QStringLiteral("qtVersion"), QString::fromLatin1(qVersion())},
        {QStringLiteral("qtCorePath"), QString::fromWCharArray(modulePath)},
        {QStringLiteral("implementation"), original ? QStringLiteral("original presenter_lib.dll static_metacall")
            : QStringLiteral("reconstructed; not a call into presenter_lib.dll")},
        {QStringLiteral("rows"), rows}};
    QSaveFile output(QString::fromLocal8Bit(argv[2]));
    const auto bytes = QJsonDocument(report).toJson();
    if (!output.open(QIODevice::WriteOnly) || output.write(bytes) != bytes.size() || !output.commit()) return 6;
    return 0;
}
