@echo off
chcp 65001 >nul
REM RomCloud - Cai dat 1 cham cho Windows (double-click file nay)
cd /d "%~dp0"

echo ==========================================
echo   Cai dat RomCloud 1 cham (Windows)
echo ==========================================
echo 1. Cam TrimUI Brick (dang bat) vao may.
echo 2. Tren may Brick: de che do USB = ADB.
echo 3. Lan dau: cai driver USB 1 lan bang Zadig
echo    (mo https://zadig.akeo.ie, chon thiet bi
echo    Android/TrimUI -^> Install WinUSB). Bo qua
echo    neu may da nhan adb.
echo.

if not exist "platform-tools\adb.exe" (
  echo [*] Dang tai cong cu adb (chi 1 lan dau)...
  where curl.exe >nul 2>&1
  if %errorlevel%==0 (
    curl.exe -L --progress-bar -o "%TEMP%\rc-pt.zip" "https://dl.google.com/android/repository/platform-tools-latest-windows.zip"
  ) else (
    powershell -NoProfile -Command "Invoke-WebRequest -Uri 'https://dl.google.com/android/repository/platform-tools-latest-windows.zip' -OutFile $env:TEMP\rc-pt.zip"
  )
  if errorlevel 1 (
    echo [!] Khong tai duoc adb. Kiem tra mang.
    pause & exit /b 1
  )
  powershell -NoProfile -Command "Remove-Item -Recurse -Force .\pt-tmp -ErrorAction SilentlyContinue; Expand-Archive -Path $env:TEMP\rc-pt.zip -DestinationPath .\pt-tmp -Force; New-Item -ItemType Directory -Force .\platform-tools | Out-Null; Move-Item .\pt-tmp\platform-tools\* .\platform-tools\ -Force; Remove-Item -Recurse -Force .\pt-tmp"
)

echo [*] Nhan phim bat ky khi da cam cap...
pause >nul
platform-tools\adb.exe kill-server >nul 2>&1
echo [*] Doi may hien len...
platform-tools\adb.exe wait-for-device
echo [*] Thay may roi.

echo [*] Dang hoi ban RomCloud moi nhat...
powershell -NoProfile -Command "$r = Invoke-RestMethod 'https://api.github.com/repos/bun2it/RomCloud/releases/latest'; ($r.assets | Where-Object { $_.name -like '*.zip' })[0].browser_download_url" > "%TEMP%\rc-url.txt"
set /p URL=<"%TEMP%\rc-url.txt"
if "%URL%"=="" (
  echo [!] Khong lay duoc link tai.
  pause & exit /b 1
)
echo [*] Dang tai ban moi (~54MB)...
where curl.exe >nul 2>&1
if %errorlevel%==0 (
  curl.exe -L --progress-bar -o "%TEMP%\romcloud.zip" "%URL%"
) else (
  powershell -NoProfile -Command "Invoke-WebRequest -Uri '%URL%' -OutFile $env:TEMP\romcloud.zip"
)
if errorlevel 1 (
  echo [!] Tai zip loi.
  pause & exit /b 1
)

echo [*] Dang day vao the nho...
for %%f in ("%TEMP%\romcloud.zip") do set TOT=%%~zf
start /b "rcpush" platform-tools\adb.exe push "%TEMP%\romcloud.zip" /mnt/SDCARD/RomCloud-install.zip >push.log 2>&1
set CNT=0
:pushloop
timeout /t 2 /nobreak >nul
set /a CNT+=1
platform-tools\adb.exe shell "stat -c %%s /mnt/SDCARD/RomCloud-install.zip | tr -d '\r'" > size.txt 2>nul
set SZ=0
for /f "delims=" %%s in (size.txt) do set SZ=%%s
echo %SZ% | findstr /r "^[0-9][0-9]*$" >nul || set SZ=0
set /a PCT=%SZ%/(%TOT%/100)
set /a SZMB=%SZ%/1048576
set /a TOTMB=%TOT%/1048576
echo Day file... %SZMB%/%TOTMB%MB ^(%PCT%%%^)
if %SZ% GEQ %TOT% goto pushdone
if %CNT% LSS 150 goto pushloop
echo [!] Day file qua lau/timeout. Thu rut cap cam lai.
pause & exit /b 1
:pushdone
del size.txt push.log 2>nul

echo [*] Dang bung + cap quyen...
platform-tools\adb.exe shell "cd /mnt/SDCARD && unzip -o -q RomCloud-install.zip && chmod +x Apps/RomCloud/launch.sh Apps/RomCloud/bin/RomCloud Apps/RomCloud/bin/gamecast_d && rm -f RomCloud-install.zip && sync && echo DONE"

echo.
echo ==========================================
echo   XONG! Rut cap, mo RomCloud tren may.
echo ==========================================
del "%TEMP%\romcloud.zip" 2>nul
pause
