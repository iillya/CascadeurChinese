#pragma once

#include <windows.h>
#include <string>

namespace CascadeurAssociation {
constexpr wchar_t kExtension[] = L"Software\\Classes\\.casc";
constexpr wchar_t kClass[] = L"Software\\Classes\\CascadeurChinese.casc";
constexpr wchar_t kBackup[] = L"Software\\CascadeurChineseLocalizer\\AssocBackup";
constexpr wchar_t kProgId[] = L"CascadeurChinese.casc";

struct Key {
    HKEY value = nullptr;
    ~Key() { if (value) RegCloseKey(value); }
    Key() = default;
    Key(const Key&) = delete;
    Key& operator=(const Key&) = delete;
};

inline LSTATUS readString(HKEY root, const wchar_t* path, const wchar_t* name,
                          std::wstring& result) {
    DWORD size = 0;
    LSTATUS status = RegGetValueW(root, path, name, RRF_RT_REG_SZ, nullptr, nullptr, &size);
    if (status != ERROR_SUCCESS) return status;
    std::wstring buffer(size / sizeof(wchar_t) + 1, L'\0');
    status = RegGetValueW(root, path, name, RRF_RT_REG_SZ, nullptr, buffer.data(), &size);
    if (status == ERROR_SUCCESS) result.assign(buffer.c_str());
    return status;
}

inline bool writeString(HKEY root, const wchar_t* path, const wchar_t* name,
                         const std::wstring& text) {
    Key key;
    if (RegCreateKeyExW(root, path, 0, nullptr, 0, KEY_SET_VALUE, nullptr,
                        &key.value, nullptr) != ERROR_SUCCESS) return false;
    return RegSetValueExW(key.value, name, 0, REG_SZ,
        reinterpret_cast<const BYTE*>(text.c_str()),
        static_cast<DWORD>((text.size() + 1) * sizeof(wchar_t))) == ERROR_SUCCESS;
}

inline bool install(HKEY user, const std::wstring& launcher) {
    Key backup;
    if (RegCreateKeyExW(user, kBackup, 0, nullptr, 0, KEY_READ | KEY_WRITE,
                        nullptr, &backup.value, nullptr) != ERROR_SUCCESS) return false;
    DWORD complete = 0, size = sizeof(complete);
    const LSTATUS completeStatus = RegGetValueW(backup.value, nullptr, L"backup_complete",
        RRF_RT_REG_DWORD, nullptr, &complete, &size);
    if (completeStatus != ERROR_SUCCESS && completeStatus != ERROR_FILE_NOT_FOUND) return false;
    if (!complete) {
        std::wstring previous;
        const LSTATUS status = readString(user, kExtension, nullptr, previous);
        if (status != ERROR_SUCCESS && status != ERROR_FILE_NOT_FOUND) return false;
        // Without a backup we cannot guess the pre-existing association.
        if (status == ERROR_SUCCESS && previous == kProgId) return false;
        const DWORD hadDefault = status == ERROR_SUCCESS ? 1 : 0;
        if (hadDefault && !writeString(backup.value, L"", L"casc_default", previous)) return false;
        if (RegSetValueExW(backup.value, L"casc_had_default", 0, REG_DWORD,
            reinterpret_cast<const BYTE*>(&hadDefault), sizeof(hadDefault)) != ERROR_SUCCESS) return false;
        complete = 1;
        if (RegSetValueExW(backup.value, L"backup_complete", 0, REG_DWORD,
            reinterpret_cast<const BYTE*>(&complete), sizeof(complete)) != ERROR_SUCCESS) return false;
    }
    // Create the command before making it the default; a failed registration
    // must not leave .casc pointing at a class without an open command.
    const std::wstring command = std::wstring(kClass) + L"\\shell\\open\\command";
    if (!writeString(user, command.c_str(), nullptr, L"\"" + launcher + L"\" \"%1\"") ||
        !writeString(user, kClass, nullptr, L"Cascadeur 中文版工程") ||
        !writeString(user, (std::wstring(kClass) + L"\\DefaultIcon").c_str(), nullptr,
                     L"\"" + launcher + L"\",0")) return false;
    Key openWith;
    if (RegCreateKeyExW(user, (std::wstring(kExtension) + L"\\OpenWithProgids").c_str(),
        0, nullptr, 0, KEY_SET_VALUE, nullptr, &openWith.value, nullptr) != ERROR_SUCCESS ||
        RegSetValueExW(openWith.value, kProgId, 0, REG_NONE, nullptr, 0) != ERROR_SUCCESS) return false;
    return writeString(user, kExtension, nullptr, kProgId);
}

inline bool uninstall(HKEY user, const std::wstring& launcher) {
    std::wstring command;
    const LSTATUS commandStatus = readString(user,
        (std::wstring(kClass) + L"\\shell\\open\\command").c_str(), nullptr, command);
    if (commandStatus != ERROR_SUCCESS && commandStatus != ERROR_FILE_NOT_FOUND) return false;
    // Another installed copy owns this registration; leave it alone.
    if (commandStatus == ERROR_SUCCESS && command != L"\"" + launcher + L"\" \"%1\"") return true;
    std::wstring current;
    const LSTATUS currentStatus = readString(user, kExtension, nullptr, current);
    if (currentStatus != ERROR_SUCCESS && currentStatus != ERROR_FILE_NOT_FOUND) return false;
    if (current == kProgId) {
        DWORD hadDefault = 0, size = sizeof(hadDefault);
        if (RegGetValueW(user, kBackup, L"casc_had_default", RRF_RT_REG_DWORD,
            nullptr, &hadDefault, &size) != ERROR_SUCCESS) return false;
        if (hadDefault) {
            std::wstring previous;
            if (readString(user, kBackup, L"casc_default", previous) != ERROR_SUCCESS ||
                previous == kProgId || !writeString(user, kExtension, nullptr, previous)) return false;
        } else {
            Key extension;
            if (RegOpenKeyExW(user, kExtension, 0, KEY_SET_VALUE, &extension.value) != ERROR_SUCCESS ||
                RegDeleteValueW(extension.value, nullptr) != ERROR_SUCCESS) return false;
        }
    }
    // The user may have selected a different application since installation.
    // In that case only remove our class and backup, never that new default.
    Key openWith;
    if (RegOpenKeyExW(user, (std::wstring(kExtension) + L"\\OpenWithProgids").c_str(),
        0, KEY_SET_VALUE, &openWith.value) == ERROR_SUCCESS) {
        const LSTATUS status = RegDeleteValueW(openWith.value, kProgId);
        if (status != ERROR_SUCCESS && status != ERROR_FILE_NOT_FOUND) return false;
    }
    for (const wchar_t* path : {kClass, kBackup}) {
        const LSTATUS status = RegDeleteTreeW(user, path);
        if (status != ERROR_SUCCESS && status != ERROR_FILE_NOT_FOUND) return false;
    }
    return true;
}
} // namespace CascadeurAssociation
