@echo off
setlocal
rem Copyright (c) 2026 Skitzelles - part of Game Vault's arcade loader, GPL-3.0. 32-bit stand-in DLLs for 32-bit games
rem (dinput8, dsound) and d3d8to9 as d3d8.dll -> ..\out\x86dll
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvarsamd64_x86.bat" >nul 2>nul
cd /d "%~dp0"
if not exist ..\out\x86dll mkdir ..\out\x86dll
if not exist ..\out\x86obj mkdir ..\out\x86obj
cl /nologo /O2 /MT /GS- /LD /Fo:..\out\x86obj\ /Fe:..\out\x86dll\dinput8.dll dinput8.cpp /link /DEF:dinput8.def kernel32.lib || exit /b 1
cl /nologo /O2 /MT /GS- /LD /Fo:..\out\x86obj\ /Fe:..\out\x86dll\dsound.dll dsound.cpp /link /DEF:dsound.def kernel32.lib || exit /b 1
cl /nologo /O2 /MT /GS- /LD /Fo:..\out\x86obj\ /Fe:..\out\x86dll\idmacdrv32.dll idmacdrv32.cpp /link /DEF:idmacdrv32.def kernel32.lib || exit /b 1
cl /nologo /O2 /MT /GS- /LD /Fo:..\out\x86obj\ /Fe:..\out\x86dll\msvcrt.dll msvcrt.cpp /link /DEF:msvcrt.def kernel32.lib legacy_stdio_definitions.lib || exit /b 1
call ..\..\deps\d3d8to9\build_x86.bat >nul || exit /b 1
cd /d "%~dp0"
copy /y ..\..\deps\d3d8to9\out\d3d8.dll ..\out\x86dll\ >nul
echo X86DLL_OK
