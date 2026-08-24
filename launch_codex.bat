@ECHO OFF
SETLOCAL

SET "USDROOT=C:\dev\usd-26.03"
SET "USDEXTRA=C:\Users\paolo\Desktop\usd-26.03-extra"
SET "PXR_PLUGINPATH_NAME=%USDROOT%;%USDROOT%\plugin\usd;%USDEXTRA%\plugin\usd"
SET "PYTHONPATH=%USDROOT%\lib\python;%USDEXTRA%\lib\python;%PYTHONPATH%"
SET "PATH=%USDROOT%\bin;%USDEXTRA%\bin;%USDROOT%\lib;%USDEXTRA%\lib;%PATH%"

SET "SCENE_FILE=%~1"
IF "%SCENE_FILE%"=="" SET "SCENE_FILE=C:\Users\paolo\Desktop\openusd\Kitchen_set\Kitchen_set.usd"

ECHO Launching usdview with hdCodex: %SCENE_FILE%
usdview "%SCENE_FILE%" --renderer "Codex GPU Path Tracer"

