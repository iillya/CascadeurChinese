@echo off
setlocal
set "ROOT=%~dp0.."
set "QT=%ROOT%\..\_ThirdParty\Qt\6.5.3\msvc2019_64"
set "VCVARS=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
if not exist "%VCVARS%" exit /b 2
call "%VCVARS%" >nul
if errorlevel 1 exit /b 3
if not exist "%ROOT%\build\text-utils-probe" mkdir "%ROOT%\build\text-utils-probe"
cl /nologo /EHsc /std:c++17 /Zc:__cplusplus /permissive- /O2 /W4 /WX /utf-8 /MD /DQT_NO_DEBUG ^
 /I"%QT%\include" /I"%QT%\include\QtCore" ^
 "%ROOT%\source\analysis\text_utils_reference.cpp" ^
 /Fo"%ROOT%\build\text-utils-probe\reference.obj" ^
 /Fe"%ROOT%\build\text-utils-probe\reference.exe" ^
 /link /LIBPATH:"%QT%\lib" Qt6Core.lib
exit /b %errorlevel%
