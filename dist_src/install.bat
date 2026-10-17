@echo off
rem Windows installer: gets an embeddable Python if none is needed from the
rem system, then runs src\tools\install.py (which does the actual work).
rem Usage: install.bat ["<game folder containing DUCKTALE.EXE>"]
setlocal
cd /d "%~dp0"
if not exist "%~dp0src\tools\install.py" (
    echo This is the installer template of the source repository.
    echo Run it from the release package: python make_dist.py creates dist\DuckTalesDOS,
    echo then run dist\DuckTalesDOS\install.bat.
    goto :fail
)
set "TC=%~dp0toolchain"
if not exist "%TC%" mkdir "%TC%"
rem Windows' own tar (another tar on PATH may not understand C:\ paths)
set "TAR=%SystemRoot%\System32\tar.exe"
set "PY_URL=https://www.python.org/ftp/python/3.12.10/python-3.12.10-embed-amd64.zip"
set "PY_SHA=4acbed6dd1c744b0376e3b1cf57ce906f9dc9e95e68824584c8099a63025a3c3"

if not exist "%TC%\python\python.exe" (
    echo Downloading %PY_URL%
    curl -fL --progress-bar -o "%TC%\python.zip" "%PY_URL%" || goto :fail
    certutil -hashfile "%TC%\python.zip" SHA256 | findstr /i /x "%PY_SHA%" >nul
    if errorlevel 1 (
        echo Checksum mismatch - deleting the download.
        del "%TC%\python.zip"
        goto :fail
    )
    mkdir "%TC%\python"
    "%TAR%" -xf "%TC%\python.zip" -C "%TC%\python" || goto :fail
    del "%TC%\python.zip"
)

"%TC%\python\python.exe" "%~dp0src\tools\install.py" %* || goto :fail
pause
exit /b 0

:fail
echo.
echo INSTALL FAILED.
pause
exit /b 1
