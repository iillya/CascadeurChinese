@echo off
setlocal
set "ROOT=%~dp0.."
set "QT=%ROOT%\..\_ThirdParty\Qt\6.5.3\msvc2019_64"
set "OUT=%ROOT%\build\audit-test"
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b 1
if not exist "%OUT%" mkdir "%OUT%"
cl /nologo /EHsc /std:c++17 /Zc:__cplusplus /permissive- /O2 /W4 /WX /utf-8 /MD /DQT_NO_DEBUG /DQT_GUI_LIB ^
 /external:I"%QT%\include" /external:W0 /I"%ROOT%\source\detours" "%ROOT%\source\analysis\runtime_audit_test.cpp" ^
 /Fo"%OUT%\runtime.obj" /Fe:"%OUT%\runtime.exe" ^
 /link /SUBSYSTEM:CONSOLE /NODEFAULTLIB:LIBCMT /LIBPATH:"%QT%\lib" Qt6Core.lib Qt6Gui.lib Qt6Quick.lib Qt6Qml.lib "%ROOT%\build\obj\detours.lib" user32.lib shell32.lib version.lib
if errorlevel 1 exit /b 1
set "PATH=C:\Program Files\Cascadeur;%QT%\bin;%PATH%"
set "QT_PLUGIN_PATH=C:\Program Files\Cascadeur\plugins"
set "QML2_IMPORT_PATH=C:\Program Files\Cascadeur\qml"
set "QT_QPA_PLATFORM=offscreen"
"%OUT%\runtime.exe"
if errorlevel 1 exit /b 1
python -m unittest discover -s "%ROOT%\scripts" -p "test_inno_package.py"
if errorlevel 1 exit /b 1
cl /nologo /EHsc /std:c++17 /O2 /W4 /WX /utf-8 "%ROOT%\source\analysis\launcher_compatibility_test.cpp" ^
 /Fo"%OUT%\launcher.obj" /Fe:"%OUT%\launcher.exe" /link /SUBSYSTEM:CONSOLE user32.lib version.lib
if errorlevel 1 exit /b 1
"%OUT%\launcher.exe" "C:\Program Files\Cascadeur\Qt6Core.dll" "%ROOT%\..\_ThirdParty\Qt\6.6.0\msvc2019_64\bin\Qt6Core.dll"
exit /b %errorlevel%
