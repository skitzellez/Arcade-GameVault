@echo off
rem Copyright (c) 2026 Skitzelles - part of Game Vault's arcade loader, GPL-3.0. Builds dist\ArcadeLoader_x64.appx.
setlocal
set SDK=C:\Program Files (x86)\Windows Kits\10\bin\10.0.26100.0\x64
cd /d "%~dp0"
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul 2>nul
if not exist out\app mkdir out\app
cl /nologo /std:c++17 /EHsc /O2 /W3 /MD /DNOMINMAX /DUNICODE /D_UNICODE /DWINAPI_FAMILY=WINAPI_FAMILY_APP /D__WRL_NO_DEFAULT_LIB__ /Fo:out\app\ /Fe:pkg\ArcadeLoader.exe src\uwp_main.cpp src\peload.cpp src\peload32.cpp src\shims.cpp src\dinput.cpp src\arcade_io.cpp src\msvcrt_compat.cpp src\d3d9_bridge32.cpp src\memtally.cpp src\tbbmalloc_standin.cpp src\filebacked.cpp src\xaudio27.cpp src\controls.cpp src\europa.cpp src\winsock32.cpp src\acio32.cpp src\gdi32.cpp src\ringedge.cpp src\gvaudio.cpp src\netfs.cpp src\window_xbox.cpp /link /APPCONTAINER /SUBSYSTEM:WINDOWS /LIBPATH:"%VCToolsInstallDir%lib\x64\store" WindowsApp.lib d3d11.lib dxgi.lib xinputuap.lib xaudio2.lib ws2_32.lib || exit /b 1
copy /y out\hello_nocrt.exe pkg\programs\ >nul
copy /y out\hello_crt.exe pkg\programs\ >nul
copy /y out\d3d11_window.exe pkg\programs\ >nul
copy /y out\xinput_test.exe pkg\programs\ >nul
copy /y out\d3d11_factory.exe pkg\programs\ >nul
copy /y out\mode32_test.exe pkg\programs\ >nul
copy /y out\hello32.exe pkg\programs\ >nul
copy /y out\crt32.exe pkg\programs\ >nul
copy /y out\usedll32.exe pkg\programs\ >nul
copy /y out\testdll32.dll pkg\programs\ >nul
copy /y out\thread32.exe pkg\programs\ >nul
copy /y out\rawthread32.exe pkg\programs\ >nul
copy /y out\exc32.exe pkg\programs\ >nul
copy /y out\d3d9_test.exe pkg\programs\ >nul
copy /y out\d3d9_test32.exe pkg\programs\ >nul
rem DXVK (D3D9 on Vulkan, zlib licence) for the programs, mapped by the loader; Mesa's Vulkan-on-D3D12 driver (Dozen,
rem MIT) loaded as a packaged DLL, with the host-window hook (deps\mesa-arcade, branch gv-arcade-dzn)
call x86dll\build.bat >nul || exit /b 1
cd /d "%~dp0"
if not exist pkg\x86 mkdir pkg\x86
copy /y out\x86dll\*.dll pkg\x86\ >nul || exit /b 1
if not exist pkg\x64 mkdir pkg\x64
copy /y ..\deps\dxvk-src\build64\src\d3d9\d3d9.dll pkg\x64\ >nul || exit /b 1
copy /y ..\deps\mesa-arcade\build-arcade-dzn\src\microsoft\vulkan\vulkan_dzn.dll pkg\ >nul || exit /b 1
rem Microsoft's DXIL signer (redistributable, the same file xemu ships): Dozen needs it to start
copy /y ..\XemuUWP\pkg\dxil.dll pkg\ >nul || exit /b 1
rem Microsoft redistributables (DirectX June 2010 + VC2010) from this PC, mapped by the loader
for %%d in (D3DX9_43.dll D3DCompiler_43.dll d3dx11_43.dll X3DAudio1_7.dll XAPOFX1_5.dll msvcr100.dll msvcp100.dll) do if exist C:\Windows\System32\%%d copy /y C:\Windows\System32\%%d pkg\x64\ >nul
rem 32-bit DirectX helpers older games expect but often leave out: d3dx9_27 (Battle Fantasia), d3dx9_42 + d3dcompiler_42 (Caladrius AC)
for %%d in (d3dx9_27.dll D3DX9_42.dll D3DCompiler_42.dll) do copy /y C:\Windows\SysWOW64\%%d pkg\x86\ >nul || exit /b 1
if not exist dist mkdir dist
if exist dist\ArcadeLoader_x64.appx del dist\ArcadeLoader_x64.appx
rem TeknoParrot game profiles (GPL-3.0, TeknoParrotUI 2018): the controls, mapped by controls.cpp
if not exist pkg\tpprofiles mkdir pkg\tpprofiles
copy /y tpprofiles\*.xml pkg\tpprofiles\ >nul
rem libsmb2 (LGPL-2.1): the same smb2.dll Game Vault ships, for games played straight from the NAS (netfs.cpp)
copy /y ..\GameVault\pkg\smb2.dll pkg\ >nul
"%SDK%\makeappx.exe" pack /o /d pkg /p dist\ArcadeLoader_x64.appx >nul || exit /b 1
if not defined PFX_PASSWORD for /f "usebackq delims=" %%p in (`powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0..\GameVault\pfx_password.ps1"`) do set "PFX_PASSWORD=%%p"
"%SDK%\signtool.exe" sign /fd SHA256 /a /f ..\GameVault\cert\GameVault.pfx /p %PFX_PASSWORD% dist\ArcadeLoader_x64.appx >nul || exit /b 1
echo XBOX_BUILD_OK
