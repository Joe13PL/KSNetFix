@echo off
setlocal
title KSNetFix 2.2 - odinstalowanie
set "GAME=%~1"
if not "%GAME%"=="" goto check
for /f "tokens=2,*" %%A in ('reg query "HKLM\SOFTWARE\WOW6432Node\Reality Pump\KnightShift\BaseGame\FileSystem" /v outputdir 2^>nul ^| find /i "outputdir"') do set "GAME=%%B"
if "%GAME%"=="" for /f "tokens=2,*" %%A in ('reg query "HKLM\SOFTWARE\Reality Pump\KnightShift\BaseGame\FileSystem" /v outputdir 2^>nul ^| find /i "outputdir"') do set "GAME=%%B"
if "%GAME%"=="" set "GAME=%ProgramFiles(x86)%\Steam\steamapps\common\KnightShift"

:check
if not exist "%GAME%\KnightShift.ex1" (
  echo Nie znaleziono gry KnightShift w: %GAME%
  goto end
)
tasklist /fi "imagename eq KnightShift.ex1" | find /i "KnightShift" >nul && goto running
tasklist /fi "imagename eq KnightShift.ex2" | find /i "KnightShift" >nul && goto running

for %%F in (dinput8.dll steam_api.dll ksnetfix.ini ksnetfix.ini.bak ksnettrace.log) do (
  if exist "%GAME%\%%F" del /q "%GAME%\%%F"
)
del /q "%GAME%\ksnetfix*.log" 2>nul
if exist "%GAME%\dinput8.dll" (
  echo Nie udalo sie usunac plikow - uruchom jako administrator.
  goto end
)
echo KSNetFix usuniety. Gra wrocila do stanu oryginalnego.
goto end

:running
echo Gra jest uruchomiona - zamknij ja i uruchom ponownie.

:end
echo.
pause
