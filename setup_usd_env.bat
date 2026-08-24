@ECHO OFF

REM Shared standalone OpenUSD runtime environment. Keep USD tools, launchers,
REM and validation scripts on this one dependency path.
SET "USDROOT=C:\dev\usd-26.03"
SET "USDEXTRA=C:\Users\paolo\Desktop\usd-26.03-extra"
SET "HDCODEX_PYTHON=C:\Users\paolo\AppData\Local\Programs\Python\Python312"

SET "PXR_PLUGINPATH_NAME=%USDROOT%;%USDROOT%\plugin\usd;%USDEXTRA%\plugin\usd"
SET "PYTHONPATH=%USDROOT%\lib\python;%USDEXTRA%\lib\python;%PYTHONPATH%"
SET "PATH=%HDCODEX_PYTHON%;%USDROOT%\bin;%USDEXTRA%\bin;%USDROOT%\lib;%USDEXTRA%\lib;%PATH%"

IF NOT EXIST "%HDCODEX_PYTHON%\python.exe" (
  ECHO Standalone OpenUSD Python was not found at %HDCODEX_PYTHON%.
  EXIT /B 1
)
