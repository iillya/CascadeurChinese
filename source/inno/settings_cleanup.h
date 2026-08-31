#pragma once
// Only the plugin's one persisted hotkey file is removed, never the user profile.
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "uuid.lib")

namespace InstallerSettings {
inline std::wstring directory() {
    DWORD session = 0;
    if (!ProcessIdToSessionId(GetCurrentProcessId(), &session)) return {};
    Handle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
    if (snapshot.value == INVALID_HANDLE_VALUE) return {};
    PROCESSENTRY32W item{};
    item.dwSize = sizeof(item);
    if (!Process32FirstW(snapshot.value, &item)) return {};
    do {
        DWORD candidate = 0;
        if (_wcsicmp(item.szExeFile, L"explorer.exe") != 0 ||
            !ProcessIdToSessionId(item.th32ProcessID, &candidate) || candidate != session) continue;
        Handle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, item.th32ProcessID));
        HANDLE raw = nullptr;
        if (!process.value || !OpenProcessToken(process.value, TOKEN_QUERY | TOKEN_IMPERSONATE, &raw)) continue;
        Handle token(raw);
        PWSTR local = nullptr;
        if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_DONT_VERIFY, token.value, &local))) continue;
        const std::wstring result = std::wstring(local) + L"\\" + CHINESE_SETTINGS_PRODUCT;
        CoTaskMemFree(local);
        return result;
    } while (Process32NextW(snapshot.value, &item));
    return {};
}
inline bool remove(const std::wstring& root) {
    // Reject linked ancestors as well as a linked settings file. Do not follow
    // an install-state path to a different account or an arbitrary directory.
    auto parent = std::filesystem::path(root);
    while (!parent.empty()) {
        const DWORD attr = GetFileAttributesW(parent.c_str());
        if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_REPARSE_POINT)) return false;
        const auto next = parent.parent_path();
        if (next == parent) break;
        parent = next;
    }
    const auto file = root + L"\\" + CHINESE_SETTINGS_FILE;
    const DWORD attr = GetFileAttributesW(file.c_str());
    if (attr != INVALID_FILE_ATTRIBUTES) {
        if ((attr & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) || !DeleteFileW(file.c_str())) return false;
    } else if (GetLastError() != ERROR_FILE_NOT_FOUND && GetLastError() != ERROR_PATH_NOT_FOUND) return false;
    RemoveDirectoryW(root.c_str()); // Empty directory only; unrelated files remain.
    return true;
}
}

extern "C" __declspec(dllexport) BOOL __stdcall SettingsDirectory(wchar_t* output, unsigned capacity) {
    try {
        const auto root = InstallerSettings::directory();
        if (root.empty() || !output || root.size() >= capacity) return FALSE;
        wcscpy_s(output, capacity, root.c_str());
        return TRUE;
    } catch (...) { return FALSE; }
}
extern "C" __declspec(dllexport) BOOL __stdcall CleanupSettings(const wchar_t* expected) {
    try {
        const auto root = InstallerSettings::directory();
        if (root.empty() || !expected || !*expected || _wcsicmp(root.c_str(), expected) != 0) return FALSE;
        return InstallerSettings::remove(root);
    } catch (...) { return FALSE; }
}
