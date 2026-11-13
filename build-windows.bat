@echo off
chcp 65001 >nul
setlocal EnableDelayedExpansion
cd /d "%~dp0"

echo.
echo  ============================================================
echo   LyricSync  -  VST3 / Standalone 一键构建
echo  ============================================================
echo.

set "CONFIG=Release"
set "BUILD_DIR=build"
set "JUCE_LOCAL=%CD%\third_party\JUCE"

where cmake >nul 2>&1
if errorlevel 1 (
    echo  [错误] 没有找到 cmake。
    echo  请安装 Visual Studio 2022，并勾选：
    echo    - 使用 C++ 的桌面开发
    echo    - 适用于 Windows 的 C++ CMake 工具
    echo  安装完成后，从「x64 Native Tools Command Prompt for VS 2022」重新运行本脚本。
    exit /b 1
)

echo [1/4] 准备 JUCE 7.0.12 ...
if not exist "%JUCE_LOCAL%\CMakeLists.txt" (
    if not exist third_party mkdir third_party
    git clone --depth 1 --branch 7.0.12 https://github.com/juce-framework/JUCE.git "%JUCE_LOCAL%"
    if errorlevel 1 (
        echo.
        echo  [提示] 自动下载失败（国内网络常见）。请手动下载：
        echo     https://github.com/juce-framework/JUCE/archive/refs/tags/7.0.12.zip
        echo  解压后把里面的 JUCE-7.0.12 文件夹改名为 JUCE，放到：
        echo     %JUCE_LOCAL%
        echo  然后重新运行本脚本。
        exit /b 1
    )
) else (
    echo  已存在本地 JUCE，跳过下载。
)

echo.
echo [2/4] 配置 CMake 工程 ...
cmake -S . -B "%BUILD_DIR%" -G "Visual Studio 17 2022" -A x64 -DJUCE_DIR="%JUCE_LOCAL%"
if errorlevel 1 (
    echo  [错误] CMake 配置失败。若你装的是 VS 2019，请把上面的生成器改成 "Visual Studio 16 2019"。
    exit /b 1
)

echo.
echo [3/4] 编译 Release ...
cmake --build "%BUILD_DIR%" --config %CONFIG% --parallel
if errorlevel 1 (
    echo  [错误] 编译失败，请把上面的报错信息发给我，我来修。
    exit /b 1
)

echo.
echo [4/4] 产物位置：
echo   VST3        : %CD%\%BUILD_DIR%\LyricSync_artefacts\%CONFIG%\VST3\LyricSync.vst3
echo   独立程序    : %CD%\%BUILD_DIR%\LyricSync_artefacts\%CONFIG%\Standalone\LyricSync.exe
echo.
echo  把 .vst3 文件夹整个复制到下面任一目录后，在 DAW 里重新扫描插件：
echo   C:\Program Files\Common Files\VST3
echo   %%LOCALAPPDATA%%\Programs\Common\VST3
echo.
echo  也可以先双击 Standalone 版 LyricSync.exe 预览界面（勾选「手动预览」可模拟播放/跳转）。
echo.
pause
