@echo off
setlocal enabledelayedexpansion

:: ─────────────────────────────────────────────────────────────────
:: Foxhole ESP – Internal DLL build script (EWDK / cl.exe)
:: Compile: x64 DLL  –  inject into Foxhole-Win64-Shipping.exe
:: ─────────────────────────────────────────────────────────────────

:: Auto-search for EWDK on mounted drives
set EWDK_ROOT=
set EWDK_ENV=

echo Searching for EWDK on mounted drives...

:: Check all possible drives C through Z
for %%D in (C D E F G H I J K L M N O P Q R S T U V W X Y Z) do (
    :: Check for EWDK directly on drive root (e.g., D:\BuildEnv\SetupBuildEnv.cmd)
    if exist "%%D:\BuildEnv\SetupBuildEnv.cmd" (
        set EWDK_ROOT=%%D:\
        set EWDK_ENV=!EWDK_ROOT!BuildEnv\SetupBuildEnv.cmd
        echo Found EWDK at: !EWDK_ROOT!
        goto :ewdk_found
    )
    
    :: Check for nested EWDK in \ewdk\ folder
    if exist "%%D:\ewdk\EWDK_ge_release_svc_prod1_26100_250904-1728\BuildEnv\SetupBuildEnv.cmd" (
        set EWDK_ROOT=%%D:\ewdk\EWDK_ge_release_svc_prod1_26100_250904-1728
        set EWDK_ENV=!EWDK_ROOT!\BuildEnv\SetupBuildEnv.cmd
        echo Found EWDK at: !EWDK_ROOT!
        goto :ewdk_found
    )
)

:ewdk_found
if not exist "%EWDK_ENV%" (
    echo ERROR: EWDK not found on any mounted drive.
    echo Please ensure EWDK is installed.
    goto :fail
)

set SDK_VER=10.0.26100.0
set MSVC_VER=14.44.35207

set MSVC_INC=%EWDK_ROOT%\Program Files\Microsoft Visual Studio\2022\BuildTools\VC\Tools\MSVC\%MSVC_VER%\include
set SDK_UCRT_INC=%EWDK_ROOT%\Program Files\Windows Kits\10\Include\%SDK_VER%\ucrt
set SDK_SHARED_INC=%EWDK_ROOT%\Program Files\Windows Kits\10\Include\%SDK_VER%\shared
set SDK_UM_INC=%EWDK_ROOT%\Program Files\Windows Kits\10\Include\%SDK_VER%\um
set MSVC_LIB=%EWDK_ROOT%\Program Files\Microsoft Visual Studio\2022\BuildTools\VC\Tools\MSVC\%MSVC_VER%\lib\x64
set SDK_UCRT_LIB=%EWDK_ROOT%\Program Files\Windows Kits\10\Lib\%SDK_VER%\ucrt\x64
set SDK_UM_LIB=%EWDK_ROOT%\Program Files\Windows Kits\10\Lib\%SDK_VER%\um\x64
set CL_EXE=%EWDK_ROOT%\Program Files\Microsoft Visual Studio\2022\BuildTools\VC\Tools\MSVC\%MSVC_VER%\bin\HostX64\x64\cl.exe
set LINK_EXE=%EWDK_ROOT%\Program Files\Microsoft Visual Studio\2022\BuildTools\VC\Tools\MSVC\%MSVC_VER%\bin\HostX64\x64\link.exe

set ROOT=%~dp0
set IMGUI_DIR=%ROOT%..\DeepRock\imgui
set BACKENDS_DIR=%ROOT%..\DeepRock\imgui\backends

:: ── Set up EWDK environment ───────────────────────────────────────
echo [1/3] Setting up EWDK build environment...
call "%EWDK_ENV%"
if %ERRORLEVEL% neq 0 (
    echo ERROR: Failed to initialise EWDK environment.
    echo        Check path: %EWDK_ENV%
    goto :fail
)

:: Return to project directory in case SetupBuildEnv.cmd changed it
cd /d "%ROOT%"

if not exist obj mkdir obj
if not exist bin mkdir bin

:: ── Compiler response file  (compile-only; linking handled separately)
(
echo /c
echo /nologo
echo /O2
echo /MT
echo /W3
echo /std:c++20
echo /EHa
echo /DNDEBUG
echo /I"%ROOT%"
echo /I"%ROOT%4.24.3-0+++UE4+Release-4.24-War\CppSDK"
echo /I"%IMGUI_DIR%"
echo /I"%BACKENDS_DIR%"
echo /I"%MSVC_INC%"
echo /I"%SDK_UCRT_INC%"
echo /I"%SDK_SHARED_INC%"
echo /I"%SDK_UM_INC%"
echo /Fo"obj\\"
echo "%ROOT%main.cpp"
echo "%ROOT%4.24.3-0+++UE4+Release-4.24-War\CppSDK\SDK\Basic.cpp"
echo "%ROOT%4.24.3-0+++UE4+Release-4.24-War\CppSDK\SDK\CoreUObject_functions.cpp"
echo "%IMGUI_DIR%\imgui.cpp"
echo "%IMGUI_DIR%\imgui_draw.cpp"
echo "%IMGUI_DIR%\imgui_tables.cpp"
echo "%IMGUI_DIR%\imgui_widgets.cpp"
echo "%BACKENDS_DIR%\imgui_impl_win32.cpp"
echo "%BACKENDS_DIR%\imgui_impl_dx11.cpp"
) > obj\cl.rsp

:: ── Linker response file  (obj files + flags; avoids @rsp-via-/link issue)
(
echo /DLL
echo /MACHINE:X64
echo /OUT:"bin\foxhole_esp.dll"
echo /LIBPATH:"%MSVC_LIB%"
echo /LIBPATH:"%SDK_UCRT_LIB%"
echo /LIBPATH:"%SDK_UM_LIB%"
echo d3d11.lib
echo dxgi.lib
echo user32.lib
echo kernel32.lib
echo gdi32.lib
echo shell32.lib
echo imm32.lib
echo version.lib
echo obj\main.obj
echo obj\Basic.obj
echo obj\CoreUObject_functions.obj
echo obj\imgui.obj
echo obj\imgui_draw.obj
echo obj\imgui_tables.obj
echo obj\imgui_widgets.obj
echo obj\imgui_impl_win32.obj
echo obj\imgui_impl_dx11.obj
) > obj\link.rsp

:: ── Compile (object files only)
echo [2/3] Compiling sources...
"%CL_EXE%" @obj\cl.rsp
if %ERRORLEVEL% neq 0 goto :fail

:: ── Link
echo [3/3] Linking DLL...
"%LINK_EXE%" @obj\link.rsp
if %ERRORLEVEL% neq 0 goto :fail

:: ── Done ─────────────────────────────────────────────────────────
echo.
echo BUILD SUCCEEDED
echo        Output: bin\foxhole_esp.dll
echo.
echo Inject foxhole_esp.dll into Foxhole-Win64-Shipping.exe
echo Press INSERT in-game to toggle the menu.
goto :end

:fail
echo.
echo *** BUILD FAILED (errorlevel %ERRORLEVEL%) ***
echo.
exit /b 1

:end
endlocal
