// All registry mutations are confined to a unique disposable HKCU test key.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <cstdio>
#include <stdexcept>
#include "../association_proxy.h"

namespace P = CascadeurProxy;
const std::wstring root = L"C:\\Cascadeur Test";
const std::wstring launcher = root + L"\\ChineseLauncher\\CascadeurChineseLauncher.exe";
const auto original = P::stringValue(L"\"" + root + L"\\cascadeur.exe\" --mode project \"%1\"");
const auto proxy = P::stringValue(L"\"" + launcher + L"\" --mode project \"%1\"");
#define CHECK(x) do { if (!(x)) throw std::runtime_error("line " + std::to_string(__LINE__) + ": " #x); } while (false)

struct Fixture {
    std::wstring location;
    HKEY base = nullptr, user = nullptr, machine = nullptr;
    explicit Fixture(unsigned index) {
        location = L"Software\\CascadeurChineseProxyChecks\\" + std::to_wstring(GetCurrentProcessId()) +
            L"-" + std::to_wstring(GetTickCount64()) + L"-" + std::to_wstring(index);
        const auto created = RegCreateKeyExW(HKEY_CURRENT_USER, location.c_str(), 0, nullptr, REG_OPTION_NON_VOLATILE,
                             KEY_ALL_ACCESS, nullptr, &base, nullptr);
        if (created != ERROR_SUCCESS) std::fprintf(stderr, "Registry fixture creation status: %ld\n", created);
        CHECK(created == ERROR_SUCCESS);
        CHECK(RegCreateKeyExW(base, L"User", 0, nullptr, REG_OPTION_NON_VOLATILE, KEY_ALL_ACCESS, nullptr, &user, nullptr) == ERROR_SUCCESS);
        CHECK(RegCreateKeyExW(base, L"Machine", 0, nullptr, REG_OPTION_NON_VOLATILE, KEY_ALL_ACCESS, nullptr, &machine, nullptr) == ERROR_SUCCESS);
        CHECK(P::write(machine, P::path(0), nullptr, original));
    }
    ~Fixture() {
        if (user) RegCloseKey(user);
        if (machine) RegCloseKey(machine);
        if (base) RegCloseKey(base);
        // This generated test subtree, never real Software\Classes, is disposable.
        if (location.rfind(L"Software\\CascadeurChineseProxyChecks\\", 0) == 0)
            RegDeleteTreeW(HKEY_CURRENT_USER, location.c_str());
        SHDeleteEmptyKeyW(HKEY_CURRENT_USER, L"Software\\CascadeurChineseProxyChecks");
    }
    P::Value current(unsigned i = 0) { P::Value v; CHECK(P::read(user, P::path(i), nullptr, v)); return v; }
};
bool failBackup(const wchar_t* phase, unsigned) { return wcscmp(phase, L"before-complete") != 0; }
bool failSecond(const wchar_t* phase, unsigned index) { return wcscmp(phase, L"before-apply") != 0 || index != 1; }

int main() {
    try {
        std::wstring error;
        unsigned count = 0;
        {
            Fixture f(++count);
            CHECK(P::install(f.user, f.machine, root, error));
            CHECK(f.current() == proxy);
            P::Value machine; CHECK(P::read(f.machine, P::path(0), nullptr, machine)); CHECK(machine == original);
            CHECK(P::uninstall(f.user, root, error)); CHECK(!f.current().present);
            std::puts("PASS: user-only overlay, exact arguments, machine unchanged, restore inheritance");
        }
        {
            Fixture f(++count);
            auto expanded = P::stringValue(L"\"" + root + L"\\cascadeur.exe\"\t\"%1\" --cache %TEMP%", REG_EXPAND_SZ);
            CHECK(P::write(f.user, P::path(0), nullptr, expanded));
            CHECK(P::install(f.user, f.machine, root, error));
            CHECK(P::install(f.user, f.machine, root, error));
            CHECK(P::uninstall(f.user, root, error)); CHECK(f.current() == expanded);
            std::puts("PASS: reinstall keeps first raw backup, REG_EXPAND_SZ and tab arguments preserved");
        }
        {
            Fixture f(++count);
            CHECK(P::install(f.user, f.machine, root, error));
            auto external = P::stringValue(L"\"D:\\Other\\viewer.exe\" \"%1\"");
            CHECK(P::write(f.user, P::path(0), nullptr, external));
            CHECK(!P::install(f.user, f.machine, root, error));
            CHECK(P::uninstall(f.user, root, error)); CHECK(f.current() == external);
            std::puts("PASS: later user/official command change survives reinstall refusal and uninstall");
        }
        {
            Fixture f(++count);
            CHECK(P::install(f.user, f.machine, root, error));
            CHECK(P::write(f.user, P::path(0), L"UserAddedValue", P::number(42)));
            CHECK(P::write(f.user, P::nodes(0)[0] + L"\\UserAddedChild", L"Value", P::number(8)));
            CHECK(P::uninstall(f.user, root, error)); CHECK(!f.current().present);
            DWORD value = 0;
            CHECK(P::dword(f.user, P::path(0), L"UserAddedValue", value) && value == 42);
            CHECK(P::dword(f.user, P::nodes(0)[0] + L"\\UserAddedChild", L"Value", value) && value == 8);
            std::puts("PASS: cleanup preserves added values/subkeys under official handler");
        }
        {
            Fixture f(++count);
            CHECK(!P::install(f.user, f.machine, root, error, failBackup));
            CHECK(!f.current().present);
            DWORD complete = 0;
            CHECK(!P::dword(f.user, P::journal(0), L"Complete", complete));
            CHECK(!P::install(f.user, f.machine, root, error));
            CHECK(!P::uninstall(f.user, root, error));
            std::puts("PASS: incomplete backup never changes handler or masquerades as completed");
        }
        {
            Fixture f(++count);
            CHECK(P::write(f.machine, P::path(1), nullptr, original));
            CHECK(!P::install(f.user, f.machine, root, error, failSecond));
            CHECK(!f.current().present && !f.current(1).present);
            CHECK(P::install(f.user, f.machine, root, error));
            CHECK(!P::install(f.user, f.machine, root, error, failSecond));
            CHECK(f.current() == proxy && f.current(1) == proxy);
            CHECK(P::uninstall(f.user, root, error));
            std::puts("PASS: failed initial apply rolls back; failed upgrade preserves prior working proxy");
        }
        {
            Fixture f(++count);
            CHECK(P::install(f.user, f.machine, root, error));
            CHECK(!P::uninstall(f.user, L"D:\\Different Cascadeur", error));
            CHECK(f.current() == proxy);
            CHECK(P::write(f.user, P::journal(0), L"Complete", P::number(0)));
            CHECK(!P::uninstall(f.user, root, error)); CHECK(f.current() == proxy);
            std::puts("PASS: other installation and corrupt journal cannot restore/delete this proxy");
        }
        {
            Fixture f(++count);
            CHECK(P::write(f.user, P::path(0), nullptr, proxy));
            CHECK(!P::install(f.user, f.machine, root, error));
            CHECK(!P::uninstall(f.user, root, error)); CHECK(f.current() == proxy);
            std::puts("PASS: missing backup with live proxy fails closed");
        }
        {
            Fixture f(++count);
            const std::wstring choice = L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\FileExts\\.casc\\UserChoice";
            CHECK(P::write(f.user, choice, L"ProgId", P::stringValue(L"User.Selected.App")));
            CHECK(P::write(f.user, choice, L"Hash", P::stringValue(L"do-not-touch")));
            CHECK(P::write(f.user, L"Software\\Classes\\.casc", nullptr, P::stringValue(L"User.Selected.App")));
            CHECK(P::install(f.user, f.machine, root, error)); CHECK(P::uninstall(f.user, root, error));
            P::Value v;
            CHECK(P::read(f.user, choice, L"Hash", v) && v == P::stringValue(L"do-not-touch"));
            CHECK(P::read(f.user, choice, L"ProgId", v) && v == P::stringValue(L"User.Selected.App"));
            CHECK(P::read(f.user, L"Software\\Classes\\.casc", nullptr, v) && v == P::stringValue(L"User.Selected.App"));
            std::puts("PASS: UserChoice and extension default untouched");
        }
        {
            Fixture f(++count);
            CHECK(P::write(f.machine, P::path(0), nullptr, P::stringValue(L"\"E:\\Cascadeur\\cascadeur.exe\" \"%1\"")));
            CHECK(!P::install(f.user, f.machine, root, error));
            CHECK(P::install(f.user, f.machine, root, error, nullptr, true));
            CHECK(P::uninstall(f.user, root, error)); CHECK(!f.current().present);
            CHECK(P::write(f.machine, P::path(0), nullptr, P::stringValue(L"\"E:\\Other\\other.exe\" \"%1\"")));
            CHECK(!P::install(f.user, f.machine, root, error, nullptr, true));
            std::puts("PASS: explicit other-Cascadeur target supported; unrelated executables rejected");
        }
        std::printf("PASS: %u isolated registry scenarios\n", count);
        return 0;
    } catch (const std::exception& e) { std::fprintf(stderr, "FAIL: %s\n", e.what()); return 1; }
}
