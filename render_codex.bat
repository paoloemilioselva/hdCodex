@ECHO OFF
SETLOCAL

CALL "%~dp0setup_usd_env.bat"
IF ERRORLEVEL 1 EXIT /B %ERRORLEVEL%

SET "SCENE_FILE=%~1"
SET "OUTPUT_FILE=%~2"
IF "%SCENE_FILE%"=="" SET "SCENE_FILE=C:\Users\paolo\Desktop\code\assets\full_assets\OpenChessSet\chess_set.usda"
IF "%OUTPUT_FILE%"=="" SET "OUTPUT_FILE=%~dp0build\validation\chess_codex.png"

CALL usdrecord --renderer "Codex GPU Path Tracer" --disableCameraLight %*
EXIT /B %ERRORLEVEL%
