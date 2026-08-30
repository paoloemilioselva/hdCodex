@ECHO OFF
SETLOCAL

SET "HDCODEX_SAMPLES_PER_PIXEL=1024"
SET "HDCODEX_SAMPLES_PER_UPDATE=32"

CALL "%~dp0render_codex.bat" --imageWidth 1024 --camera PhysCamera001 "%~dp0gallery\intel_sponza.usda" "%~dp0gallery\intel_sponza.jpg"
IF ERRORLEVEL 1 EXIT /B %ERRORLEVEL%

CALL "%~dp0render_codex.bat" --imageWidth 1024 --camera renderCam "%~dp0gallery\chess_board.usda" "%~dp0gallery\chess_board.jpg"
IF ERRORLEVEL 1 EXIT /B %ERRORLEVEL%

CALL "%~dp0render_codex.bat" --imageWidth 1024 --camera camera "%~dp0gallery\shader_ball_gold.usda" "%~dp0gallery\shader_ball_gold.jpg"
IF ERRORLEVEL 1 EXIT /B %ERRORLEVEL%

CALL "%~dp0render_codex.bat" --imageWidth 1024 --camera camera "%~dp0gallery\shader_ball_glass.usda" "%~dp0gallery\shader_ball_glass.jpg"
IF ERRORLEVEL 1 EXIT /B %ERRORLEVEL%

CALL "%~dp0render_codex.bat" --imageWidth 1024 --camera camera "%~dp0gallery\shader_ball_bubblegum.usda" "%~dp0gallery\shader_ball_bubblegum.jpg"
IF ERRORLEVEL 1 EXIT /B %ERRORLEVEL%

CALL "%~dp0render_codex.bat" --imageWidth 1024 --camera renderCam "%~dp0gallery\pixar_kitchen.usda" "%~dp0gallery\pixar_kitchen.jpg"
IF ERRORLEVEL 1 EXIT /B %ERRORLEVEL%

CALL "%~dp0render_codex.bat" --imageWidth 1024 --purposes render --camera mono "%~dp0gallery\collectiveproject001.usda" "%~dp0gallery\collectiveproject001.jpg"
IF ERRORLEVEL 1 EXIT /B %ERRORLEVEL%

CALL "%~dp0render_codex.bat" --imageWidth 1024 --purposes render --camera renderCam_mainCU "%~dp0gallery\openpbr_playground.usda" "%~dp0gallery\openpbr_playground.jpg"
EXIT /B %ERRORLEVEL%
