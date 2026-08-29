// ============================================================================
//  Cascadeur Chinese Localizer - Self-contained installer (GUI EXE)
// ============================================================================
//  Mirrors the Toolbag localizer installer:
//    * elevated (requireAdministrator manifest)
//    * user picks the Cascadeur install dir (win64 folder with cascadeur.exe)
//    * extracts an embedded payload, installs the hook + launcher + dictionary
//    * creates a "Cascadeur 中文版" desktop / start-menu shortcut
//    * registers the .casc association to open through the launcher
//    * uninstall removes the files, shortcut and restores the original .casc binding
//
//  Embedded payload format (appended after the PE):
//    [ file data... ][ entries... ][ manifestSize u32 ][ count u32 ][ magic u64 ]
//    entry: [ nameLen u32 ][ name bytes ][ offset u64 ][ size u64 ][ sha256 32 ]
// ============================================================================

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <bcrypt.h>
#include <aclapi.h>
#include <sddl.h>
#include <tlhelp32.h>
#include <commctrl.h>
#include <shlobj.h>
#include <shellapi.h>
#include <objbase.h>
#include <string>
#include <vector>
#include <cstdint>
#include <filesystem>
#include <limits>

#pragma comment(lib, "Comctl32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "bcrypt.lib")
#pragma comment(linker,"/manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

namespace {

constexpr uint64_t kMagic = 0x314D4B545343ull; // "SCSTM1"
const wchar_t* kAppTitle = L"Cascadeur 中文补丁安装程序";
constexpr int kInstallBtn = 1001;
constexpr int kUninstallBtn = 1002;
constexpr int kDirEdit = 1003;
constexpr int kBrowseBtn = 1004;

struct PayloadEntry {
    std::wstring name;
    uint64_t offset;
    uint64_t size;
    BYTE sha256[32];
};

struct InstallerState {
    std::wstring dir;
    std::vector<PayloadEntry> entries;
    HWND dirEdit;
    HFONT uiFont = nullptr;
    int operationId = 0;
    HWND window = nullptr;
};

int ScaleDpi(int value, UINT dpi) { return MulDiv(value, (int)dpi, 96); }

std::wstring ExePath() {
    std::vector<wchar_t> buf(1024);
    while (buf.size() <= 32768) {
        DWORD n = GetModuleFileNameW(nullptr, buf.data(), (DWORD)buf.size());
        if (!n) return L"";
        if (n < buf.size() - 1) return std::wstring(buf.data(), n);
        buf.resize(buf.size() * 2);
    }
    return L"";
}

std::wstring ParentDir(const std::wstring& p) {
    size_t s = p.find_last_of(L"\\/");
    return s == std::wstring::npos ? L"" : p.substr(0, s);
}

bool IsDir(const std::wstring& p) {
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

bool IsFile(const std::wstring& p) {
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

bool HasCascadeurExe(const std::wstring& dir) {
    return IsFile(dir + L"\\cascadeur.exe");
}

bool IsSafePayloadName(const std::wstring& name) {
    if (name.empty() || name.find(L':') != std::wstring::npos) return false;
    const std::filesystem::path path(name);
    if (path.is_absolute() || path.has_root_name() || path.has_root_directory())
        return false;
    for (const auto& component : path) {
        if (component == L"." || component == L".." || component.empty())
            return false;
    }
    return true;
}

bool Utf8ToWide(const std::string& text, std::wstring& result) {
    if (text.empty()) return false;
    const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                          text.data(), (int)text.size(),
                                          nullptr, 0);
    if (count <= 0) return false;
    result.resize((size_t)count);
    return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                               text.data(), (int)text.size(), result.data(),
                               count) == count;
}

bool PayloadHashMatches(const PayloadEntry& entry,
                        const std::vector<BYTE>& payload) {
    if (entry.offset > payload.size() || entry.size > payload.size() - entry.offset ||
        entry.size > (std::numeric_limits<ULONG>::max)()) return false;
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    DWORD objectSize = 0, hashSize = 0, returned = 0;
    bool ok = BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM,
                                          nullptr, 0) == 0 &&
              BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH,
                                reinterpret_cast<BYTE*>(&objectSize),
                                sizeof(objectSize), &returned, 0) == 0 &&
              BCryptGetProperty(algorithm, BCRYPT_HASH_LENGTH,
                                reinterpret_cast<BYTE*>(&hashSize),
                                sizeof(hashSize), &returned, 0) == 0 &&
              hashSize == sizeof(entry.sha256);
    std::vector<BYTE> object(ok ? objectSize : 0);
    BYTE digest[32] = {};
    if (ok)
        ok = BCryptCreateHash(algorithm, &hash, object.data(), objectSize,
                              nullptr, 0, 0) == 0 &&
             BCryptHashData(hash, const_cast<BYTE*>(payload.data() + entry.offset),
                            (ULONG)entry.size, 0) == 0 &&
             BCryptFinishHash(hash, digest, sizeof(digest), 0) == 0 &&
             memcmp(digest, entry.sha256, sizeof(digest)) == 0;
    if (hash) BCryptDestroyHash(hash);
    if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
    return ok;
}

// Cascadeur currently installs directly into Program Files\Cascadeur.
std::wstring FindDefaultCascadeurDir() {
    wchar_t pf[MAX_PATH] = {};
    DWORD len = GetEnvironmentVariableW(L"ProgramFiles", pf, MAX_PATH);
    if (len == 0 || len >= MAX_PATH)
        len = GetEnvironmentVariableW(L"ProgramW6432", pf, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) return L"";
    const std::wstring scanRoot = std::wstring(pf) + L"\\Cascadeur";
    return IsFile(scanRoot + L"\\cascadeur.exe") ? scanRoot : L"";
}

bool CreateShortcut(const std::wstring& lnkPath, const std::wstring& target,
                    const std::wstring& workDir) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    IShellLinkW* link = nullptr;
    if (!SUCCEEDED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
                                    IID_IShellLinkW, (void**)&link))) {
        CoUninitialize();
        return false;
    }
    IPersistFile* file = nullptr;
    bool ok = false;
    if (SUCCEEDED(link->QueryInterface(IID_IPersistFile, (void**)&file))) {
        link->SetPath(target.c_str());
        link->SetWorkingDirectory(workDir.c_str());
        link->SetDescription(L"Cascadeur 中文版");
        if (SUCCEEDED(file->Save(lnkPath.c_str(), TRUE))) ok = true;
        file->Release();
    }
    link->Release();
    CoUninitialize();
    return ok;
}

// The installer runs elevated (requireAdministrator), so the shortcut it writes
// is owned by the elevated account. Grant the target user full control on the
// .lnk so a normal user can always open it; otherwise Windows reports that the
// item referenced by the shortcut cannot be accessed / no permission.
bool GrantShortcutAccess(const std::wstring& lnkPath) {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
    DWORD required = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &required);
    std::vector<unsigned char> tokenInfo(required);
    const bool tokenRead = required > 0 && GetTokenInformation(
        token, TokenUser, tokenInfo.data(), required, &required) != FALSE;
    CloseHandle(token);
    if (!tokenRead) return false;
    auto* tokenUser = reinterpret_cast<TOKEN_USER*>(tokenInfo.data());

    PSECURITY_DESCRIPTOR securityDescriptor = nullptr;
    PACL currentDacl = nullptr;
    if (GetNamedSecurityInfoW(const_cast<wchar_t*>(lnkPath.c_str()), SE_FILE_OBJECT,
                              DACL_SECURITY_INFORMATION, nullptr, nullptr, &currentDacl,
                              nullptr, &securityDescriptor) != ERROR_SUCCESS) return false;

    EXPLICIT_ACCESSW access{};
    access.grfAccessPermissions = GENERIC_ALL | WRITE_DAC | WRITE_OWNER | DELETE;
    access.grfAccessMode = GRANT_ACCESS;
    access.grfInheritance = NO_INHERITANCE;
    access.Trustee.TrusteeForm = TRUSTEE_IS_SID;
    access.Trustee.TrusteeType = TRUSTEE_IS_USER;
    access.Trustee.ptstrName = static_cast<LPWSTR>(tokenUser->User.Sid);

    PACL updatedDacl = nullptr;
    const DWORD aclCreated = SetEntriesInAclW(1, &access, currentDacl, &updatedDacl);
    bool applied = false;
    if (aclCreated == ERROR_SUCCESS) {
        // DACL grant is what makes the shortcut openable; setting the owner is
        // cosmetic and may fail without SeRestorePrivilege, so apply it
        // independently and never let it block the access grant.
        applied = SetNamedSecurityInfoW(
            const_cast<wchar_t*>(lnkPath.c_str()), SE_FILE_OBJECT,
            DACL_SECURITY_INFORMATION, nullptr, nullptr, updatedDacl,
            nullptr) == ERROR_SUCCESS;
        SetNamedSecurityInfoW(const_cast<wchar_t*>(lnkPath.c_str()), SE_FILE_OBJECT,
                              OWNER_SECURITY_INFORMATION,
                              static_cast<PSID>(tokenUser->User.Sid),
                              nullptr, nullptr, nullptr);
    }
    if (updatedDacl) LocalFree(updatedDacl);
    LocalFree(securityDescriptor);
    return applied;
}

// The installer runs elevated, so the process identity is an administrator
// account and NOT necessarily the person who will actually run Cascadeur.
// Creating shortcuts against the elevated token puts them in the wrong user's
// Desktop / Start Menu (the user then reports "no shortcut was created").
// Resolve the interactive console session (the explorer.exe owner of the
// current session) and place shortcuts in *that* user's real folders instead.
std::wstring InteractiveSessionSid() {
    DWORD sessionId = 0;
    ProcessIdToSessionId(GetCurrentProcessId(), &sessionId);
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return L"";
    std::wstring found;
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (Process32FirstW(snapshot, &entry)) {
        do {
            if (_wcsicmp(entry.szExeFile, L"explorer.exe") != 0) continue;
            DWORD processSession = 0;
            if (!ProcessIdToSessionId(entry.th32ProcessID, &processSession) ||
                processSession != sessionId) continue;
            HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,
                                        FALSE, entry.th32ProcessID);
            if (!process) continue;
            HANDLE token = nullptr;
            if (OpenProcessToken(process, TOKEN_QUERY, &token)) {
                DWORD required = 0;
                GetTokenInformation(token, TokenUser, nullptr, 0, &required);
                std::vector<unsigned char> buffer(required);
                if (required > 0 && GetTokenInformation(
                        token, TokenUser, buffer.data(), required, &required)) {
                    TOKEN_USER* user = reinterpret_cast<TOKEN_USER*>(buffer.data());
                    LPWSTR sidString = nullptr;
                    if (ConvertSidToStringSidW(user->User.Sid, &sidString)) {
                        found = sidString;
                        LocalFree(sidString);
                    }
                }
                CloseHandle(token);
            }
            CloseHandle(process);
            if (!found.empty()) break;
        } while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return found;
}

// Read the interactive user's real folder path from HKEY_USERS\<SID>, which
// reflects Desktop redirection (e.g. OneDrive) and expands %USERPROFILE%.
std::wstring InteractiveUserShellFolder(const wchar_t* valueName) {
    const std::wstring sid = InteractiveSessionSid();
    if (sid.empty()) return L"";
    HKEY hive = nullptr;
    if (RegOpenKeyExW(HKEY_USERS, sid.c_str(), 0, KEY_READ, &hive) != ERROR_SUCCESS)
        return L"";
    HKEY shellFolders = nullptr;
    if (RegOpenKeyExW(hive,
            L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\User Shell Folders",
            0, KEY_READ, &shellFolders) != ERROR_SUCCESS) {
        RegCloseKey(hive);
        return L"";
    }
    wchar_t raw[2048] = {};
    DWORD size = sizeof(raw);
    DWORD type = 0;
    std::wstring value;
    if (RegQueryValueExW(shellFolders, valueName, nullptr, &type,
                         reinterpret_cast<BYTE*>(raw), &size) == ERROR_SUCCESS)
        value = raw;
    RegCloseKey(shellFolders);
    RegCloseKey(hive);
    if (value.empty()) return L"";
    std::vector<wchar_t> expanded(8192);
    const DWORD length = ExpandEnvironmentStringsW(
        value.c_str(), expanded.data(), static_cast<DWORD>(expanded.size()));
    if (length && length <= expanded.size())
        return std::wstring(expanded.data(), length - 1);
    return value;
}

// Back up and register the .casc association (HKCU, so no machine-wide risk).
bool SetCascAssociation(const std::wstring& launcher) {
    const wchar_t* progId = L"CascadeurChinese.casc";
    std::wstring openCmdStr = L"\"" + launcher + L"\" \"%1\"";
    const wchar_t* openCmd = openCmdStr.c_str();
    // Back up the original user-level default once. A repair/reinstall must
    // never replace this backup with our own ProgID.
    HKEY bk = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER,
                        L"Software\\CascadeurChineseLocalizer\\AssocBackup",
                        0, nullptr, 0, KEY_READ | KEY_WRITE, nullptr, &bk, nullptr) == ERROR_SUCCESS) {
        DWORD complete = 0, completeSize = sizeof(complete);
        if (RegQueryValueExW(bk, L"backup_complete", nullptr, nullptr,
                             reinterpret_cast<BYTE*>(&complete), &completeSize) != ERROR_SUCCESS || !complete) {
            wchar_t cur[512] = {};
            DWORD cb = sizeof(cur);
            DWORD hadDefault = 0;
            if (RegQueryValueExW(HKEY_CURRENT_USER, L"Software\\Classes\\.casc", nullptr,
                                 nullptr, reinterpret_cast<BYTE*>(cur), &cb) == ERROR_SUCCESS) {
                RegSetValueExW(bk, L"casc_default", 0, REG_SZ,
                               reinterpret_cast<const BYTE*>(cur), cb);
                hadDefault = 1;
            }
            RegSetValueExW(bk, L"casc_had_default", 0, REG_DWORD,
                           reinterpret_cast<const BYTE*>(&hadDefault), sizeof(hadDefault));
            complete = 1;
            RegSetValueExW(bk, L"backup_complete", 0, REG_DWORD,
                           reinterpret_cast<const BYTE*>(&complete), sizeof(complete));
        }
        RegCloseKey(bk);
    }
    HKEY hk = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\Classes\\.casc", 0, nullptr,
                        0, KEY_WRITE, nullptr, &hk, nullptr) != ERROR_SUCCESS) return false;
    RegSetValueExW(hk, nullptr, 0, REG_SZ, (const BYTE*)progId, (DWORD)((wcslen(progId) + 1) * 2));
    RegCloseKey(hk);

    const std::wstring classPath = std::wstring(L"Software\\Classes\\") + progId;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, classPath.c_str(), 0, nullptr, 0,
                        KEY_WRITE, nullptr, &hk, nullptr) == ERROR_SUCCESS) {
        const wchar_t* description = L"Cascadeur 中文版工程";
        RegSetValueExW(hk, nullptr, 0, REG_SZ, reinterpret_cast<const BYTE*>(description),
                       DWORD((wcslen(description) + 1) * sizeof(wchar_t)));
        RegCloseKey(hk);
    }
    const std::wstring iconPath = classPath + L"\\DefaultIcon";
    if (RegCreateKeyExW(HKEY_CURRENT_USER, iconPath.c_str(), 0, nullptr, 0,
                        KEY_WRITE, nullptr, &hk, nullptr) == ERROR_SUCCESS) {
        const std::wstring icon = L"\"" + launcher + L"\",0";
        RegSetValueExW(hk, nullptr, 0, REG_SZ, reinterpret_cast<const BYTE*>(icon.c_str()),
                       DWORD((icon.size() + 1) * sizeof(wchar_t)));
        RegCloseKey(hk);
    }

    HKEY cmd = nullptr;
    std::wstring cmdPath = std::wstring(L"Software\\Classes\\") + progId + L"\\shell\\open\\command";
    if (RegCreateKeyExW(HKEY_CURRENT_USER, cmdPath.c_str(), 0, nullptr, 0, KEY_WRITE,
                        nullptr, &cmd, nullptr) != ERROR_SUCCESS) return false;
    RegSetValueExW(cmd, nullptr, 0, REG_SZ, (const BYTE*)openCmd, (DWORD)((wcslen(openCmd) + 1) * 2));
    RegCloseKey(cmd);

    HKEY openWith = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\Classes\\.casc\\OpenWithProgids",
                        0, nullptr, 0, KEY_WRITE, nullptr, &openWith, nullptr) == ERROR_SUCCESS) {
        RegSetValueExW(openWith, progId, 0, REG_NONE, nullptr, 0);
        RegCloseKey(openWith);
    }

    SHChangeNotify(SHCNE_ASSOCCHANGED, 0, nullptr, nullptr);
    return true;
}

void RestoreCascAssociation() {
    HKEY bk = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\CascadeurChineseLocalizer\\AssocBackup",
                      0, KEY_READ, &bk) == ERROR_SUCCESS) {
        DWORD hadDefault = 0; DWORD hsz = sizeof(hadDefault);
        if (RegQueryValueExW(bk, L"casc_had_default", nullptr, nullptr,
                             (BYTE*)&hadDefault, &hsz) == ERROR_SUCCESS && hadDefault != 0) {
            // A user-level .casc default existed before we installed -> restore it.
            wchar_t cur[512] = {};
            DWORD cb = sizeof(cur);
            if (RegQueryValueExW(bk, L"casc_default", nullptr, nullptr,
                                 (BYTE*)cur, &cb) == ERROR_SUCCESS) {
                HKEY hk = nullptr;
                if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\Classes\\.casc", 0, nullptr,
                                    0, KEY_WRITE, nullptr, &hk, nullptr) == ERROR_SUCCESS) {
                    RegSetValueExW(hk, nullptr, 0, REG_SZ, (const BYTE*)cur, (DWORD)cb);
                    RegCloseKey(hk);
                }
            }
        } else {
            // No user-level default before install: drop our override so Windows
            // falls back to the original machine (HKLM) .casc association.
            HKEY hk = nullptr;
            if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Classes\\.casc", 0,
                              KEY_SET_VALUE, &hk) == ERROR_SUCCESS) {
                RegDeleteValueW(hk, nullptr);
                RegCloseKey(hk);
            }
        }
        RegCloseKey(bk);
    }
    RegDeleteTreeW(HKEY_CURRENT_USER, L"Software\\Classes\\CascadeurChinese.casc");
    // Remove our OpenWithProgids entry for .casc.
    HKEY ow = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Classes\\.casc\\OpenWithProgids", 0,
                      KEY_SET_VALUE, &ow) == ERROR_SUCCESS) {
        RegDeleteValueW(ow, L"CascadeurChinese.casc");
        RegCloseKey(ow);
    }
    // Remove the launcher's "Applications" registration we created.
    RegDeleteTreeW(HKEY_CURRENT_USER, L"Software\\Classes\\Applications\\CascadeurChineseLauncher.exe");
    RegDeleteTreeW(HKEY_CURRENT_USER, L"Software\\CascadeurChineseLocalizer\\AssocBackup");
    SHChangeNotify(SHCNE_ASSOCCHANGED, 0, nullptr, nullptr);
}

// --- embedded payload ------------------------------------------------------
bool ReadWholeFile(const std::wstring& path, std::vector<BYTE>& out) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz{}; if (!GetFileSizeEx(h, &sz)) { CloseHandle(h); return false; }
    if (sz.QuadPart <= 0 || sz.QuadPart > 128ll * 1024 * 1024) { CloseHandle(h); return false; }
    out.resize((size_t)sz.QuadPart);
    DWORD read = 0; BOOL ok = ReadFile(h, out.data(), (DWORD)out.size(), &read, nullptr);
    CloseHandle(h);
    return ok && read == out.size();
}

bool LoadPayload(InstallerState& st) {
    st.entries.clear();
    std::vector<BYTE> bytes;
    if (!ReadWholeFile(ExePath(), bytes)) return false;
    if (bytes.size() < 16) return false;
    const size_t tail = bytes.size() - 16;
    // Tail layout written by embed_files.py: [ manifestSize u32 ][ count u32 ][ magic u64 ]
    uint32_t manifestSize = 0; memcpy(&manifestSize, &bytes[tail], 4);
    uint32_t count = 0;        memcpy(&count,        &bytes[tail + 4], 4);
    uint64_t magic = 0;        memcpy(&magic,        &bytes[tail + 8], 8);
    if (magic != kMagic) return false;
    if (count > 512 || (uint64_t)manifestSize > bytes.size()) return false;
    if (manifestSize > tail) return false;      // guard against offset underflow
    const size_t manifest = tail - manifestSize;
    const char* p = (const char*)&bytes[manifest];
    const char* manifestEnd = (const char*)&bytes[tail];
    for (uint32_t i = 0; i < count; ++i) {
        if (manifestEnd - p < 4) return false;
        uint32_t nameLen = 0; memcpy(&nameLen, p, 4); p += 4;
        if (nameLen > 512 || (size_t)(manifestEnd - p) < nameLen) return false;
        PayloadEntry e;
        std::string name(p, nameLen); p += nameLen;
        if (!Utf8ToWide(name, e.name) || !IsSafePayloadName(e.name)) return false;
        // offset (8) + size (8) + SHA-256 (32)
        if (manifestEnd - p < 48) return false;
        memcpy(&e.offset, p, 8); p += 8;
        memcpy(&e.size, p, 8); p += 8;
        memcpy(e.sha256, p, 32); p += 32;
        st.entries.push_back(e);
    }
    if (p != manifestEnd) return false;
    // Every payload blob must lie entirely before the manifest/tail, otherwise
    // WriteFileFromPayload would read into (or past) the manifest region.
    for (const auto& e : st.entries) {
        if (e.offset > manifest || e.size > manifest - e.offset) return false;
    }
    return true;
}

bool WriteFileFromPayload(const std::wstring& dest, const PayloadEntry& e,
                          const std::vector<BYTE>& payload) {
    if (!PayloadHashMatches(e, payload)) return false;
    HANDLE h = CreateFileW(dest.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD wrote = 0;
    BOOL ok = WriteFile(h, &payload[e.offset], (DWORD)e.size, &wrote, nullptr);
    CloseHandle(h);
    return ok && wrote == e.size;
}

// --- install -----------------------------------------------------------------
bool Install(InstallerState& st) {
    std::vector<BYTE> payload;
    if (!ReadWholeFile(ExePath(), payload)) return false;
    std::wstring dir = st.dir;
    if (!HasCascadeurExe(dir)) return false;
    const std::wstring installDir = dir + L"\\ChineseLauncher";
    if (!IsDir(installDir) && !CreateDirectoryW(installDir.c_str(), nullptr))
        return false;

    for (const auto& e : st.entries) {
        std::wstring target = installDir + L"\\" + e.name;
        std::wstring parent = ParentDir(target);
        if (!IsDir(parent)) CreateDirectoryW(parent.c_str(), nullptr);
        if (!WriteFileFromPayload(target, e, payload)) return false;
    }

    // Create Desktop + Start Menu "Cascadeur 中文版" shortcuts in the
    // interactive user's real folders (handles elevation + OneDrive redirect).
    std::wstring launcher = installDir + L"\\CascadeurChineseLauncher.exe";
    std::wstring name = L"Cascadeur 中文版";
    if (IsFile(launcher)) {
        // Per-user Desktop, resolved from the interactive user's SID.
        std::wstring desktop = InteractiveUserShellFolder(L"Desktop");
        if (desktop.empty()) {
            PWSTR p = nullptr;
            if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Desktop, 0, nullptr, &p)) && p) {
                desktop = p;
                CoTaskMemFree(p);
            }
        }
        if (!desktop.empty()) {
            const std::wstring lnk = desktop + L"\\" + name + L".lnk";
            if (CreateShortcut(lnk, launcher, dir)) GrantShortcutAccess(lnk);
        }
        // Per-user Start Menu (unlike FOLDERID_CommonStartMenu, which is shared
        // and needs admin). Use the interactive user's Programs folder.
        std::wstring programs = InteractiveUserShellFolder(L"Programs");
        if (programs.empty()) {
            PWSTR p = nullptr;
            if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Programs, 0, nullptr, &p)) && p) {
                programs = p;
                CoTaskMemFree(p);
            }
        }
        if (!programs.empty()) {
            const std::wstring lnk = programs + L"\\" + name + L".lnk";
            if (CreateShortcut(lnk, launcher, dir)) GrantShortcutAccess(lnk);
        }
    }
    SetCascAssociation(launcher);
    return true;
}

// --- uninstall ---------------------------------------------------------------
bool Uninstall(InstallerState& st) {
    std::wstring dir = st.dir;
    std::error_code removeError;
    std::filesystem::remove_all(
        std::filesystem::path(dir) / L"ChineseLauncher", removeError);
    if (removeError) return false;
    // Remove shortcuts from the interactive user's folders (the place the
    // installer actually wrote them to).
    std::wstring name = L"Cascadeur 中文版.lnk";
    std::wstring desktop = InteractiveUserShellFolder(L"Desktop");
    if (desktop.empty()) {
        PWSTR p = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Desktop, 0, nullptr, &p)) && p) {
            desktop = p;
            CoTaskMemFree(p);
        }
    }
    std::wstring programs = InteractiveUserShellFolder(L"Programs");
    if (programs.empty()) {
        PWSTR p = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Programs, 0, nullptr, &p)) && p) {
            programs = p;
            CoTaskMemFree(p);
        }
    }
    if (!desktop.empty()) DeleteFileW((desktop + L"\\" + name).c_str());
    if (!programs.empty()) DeleteFileW((programs + L"\\" + name).c_str());
    RestoreCascAssociation();
    return true;
}

// --- installer window (Toolbag-style, DPI-aware Win32 UI) -------------------
static std::wstring GetControlText(HWND h) {
    std::vector<wchar_t> buf(1024);
    while (buf.size() <= 32768) {
        const int n = GetWindowTextW(h, buf.data(), (int)buf.size());
        if (n < (int)buf.size() - 1) return std::wstring(buf.data(), n);
        buf.resize(buf.size() * 2);
    }
    return L"";
}

static bool SelectCascadeurDirectory(std::wstring& dir) {
    wchar_t folder[MAX_PATH] = {};
    BROWSEINFOW bi{};
    bi.lpszTitle = L"请选择 Cascadeur 安装目录（含 cascadeur.exe 的 win64 文件夹）";
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    PIDLIST_ABSOLUTE pid = SHBrowseForFolderW(&bi);
    if (!pid) return false;
    const bool ok = SHGetPathFromIDListW(pid, folder);
    CoTaskMemFree(pid);
    if (ok) dir = folder;
    return ok;
}

LRESULT CALLBACK InstallerWindowProcedure(HWND window, UINT message,
                                           WPARAM wParam, LPARAM lParam) {
    auto* state = reinterpret_cast<InstallerState*>(
        GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        state = reinterpret_cast<InstallerState*>(create->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
    }
    switch (message) {
    case WM_CREATE: {
        const UINT dpi = GetDpiForWindow(window);
        state->uiFont = CreateFontW(-MulDiv(10, (int)dpi, 72), 0, 0, 0,
            FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
            CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Microsoft YaHei UI");
        HWND label = CreateWindowExW(0, L"STATIC", L"Cascadeur 安装目录",
            WS_CHILD | WS_VISIBLE, ScaleDpi(24, dpi), ScaleDpi(20, dpi),
            ScaleDpi(180, dpi), ScaleDpi(22, dpi), window, nullptr, nullptr, nullptr);
        state->dirEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", state->dir.c_str(),
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
            ScaleDpi(24, dpi), ScaleDpi(45, dpi), ScaleDpi(455, dpi), ScaleDpi(31, dpi),
            window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kDirEdit)),
            nullptr, nullptr);
        HWND browse = CreateWindowExW(0, L"BUTTON", L"浏览…",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
            ScaleDpi(489, dpi), ScaleDpi(44, dpi), ScaleDpi(87, dpi), ScaleDpi(33, dpi),
            window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kBrowseBtn)),
            nullptr, nullptr);
        HWND install = CreateWindowExW(0, L"BUTTON", L"安装汉化",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
            ScaleDpi(300, dpi), ScaleDpi(98, dpi), ScaleDpi(130, dpi), ScaleDpi(36, dpi),
            window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kInstallBtn)),
            nullptr, nullptr);
        HWND uninstall = CreateWindowExW(0, L"BUTTON", L"拆卸汉化",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
            ScaleDpi(446, dpi), ScaleDpi(98, dpi), ScaleDpi(130, dpi), ScaleDpi(36, dpi),
            window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kUninstallBtn)),
            nullptr, nullptr);
        for (HWND c : {label, state->dirEdit, browse, install, uninstall})
            SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(state->uiFont), TRUE);
        return 0;
    }
    case WM_CTLCOLORSTATIC: {
        HDC dc = reinterpret_cast<HDC>(wParam);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, GetSysColor(COLOR_WINDOWTEXT));
        return reinterpret_cast<LRESULT>(GetStockObject(WHITE_BRUSH));
    }
    case WM_COMMAND: {
        if (!state) return 0;
        const int id = LOWORD(wParam);
        if (id == kBrowseBtn) {
            std::wstring sel = GetControlText(state->dirEdit);
            if (SelectCascadeurDirectory(sel))
                SetWindowTextW(state->dirEdit, sel.c_str());
            return 0;
        }
        if (id == kInstallBtn || id == kUninstallBtn) {
            state->dir = GetControlText(state->dirEdit);
            while (!state->dir.empty() &&
                   (state->dir.back() == L' ' || state->dir.back() == L'\\'))
                state->dir.pop_back();
            if (state->dir.empty() || !HasCascadeurExe(state->dir)) {
                MessageBoxW(window,
                    L"所选目录不是有效的 Cascadeur 安装目录。\n\n"
                    L"请选择包含 cascadeur.exe 的 win64 目录。",
                    kAppTitle, MB_OK | MB_ICONERROR);
                SetFocus(state->dirEdit);
                return 0;
            }
            state->operationId = id;
            DestroyWindow(window);
            return 0;
        }
        return 0;
    }
    case WM_CLOSE:
        DestroyWindow(window);
        return 0;
    case WM_DESTROY:
        if (state && state->uiFont) DeleteObject(state->uiFont);
        state->uiFont = nullptr;
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

// Scale helper exposed to the window proc (uses the installer's own window dpi).
int ShowInstallerWindow(HINSTANCE instance, InstallerState& state) {
    const wchar_t* cls = L"CascadeurChineseInstallerWindow";
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = InstallerWindowProcedure;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    wc.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(101));
    wc.hIconSm = wc.hIcon;
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    wc.lpszClassName = cls;
    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        return 0;

    const UINT dpi = GetDpiForSystem();
    RECT frame = {0, 0, ScaleDpi(600, dpi), ScaleDpi(155, dpi)};
    AdjustWindowRectExForDpi(&frame, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
                             FALSE, WS_EX_DLGMODALFRAME, dpi);
    const int width = frame.right - frame.left;
    const int height = frame.bottom - frame.top;
    const int x = (GetSystemMetrics(SM_CXSCREEN) - width) / 2;
    const int y = (GetSystemMetrics(SM_CYSCREEN) - height) / 2;
    HWND window = CreateWindowExW(WS_EX_DLGMODALFRAME, cls, kAppTitle,
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
        x, y, width, height, nullptr, nullptr, instance, &state);
    if (!window) return 0;
    state.window = window;
    ShowWindow(window, SW_SHOW);
    UpdateWindow(window);

    MSG message = {};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(window, &message)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }
    return state.operationId;
}

} // namespace

int APIENTRY wWinMain(HINSTANCE hinst, HINSTANCE, PWSTR, int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    INITCOMMONCONTROLSEX icc{}; icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&icc);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);

    InstallerState state{};
    if (!LoadPayload(state)) {
        MessageBoxW(nullptr, L"安装程序缺少必要数据，请重新下载完整版本。",
                    kAppTitle, MB_OK | MB_ICONERROR);
        CoUninitialize();
        return 1;
    }
    const std::wstring def = FindDefaultCascadeurDir();
    if (!def.empty()) state.dir = def;

    const int operationId = ShowInstallerWindow(hinst, state);
    if (operationId == 0) { CoUninitialize(); return 0; }
    const bool uninstall = operationId == kUninstallBtn;
    const bool ok = uninstall ? Uninstall(state) : Install(state);
    if (ok) {
        MessageBoxW(nullptr,
            uninstall ? L"汉化已拆卸。" :
            L"汉化安装完成。\n\n请通过“Cascadeur 中文版”快捷方式启动，或双击 .casc 文件。",
            kAppTitle, MB_OK | MB_ICONINFORMATION);
    } else {
        MessageBoxW(nullptr,
            uninstall ? L"拆卸失败。" : L"安装失败。",
            kAppTitle, MB_OK | MB_ICONERROR);
    }
    CoUninitialize();
    return ok ? 0 : 2;
}
