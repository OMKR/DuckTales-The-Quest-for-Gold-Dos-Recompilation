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

rem a Python left broken (an interrupted extraction, antivirus software)
rem is fetched again
if exist "%TC%\python\python.exe" (
    "%TC%\python\python.exe" -c "import zipfile, json, hashlib, urllib.request" >nul 2>&1
    if errorlevel 1 (
        echo The Python in %TC%\python does not run - fetching it again.
        rmdir /s /q "%TC%\python"
    )
)
if not exist "%TC%\python\python.exe" (
    echo Downloading %PY_URL%
    curl -fL --progress-bar -o "%TC%\python.zip" "%PY_URL%" || goto :fail
    certutil -hashfile "%TC%\python.zip" SHA256 | findstr /i /x "%PY_SHA%" >nul
    if errorlevel 1 (
        echo Checksum mismatch - deleting the download.
        del "%TC%\python.zip"
        goto :fail
    )
    rem extracted beside, then renamed: an interrupted extraction leaves
    rem no folder that looks finished
    if exist "%TC%\python.tmp" rmdir /s /q "%TC%\python.tmp"
    mkdir "%TC%\python.tmp"
    "%TAR%" -xf "%TC%\python.zip" -C "%TC%\python.tmp" || goto :fail
    del "%TC%\python.zip"
    call :rename_python || goto :fail
)
"%TC%\python\python.exe" -c "import zipfile" >nul 2>&1
if errorlevel 1 (
    echo The Python in %TC%\python does not run. Antivirus software may have
    echo blocked or removed it: allow the toolchain folder and run the installer again.
    goto :fail
)

"%TC%\python\python.exe" "%~dp0src\tools\install.py" %* || goto :fail
pause
exit /b 0

:rename_python
rem antivirus software scanning the new files can hold the rename for a while
for /l %%i in (1,1,10) do (
    move "%TC%\python.tmp" "%TC%\python" >nul 2>&1 && exit /b 0
    timeout /t 1 /nobreak >nul
)
xcopy /e /i /q /y "%TC%\python.tmp" "%TC%\python" >nul || exit /b 1
rmdir /s /q "%TC%\python.tmp"
exit /b 0

:fail
echo.
echo INSTALL FAILED.
pause
exit /b 1
