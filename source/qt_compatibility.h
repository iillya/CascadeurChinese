#pragma once

// Explicitly tested runtime pairs, not a blanket Qt 6/private-ABI guarantee.
// No Qt headers: shared by the launcher, installer and injected hook.
#include <windows.h>
#include <cstring>
#include <string>
#include <vector>

namespace CascadeurQtCompatibility {
constexpr bool supported(unsigned major, unsigned minor, unsigned patch) {
    return major == 6 && minor == 5 && (patch == 1 || patch == 3);
}

inline bool supportedRuntime(const char* version) {
    return version && (std::strcmp(version, "6.5.1") == 0 || std::strcmp(version, "6.5.3") == 0);
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

inline bool supportedDirectory(const std::wstring& root) {
    const DWORD version = fileVersion(root + L"\\Qt6Core.dll");
    if (!version) return false;
    // Individually supported DLLs must not be mixed across patch releases.
    for (const auto* module : modules)
        if (fileVersion(root + L"\\" + module) != version) return false;
    return GetFileAttributesW((root + L"\\Qt5Core.dll").c_str()) == INVALID_FILE_ATTRIBUTES;
}
}
