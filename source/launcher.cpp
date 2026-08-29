// ============================================================================
//  Cascadeur Chinese Localizer - Launcher
// ============================================================================
//  Cascadeur has no plug-in entry point, so this launcher (like the Toolbag
//  localizer) starts cascadeur.exe SUSPENDED, injects the hook DLL via
//  a remote LoadLibraryW thread, then resumes. Original program files are
//  never modified. Any command line (for example a .casc path) is passed
//  through unchanged to Cascadeur.
// ============================================================================

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>
#include <string>
#include <vector>

namespace {

std::wstring ParentDir(const std::wstring& path) {
    size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? L"" : path.substr(0, slash);
}

bool IsFile(const std::wstring& path) {
    DWORD attr = GetFileAttributesW(path.c_str());
    return attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY);
}

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

std::wstring FindCascadeurRoot(const std::wstring& start) {
    std::wstring dir = start;
    for (int depth = 0; depth < 12; ++depth) {
        if (IsFile(dir + L"\\cascadeur.exe")) return dir;
        std::wstring parent = ParentDir(dir);
        if (parent.empty() || parent == dir) break;
        dir = parent;
    }
    // Current Cascadeur releases install directly into Program Files\Cascadeur.
    wchar_t pf[MAX_PATH] = {};
    DWORD pfLen = GetEnvironmentVariableW(L"ProgramFiles", pf, MAX_PATH);
    if (pfLen == 0 || pfLen >= MAX_PATH)
        pfLen = GetEnvironmentVariableW(L"ProgramW6432", pf, MAX_PATH);
    if (pfLen && pfLen < MAX_PATH) {
        const std::wstring root = std::wstring(pf) + L"\\Cascadeur";
        if (IsFile(root + L"\\cascadeur.exe")) return root;
    }
    return L"";
}

LPTHREAD_START_ROUTINE ResolveLoadLibrary(DWORD pid) {
    HMODULE local = GetModuleHandleW(L"kernel32.dll");
    if (!local) return nullptr;
    FARPROC fn = GetProcAddress(local, "LoadLibraryW");
    if (!fn) return nullptr;
    uintptr_t rva = (uintptr_t)fn - (uintptr_t)local;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (snap != INVALID_HANDLE_VALUE) {
        MODULEENTRY32W mod{}; mod.dwSize = sizeof(mod);
        if (Module32FirstW(snap, &mod)) {
            do {
                if (_wcsicmp(mod.szModule, L"kernel32.dll") == 0) {
                    CloseHandle(snap);
                    return (LPTHREAD_START_ROUTINE)((uintptr_t)mod.modBaseAddr + rva);
                }
            } while (Module32NextW(snap, &mod));
        }
        CloseHandle(snap);
    }
    // A newly created suspended process can temporarily reject module
    // snapshots on some Windows builds. Both processes have the same
    // architecture and share the boot-time system DLL mapping, so retain the
    // proven compatibility fallback used by the stable launcher.
    return reinterpret_cast<LPTHREAD_START_ROUTINE>(fn);
}

} // namespace

int APIENTRY wWinMain(HINSTANCE, HINSTANCE, PWSTR cmdLine, int) {
    const std::wstring own = ExePath();
    const std::wstring dir = ParentDir(own);
    const std::wstring root = FindCascadeurRoot(dir);
    const std::wstring exePath = root + L"\\cascadeur.exe";

    const std::wstring dllPath = dir + L"\\CascadeurChineseHook.dll";

    if (root.empty() || !IsFile(dllPath) || !IsFile(exePath)) {
        MessageBoxW(nullptr,
                    L"找不到 cascadeur.exe 或汉化 DLL。\n"
                    L"请重新运行安装程序，修复 ChineseLauncher 目录。",
                    L"Cascadeur 中文补丁", MB_OK | MB_ICONERROR);
        return 1;
    }

    STARTUPINFOW si{}; si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    std::wstring cl = L"\"" + exePath + L"\"";
    if (cmdLine && *cmdLine) { cl += L" "; cl += cmdLine; }
    std::vector<wchar_t> clBuf(cl.begin(), cl.end()); clBuf.push_back(L'\0');

    if (!CreateProcessW(exePath.c_str(), clBuf.data(), nullptr, nullptr, FALSE,
                        CREATE_SUSPENDED, nullptr, root.c_str(), &si, &pi)) {
        MessageBoxW(nullptr, L"无法启动 cascadeur.exe。",
                    L"Cascadeur 中文补丁", MB_OK | MB_ICONERROR);
        return 2;
    }

    size_t bytes = (dllPath.size() + 1) * sizeof(wchar_t);
    void* remote = VirtualAllocEx(pi.hProcess, nullptr, bytes,
                                  MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    SIZE_T written = 0;
    bool pathOk = remote &&
        WriteProcessMemory(pi.hProcess, remote, dllPath.c_str(), bytes, &written) &&
        written == bytes;

    auto loadLib = ResolveLoadLibrary(pi.dwProcessId);
    HANDLE thread = nullptr;
    if (pathOk && loadLib)
        thread = CreateRemoteThread(pi.hProcess, nullptr, 0, loadLib, remote, 0, nullptr);

    bool ok = false;
    bool remoteThreadCompleted = false;
    if (thread) {
        DWORD wait = WaitForSingleObject(thread, 30000);
        remoteThreadCompleted = wait == WAIT_OBJECT_0;
        DWORD loadResult = 0;
        const bool loaded = wait == WAIT_OBJECT_0 &&
                            GetExitCodeThread(thread, &loadResult) &&
                            loadResult != 0;
        CloseHandle(thread);
        ok = loaded;
    }

    if (ok && ResumeThread(pi.hThread) != (DWORD)-1) {
        ok = true;
    } else {
        ok = false;
        TerminateProcess(pi.hProcess, 3);
        WaitForSingleObject(pi.hProcess, 5000);
        MessageBoxW(nullptr, L"汉化注入失败，已终止 Cascadeur（原文件未受影响）。",
                    L"Cascadeur 中文补丁", MB_OK | MB_ICONERROR);
    }

    // A timed-out remote thread may still be reading the DLL path. The failure
    // path terminates the child, so let process teardown reclaim the memory.
    if (remote && remoteThreadCompleted)
        VirtualFreeEx(pi.hProcess, remote, 0, MEM_RELEASE);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return ok ? 0 : 3;
}
