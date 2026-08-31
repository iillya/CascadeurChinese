@echo off
setlocal
set "ROOT=%~dp0.."
set "QT=%ROOT%\..\_ThirdParty\Qt\6.5.3\msvc2019_64"
set "OUT=%ROOT%\build\compatibility-test"
set "OLDHOST=%~1"
if not defined OLDHOST set "OLDHOST=E:\Cascadeur"
set "NEWHOST=%~2"
if not defined NEWHOST set "NEWHOST=C:\Program Files\Cascadeur"
if not exist "%OLDHOST%\Qt6Quick.dll" exit /b 1
if not exist "%NEWHOST%\Qt6Quick.dll" exit /b 1
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b 1
if not exist "%OUT%" mkdir "%OUT%"
cl /nologo /EHsc /std:c++17 /Zc:__cplusplus /permissive- /O2 /W4 /WX /utf-8 /MD /DQT_NO_DEBUG /DQT_GUI_LIB ^
 /external:I"%QT%\include" /external:W0 /I"%ROOT%\source\detours" "%ROOT%\source\analysis\qt_render_compatibility_test.cpp" ^
 /Fo"%OUT%\render.obj" /Fe:"%OUT%\render.exe" ^
 /link /SUBSYSTEM:CONSOLE /NODEFAULTLIB:LIBCMT /LIBPATH:"%QT%\lib" Qt6Core.lib Qt6Gui.lib Qt6Quick.lib Qt6Qml.lib "%ROOT%\build\obj\detours.lib" user32.lib shell32.lib version.lib
if errorlevel 1 exit /b 1
set "BASE_PATH=%PATH%"
call :testHost "%OLDHOST%" 6.5.1
if errorlevel 1 exit /b 1
call :testHost "%NEWHOST%" 6.5.3
exit /b %errorlevel%

:testHost
set "PATH=%~1;%QT%\bin;%BASE_PATH%"
set "QT_PLUGIN_PATH=%~1;%~1\plugins"
set "QML2_IMPORT_PATH=%~1\qml"
set "QML_IMPORT_PATH=%~1\qml"
rem The 2024 host ships only qwindows; do not borrow a newer QPA plugin.
set "QT_QPA_PLATFORM=windows"
set "QT_QUICK_BACKEND=software"
set "QSG_RENDER_LOOP=basic"
echo Testing actual host Qt: %~1
"%OUT%\render.exe" %~2
if errorlevel 1 exit /b 1
"%ROOT%\build\audit-test\runtime.exe"
if errorlevel 1 exit /b 1
"%ROOT%\build\hotkey-test\test.exe"
if errorlevel 1 exit /b 1
"%ROOT%\build\language-test\test.exe" "%ROOT%\source\hook.cpp"
if errorlevel 1 exit /b 1
"%ROOT%\build\audit-test\launcher.exe" "%~1\Qt6Core.dll" "%ROOT%\..\_ThirdParty\Qt\6.6.0\msvc2019_64\bin\Qt6Core.dll"
if errorlevel 1 exit /b 1
set "PYTHONPATH=%ROOT%\build\extraction-deps;%PYTHONPATH%"
python "%~dp0audit_release.py" --host "%~1"
exit /b %errorlevel%
