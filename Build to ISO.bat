@echo off
rem ==========================================================================
rem  Build to ISO - builds wrldos.iso and wrldos64.iso for THE WRLD OS with one double-click.
rem
rem  The kernel has to be compiled with Linux tools, so this uses WSL (Windows
rem  Subsystem for Linux). If WSL is not available but Docker Desktop is, it
rem  builds in a Docker container instead. Missing build tools are installed
rem  automatically the first time. The ISO is written next to this file.
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

:done
echo.
if not exist "wrldos64.iso" goto :failed
echo   Finished! Your ISOs are here:
echo   %~dp0wrldos64.iso   (64-bit edition, the main image: WebKit browser, 64-bit .exe programs)
echo   %~dp0wrldos.iso     (32-bit edition, for 32-bit machines and 32-bit .exe programs)
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
echo   This build needs WSL - Windows Subsystem for Linux - with Ubuntu installed,
echo   or Docker Desktop running.
echo.
echo   To install WSL, run this in PowerShell as administrator, then restart:
echo       wsl --install -d Ubuntu
echo.
set "ANSWER="
set /p "ANSWER=Install WSL with Ubuntu now? An administrator prompt will appear. [Y/N] "
if /i not "%ANSWER%"=="Y" goto :nowsl_end
powershell.exe -NoProfile -Command "Start-Process wsl.exe -ArgumentList '--install','-d','Ubuntu' -Verb RunAs"
echo.
echo   When the installation finishes, restart Windows, open "Ubuntu" from the
echo   Start menu once to create your Linux user name and password, then run
echo   "Build to ISO.bat" again.
:nowsl_end
echo.
pause
exit /b 1
