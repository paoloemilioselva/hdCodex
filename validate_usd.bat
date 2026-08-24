@ECHO OFF
SETLOCAL

CALL "%~dp0setup_usd_env.bat"
IF ERRORLEVEL 1 EXIT /B %ERRORLEVEL%

"%HDCODEX_PYTHON%\python.exe" "%~dp0tools\validate_install.py"
EXIT /B %ERRORLEVEL%
