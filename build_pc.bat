@echo off
rem Copyright (c) 2026 Skitzelles - part of Game Vault's arcade loader, GPL-3.0. PC test build + test programs.
setlocal
cd /d "%~dp0"
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul 2>nul
if not exist out mkdir out
cl /nologo /std:c++17 /EHsc /O2 /W3 /MD /DNOMINMAX /DUNICODE /D_UNICODE /Fo:out\ /Fe:out\ArcadeLoaderPC.exe src\pc_main.cpp src\peload.cpp src\peload32.cpp src\shims.cpp src\dinput.cpp src\arcade_io.cpp src\msvcrt_compat.cpp src\d3d9_bridge32.cpp src\memtally.cpp src\tbbmalloc_standin.cpp src\filebacked.cpp src\xaudio27.cpp src\controls.cpp src\europa.cpp src\winsock32.cpp src\acio32.cpp src\gdi32.cpp src\ringedge.cpp src\gvaudio.cpp src\netfs.cpp user32.lib ole32.lib xaudio2.lib ws2_32.lib onecore.lib dwrite.lib || exit /b 1
cl /nologo /O1 /GS- /Fo:out\ /Fe:out\hello_nocrt.exe test\hello_nocrt.c /link /NODEFAULTLIB /ENTRY:Start /SUBSYSTEM:WINDOWS kernel32.lib user32.lib || exit /b 1
cl /nologo /std:c++17 /EHsc /O2 /MD /Fo:out\ /Fe:out\hello_crt.exe test\hello_crt.cpp || exit /b 1
cl /nologo /std:c++17 /EHsc /O2 /MD /Fo:out\ /Fe:out\d3d11_window.exe test\d3d11_window.cpp /link /SUBSYSTEM:WINDOWS || exit /b 1
cl /nologo /std:c++17 /EHsc /O2 /MD /Fo:out\ /Fe:out\xinput_test.exe test\xinput_test.cpp || exit /b 1
cl /nologo /std:c++17 /EHsc /O2 /MD /Fo:out\ /Fe:out\d3d11_factory.exe test\d3d11_factory.cpp /link /SUBSYSTEM:WINDOWS || exit /b 1
cl /nologo /std:c++17 /EHsc /O2 /MD /Fo:out\ /Fe:out\mode32_test.exe test\mode32_test.cpp || exit /b 1
cl /nologo /std:c++17 /EHsc /O2 /MD /Fo:out\ /Fe:out\d3d9_test.exe test\d3d9_test.cpp /link /SUBSYSTEM:WINDOWS user32.lib || exit /b 1
rem TeknoParrot game profiles (GPL-3.0, TeknoParrotUI 2018): the controls, mapped by controls.cpp
if not exist out\tpprofiles mkdir out\tpprofiles
copy /y tpprofiles\*.xml out\tpprofiles\ >nul
echo PC_BUILD_OK
