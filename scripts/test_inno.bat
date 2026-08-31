@echo off
setlocal
set "ROOT=%~dp0.."
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b 1
if not exist "%ROOT%\build\inno" mkdir "%ROOT%\build\inno"
cl /nologo /O2 /W4 /WX /MT /EHsc /std:c++17 /utf-8 /external:env:INCLUDE /external:W0 ^
 "%ROOT%\source\inno\tests\association_proxy_test.cpp" /Fo"%ROOT%\build\inno\proxy-test.obj" ^
 /Fe:"%ROOT%\build\inno\proxy-test.exe" /link advapi32.lib shlwapi.lib
if errorlevel 1 exit /b 1
"%ROOT%\build\inno\proxy-test.exe"
if errorlevel 1 exit /b 1
rc /nologo /fo "%ROOT%\build\inno\test-host.res" "%ROOT%\source\inno\tests\host.rc"
if errorlevel 1 exit /b 1
cl /nologo /O2 /W4 /WX /MT /EHsc "%ROOT%\source\inno\tests\host.cpp" ^
 /Fo"%ROOT%\build\inno\test-host.obj" "%ROOT%\build\inno\test-host.res" ^
 /Fe:"%ROOT%\build\inno\test-host.exe"
if errorlevel 1 exit /b 1
python "%ROOT%\source\inno\package.py" --test-mode
if errorlevel 1 exit /b 1
python "%~dp0inno_smoke_test.py"
exit /b %errorlevel%
