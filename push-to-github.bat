@echo off
setlocal
cd /d "%~dp0"

echo ==========================================================
echo   LyricSync VST3 - one click push to GitHub
echo ==========================================================
echo.
echo   Step A: create an EMPTY public repo at https://github.com/new
echo   Step B: copy its URL, then paste it below.
echo.

if "%~1"=="" (
  set "REPO="
  set /p "REPO=Paste repo URL (https://github.com/USER/REPO.git): "
) else (
  set "REPO=%~1"
)

if "%REPO%"=="" (
  echo [ERROR] No URL entered. Aborted.
  pause
  exit /b 1
)

where git >nul 2>nul
if errorlevel 1 (
  echo [ERROR] git not found in PATH.
  pause
  exit /b 1
)

if not exist ".git" (
  echo [1/4] Initialising local repository...
  git init -b main
  git config user.name "LyricSync Build"
  git config user.email "build@users.noreply.github.com"
  git add -A
  git commit -m "LyricSync VST3 source"
) else (
  echo [1/4] Committing current files...
  git add -A
  git commit -m "Update LyricSync VST3 source" 2>nul
)

echo [2/4] Setting remote origin...
git remote remove origin >nul 2>&1
git remote add origin %REPO%
git branch -M main

echo [3/4] Pushing to GitHub...
echo       A browser window may pop up for GitHub sign-in. Please approve it.
git push -u origin main
if errorlevel 1 (
  echo.
  echo [FAILED] Push failed. Check URL / network / login, then run this again.
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
