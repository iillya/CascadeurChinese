#pragma once

// Qt ABI-family capability checks shared by the launcher, installer and hook.
// Private Qt Quick entry points are probed separately by the hook before use.
#include <windows.h>
#include <string>
#include <vector>

namespace CascadeurQtCompatibility {
constexpr bool supported(unsigned major, unsigned, unsigned) {
    return major == 6;
}

inline bool supportedRuntime(const char* version) {
    return version && version[0] == '6' && version[1] == '.';
}

inline DWORD fileVersion(const std::wstring& path) {
    DWORD unused = 0;
    const DWORD size = GetFileVersionInfoSizeW(path.c_str(), &unused);
    if (!size || size > 1024 * 1024) return 0;
    std::vector<BYTE> bytes(size);
    if (!GetFileVersionInfoW(path.c_str(), 0, size, bytes.data())) return 0;
    VS_FIXEDFILEINFO* version = nullptr;
    UINT length = 0;
    if (!VerQueryValueW(bytes.data(), L"\\", reinterpret_cast<void**>(&version), &length) ||
        length < sizeof(*version) || version->dwSignature != 0xfeef04bd) return 0;
    const unsigned major = HIWORD(version->dwFileVersionMS);
    const unsigned minor = LOWORD(version->dwFileVersionMS);
    const unsigned patch = HIWORD(version->dwFileVersionLS);
    return supported(major, minor, patch) ? (major << 16) | (minor << 8) | patch : 0;
}

constexpr const wchar_t* modules[] = {L"Qt6Core.dll", L"Qt6Gui.dll", L"Qt6Qml.dll", L"Qt6Quick.dll"};

inline bool hasRequiredQuickExports(const std::wstring& path) {
    HMODULE quick = LoadLibraryExW(path.c_str(), nullptr,
                                   DONT_RESOLVE_DLL_REFERENCES);
    if (!quick) return false;
    constexpr const char* required[] = {
        "?addTextLayout@QQuickTextNode@@QEAAXAEBVQPointF@@PEAVQTextLayout@@AEBVQColor@@W4TextStyle@QQuickText@@2222HHHH@Z",
        "??0QQuickTextNode@@QEAA@PEAVQQuickItem@@@Z",
        "??1QQuickTextNode@@UEAA@XZ",
    };
    bool available = true;
    for (const char* symbol : required) {
        if (!GetProcAddress(quick, symbol)) {
            available = false;
            break;
        }
    }
    FreeLibrary(quick);
    return available;
}

inline bool supportedDirectory(const std::wstring& root) {
    const DWORD version = fileVersion(root + L"\\Qt6Core.dll");
    if (!version) return false;
    // A mixed Qt directory is not one coherent ABI family.
    for (const auto* module : modules)
        if (fileVersion(root + L"\\" + module) != version) return false;
    return GetFileAttributesW((root + L"\\Qt5Core.dll").c_str()) == INVALID_FILE_ATTRIBUTES &&
           hasRequiredQuickExports(root + L"\\Qt6Quick.dll");
}
}
