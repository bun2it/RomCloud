@echo off
chcp 65001 >nul
REM RomCloud - Cai dat 1 cham cho Windows qua USB Storage (double-click file nay)
REM Khong can driver Zadig, khong can adb: chep file thang vao the nho.
cd /d "%~dp0"

echo ==========================================
echo   Cai dat RomCloud 1 cham (Windows)
echo   (qua USB Storage - khong can driver/adb)
echo ==========================================
echo 1. Tren may Brick: mo app USB Storage (trong Apps he thong).
echo 2. Cho the nho hien len may tinh roi bam phim bat ky.
echo.
pause >nul

echo [*] Dang tim the nho (toi da 90s)...
set DRIVE=
set WT=0
:waitloop
for %%d in (D E F G H I J K L M N O P Q R S T U V W X Y Z) do (
  if exist "%%d:\Apps\RomCloud\" set DRIVE=%%d:
)
if not defined DRIVE (
  for %%d in (D E F G H I J K L M N O P Q R S T U V W X Y Z) do (
    if exist "%%d:\Emus\" if exist "%%d:\Roms\" set DRIVE=%%d:
  )
)
if defined DRIVE goto drivefound
set /a WT+=1
if %WT% GEQ 30 (
  echo [!] Khong thay the nho. Ban da mo app USB Storage tren Brick chua?
  pause & exit /b 1
)
timeout /t 3 /nobreak >nul
goto waitloop
:drivefound
echo [*] Thay the nho: %DRIVE% (ok)

echo [*] Dang hoi ban RomCloud moi nhat...
powershell -NoProfile -Command "$r = Invoke-RestMethod 'https://api.github.com/repos/bun2it/RomCloud/releases/latest'; ($r.assets | Where-Object { $_.name -like 'RomCloud-v*.zip' })[0].browser_download_url" > "%TEMP%\rc-url.txt"
set /p URL=<"%TEMP%\rc-url.txt"
if "%URL%"=="" (
  echo [!] Khong lay duoc link tai. Kiem tra mang.
  pause & exit /b 1
)
echo [*] Dang tai ban moi...
powershell -NoProfile -Command "Invoke-WebRequest -Uri '%URL%' -OutFile $env:TEMP\romcloud.zip"
if errorlevel 1 (
  echo [!] Tai zip loi. Kiem tra mang.
  pause & exit /b 1
)

echo [*] Dang bung vao the nho (ghi de, giu nguyen game/save)...
powershell -NoProfile -Command "Expand-Archive -Path $env:TEMP\romcloud.zip -DestinationPath '%DRIVE%\' -Force"
if errorlevel 1 (
  echo [!] Bung zip loi.
  pause & exit /b 1
)
del "%TEMP%\romcloud.zip" 2>nul

echo [*] Dang eject the nho (an toan truoc khi rut)...
powershell -NoProfile -Command "$s = New-Object -ComObject Shell.Application; $s.Namespace(17).ParseName('%DRIVE%').InvokeVerb('Eject')" >nul 2>&1

echo.
echo ==========================================
echo   XONG! Thoat app USB Storage tren Brick,
echo   mo RomCloud tren may va dung.
echo   (Neu may van bao o USB: eject tay trong
echo   Explorer roi thoat app tren Brick.)
echo ==========================================
pause
