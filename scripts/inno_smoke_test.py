"""Opt-in real installer smoke test against an inert, isolated host fixture.

Only the test package may run here. Its registry key is separate, its [Icons]
and [Registry] sections are disabled, and it does not request elevation.
Evidence is retained below build/inno/tests; no recursive workspace cleanup.
"""
import ctypes
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import time
import uuid
import winreg

ROOT = Path(__file__).resolve().parents[1]
REGISTRY_KEY = r"Software\Microsoft\Windows\CurrentVersion\Uninstall\CascadeurChinese.Inno.IsolatedTests_is1"


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def registered():
    try:
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER, REGISTRY_KEY):
            return True
    except FileNotFoundError:
        return False


def main():
    if registered():
        raise SystemExit("Previous isolated-test installation exists; inspect/uninstall it before rerunning.")
    manifest = json.loads((ROOT / "build/inno/test-manifest.json").read_text(encoding="utf-8"))
    installer = ROOT / "build/inno/test-package/CascadeurChineseInstaller.exe"
    assert manifest["test_mode"] is True and manifest["sha256"] == sha(installer)
    support = ctypes.WinDLL(str(ROOT / "build/inno/support.dll"))
    support.CheckTarget.argtypes = [ctypes.c_wchar_p, ctypes.c_int, ctypes.c_wchar_p, ctypes.c_uint]
    support.CheckTarget.restype = ctypes.c_int
    evidence = ROOT / "build/inno/tests" / ("run-" + uuid.uuid4().hex[:10])
    host = evidence / "宿主 目录"
    host.mkdir(parents=True)
    fixture = ROOT / "build/inno/test-host.exe"
    host_files = ["cascadeur.exe", "Qt6Core.dll", "Qt6Gui.dll", "Qt6Qml.dll", "Qt6Quick.dll"]
    for name in host_files:
        shutil.copyfile(fixture, host / name)
    originals = {name: sha(host / name) for name in host_files}
    results = []

    def check(condition, title):
        if not condition:
            raise AssertionError(title)
        results.append(title)
        print("PASS:", title, flush=True)

    def valid(path=host, version=True):
        message = ctypes.create_unicode_buffer(1024)
        ok = support.CheckTarget(str(path), version, message, 1024)
        return bool(ok), message.value

    def run_install(title, path=host, expect_success=True):
        result = subprocess.run([str(installer), "/VERYSILENT", "/SUPPRESSMSGBOXES", "/NORESTART",
                                 "/TASKS=", f"/DIR={path}", f"/LOG={evidence / (title + '.log')}"],
                                timeout=60, creationflags=subprocess.CREATE_NO_WINDOW)
        check((result.returncode == 0) == expect_success, title + f" (exit {result.returncode})")

    check(valid()[0], "Qt 6.5.3 x64 fixture accepted")
    check(not valid(Path("C:/"))[0], "drive root rejected")
    check(not valid(host / "ChineseLauncher")[0], "wrong directory rejected")
    shutil.copyfile(ROOT / "build/out/CascadeurChineseLauncher.exe", host / "Qt6Core.dll")
    check(not valid()[0], "missing/mismatched Qt version rejected")
    run_install("reject-bad-qt", expect_success=False)
    check(not (host / "ChineseLauncher").exists(), "rejected install wrote no payload")
    shutil.copyfile(fixture, host / "Qt6Core.dll")
    shutil.copyfile(fixture, host / "Qt5Core.dll")
    check(not valid()[0], "mixed Qt5/Qt6 host rejected")
    (host / "Qt5Core.dll").unlink()
    process = subprocess.Popen([str(host / "cascadeur.exe")], creationflags=subprocess.CREATE_NO_WINDOW)
    try:
        time.sleep(0.1)
        check(not valid()[0], "running host rejected")
        run_install("reject-running-host", expect_success=False)
        check(process.poll() is None, "installer did not terminate the host")
    finally:
        process.terminate()  # Only the inert fixture created by this test.
        process.wait(timeout=10)

    run_install("fresh-install")
    installed = host / "ChineseLauncher"
    for entry in manifest["payload"]:
        check(sha(installed / entry["name"]) == entry["sha256"], "payload hash " + entry["name"])
    check(registered(), "standard uninstall record exists")
    run_install("repeat-install")
    check(len(list((installed / ".inno/backups").iterdir())) >= 1, "upgrade snapshot retained")
    second = evidence / "other-host"
    second.mkdir()
    for name in host_files:
        shutil.copyfile(fixture, second / name)
    run_install("reject-second-location", second, expect_success=False)

    dictionary = installed / "translations/dictionary_zh.json"
    custom = dictionary.read_bytes() + b"\n "
    dictionary.write_bytes(custom)
    user_dict = installed / "translations/user_zh.json"
    user_dict.write_text('{"translations":{"Test":"test"}}', encoding="utf-8")
    run_install("preserve-edited-dictionary")
    check(dictionary.read_bytes() == custom, "edited dictionary preserved in place")
    check(sha(installed / ".inno/defaults/dictionary_zh.json") == sha(ROOT / "translations/dictionary_zh.json"),
          "new default dictionary available separately")

    uninstaller = installed / ".inno/unins000.exe"
    process = subprocess.Popen([str(host / "cascadeur.exe")], creationflags=subprocess.CREATE_NO_WINDOW)
    try:
        time.sleep(0.1)
        result = subprocess.run([str(uninstaller), "/VERYSILENT", "/SUPPRESSMSGBOXES", "/NORESTART",
                                 f"/LOG={evidence / 'uninstall-running.log'}"], timeout=60)
        check(result.returncode != 0 and (installed / "CascadeurChineseHook.dll").is_file(),
              "uninstall refused while host is running")
    finally:
        process.terminate()
        process.wait(timeout=10)
    result = subprocess.run([str(uninstaller), "/VERYSILENT", "/SUPPRESSMSGBOXES", "/NORESTART",
                             f"/LOG={evidence / 'uninstall.log'}"], timeout=60)
    check(result.returncode == 0, "uninstall completed")
    check(not registered(), "uninstall record removed")
    check(not (installed / "CascadeurChineseHook.dll").exists(), "owned hook removed")
    check(not (installed / "translations/numeric_templates.json").exists(), "unmodified packaged config removed")
    check(dictionary.read_bytes() == custom and user_dict.exists(), "edited and extra dictionaries survive uninstall")
    deadline = time.monotonic() + 5
    while (installed / ".inno").exists() and time.monotonic() < deadline:
        time.sleep(0.05)
    check(not (installed / ".inno").exists(), "installer snapshots, defaults and state fully removed")
    check(all(sha(host / name) == digest for name, digest in originals.items()), "host files unchanged")
    (evidence / "results.json").write_text(json.dumps(results, ensure_ascii=False, indent=2), encoding="utf-8")
    print("Evidence:", evidence)


if __name__ == "__main__":
    main()
