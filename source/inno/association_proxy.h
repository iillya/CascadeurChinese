#pragma once
// A user-level overlay of verified Cascadeur shell commands. No extension
// default, UserChoice, machine-hive or unrelated handler writes.
#include <windows.h>
#include <shlwapi.h>
#include <string>
#include <vector>
#include <cstring>

namespace CascadeurProxy {
constexpr wchar_t kJournal[] = L"Software\\CascadeurChineseLocalizer\\InnoProxyV1";
constexpr const wchar_t* kHandlers[] = {L"Cascadeur.scene", L"Applications\\cascadeur.exe"};
struct Key {
    HKEY value = nullptr;
    ~Key() { if (value) RegCloseKey(value); }
    Key() = default;
    Key(const Key&) = delete;
    Key& operator=(const Key&) = delete;
};
struct Value {
    bool present = false;
    DWORD type = REG_NONE;
    std::vector<BYTE> data;
    bool operator==(const Value& other) const {
        return present == other.present && (!present || (type == other.type && data == other.data));
    }
};
inline bool read(HKEY root, const std::wstring& path, const wchar_t* name, Value& out) {
    out = {};
    Key key;
    auto status = RegOpenKeyExW(root, path.c_str(), 0, KEY_QUERY_VALUE, &key.value);
    if (status == ERROR_FILE_NOT_FOUND || status == ERROR_PATH_NOT_FOUND) return true;
    if (status != ERROR_SUCCESS) return false;
    DWORD bytes = 0;
    status = RegQueryValueExW(key.value, name, nullptr, &out.type, nullptr, &bytes);
    if (status == ERROR_FILE_NOT_FOUND) return true;
    if (status != ERROR_SUCCESS || bytes > 65536) return false;
    out.data.resize(bytes);
    status = RegQueryValueExW(key.value, name, nullptr, &out.type, out.data.data(), &bytes);
    if (status != ERROR_SUCCESS) return false;
    out.data.resize(bytes);
    out.present = true;
    return true;
}
inline Value stringValue(const std::wstring& text, DWORD type = REG_SZ) {
    Value value;
    value.present = true;
    value.type = type;
    value.data.resize((text.size() + 1) * sizeof(wchar_t));
    std::memcpy(value.data.data(), text.c_str(), value.data.size());
    return value;
}
inline Value number(DWORD n) {
    Value value;
    value.present = true;
    value.type = REG_DWORD;
    value.data.resize(sizeof(n));
    std::memcpy(value.data.data(), &n, sizeof(n));
    return value;
}
inline bool text(const Value& value, std::wstring& result) {
    if (!value.present || (value.type != REG_SZ && value.type != REG_EXPAND_SZ) ||
        value.data.size() < sizeof(wchar_t) || value.data.size() % sizeof(wchar_t)) return false;
    std::wstring buffer(value.data.size() / sizeof(wchar_t), L'\0');
    std::memcpy(buffer.data(), value.data.data(), value.data.size());
    if (buffer.back() != L'\0' || buffer.find(L'\0') != buffer.size() - 1) return false;
    buffer.pop_back();
    result = buffer;
    return true;
}
inline bool dword(HKEY root, const std::wstring& path, const wchar_t* name, DWORD& out) {
    Value v;
    if (!read(root, path, name, v) || !v.present || v.type != REG_DWORD || v.data.size() != sizeof(out)) return false;
    std::memcpy(&out, v.data.data(), sizeof(out));
    return true;
}
inline bool write(HKEY root, const std::wstring& path, const wchar_t* name, const Value& value) {
    Key key;
    if (!value.present) {
        const auto open = RegOpenKeyExW(root, path.c_str(), 0, KEY_SET_VALUE, &key.value);
        if (open == ERROR_FILE_NOT_FOUND || open == ERROR_PATH_NOT_FOUND) return true;
        if (open != ERROR_SUCCESS) return false;
        const auto status = RegDeleteValueW(key.value, name);
        return status == ERROR_SUCCESS || status == ERROR_FILE_NOT_FOUND;
    }
    if (RegCreateKeyExW(root, path.c_str(), 0, nullptr, 0, KEY_SET_VALUE, nullptr,
                        &key.value, nullptr) != ERROR_SUCCESS) return false;
    return RegSetValueExW(key.value, name, 0, value.type, value.data.data(),
                         static_cast<DWORD>(value.data.size())) == ERROR_SUCCESS;
}
inline std::wstring path(unsigned index) { return L"Software\\Classes\\" + std::wstring(kHandlers[index]) + L"\\shell\\open\\command"; }
inline std::wstring journal(unsigned index) { return std::wstring(kJournal) + L"\\" + std::to_wstring(index); }

struct Record {
    unsigned index = 0;
    DWORD createdMask = 0;
    bool saved = false;
    Value original, proxy;
};
inline std::vector<std::wstring> nodes(unsigned index) {
    std::wstring node = L"Software\\Classes\\" + std::wstring(kHandlers[index]);
    return {node, node + L"\\shell", node + L"\\shell\\open", node + L"\\shell\\open\\command"};
}

// Replace only the executable token; preserve all original argument text.
inline bool makeProxy(const Value& effective, const std::wstring& officialExe,
                      const std::wstring& launcher, Value& proxy, bool allowOtherInstall = false) {
    std::wstring command;
    if (!text(effective, command) || command.empty() || command.front() == L' ') return false;
    std::wstring executable, arguments;
    if (command.front() == L'"') {
        const auto end = command.find(L'"', 1);
        if (end == std::wstring::npos) return false;
        executable = command.substr(1, end - 1);
        arguments = command.substr(end + 1);
    } else {
        const auto end = command.find_first_of(L" \t");
        executable = command.substr(0, end);
        if (end != std::wstring::npos) arguments = command.substr(end);
    }
    if (effective.type == REG_EXPAND_SZ) {
        wchar_t expanded[32768]{};
        const DWORD size = ExpandEnvironmentStringsW(executable.c_str(), expanded, 32768);
        if (!size || size > 32768) return false;
        executable = expanded;
    }
    const auto slash = executable.find_last_of(L"\\/");
    const bool otherCascadeur = allowOtherInstall && executable.size() > 3 && executable[1] == L':' &&
        executable[2] == L'\\' && slash != std::wstring::npos &&
        _wcsicmp(executable.substr(slash + 1).c_str(), L"cascadeur.exe") == 0;
    if ((_wcsicmp(executable.c_str(), officialExe.c_str()) != 0 && !otherCascadeur) || arguments.empty() ||
        (arguments.front() != L' ' && arguments.front() != L'\t') ||
        (arguments.find(L"%1") == std::wstring::npos && arguments.find(L"%L") == std::wstring::npos &&
         arguments.find(L"%l") == std::wstring::npos && arguments.find(L"%*") == std::wstring::npos)) return false;
    proxy = stringValue(L"\"" + launcher + L"\"" + arguments, effective.type);
    return true;
}

inline bool load(HKEY user, unsigned index, const std::wstring& launcher, Record& record, std::wstring& error) {
    record.index = index;
    const auto key = journal(index);
    Value owner;
    if (!read(user, key, L"Owner", owner)) { error = L"无法读取关联备份。"; return false; }
    Key probe;
    const auto status = RegOpenKeyExW(user, key.c_str(), 0, KEY_READ, &probe.value);
    if (status == ERROR_FILE_NOT_FOUND || status == ERROR_PATH_NOT_FOUND) return true;
    if (status != ERROR_SUCCESS) { error = L"无法打开关联备份。"; return false; }
    DWORD complete = 0, present = 0;
    std::wstring ownerText, proxyText;
    if (!text(owner, ownerText) || ownerText != launcher || !dword(user, key, L"Complete", complete) || complete != 1 ||
        !dword(user, key, L"OriginalPresent", present) || present > 1 ||
        !dword(user, key, L"CreatedMask", record.createdMask) || record.createdMask > 15 ||
        !read(user, key, L"Original", record.original) || record.original.present != (present == 1) ||
        !read(user, key, L"Proxy", record.proxy) || !text(record.proxy, proxyText) ||
        proxyText.rfind(L"\"" + launcher + L"\"", 0) != 0 ||
        proxyText.size() <= launcher.size() + 2 ||
        (proxyText[launcher.size() + 2] != L' ' && proxyText[launcher.size() + 2] != L'\t')) {
        error = L"关联备份不完整、已损坏或属于另一份安装；已停止操作，保留备份供恢复。";
        return false;
    }
    record.saved = true;
    return true;
}

inline bool prepare(HKEY user, HKEY machine, const std::wstring& root,
                    std::vector<Record>& records, std::wstring& error, bool allowOtherInstall = false) {
    records.clear();
    const auto launcher = root + L"\\ChineseLauncher\\CascadeurChineseLauncher.exe";
    for (unsigned i = 0; i < 2; ++i) {
        Record record;
        if (!load(user, i, launcher, record, error)) return false;
        Value current;
        if (!read(user, path(i), nullptr, current)) { error = L"无法读取当前打开命令。"; return false; }
        if (record.saved) {
            if (!(current == record.proxy) && !(current == record.original)) {
                error = L"打开命令在上次安装后已被修改；不会覆盖用户或官方更新的选择。";
                return false;
            }
        } else {
            Value effective = current;
            if (!effective.present && !read(machine, path(i), nullptr, effective)) {
                error = L"无法读取官方打开命令。"; return false;
            }
            if (!effective.present && i == 1) continue; // Do not invent optional handlers.
            if (!makeProxy(effective, root + L"\\cascadeur.exe", launcher, record.proxy, allowOtherInstall)) {
                error = L"官方打开命令不属于所选 Cascadeur 目录，或参数格式尚未验证。请选择对应目录，或取消工程关联选项。";
                return false;
            }
            record.original = current;
            const auto parents = nodes(i);
            for (unsigned n = 0; n < parents.size(); ++n) {
                Key probe;
                const auto status = RegOpenKeyExW(user, parents[n].c_str(), 0, KEY_READ, &probe.value);
                if (status == ERROR_FILE_NOT_FOUND || status == ERROR_PATH_NOT_FOUND) record.createdMask |= 1u << n;
                else if (status != ERROR_SUCCESS) { error = L"无法检查关联键的原始状态。"; return false; }
            }
        }
        records.push_back(record);
    }
    return true;
}

// Tests can fail a specific mutation. Production never supplies this hook.
using FaultHook = bool (*)(const wchar_t* phase, unsigned index);
inline bool save(HKEY user, const Record& record, const std::wstring& launcher, FaultHook fault) {
    if (record.saved) return true;
    const auto key = journal(record.index);
    if (!write(user, key, L"Owner", stringValue(launcher)) ||
        !write(user, key, L"OriginalPresent", number(record.original.present ? 1 : 0)) ||
        !write(user, key, L"Original", record.original) ||
        !write(user, key, L"CreatedMask", number(record.createdMask)) ||
        !write(user, key, L"Proxy", record.proxy)) return false;
    if (fault && !fault(L"before-complete", record.index)) return false;
    if (!write(user, key, L"Complete", number(1))) return false;
    Key handle;
    if (RegOpenKeyExW(user, key.c_str(), 0, KEY_READ, &handle.value) != ERROR_SUCCESS ||
        RegFlushKey(handle.value) != ERROR_SUCCESS) return false;
    Record verified;
    std::wstring error;
    return load(user, record.index, launcher, verified, error) && verified.saved &&
        verified.original == record.original && verified.proxy == record.proxy && verified.createdMask == record.createdMask;
}

inline bool clearJournal(HKEY user, unsigned index) {
    const auto key = journal(index);
    // Never remove unknown values, even under our own journal namespace.
    for (const auto* name : {L"Complete", L"Owner", L"OriginalPresent", L"Original", L"CreatedMask", L"Proxy"})
        if (!write(user, key, name, {})) return false;
    SHDeleteEmptyKeyW(user, key.c_str());
    SHDeleteEmptyKeyW(user, kJournal);
    return true;
}

inline bool restoreRecord(HKEY user, const Record& record) {
    Value current;
    if (!read(user, path(record.index), nullptr, current)) return false;
    if (current == record.proxy) {
        if (!write(user, path(record.index), nullptr, record.original)) return false;
        const auto parents = nodes(record.index);
        for (unsigned n = static_cast<unsigned>(parents.size()); n-- > 0;)
            if (record.createdMask & (1u << n)) SHDeleteEmptyKeyW(user, parents[n].c_str());
    }
    // A different value belongs to the user/another installer. Preserve it.
    return clearJournal(user, record.index);
}

inline bool install(HKEY user, HKEY machine, const std::wstring& root,
                    std::wstring& error, FaultHook fault = nullptr, bool allowOtherInstall = false) {
    std::vector<Record> records;
    if (!prepare(user, machine, root, records, error, allowOtherInstall)) return false;
    std::vector<Value> before(records.size());
    for (size_t i = 0; i < records.size(); ++i)
        if (!read(user, path(records[i].index), nullptr, before[i])) return false;
    const auto launcher = root + L"\\ChineseLauncher\\CascadeurChineseLauncher.exe";
    for (const auto& record : records) {
        if (!save(user, record, launcher, fault)) {
            error = L"关联备份未完整提交；未修改任何打开命令。保留备份供检查。";
            return false;
        }
    }
    for (const auto& record : records) {
        Value current;
        if (!read(user, path(record.index), nullptr, current) ||
            (!(current == record.original) && !(current == record.proxy)) ||
            (fault && !fault(L"before-apply", record.index)) ||
            !write(user, path(record.index), nullptr, record.proxy)) {
            bool restored = true;
            for (size_t n = 0; n < records.size(); ++n) {
                const auto& undo = records[n];
                Value now;
                if (!read(user, path(undo.index), nullptr, now)) { restored = false; continue; }
                if (now == undo.proxy && !(now == before[n])) {
                    if (!write(user, path(undo.index), nullptr, before[n])) { restored = false; continue; }
                    if (!before[n].present) {
                        const auto parents = nodes(undo.index);
                        for (unsigned p = static_cast<unsigned>(parents.size()); p-- > 0;)
                            if (undo.createdMask & (1u << p)) SHDeleteEmptyKeyW(user, parents[p].c_str());
                    }
                }
                if (!undo.saved) restored = clearJournal(user, undo.index) && restored;
            }
            error = restored ? L"关联写入失败；本次接管已撤回，未覆盖外部修改。" :
                               L"关联写入失败且恢复未完成；请保留文件和备份，勿继续拆卸。";
            return false;
        }
    }
    return true;
}

inline bool uninstall(HKEY user, const std::wstring& root, std::wstring& error) {
    std::vector<Record> records;
    const auto launcher = root + L"\\ChineseLauncher\\CascadeurChineseLauncher.exe";
    // Validate all journals before changing even the first command.
    for (unsigned i = 0; i < 2; ++i) {
        Record record;
        if (!load(user, i, launcher, record, error)) return false;
        if (record.saved) records.push_back(record);
        else {
            Value current;
            std::wstring command;
            if (!read(user, path(i), nullptr, current)) return false;
            if (text(current, command) && command.rfind(L"\"" + launcher + L"\"", 0) == 0) {
                error = L"打开命令仍指向汉化，但原始备份已缺失；拒绝猜测原关联。";
                return false;
            }
        }
    }
    for (const auto& record : records)
        if (!restoreRecord(user, record)) { error = L"无法恢复关联；请保留原安装目录和备份后重试。"; return false; }
    return true;
}
}
