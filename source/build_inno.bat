@echo off
setlocal
set "ROOT=%~dp0.."
if /I "%~1"=="--package-only" goto support
call "%~dp0build.bat" --runtime-only
if errorlevel 1 exit /b 1
:support
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b 1
if not exist "%ROOT%\build\inno" mkdir "%ROOT%\build\inno"
cl /nologo /O2 /W4 /WX /MT /LD /EHsc /std:c++17 /utf-8 /external:env:INCLUDE /external:W0 ^
 "%~dp0inno\support.cpp" /Fo"%ROOT%\build\inno\support.obj" /Fe:"%ROOT%\build\inno\support.dll" ^
 /link /IMPLIB:"%ROOT%\build\inno\support.lib" version.lib advapi32.lib shell32.lib shlwapi.lib
if errorlevel 1 exit /b 1
python "%~dp0inno\package.py"
exit /b %errorlevel%
