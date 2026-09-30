@echo off
setlocal
title KSNetFix 2.5.10 - instalacja
set "SRC=%~dp0"
set "GAME=%~1"

rem Folder gry: parametr, albo wpis w rejestrze zapisany przez Steam przy instalacji gry.
if not "%GAME%"=="" goto check
for /f "tokens=2,*" %%A in ('reg query "HKLM\SOFTWARE\WOW6432Node\Reality Pump\KnightShift\BaseGame\FileSystem" /v outputdir 2^>nul ^| find /i "outputdir"') do set "GAME=%%B"
if "%GAME%"=="" for /f "tokens=2,*" %%A in ('reg query "HKLM\SOFTWARE\Reality Pump\KnightShift\BaseGame\FileSystem" /v outputdir 2^>nul ^| find /i "outputdir"') do set "GAME=%%B"
if "%GAME%"=="" set "GAME=%ProgramFiles(x86)%\Steam\steamapps\common\KnightShift"

:check
if not exist "%GAME%\KnightShift.ex1" (
  echo Nie znaleziono gry KnightShift w:
  echo   %GAME%
  echo.
  echo Zainstaluj recznie ^(INSTRUKCJA.txt^) albo przeciagnij folder gry na ten plik.
  goto fail
)
tasklist /fi "imagename eq KnightShift.ex1" | find /i "KnightShift" >nul && goto running
tasklist /fi "imagename eq KnightShift.ex2" | find /i "KnightShift" >nul && goto running

echo Folder gry: %GAME%
echo.
copy /y "%SRC%dinput8.dll" "%GAME%\" >nul || goto denied
copy /y "%SRC%steam_api.dll" "%GAME%\" >nul || goto denied
if exist "%GAME%\ksnetfix.ini" (
  copy /y "%GAME%\ksnetfix.ini" "%GAME%\ksnetfix.ini.bak" >nul
  echo Poprzednie ustawienia zapisano jako ksnetfix.ini.bak
)
copy /y "%SRC%ksnetfix.ini" "%GAME%\" >nul || goto denied

echo Zainstalowano KSNetFix 2.5.10:
echo   dinput8.dll, steam_api.dll, ksnetfix.ini
echo.
echo Uruchom gre normalnie ze Steama. Szczegoly w pliku INSTRUKCJA.txt.
echo.
pause
exit /b 0

:running
echo Gra jest uruchomiona - zamknij ja i uruchom instalacje ponownie.
goto fail

:denied
echo Nie udalo sie skopiowac plikow ^(brak uprawnien^).
echo Kliknij ZAINSTALUJ.bat prawym przyciskiem i wybierz "Uruchom jako administrator".
goto fail

:fail
echo.
pause
exit /b 1
