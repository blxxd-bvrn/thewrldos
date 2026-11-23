@echo off
rem ==========================================================================
rem  Build to ISO - builds wrldos.iso and wrldos64.iso for THE WRLD OS with one double-click.
rem
rem  The kernel has to be compiled with Linux tools, so this uses WSL (Windows
rem  Subsystem for Linux). If WSL is not available but Docker Desktop is, it
rem  builds in a Docker container instead. Missing build tools are installed
rem  automatically the first time. The ISO is written next to this file.
rem
rem  With neither, it offers two ways that need nothing installed (tools\buildvm\build-iso.ps1):
rem  download the ISOs GitHub built, or build in a portable Linux VM it downloads.
rem  "Build to ISO.bat download" or "Build to ISO.bat vm" picks one of those directly.
rem ==========================================================================
setlocal EnableExtensions
title Build THE WRLD OS ISO
cd /d "%~dp0"
rem folder of this file, without the trailing backslash
set "HERE=%~dp0"
set "HERE=%HERE:~0,-1%"

echo.
echo   THE WRLD OS - Build to ISO
echo   ==========================
echo.

if not exist "Makefile" goto :notsource
if not exist "tools\build-iso.sh" goto :notsource
if /i "%~1"=="download" goto :download
if /i "%~1"=="vm" goto :buildvm

rem ---- WSL with a Linux distribution installed? ----
where wsl.exe >nul 2>nul || goto :trydocker
wsl.exe -e true >nul 2>nul || goto :trydocker

set "WSLDIR="
for /f "usebackq delims=" %%P in (`wsl.exe -e wslpath -a "%HERE%"`) do set "WSLDIR=%%P"
if not defined WSLDIR goto :trydocker

echo Building with WSL in: %WSLDIR%
echo The first build installs the Linux build tools and can take a few minutes.
echo.
wsl.exe --cd "%HERE%" -e bash -c "tr -d '\r' < tools/build-iso.sh > /tmp/wrld-build-iso.sh && bash /tmp/wrld-build-iso.sh ."
if errorlevel 1 goto :failed
goto :done

:trydocker
where docker.exe >nul 2>nul || goto :nowsl
docker.exe info >nul 2>nul || goto :nowsl
echo WSL is not set up, building with Docker instead.
echo.
docker.exe run --rm -v "%HERE%:/src" ubuntu:24.04 bash -c "tr -d '\r' < /src/tools/build-iso.sh > /tmp/wrld-build-iso.sh && bash /tmp/wrld-build-iso.sh /src"
if errorlevel 1 goto :failed
goto :done

:download
echo Downloading the ISOs that GitHub built from the newest version of the project.
echo (Your own changes are not in them: build with WSL, Docker or the build VM for that.)
echo.
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%HERE%\tools\buildvm\build-iso.ps1" -Mode download -Src "%HERE%"
if errorlevel 1 goto :failed
goto :done

:buildvm
echo Building in the portable Linux build VM (nothing is installed; no administrator rights).
echo The first time it downloads the build kit (about 1 GB) into "%LOCALAPPDATA%\WRLD OS build VM".
echo.
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%HERE%\tools\buildvm\build-iso.ps1" -Mode vm -Src "%HERE%"
if errorlevel 1 goto :failed
goto :done

:done
echo.
if not exist "wrldos.iso" goto :failed
echo   Finished! Your ISOs are here:
echo   %~dp0wrldos.iso     (the main CD: both editions. A 64-bit PC starts the 64-bit
echo                       edition with the WebKit browser; a 32-bit PC the 32-bit one)
echo   %~dp0wrldos64.iso   (the 64-bit edition alone)
echo.
echo   Boot one in VirtualBox, VMware or QEMU - legacy BIOS boot, at least 256 MB RAM,
echo   and an IDE hard disk if you want to install THE WRLD OS to it. The 64-bit
echo   edition needs a 64-bit VM (for VirtualBox: an "Other/Unknown (64-bit)" type).
echo.
pause
exit /b 0

:failed
echo.
echo   The build failed. Scroll up to see the error message.
echo.
pause
exit /b 1

:notsource
echo   Put "Build to ISO.bat" in the THE WRLD OS source folder - the one with
echo   the Makefile - and run it from there.
echo.
pause
exit /b 1

:nowsl
echo   This PC has neither WSL (Windows Subsystem for Linux) nor Docker Desktop, which the
echo   normal build uses. Choose one of these instead:
echo.
echo     1  Download ready-made ISOs. A few minutes; nothing is installed. GitHub builds them
echo        from the newest version of the project, so your own changes are NOT in them.
echo.
echo     2  Build your sources in a portable Linux VM. Nothing is installed and no administrator
echo        rights are needed. The first time it downloads the build kit (about 1 GB). It takes
echo        an hour or more unless "Windows Hypervisor Platform" is turned on in Windows Features.
echo.
echo     3  Install WSL with Ubuntu (an administrator prompt, then a restart). After that, builds
echo        are quick and use your own changes.
echo.
echo     Q  Quit
echo.
choice /c 123Q /n /m "Your choice [1, 2, 3 or Q]: "
if errorlevel 4 goto :nowsl_quit
if errorlevel 3 goto :installwsl
if errorlevel 2 goto :buildvm
goto :download

:installwsl
powershell.exe -NoProfile -Command "Start-Process wsl.exe -ArgumentList '--install','-d','Ubuntu' -Verb RunAs"
echo.
echo   When the installation finishes, restart Windows, open "Ubuntu" from the
echo   Start menu once to create your Linux user name and password, then run
echo   "Build to ISO.bat" again.
:nowsl_quit
echo.
pause
exit /b 1
