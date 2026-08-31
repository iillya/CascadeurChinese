// Host validation, user-level command proxy and scoped legacy-association cleanup.
// No Qt dependency, process injection, process termination or shell execution.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <tlhelp32.h>
#include <sddl.h>
#include <shlobj.h>
#include <filesystem>
#include <string>
#include <vector>
#include "../file_association.h"
#include "association_proxy.h"
#include "../qt_compatibility.h"

namespace {
struct Handle {
    HANDLE value;
    explicit Handle(HANDLE v) : value(v) {}
    ~Handle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
};

bool fail(const wchar_t* text, wchar_t* out, unsigned capacity) {
    if (out && capacity) wcsncpy_s(out, capacity, text, _TRUNCATE);
    return false;
}

bool regularFile(const std::wstring& path) {
    const DWORD attr = GetFileAttributesW(path.c_str());
    return attr != INVALID_FILE_ATTRIBUTES && !(attr & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT));
}

bool amd64(const std::wstring& path) {
    Handle file(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (file.value == INVALID_HANDLE_VALUE) return false;
    IMAGE_DOS_HEADER dos{};
    DWORD count = 0, signature = 0;
    IMAGE_FILE_HEADER pe{};
    LARGE_INTEGER offset{};
    if (!ReadFile(file.value, &dos, sizeof(dos), &count, nullptr) ||
        count != sizeof(dos) || dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew < sizeof(dos)) return false;
    offset.QuadPart = dos.e_lfanew;
    return SetFilePointerEx(file.value, offset, nullptr, FILE_BEGIN) &&
        ReadFile(file.value, &signature, sizeof(signature), &count, nullptr) && count == sizeof(signature) &&
        signature == IMAGE_NT_SIGNATURE && ReadFile(file.value, &pe, sizeof(pe), &count, nullptr) &&
        count == sizeof(pe) && pe.Machine == IMAGE_FILE_MACHINE_AMD64;
}

bool safePath(const std::wstring& root) {
    // A host directory, not a drive root, UNC share or Win32 device path.
    if (root.size() < 4 || root.size() > 180 || root[1] != L':' || root[2] != L'\\' ||
        !((root[0] >= L'A' && root[0] <= L'Z') || (root[0] >= L'a' && root[0] <= L'z')) ||
        root.find_first_of(L"\"<>|?*") != std::wstring::npos || root.find(L':', 2) != std::wstring::npos) return false;
    wchar_t full[32768]{};
    const DWORD count = GetFullPathNameW(root.c_str(), 32768, full, nullptr);
    if (!count || count >= 32768 || _wcsicmp(full, root.c_str()) != 0) return false;
    std::filesystem::path current(root);
    while (!current.empty()) {
        const DWORD attributes = GetFileAttributesW(current.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) return false;
        const auto parent = current.parent_path();
        if (current == parent) break;
        current = parent;
    }
    return (GetFileAttributesW(root.c_str()) & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

bool safeTree(const std::filesystem::path& path, unsigned depth, unsigned& count) {
    const DWORD attr = GetFileAttributesW(path.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES)
        return GetLastError() == ERROR_FILE_NOT_FOUND || GetLastError() == ERROR_PATH_NOT_FOUND;
    if ((attr & FILE_ATTRIBUTE_REPARSE_POINT) || depth > 16 || ++count > 10000) return false;
    if (!(attr & FILE_ATTRIBUTE_DIRECTORY)) return true;
    std::error_code error;
    std::filesystem::directory_iterator it(path, error), end;
    if (error) return false;
    while (it != end) {
        if (!safeTree(it->path(), depth + 1, count)) return false;
        it.increment(error);
        if (error) return false;
    }
    return true;
}

bool hostStopped(const std::wstring& root) {
    Handle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
    if (snapshot.value == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32W item{};
    item.dwSize = sizeof(item);
    if (!Process32FirstW(snapshot.value, &item)) return false;
    do {
        if (_wcsicmp(item.szExeFile, L"cascadeur.exe") != 0 &&
            _wcsicmp(item.szExeFile, L"CascadeurChineseLauncher.exe") != 0) continue;
        Handle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, item.th32ProcessID));
        if (!process.value) {
            if (GetLastError() == ERROR_INVALID_PARAMETER) continue; // Exited meanwhile.
            return false;
        }
        wchar_t path[32768]{};
        DWORD length = 32768;
        if (!QueryFullProcessImageNameW(process.value, 0, path, &length)) return false;
        if (_wcsicmp(path, (root + L"\\cascadeur.exe").c_str()) == 0 ||
            _wcsicmp(path, (root + L"\\ChineseLauncher\\CascadeurChineseLauncher.exe").c_str()) == 0) return false;
    } while (Process32NextW(snapshot.value, &item));
    return GetLastError() == ERROR_NO_MORE_FILES;
}

std::wstring interactiveSid() {
    DWORD session = 0;
    if (!ProcessIdToSessionId(GetCurrentProcessId(), &session)) return {};
    Handle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
    if (snapshot.value == INVALID_HANDLE_VALUE) return {};
    PROCESSENTRY32W item{};
    item.dwSize = sizeof(item);
    if (!Process32FirstW(snapshot.value, &item)) return {};
    do {
        DWORD candidateSession = 0;
        if (_wcsicmp(item.szExeFile, L"explorer.exe") != 0 ||
            !ProcessIdToSessionId(item.th32ProcessID, &candidateSession) || candidateSession != session) continue;
        Handle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, item.th32ProcessID));
        HANDLE rawToken = nullptr;
        if (!process.value || !OpenProcessToken(process.value, TOKEN_QUERY, &rawToken)) continue;
        Handle token(rawToken);
        DWORD size = 0;
        GetTokenInformation(token.value, TokenUser, nullptr, 0, &size);
        if (!size || size > 65536) continue;
        std::vector<BYTE> bytes(size);
        if (!GetTokenInformation(token.value, TokenUser, bytes.data(), size, &size)) continue;
        LPWSTR sid = nullptr;
        if (!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(bytes.data())->User.Sid, &sid)) continue;
        const std::wstring result(sid);
        LocalFree(sid);
        return result;
    } while (Process32NextW(snapshot.value, &item));
    return {};
}
}

extern "C" __declspec(dllexport) BOOL __stdcall CheckTarget(
    const wchar_t* directory, BOOL checkVersion, wchar_t* message, unsigned capacity) {
    if (message && capacity) message[0] = L'\0';
    try {
        const std::wstring root(directory ? directory : L"");
        if (!safePath(root)) return fail(L"请选择本地、无目录链接的 Cascadeur 目录；路径长度上限为 180 字符。", message, capacity);
        unsigned count = 0;
        if (!safeTree(std::filesystem::path(root) / L"ChineseLauncher", 0, count))
            return fail(L"ChineseLauncher 内含目录链接、不可访问文件或超出检查上限，已停止操作。", message, capacity);
        if (checkVersion) {
            if (!regularFile(root + L"\\cascadeur.exe") || !amd64(root + L"\\cascadeur.exe"))
                return fail(L"请选择包含 x64 cascadeur.exe 的软件目录，不要选择 ChineseLauncher 子目录。", message, capacity);
            if (!CascadeurQtCompatibility::supportedDirectory(root))
                return fail(L"目标 Qt 组件不完整或不兼容。支持 Qt 6.5.1、6.5.3 x64，且各组件须为同一版本。", message, capacity);
            for (const wchar_t* name : CascadeurQtCompatibility::modules) {
                const std::wstring file = root + L"\\" + name;
                if (!regularFile(file) || !amd64(file))
                    return fail(L"目标 Qt 组件必须为完整的 x64 文件。", message, capacity);
            }
            if (GetFileAttributesW((root + L"\\Qt5Core.dll").c_str()) != INVALID_FILE_ATTRIBUTES)
                return fail(L"检测到 Qt5Core.dll，不属于当前已验证版本。", message, capacity);
        }
        if (!hostStopped(root))
            return fail(L"Cascadeur 或中文启动器正在运行，或无法确认进程状态。请保存工程并正常退出后重试。", message, capacity);
        return TRUE;
    } catch (...) { return fail(L"目录检查出现异常，未执行安装或卸载。", message, capacity); }
}

// Record only an existing legacy association owned by this exact launcher.
extern "C" __declspec(dllexport) BOOL __stdcall LegacyOwner(
    const wchar_t* launcher, wchar_t* owner, unsigned capacity) {
    if (!launcher || !owner || capacity < 185) return FALSE;
    owner[0] = L'\0';
    try {
        const auto sid = interactiveSid();
        if (sid.empty()) return TRUE; // No interactive user: do not guess HKCU.
        CascadeurAssociation::Key user;
        if (RegOpenKeyExW(HKEY_USERS, sid.c_str(), 0, KEY_READ, &user.value) != ERROR_SUCCESS) return FALSE;
        std::wstring command;
        const auto status = CascadeurAssociation::readString(user.value,
            (std::wstring(CascadeurAssociation::kClass) + L"\\shell\\open\\command").c_str(), nullptr, command);
        if (status != ERROR_SUCCESS && status != ERROR_FILE_NOT_FOUND) return FALSE;
        if (command == L"\"" + std::wstring(launcher) + L"\" \"%1\"")
            wcscpy_s(owner, capacity, sid.c_str());
        return TRUE;
    } catch (...) { return FALSE; }
}

extern "C" __declspec(dllexport) BOOL __stdcall RestoreLegacy(const wchar_t* sid, const wchar_t* launcher) {
    if (!sid || !*sid) return TRUE;
    if (!launcher || wcslen(sid) > 184) return FALSE;
    try {
        PSID parsed = nullptr;
        if (!ConvertStringSidToSidW(sid, &parsed)) return FALSE;
        const bool valid = IsValidSid(parsed) != FALSE;
        LocalFree(parsed);
        if (!valid) return FALSE;
        CascadeurAssociation::Key user;
        if (RegOpenKeyExW(HKEY_USERS, sid, 0, KEY_READ | KEY_WRITE, &user.value) != ERROR_SUCCESS) return FALSE;
        const bool ok = CascadeurAssociation::uninstall(user.value, launcher);
        if (ok) SHChangeNotify(SHCNE_ASSOCCHANGED, 0, nullptr, nullptr);
        return ok;
    } catch (...) { return FALSE; }
}

namespace {
bool openProxyUser(const wchar_t* sid, CascadeurProxy::Key& user, REGSAM access) {
    if (!sid || !*sid || wcslen(sid) > 184) return false;
    PSID parsed = nullptr;
    if (!ConvertStringSidToSidW(sid, &parsed)) return false;
    const bool valid = IsValidSid(parsed) != FALSE;
    LocalFree(parsed);
    return valid && RegOpenKeyExW(HKEY_USERS, sid, 0, access, &user.value) == ERROR_SUCCESS;
}
}

extern "C" __declspec(dllexport) BOOL __stdcall PlanProxy(
    const wchar_t* root, wchar_t* owner, unsigned ownerCapacity, wchar_t* message, unsigned capacity) {
    if (!root || !owner || ownerCapacity < 185) return FALSE;
    owner[0] = L'\0';
    try {
        const auto sid = interactiveSid();
        CascadeurProxy::Key user;
        if (!openProxyUser(sid.c_str(), user, KEY_READ))
            return fail(L"无法确认当前桌面用户，未接管工程关联。", message, capacity);
        std::vector<CascadeurProxy::Record> records;
        std::wstring error;
        // Explicit option: redirect the verified official handler to the
        // selected installation, even when its old registration names another
        // Cascadeur directory. The original registration is never rewritten.
        if (!CascadeurProxy::prepare(user.value, HKEY_LOCAL_MACHINE, root, records, error, true))
            return fail(error.c_str(), message, capacity);
        wcscpy_s(owner, ownerCapacity, sid.c_str());
        return TRUE;
    } catch (...) { return fail(L"工程关联预检查失败，未修改关联。", message, capacity); }
}

extern "C" __declspec(dllexport) BOOL __stdcall ApplyProxy(
    const wchar_t* root, const wchar_t* owner, wchar_t* message, unsigned capacity) {
    try {
        CascadeurProxy::Key user;
        if (!root || !openProxyUser(owner, user, KEY_READ | KEY_WRITE))
            return fail(L"无法打开已确认用户的关联配置。", message, capacity);
        std::wstring error;
        if (!CascadeurProxy::install(user.value, HKEY_LOCAL_MACHINE, root, error, nullptr, true))
            return fail(error.c_str(), message, capacity);
        SHChangeNotify(SHCNE_ASSOCCHANGED, 0, nullptr, nullptr);
        return TRUE;
    } catch (...) { return fail(L"工程关联接管失败；请保留安装目录和日志后重试。", message, capacity); }
}

extern "C" __declspec(dllexport) BOOL __stdcall RemoveProxy(
    const wchar_t* root, const wchar_t* owner, wchar_t* message, unsigned capacity) {
    if (!owner || !*owner) return TRUE;
    try {
        CascadeurProxy::Key user;
        if (!root || !openProxyUser(owner, user, KEY_READ | KEY_WRITE))
            return fail(L"原安装用户的注册表未加载，请登录该用户后重试卸载。", message, capacity);
        std::wstring error;
        if (!CascadeurProxy::uninstall(user.value, root, error)) return fail(error.c_str(), message, capacity);
        SHChangeNotify(SHCNE_ASSOCCHANGED, 0, nullptr, nullptr);
        return TRUE;
    } catch (...) { return fail(L"工程关联恢复失败，已停止卸载；请保留备份。", message, capacity); }
}

#define CHINESE_SETTINGS_PRODUCT L"CascadeurChinese"
#define CHINESE_SETTINGS_FILE L"settings.json"
#include "settings_cleanup.h"
