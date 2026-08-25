@ECHO OFF

REM Shared standalone OpenUSD runtime environment. Keep USD tools, launchers,
REM and validation scripts on this one dependency path.
SET "USDROOT=C:\dev\usd-26.03"
SET "USDEXTRA=C:\Users\paolo\Desktop\usd-26.03-extra"

REM OpenUSD was built for Python 3.12. Prefer an explicit override, then a
REM project-local runtime, before checking known standalone installations.
IF DEFINED HDCODEX_PYTHON IF EXIST "%HDCODEX_PYTHON%\python.exe" GOTO python_found
IF EXIST "%~dp0_deps\python\python.exe" (
  SET "HDCODEX_PYTHON=%~dp0_deps\python"
  GOTO python_found
)
FOR /F "delims=" %%P IN ('WHERE python.exe 2^>NUL') DO (
  SET "HDCODEX_PYTHON=%%~dpP"
  GOTO python_found
)
IF EXIST "C:\Users\paolo\AppData\Local\Programs\Python\Python312\python.exe" (
  SET "HDCODEX_PYTHON=C:\Users\paolo\AppData\Local\Programs\Python\Python312"
  GOTO python_found
)

ECHO A standalone Python 3.12 runtime was not found.
ECHO Set HDCODEX_PYTHON or unpack one beneath %~dp0_deps\python.
EXIT /B 1

:python_found

SET "PXR_PLUGINPATH_NAME=%USDROOT%;%USDROOT%\plugin\usd;%USDEXTRA%\plugin\usd"
SET "PYTHONPATH=%USDROOT%\lib\python;%USDEXTRA%\lib\python;%PYTHONPATH%"
SET "PATH=%HDCODEX_PYTHON%;%USDROOT%\bin;%USDEXTRA%\bin;%USDROOT%\lib;%USDEXTRA%\lib;%PATH%"

REM This OpenUSD installation includes the RenderMan OSL parser. Make its
REM runtime dependency visible when the matching RenderMan install is present;
REM non-RenderMan setups continue to use the standalone USD paths above.
SET "HDCODEX_RMAN=C:\Program Files\Pixar\RenderManProServer-26.3"
IF DEFINED RMANTREE IF EXIST "%RMANTREE%\lib\libprman.dll" SET "HDCODEX_RMAN=%RMANTREE%"
IF EXIST "%HDCODEX_RMAN%\lib\libprman.dll" SET "PATH=%HDCODEX_RMAN%\bin;%HDCODEX_RMAN%\lib;%PATH%"
