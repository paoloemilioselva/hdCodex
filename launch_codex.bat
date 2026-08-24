@ECHO OFF
SETLOCAL

CALL "%~dp0setup_usd_env.bat"
IF ERRORLEVEL 1 EXIT /B %ERRORLEVEL%

SET "SCENE_FILE=%~1"
IF "%SCENE_FILE%"=="" SET "SCENE_FILE=C:\Users\paolo\Desktop\openusd\Kitchen_set\Kitchen_set.usd"

ECHO Launching usdview with hdCodex: %SCENE_FILE%
CALL usdview "%SCENE_FILE%" --renderer "Codex GPU Path Tracer"
