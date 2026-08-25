@ECHO OFF
SETLOCAL

CALL "%~dp0setup_usd_env.bat"
IF ERRORLEVEL 1 EXIT /B %ERRORLEVEL%

SET "HDCODEX_INSTALL_DIR=%USDEXTRA%"
SET "VSDEVCMD=C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\Tools\VsDevCmd.bat"

WHERE cl.exe >NUL 2>NUL
IF ERRORLEVEL 1 GOTO setup_msvc
WHERE cmake.exe >NUL 2>NUL
IF ERRORLEVEL 1 GOTO setup_msvc
WHERE ninja.exe >NUL 2>NUL
IF NOT ERRORLEVEL 1 GOTO toolchain_ready

:setup_msvc
IF NOT EXIST "%VSDEVCMD%" (
  ECHO MSVC, CMake, and Ninja were not found, and the Visual Studio 2022 developer environment is unavailable.
  EXIT /B 1
)
CALL "%VSDEVCMD%" -arch=x64 -host_arch=x64
IF ERRORLEVEL 1 EXIT /B %ERRORLEVEL%

:toolchain_ready
IF NOT EXIST build MKDIR build
cmake -S . -B build -G Ninja ^
  -DCMAKE_BUILD_TYPE=RelWithDebInfo ^
  -DCMAKE_INSTALL_PREFIX="%HDCODEX_INSTALL_DIR%" ^
  -DPython3_EXECUTABLE="%HDCODEX_PYTHON%\python.exe" ^
  -DHDCODEX_OPENUSD_ROOT="%USDROOT%" ^
  -DHDCODEX_FETCH_DEPENDENCIES=ON
IF ERRORLEVEL 1 EXIT /B %ERRORLEVEL%

cmake --build build --target install
IF ERRORLEVEL 1 EXIT /B %ERRORLEVEL%

ECHO hdCodex build and install completed successfully.
