@echo off
setlocal
cd /d "%~dp0"

set "DEFAULT_REPO=https://github.com/504440217-collab/LyricSync-VST3.git"

echo ==========================================================
echo   LyricSync VST3 - one click push to GitHub
echo ==========================================================
echo.
echo   First create an EMPTY public repo at https://github.com/new
echo   Name: LyricSync-VST3   Public   (no README, no .gitignore)
echo.

set "GIT=git"
where git >nul 2>nul
if not errorlevel 1 goto GIT_OK

set "GIT="
if exist "C:\Users\Muzi\.workbuddy\binaries\PortableGit\versions\1.3.0\cmd\git.exe" set "GIT=C:\Users\Muzi\.workbuddy\binaries\PortableGit\versions\1.3.0\cmd\git.exe"
if not defined GIT if exist "C:\Program Files\Git\cmd\git.exe" set "GIT=C:\Program Files\Git\cmd\git.exe"
if not defined GIT if exist "C:\Program Files (x86)\Git\cmd\git.exe" set "GIT=C:\Program Files (x86)\Git\cmd\git.exe"
if not defined GIT if exist "%LOCALAPPDATA%\Programs\Git\cmd\git.exe" set "GIT=%LOCALAPPDATA%\Programs\Git\cmd\git.exe"
if not defined GIT (
  echo [ERROR] Cannot find git.exe. Install Git for Windows, then run again.
  pause
  exit /b 1
)

:GIT_OK
echo   git: %GIT%
echo.

set "REPO=%~1"
if not defined REPO goto ASK_URL
goto HAVE_URL

:ASK_URL
echo   Press ENTER to accept the suggested URL, or paste your own:
echo   %DEFAULT_REPO%
set "REPO="
set /p "REPO=URL: "
if not defined REPO set "REPO=%DEFAULT_REPO%"

:HAVE_URL
echo   Using: %REPO%
echo.

echo [1/4] Committing current files...
if not exist ".git" (
  "%GIT%" init -b main
  "%GIT%" config user.name "LyricSync Build"
  "%GIT%" config user.email "build@users.noreply.github.com"
)
"%GIT%" add -A
"%GIT%" commit -m "LyricSync VST3 source" 2>nul

echo [2/4] Setting remote origin...
"%GIT%" remote remove origin >nul 2>&1
"%GIT%" remote add origin %REPO%
"%GIT%" branch -M main

echo [2b/4] Reconciling remote history (avoids "fetch first" rejections)...
"%GIT%" fetch origin main >nul 2>&1
if not errorlevel 1 (
  "%GIT%" merge --allow-unrelated-histories -s ours origin/main -m "Merge remote starter files" >nul 2>&1
)

echo [3/4] Pushing to GitHub...
echo       A browser window may pop up for GitHub sign-in. Please approve it.
ver >nul
"%GIT%" push -u origin main
if errorlevel 1 (
  echo.
  echo [FAILED] Push failed. Check URL / network / login, then run this again.
  echo       Tip: paste the exact URL shown on your new repo page.
  pause
  exit /b 1
)

echo [4/4] Push succeeded.
echo.
echo   Now open your repo and click the "Actions" tab.
echo   "Build VST3 (Windows)" starts automatically on this push.
echo   When it turns green, scroll to Artifacts and download
echo   LyricSync-VST3-Windows-x64  ^=^>  LyricSync.vst3
echo.
pause
