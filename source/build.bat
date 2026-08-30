@echo off
setlocal

rem Cascadeur Chinese Localizer - Qt 6 pure display-layer build.
set "ROOT=%~dp0.."
set "CPP=%ROOT%\source"
set "OUT=%ROOT%\build\out"
set "OBJDIR=%ROOT%\build\obj"
set "RES=%ROOT%\icon"
set "QT=%ROOT%\..\_ThirdParty\Qt\6.5.3\msvc2019_64"
if not exist "%QT%\lib\Qt6Quick.lib" set "QT=%ROOT%\third_party\qt6sdk"
if not exist "%OUT%" mkdir "%OUT%"
if not exist "%OBJDIR%" mkdir "%OBJDIR%"
if not exist "%OBJDIR%\detours" mkdir "%OBJDIR%\detours"

if not exist "%QT%\lib\Qt6Quick.lib" (
    echo [ERROR] Qt 6.5.3 SDK with Qt Quick was not found at %QT%.
    exit /b 1
)

set "VCVARS=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
if not exist "%VCVARS%" set "VCVARS=C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
if not exist "%VCVARS%" (
    echo [ERROR] Visual Studio 2022 C++ tools were not found.
    exit /b 1
)
call "%VCVARS%" >nul

echo === Compiling Detours ===
cl /nologo /O2 /W3 /c /EHsc /std:c++17 /Zc:gotoScope- ^
   /DWIN32_LEAN_AND_MEAN /DNOMINMAX /D_CRT_SECURE_NO_WARNINGS ^
   /I"%CPP%\detours" /Fo"%OBJDIR%\detours\\" ^
   "%CPP%\detours\creatwth.cpp" "%CPP%\detours\detours.cpp" ^
   "%CPP%\detours\disasm.cpp" "%CPP%\detours\disolarm.cpp" ^
   "%CPP%\detours\disolarm64.cpp" "%CPP%\detours\disolia64.cpp" ^
   "%CPP%\detours\disolx64.cpp" "%CPP%\detours\disolx86.cpp" ^
   "%CPP%\detours\image.cpp" "%CPP%\detours\modules.cpp"
if errorlevel 1 exit /b 1
lib /nologo /OUT:"%OBJDIR%\detours.lib" "%OBJDIR%\detours\*.obj"
if errorlevel 1 exit /b 1

echo === Compiling Qt Quick display hook ===
cl /nologo /O2 /W3 /LD /EHsc /std:c++17 /Zc:__cplusplus /permissive- /utf-8 ^
   /I"%QT%\include" /I"%CPP%\detours" ^
   "%CPP%\hook.cpp" /Fo"%OBJDIR%\hook.obj" ^
   /Fe:"%OUT%\CascadeurChineseHook.dll" ^
   /link /IMPLIB:"%OBJDIR%\CascadeurChineseHook.lib" ^
   "%OBJDIR%\detours.lib" "%QT%\lib\Qt6Core.lib" "%QT%\lib\Qt6Gui.lib" ^
   "%QT%\lib\Qt6Qml.lib" "%QT%\lib\Qt6Quick.lib" ^
   user32.lib shell32.lib version.lib
if errorlevel 1 exit /b 1

echo === Compiling launcher ===
rc /nologo /fo "%OBJDIR%\app_icon.res" "%RES%\app_icon.rc"
if errorlevel 1 exit /b 1
cl /nologo /O2 /W3 /EHsc /utf-8 "%CPP%\launcher.cpp" ^
   /Fo"%OBJDIR%\launcher.obj" "%OBJDIR%\app_icon.res" ^
   /Fe:"%OUT%\CascadeurChineseLauncher.exe" /link /SUBSYSTEM:WINDOWS user32.lib
if errorlevel 1 exit /b 1

echo === Compiling installer ===
cl /nologo /O2 /W3 /EHsc /std:c++17 /utf-8 "%CPP%\installer.cpp" ^
   /Fo"%OBJDIR%\installer.obj" "%OBJDIR%\app_icon.res" ^
   /Fe:"%OUT%\CascadeurChineseInstaller.exe" ^
   /link /SUBSYSTEM:WINDOWS user32.lib shell32.lib ole32.lib gdi32.lib advapi32.lib ^
   /MANIFEST:EMBED /MANIFESTUAC:"level='requireAdministrator' uiAccess='false'"
if errorlevel 1 exit /b 1

if not exist "%OUT%\translations" mkdir "%OUT%\translations"
if exist "%OUT%\translations\cascadeur_ui_zh.json" del /Q "%OUT%\translations\cascadeur_ui_zh.json"
copy /Y "%ROOT%\translations\dictionary_zh.json" "%OUT%\translations\dictionary_zh.json" >nul

echo === Packing distributable installer ===
python "%ROOT%\source\embed_files.py"
if errorlevel 1 exit /b 1

echo Build complete:
echo   %OUT%\CascadeurChineseHook.dll
echo   %OUT%\CascadeurChineseLauncher.exe
echo   dist\CascadeurChineseInstaller.exe
endlocal
