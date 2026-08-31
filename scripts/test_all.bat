@echo off
setlocal
set "ROOT=%~dp0.."
for %%T in (test_hotkey_settings test_language_button test_numeric_templates test_scene_text_policy test_audit) do (
    call "%~dp0%%T.bat"
    if errorlevel 1 exit /b 1
)
set "PYTHONPATH=%ROOT%\build\extraction-deps;%PYTHONPATH%"
python -m unittest discover -s "%ROOT%\scripts" -p "test_*.py"
if errorlevel 1 exit /b 1
python "%~dp0audit_release.py"
exit /b %errorlevel%
