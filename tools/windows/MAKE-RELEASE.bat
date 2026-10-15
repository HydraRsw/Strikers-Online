@echo off
rem Puts together the Windows release players download, and zips it.
rem
rem   MAKE-RELEASE.bat BUILD_DIR [SETTINGS_APP_DIR]
rem
rem BUILD_DIR         the build folder holding strikers.exe and strikers_relay.exe
rem SETTINGS_APP_DIR  optional: a folder holding strikers-settings.exe and its Qt files
rem                   (for example the original port's Windows release)
rem
rem playit.exe (the Playit tunnel, BSD-2-Clause) goes in too, with its licence: taken from
rem BUILD_DIR if it is there, otherwise downloaded from Playit's official GitHub releases.
rem It is never committed to the repository (.gitignore).
rem
rem Writes release\SuperMarioStrikersOnline-windows-x86_64.zip in the repository.
setlocal
set "REPO=%~dp0..\.."
for %%i in ("%REPO%") do set "REPO=%%~fi"
set "BUILD=%~1"
set "SETTINGS=%~2"
set "NAME=SuperMarioStrikersOnline-windows-x86_64"
set "OUT=%REPO%\release\%NAME%"

if "%BUILD%"=="" (
    echo Usage: MAKE-RELEASE.bat BUILD_DIR [SETTINGS_APP_DIR]
    exit /b 1
)
if not exist "%BUILD%\strikers.exe" (
    echo %BUILD%\strikers.exe not found. Build the game first.
    exit /b 1
)

if exist "%OUT%" rmdir /s /q "%OUT%"
mkdir "%OUT%"

echo Copying the game...
copy /y "%BUILD%\strikers.exe" "%OUT%\" >nul
if exist "%BUILD%\strikers_relay.exe" (copy /y "%BUILD%\strikers_relay.exe" "%OUT%\" >nul) else (echo   Note: strikers_relay.exe not found; the zip will have no server.)
for %%f in (SDL3.dll webgpu_dawn.dll dxcompiler.dll dxil.dll libpng16.dll zlib1.dll) do (
    if exist "%BUILD%\%%f" copy /y "%BUILD%\%%f" "%OUT%\" >nul
)
if exist "%BUILD%\input-prompts" robocopy "%BUILD%\input-prompts" "%OUT%\input-prompts" /E /NFL /NDL /NJH /NJS /NP >nul

if not "%SETTINGS%"=="" if exist "%SETTINGS%\strikers-settings.exe" (
    echo Copying Mario Strikers Settings...
    copy /y "%SETTINGS%\strikers-settings.exe" "%OUT%\Mario Strikers Settings.exe" >nul
    for %%f in (Qt6Core.dll Qt6Gui.dll Qt6Network.dll Qt6Svg.dll Qt6Widgets.dll D3Dcompiler_47.dll concrt140.dll vccorlib140.dll msvcp140.dll msvcp140_1.dll msvcp140_2.dll msvcp140_atomic_wait.dll msvcp140_codecvt_ids.dll vcruntime140.dll vcruntime140_1.dll vcruntime140_threads.dll) do (
        if exist "%SETTINGS%\%%f" copy /y "%SETTINGS%\%%f" "%OUT%\" >nul
    )
    for %%d in (platforms styles imageformats iconengines generic networkinformation tls) do (
        if exist "%SETTINGS%\%%d" robocopy "%SETTINGS%\%%d" "%OUT%\%%d" /E /NFL /NDL /NJH /NJS /NP >nul
    )
)

echo Adding Playit (the tunnel HOST-SERVER.bat starts)...
if exist "%BUILD%\playit.exe" (
    copy /y "%BUILD%\playit.exe" "%OUT%\" >nul
    echo   playit.exe taken from %BUILD%
) else (
    powershell -NoProfile -ExecutionPolicy Bypass -File "%REPO%\tools\windows\get-playit.ps1" -Out "%OUT%\playit.exe"
)
if exist "%OUT%\playit.exe" (
    copy /y "%REPO%\release-kit\LICENSE-playit.txt" "%OUT%\" >nul
) else (
    echo   Note: could not get playit.exe; hosts will have to download it themselves.
)

echo Copying the guides and licences...
copy /y "%REPO%\release-kit\READ ME FIRST.txt" "%OUT%\" >nul
copy /y "%REPO%\release-kit\strikers.ini" "%OUT%\" >nul
copy /y "%REPO%\release-kit\HOST-SERVER.bat" "%OUT%\" >nul
copy /y "%REPO%\release-kit\UPDATE.bat" "%OUT%\" >nul
copy /y "%REPO%\release-kit\updater.ps1" "%OUT%\" >nul
rem version.txt: what the updater compares with the newest GitHub release's tag.
set "VERSION=unknown"
if exist "%REPO%\VERSION" set /p VERSION=<"%REPO%\VERSION"
>"%OUT%\version.txt" echo %VERSION%
copy /y "%REPO%\smstrikers-port\strikers.ini.example" "%OUT%\" >nul
copy /y "%REPO%\smstrikers-port\LICENSE-*.txt" "%OUT%\" >nul
if exist "%REPO%\smstrikers-port\licenses" robocopy "%REPO%\smstrikers-port\licenses" "%OUT%\licenses" /E /NFL /NDL /NJH /NJS /NP >nul

echo Zipping...
if exist "%REPO%\release\%NAME%.zip" del /q "%REPO%\release\%NAME%.zip"
powershell -NoProfile -Command "Compress-Archive -Path '%OUT%\*' -DestinationPath '%REPO%\release\%NAME%.zip' -Force"
if exist "%REPO%\release\%NAME%.zip" (
    echo Done: %REPO%\release\%NAME%.zip
    echo.
    echo   Version %VERSION%. On GitHub, give the release EXACTLY this tag: %VERSION%
    echo   ^(the updater compares the two^). For the next update, raise the number in
    echo   the VERSION file first, for example v1.1.0 -^> v1.2.0.
) else (
    echo Could not create the zip.
    exit /b 1
)
exit /b 0
