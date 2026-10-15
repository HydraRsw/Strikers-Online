@echo off
setlocal
cd /d "%~dp0"
title Super Mario Strikers Online - Server

if not exist "strikers_relay.exe" (
    echo strikers_relay.exe is missing from this folder.
    pause
    exit /b 1
)

echo ========================================================================
echo   SUPER MARIO STRIKERS ONLINE - SERVER
echo ========================================================================
echo.
echo  Starting the relay server (UDP port 7200)...
start "Strikers Relay Server" "%~dp0strikers_relay.exe" 7200

if exist "playit.exe" (
    echo  Starting the Playit tunnel...
    start "Playit Tunnel" "%~dp0playit.exe"
) else (
    echo.
    echo  playit.exe is not in this folder, so only players on your own network
    echo  can reach the server. To let friends join over the internet:
    echo    1. Download playit for Windows from https://playit.gg/download
    echo    2. Put playit.exe in this folder and run HOST-SERVER.bat again.
)

echo.
echo ========================================================================
echo  FIRST TIME WITH PLAYIT:
echo   1. The Playit window shows a https://playit.gg/claim/... link. Open it
echo      and make a free account.
echo   2. Add a tunnel. Type: Custom (UDP). Local port: 7200.
echo   3. Playit shows your address, like name.ply.gg:12345.
echo.
echo  THEN:
echo   - Put that address on the "server =" line of strikers.ini.
echo   - Give the same address to your friends for their strikers.ini.
echo   - Anyone can now press F1 -^> Play Online -^> Create a room.
echo.
echo  Keep the relay and Playit windows open while people play.
echo ========================================================================
echo.
pause
