@echo off
setlocal
cd /d "%~dp0"

if not "%~2"=="" goto usage
if /i "%~1"=="/verify" goto args_ok
if not "%~1"=="" goto usage
:args_ok
set "APPLEPIE_DIR=%~dp0..\ApplepieManager"
if not exist "%APPLEPIE_DIR%\sdk\applepie_client.h" (
    echo [ERROR] Expected the official SDK at ..\ApplepieManager\sdk.
    exit /b 1
)

if /i "%VSCMD_ARG_TGT_ARCH%"=="x64" goto compiler_ready
if not exist "%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" (
    echo [ERROR] Install MSVC C++ Build Tools and Windows SDK, or use an x64 Developer Command Prompt.
    exit /b 1
)
for /f "usebackq tokens=*" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do call "%%i\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b 1

:compiler_ready
where cl >nul 2>nul
if errorlevel 1 (
    echo [ERROR] Could not initialize the x64 MSVC environment.
    exit /b 1
)
if not exist bin mkdir bin
if errorlevel 1 exit /b 1
if not exist bin\obj mkdir bin\obj
if not exist bin\test mkdir bin\test

cl /nologo /utf-8 /O2 /MT /LD /EHsc /std:c++17 /W4 /permissive- /wd4505 ^
    /DWIN32_LEAN_AND_MEAN /DNOMINMAX /I"%APPLEPIE_DIR%\sdk" ^
    src\*.cpp /Fo"bin\obj\\" /Fe:bin\EndfieldDataBridge.dll ^
    /link /INCREMENTAL:NO /MACHINE:X64
if errorlevel 1 exit /b 1
echo [OK] %CD%\bin\EndfieldDataBridge.dll
cl /nologo /utf-8 /O2 /MT /EHsc /std:c++17 /W4 /permissive- ^
    /DWIN32_LEAN_AND_MEAN /DNOMINMAX tools\efb_reader.cpp /Fo:bin\efb_reader.obj /Fe:bin\efb_reader.exe ^
    /link /INCREMENTAL:NO /MACHINE:X64
if errorlevel 1 exit /b 1
echo [OK] %CD%\bin\efb_reader.exe
if /i not "%~1"=="/verify" exit /b 0

rem Build an offline host from the official Core. This is not linked into the plugin.
if not exist "%APPLEPIE_DIR%\src\hub_core.h" (
    echo [ERROR] Offline verification requires the full ApplepieManager source checkout.
    exit /b 1
)
if not exist "%APPLEPIE_DIR%\deps\minhook_lib\lib\libMinHook.x64.lib" (
    echo [ERROR] Missing ApplepieManager's official x64 MinHook dependency.
    exit /b 1
)
cl /nologo /utf-8 /O2 /MT /EHsc /std:c++17 /W4 /wd4505 ^
    /I"%APPLEPIE_DIR%\sdk" /I"%APPLEPIE_DIR%\src" ^
    /I"%APPLEPIE_DIR%\deps\minhook_lib\include" ^
    tests\verify.cpp "%APPLEPIE_DIR%\src\hub_core.cpp" ^
    "%APPLEPIE_DIR%\src\hub_shared.cpp" "%APPLEPIE_DIR%\src\hub_runtime.cpp" ^
    "%APPLEPIE_DIR%\src\hub_pointers.cpp" "%APPLEPIE_DIR%\src\hub_windows.cpp" ^
    "%APPLEPIE_DIR%\src\hub_lifecycle.cpp" "%APPLEPIE_DIR%\src\hub_config.cpp" ^
    "%APPLEPIE_DIR%\deps\minhook_lib\lib\libMinHook.x64.lib" user32.lib comctl32.lib ^
    /Fo"bin\\" /Fe:bin\bridge_verify.exe ^
    /link /INCREMENTAL:NO /MACHINE:X64 /LTCG
if errorlevel 1 exit /b 1
bin\bridge_verify.exe
if errorlevel 1 exit /b 1
cl /nologo /utf-8 /O2 /MT /EHsc /std:c++17 /W4 /permissive- /wd4505 ^
    /DWIN32_LEAN_AND_MEAN /DNOMINMAX /I"%APPLEPIE_DIR%\sdk" ^
    tests\data_test.cpp src\*.cpp /Fo"bin\test\\" /Fe:bin\data_test.exe ^
    /link /INCREMENTAL:NO /MACHINE:X64
if errorlevel 1 exit /b 1
bin\data_test.exe
exit /b %ERRORLEVEL%

:usage
echo Usage: build.bat [/verify]
exit /b 2
