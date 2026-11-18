cmake_minimum_required(VERSION 3.22)

project(LyricSync VERSION 2.6.15 LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 源码中的中文提示统一按 UTF-8 编译，避免 MSVC 用系统 ANSI 代码页解读
if(MSVC)
    add_compile_options(/utf-8)
else()
    add_compile_options(-finput-charset=UTF-8 -fexec-charset=UTF-8)
endif()

# -----------------------------------------------------------------------------
# JUCE：优先使用 -DJUCE_DIR=<本地 JUCE 源码目录>，否则自动拉取固定版本
# -----------------------------------------------------------------------------
set(JUCE_TAG "7.0.12" CACHE STRING "JUCE tag used when downloading")

if(JUCE_DIR)
    add_subdirectory("${JUCE_DIR}" "${CMAKE_BINARY_DIR}/JUCE")
else()
    include(FetchContent)
    FetchContent_Declare(juce
        GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
        GIT_TAG ${JUCE_TAG}
        GIT_SHALLOW TRUE
        GIT_PROGRESS TRUE)
    FetchContent_MakeAvailable(juce)
endif()

juce_add_plugin(LyricSync
    COMPANY_NAME                  "Muzi Audio Tool"
    PLUGIN_MANUFACTURER_CODE      Muzy
    PLUGIN_CODE                   LySy
    FORMATS                       VST3 Standalone
    PRODUCT_NAME                  "LyricSync"
    DESCRIPTION                   "Timeline-synced lyrics display for DAWs"
    IS_SYNTH                      FALSE
    NEEDS_MIDI_INPUT              FALSE
    NEEDS_MIDI_OUTPUT             FALSE
    IS_MIDI_EFFECT                FALSE
    # 打轴功能需要在插件窗口里接收空格键，所以要向宿主申请键盘焦点
    EDITOR_WANTS_KEYBOARD_FOCUS   TRUE
    COPY_PLUGIN_AFTER_BUILD       FALSE
    VST3_CATEGORIES               Fx)

target_sources(LyricSync
    PRIVATE
        Source/PluginProcessor.cpp
        Source/PluginEditor.cpp
        Source/LyricsDocument.cpp
        Source/KrcLyrics.cpp
        Source/OnlineLyrics.cpp
        Source/WordLyrics.cpp
        Source/I18n.cpp
        Source/qrcodegen.c)

target_compile_definitions(LyricSync
    PUBLIC
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_DISPLAY_SPLASH_SCREEN=0
        JUCE_VST3_CAN_REPLACE_VST2=0
        JUCE_STRICT_REFCOUNTEDPOINTER=1)

target_link_libraries(LyricSync
    PRIVATE
        juce::juce_audio_utils
        juce::juce_data_structures
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)

# 在线搜词用 WinHTTP 直连（绕开 WinINet 继承的系统代理设置）
if (WIN32)
    target_link_libraries(LyricSync PUBLIC winhttp)
endif()
