@ECHO OFF
SETLOCAL

CALL "%~dp0setup_usd_env.bat"
IF ERRORLEVEL 1 EXIT /B %ERRORLEVEL%

SET "HDCODEX_SAMPLES_PER_PIXEL=1024"
SET "HDCODEX_SAMPLES_PER_UPDATE=32"
SET "HDCODEX_ENABLE_SUBDIVISION=1"
SET "HDCODEX_SUBDIVISION_LEVEL=2"
SET "HDCODEX_ENABLE_DISPLACEMENT=1"
IF NOT DEFINED HDCODEX_GALLERY_EXPOSURE SET "HDCODEX_GALLERY_EXPOSURE=0"
SET "HDCODEX_GALLERY_LINEAR=%~dp0build\gallery-linear"

IF NOT EXIST "%HDCODEX_GALLERY_LINEAR%" MKDIR "%HDCODEX_GALLERY_LINEAR%"

CALL "%~dp0render_codex.bat" --imageWidth 1024 --colorCorrectionMode disabled --camera PhysCamera001 "%~dp0gallery\intel_sponza.usda" "%HDCODEX_GALLERY_LINEAR%\intel_sponza.exr"
IF ERRORLEVEL 1 EXIT /B %ERRORLEVEL%
CALL "%~dp0display_gallery_image.bat" "%HDCODEX_GALLERY_LINEAR%\intel_sponza.exr" "%~dp0gallery\intel_sponza.jpg" "%HDCODEX_GALLERY_EXPOSURE%"
IF ERRORLEVEL 1 EXIT /B %ERRORLEVEL%

CALL "%~dp0render_codex.bat" --imageWidth 1024 --colorCorrectionMode disabled --camera renderCam "%~dp0gallery\chess_board.usda" "%HDCODEX_GALLERY_LINEAR%\chess_board.exr"
IF ERRORLEVEL 1 EXIT /B %ERRORLEVEL%
CALL "%~dp0display_gallery_image.bat" "%HDCODEX_GALLERY_LINEAR%\chess_board.exr" "%~dp0gallery\chess_board.jpg" "%HDCODEX_GALLERY_EXPOSURE%"
IF ERRORLEVEL 1 EXIT /B %ERRORLEVEL%

CALL "%~dp0render_codex.bat" --imageWidth 1024 --colorCorrectionMode disabled --camera camera "%~dp0gallery\shader_ball_gold.usda" "%HDCODEX_GALLERY_LINEAR%\shader_ball_gold.exr"
IF ERRORLEVEL 1 EXIT /B %ERRORLEVEL%
CALL "%~dp0display_gallery_image.bat" "%HDCODEX_GALLERY_LINEAR%\shader_ball_gold.exr" "%~dp0gallery\shader_ball_gold.jpg" "%HDCODEX_GALLERY_EXPOSURE%"
IF ERRORLEVEL 1 EXIT /B %ERRORLEVEL%

CALL "%~dp0render_codex.bat" --imageWidth 1024 --colorCorrectionMode disabled --camera camera "%~dp0gallery\shader_ball_glass.usda" "%HDCODEX_GALLERY_LINEAR%\shader_ball_glass.exr"
IF ERRORLEVEL 1 EXIT /B %ERRORLEVEL%
CALL "%~dp0display_gallery_image.bat" "%HDCODEX_GALLERY_LINEAR%\shader_ball_glass.exr" "%~dp0gallery\shader_ball_glass.jpg" "%HDCODEX_GALLERY_EXPOSURE%"
IF ERRORLEVEL 1 EXIT /B %ERRORLEVEL%

CALL "%~dp0render_codex.bat" --imageWidth 1024 --colorCorrectionMode disabled --camera camera "%~dp0gallery\shader_ball_bubblegum.usda" "%HDCODEX_GALLERY_LINEAR%\shader_ball_bubblegum.exr"
IF ERRORLEVEL 1 EXIT /B %ERRORLEVEL%
CALL "%~dp0display_gallery_image.bat" "%HDCODEX_GALLERY_LINEAR%\shader_ball_bubblegum.exr" "%~dp0gallery\shader_ball_bubblegum.jpg" "%HDCODEX_GALLERY_EXPOSURE%"
IF ERRORLEVEL 1 EXIT /B %ERRORLEVEL%

CALL "%~dp0render_codex.bat" --imageWidth 1024 --colorCorrectionMode disabled --camera renderCam "%~dp0gallery\pixar_kitchen.usda" "%HDCODEX_GALLERY_LINEAR%\pixar_kitchen.exr"
IF ERRORLEVEL 1 EXIT /B %ERRORLEVEL%
CALL "%~dp0display_gallery_image.bat" "%HDCODEX_GALLERY_LINEAR%\pixar_kitchen.exr" "%~dp0gallery\pixar_kitchen.jpg" "%HDCODEX_GALLERY_EXPOSURE%"
IF ERRORLEVEL 1 EXIT /B %ERRORLEVEL%

CALL "%~dp0render_codex.bat" --imageWidth 1024 --colorCorrectionMode disabled --purposes render --camera mono "%~dp0gallery\collectiveproject001.usda" "%HDCODEX_GALLERY_LINEAR%\collectiveproject001.exr"
IF ERRORLEVEL 1 EXIT /B %ERRORLEVEL%
CALL "%~dp0display_gallery_image.bat" "%HDCODEX_GALLERY_LINEAR%\collectiveproject001.exr" "%~dp0gallery\collectiveproject001.jpg" "%HDCODEX_GALLERY_EXPOSURE%"
IF ERRORLEVEL 1 EXIT /B %ERRORLEVEL%

CALL "%~dp0render_codex.bat" --imageWidth 1024 --colorCorrectionMode disabled --purposes render --camera renderCam_mainCU "%~dp0gallery\openpbr_playground.usda" "%HDCODEX_GALLERY_LINEAR%\openpbr_playground.exr"
IF ERRORLEVEL 1 EXIT /B %ERRORLEVEL%
CALL "%~dp0display_gallery_image.bat" "%HDCODEX_GALLERY_LINEAR%\openpbr_playground.exr" "%~dp0gallery\openpbr_playground.jpg" "%HDCODEX_GALLERY_EXPOSURE%"
IF ERRORLEVEL 1 EXIT /B %ERRORLEVEL%

REM The authored New Zealand mesh is a single quad, so use level 6 to provide
REM enough vertices for the height-map displacement baseline.
SET "HDCODEX_SUBDIVISION_LEVEL=6"
CALL "%~dp0render_codex.bat" --imageWidth 1024 --colorCorrectionMode disabled --camera camera "%~dp0gallery\newzealand_heightmap.usda" "%HDCODEX_GALLERY_LINEAR%\newzealand_heightmap.exr"
IF ERRORLEVEL 1 EXIT /B %ERRORLEVEL%
CALL "%~dp0display_gallery_image.bat" "%HDCODEX_GALLERY_LINEAR%\newzealand_heightmap.exr" "%~dp0gallery\newzealand_heightmap.jpg" "%HDCODEX_GALLERY_EXPOSURE%"
EXIT /B %ERRORLEVEL%
