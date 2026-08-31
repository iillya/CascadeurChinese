@echo off
setlocal
set "ROOT=%~dp0.."
set "QT=%ROOT%\..\_ThirdParty\Qt\6.5.3\msvc2019_64"
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b 1
if not exist "%ROOT%\build\language-test" mkdir "%ROOT%\build\language-test"
cl /nologo /EHsc /std:c++17 /Zc:__cplusplus /permissive- /O2 /W4 /WX /utf-8 /MD /DQT_NO_DEBUG /DQT_GUI_LIB ^
 /external:I"%QT%\include" /external:W0 "%ROOT%\source\analysis\language_button_test.cpp" ^
 /Fo"%ROOT%\build\language-test\test.obj" /Fe"%ROOT%\build\language-test\test.exe" ^
 /link /LIBPATH:"%QT%\lib" Qt6Core.lib Qt6Gui.lib Qt6Quick.lib Qt6Qml.lib Qt6Test.lib
if errorlevel 1 exit /b 1
set "PATH=C:\Program Files\Cascadeur;%QT%\bin;%PATH%"
set "QT_PLUGIN_PATH=C:\Program Files\Cascadeur\plugins"
set "QML2_IMPORT_PATH=C:\Program Files\Cascadeur\qml"
set "QT_QPA_PLATFORM=offscreen"
"%ROOT%\build\language-test\test.exe" "%ROOT%\source\hook.cpp"
exit /b %errorlevel%
