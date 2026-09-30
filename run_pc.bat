@echo off
setlocal enabledelayedexpansion
cd /d "%~dp0"
set "PATH=C:\msys64\mingw64\bin;C:\msys64\usr\bin;%PATH%"

rem 1. Kiem tra xem co yeu cau build lai hay khong (-b hoac chua co binary)
set "NEED_BUILD=0"
if "%~1"=="-b" set "NEED_BUILD=1"
if "%~1"=="build" set "NEED_BUILD=1"
if not exist "bin\RomCloud.exe" set "NEED_BUILD=1"

if "!NEED_BUILD!"=="1" (
    echo [BUILD] Dang bien dich ban moi nhat cho PC Simulator...
    bash ./build_pc.sh
    if errorlevel 1 (
        echo [ERROR] Bien dich that bai!
        pause
        exit /b 1
    )
    echo [BUILD] Bien dich thanh cong!
)

if not exist "pc_data" (
    mkdir pc_data
)
if not exist "pc_data\assets\apps_icons" (
    echo [SETUP] Dong bo assets vao pc_data...
    xcopy /E /I /Y /Q "assets" "pc_data\assets" >nul
)

rem 2. Tat QuickEdit Mode trong console de tranh click chuot lam freeze tien trinh
powershell -NoProfile -Command "$h=[System.IntPtr]::Zero; $m=0; Add-Type -Name Win32 -Namespace Win32 -MemberDefinition '[DllImport(\"kernel32.dll\")] public static extern IntPtr GetStdHandle(int n); [DllImport(\"kernel32.dll\")] public static extern bool GetConsoleMode(IntPtr h, out uint m); [DllImport(\"kernel32.dll\")] public static extern bool SetConsoleMode(IntPtr h, uint m);'; $h=[Win32.Win32]::GetStdHandle(-10); if([Win32.Win32]::GetConsoleMode($h, [ref]$m)) { [Win32.Win32]::SetConsoleMode($h, $m -band -bnot 0x0040); }" 2>nul

echo ==============================================================
echo   RomCloud PC Simulator (TrimUI Brick Pro - 1024x768)
echo ==============================================================
echo [Meo] Chay '.\run_pc.bat -b' bat cu khi nao muon build lai code moi.
echo.
echo Controls:
echo   - Di chuyen: Phim mui ten (D-Pad)
echo   - Nut A: Enter / Space / A
echo   - Nut B: ESC / Backspace / B
echo   - Nut X: X
echo   - Nut Y: Y
echo   - Vai L1 / R1: Q / E (hoac PageUp / PageDown)
echo   - Vai L2 / R2: 1 / 2 (hoac Z / C)
echo   - SELECT / START: TAB / F1
echo   - Thoat app: Home hoac dong cua so
echo ==============================================================
bin\RomCloud.exe pc_data
