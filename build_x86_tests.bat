@echo off
rem Copyright (c) 2026 Skitzelles - part of Game Vault's arcade loader, GPL-3.0. 32-bit test programs for the 32-bit bridge.
setlocal
cd /d "%~dp0"
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvarsamd64_x86.bat" >nul 2>nul
if not exist out\x86 mkdir out\x86
cl /nologo /O1 /GS- /Fo:out\x86\ /Fe:out\hello32.exe test\hello32.c /link /NODEFAULTLIB /ENTRY:Start /SUBSYSTEM:WINDOWS kernel32.lib user32.lib || exit /b 1
cl /nologo /std:c++17 /EHsc /O2 /MT /Fo:out\x86\ /Fe:out\crt32.exe test\crt32.cpp || exit /b 1
cl /nologo /O1 /GS- /LD /Fo:out\x86\ /Fe:out\testdll32.dll test\testdll32.c /link /NODEFAULTLIB /ENTRY:DllMain kernel32.lib || exit /b 1
cl /nologo /O1 /GS- /Fo:out\x86\ /Fe:out\usedll32.exe test\usedll32.c /link /NODEFAULTLIB /ENTRY:Start /SUBSYSTEM:WINDOWS kernel32.lib out\testdll32.lib || exit /b 1
cl /nologo /std:c++17 /EHsc /O2 /MT /Fo:out\x86\ /Fe:out\thread32.exe test\thread32.cpp || exit /b 1
cl /nologo /O1 /GS- /Fo:out\x86\ /Fe:out\rawthread32.exe test\rawthread32.c /link /NODEFAULTLIB /ENTRY:Start /SUBSYSTEM:WINDOWS kernel32.lib || exit /b 1
cl /nologo /std:c++17 /EHa /O1 /MT /Fo:out\x86\ /Fe:out\exc32.exe test\exc32.cpp || exit /b 1
cl /nologo /std:c++17 /EHsc /O2 /MT /Fo:out\x86\ /Fe:out\d3d9_test32.exe test\d3d9_test.cpp /link /SUBSYSTEM:WINDOWS user32.lib || exit /b 1
echo X86_OK
