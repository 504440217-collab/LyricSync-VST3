# -*- coding: utf-8 -*-
"""
Generate Source/I18n.h and Source/I18n.cpp from the bilingual string table.

Why this exists
---------------
juce::String (const char*) is an ASCII-only constructor in JUCE 7. Passing
UTF-8 Chinese bytes to it makes every byte a separate Latin-1 code point,
which is exactly the "mojibake" seen inside DAWs. All localised text must go
through juce::String::fromUTF8().

On top of that, C++ source files containing raw non-ASCII characters depend
on the compiler's source/execution charset (MSVC on en-US CI runners uses
code page 1252 unless /utf-8 is passed). To be bulletproof, the generated
C++ is PURE ASCII: every non-ASCII character is emitted as a run of \\xNN
UTF-8 byte escapes in its own string literal.

Run:  python tools/gen_i18n.py
"""
from pathlib import Path

# ---------------------------------------------------------------------------
# key, Chinese, English
# ---------------------------------------------------------------------------
TABLE = [
    ("appSubtitle",      "DAW 歌词同步  |  跟随宿主播放头",
                         "DAW lyrics  |  follows the host playhead"),
    ("currentLine",      "CURRENT LINE", "CURRENT LINE"),
    ("loadLyrics",       "载入歌词", "Load Lyrics"),
    ("pasteEdit",        "粘贴 / 编辑", "Paste / Edit"),
    ("demoLyrics",       "示例歌词", "Demo Lyrics"),
    ("themeLight",       "浅色", "Light"),
    ("themeDark",        "深色", "Dark"),
    ("reset",            "归零", "Reset"),
    ("applyLyrics",      "应用歌词", "Apply"),
    ("cancel",           "取消", "Cancel"),
    ("manualPreview",    "手动预览", "Preview"),
    ("fontSize",         "字号", "Font"),
    ("languageChinese",  "中文", "中文"),
    ("languageEnglish",  "English", "English"),
    ("uiLanguage",       "选择界面语言", "Select UI language"),
    ("separator",        "  |  ", "  |  "),
    ("minusSymbol",      "-", "-"),
    ("playSymbol",       ">", ">"),
    ("stopSymbol",       "[]", "[]"),

    ("hint",             "提示：歌词跟随宿主播放头 | 任意跳转 / 循环 / 倒退都会立即对齐 | 支持拖入 .lrc / .srt / .txt",
                         "Tip: lyrics follow the host playhead | seeking, looping and rewinding align instantly | drop .lrc / .srt / .txt"),
    ("demoFileDesc",     "示例歌词  |  拖入文件即可替换",
                         "Demo lyrics  |  drop a file to replace"),
    ("savedFileDesc",    "宿主工程中保存的歌词", "Lyrics saved in host project"),
    ("emptyStage",       "把 .lrc / .srt / .txt 拖进窗口\n或点击左下角的 载入歌词 按钮",
                         "Drop a .lrc / .srt / .txt file here\nor click Load Lyrics below"),
    ("intro",            "前奏", "Intro"),
    ("nextLineIn",       "下一句还有 ", "Next line in "),
    ("secondsSuffix",    " 秒", " s"),
    ("lineCountSuffix",  " 句", " lines"),
    ("karaokeSuffix",    "  |  逐字", "  |  karaoke"),
    ("warningPrefix",    "    ! ", "    ! "),
    ("readFailed",       "读取失败：", "Could not read: "),
    ("emptyFile",        "文件为空或无法读取（支持 UTF-8 / UTF-16 / GBK）",
                         "File is empty or unreadable (UTF-8 / UTF-16 / GBK supported)"),
    ("pastedLabel",      "粘贴的歌词文本", "Pasted lyrics"),
    ("fileChooserTitle", "选择歌词文件", "Choose a lyrics file"),

    ("statusManual",     "手动预览  |  拖动进度条定位",
                         "Preview  |  drag the slider to seek"),
    ("statusWaiting",    "等待宿主时间码...（可勾选手动预览）",
                         "Waiting for host timecode... (or enable Preview)"),
    ("statusRecording",  "录音中", "Recording"),
    ("statusPlaying",    "播放中", "Playing"),
    ("statusStopped",    "已停止", "Stopped"),
    ("statusLoop",       "  |  循环", "  |  Loop"),
    ("statusIdle",       "  |  宿主暂未推送时间码", "  |  Host timecode idle"),
    ("barPrefix",        "小节 ", "Bar "),

    # Parser warnings (used by LyricsDocument)
    ("warnNoTimestamps1", "未检测到时间戳，已按每句 ",
                          "No timestamps found; arranged at "),
    ("warnNoTimestamps2", " 秒自动排布；建议改用带时间戳的 LRC 文件。",
                          " s per line. Use a timed LRC for accuracy."),
    ("warnFirstLine1",    "第一句在 ", "First line starts at "),
    ("warnFirstLine2",    " 秒之后，若歌词整体错位请检查偏移或 LRC 的 [offset:] 标签。",
                          " s. If lyrics are offset, check Offset or the LRC [offset:] tag."),
]

DEMO_ZH = """[ti:LyricSync 演示歌词]
[ar:Muzi Audio Tools]
[00:00.00]LyricSync | 歌词同步显示
[00:04.00]把 .lrc 文件拖进窗口就能开始
[00:08.50]播放头跳到哪里
[00:12.00]歌词就跟到哪里
[00:15.50]停止、倒退、循环都不会错位
[00:20.00]因为它读的是宿主的项目时间
[00:24.50]而不是插件自己的计时器
[00:29.00]试试把播放头拖到 00:40
[00:33.00]看看下一句会不会立刻出现
[00:37.00]再跳回 00:12 也没问题
[00:40.00]你跳到了 40 秒，这一句应该已经亮起
[00:45.00]在下方可以微调整体偏移
[00:50.00]也可以粘贴自己的歌词文本
[00:55.00]支持 LRC / 逐字 LRC / SRT / 纯文本
[01:00.00]祝录音顺利
"""

DEMO_EN = """[ti:LyricSync Demo Lyrics]
[ar:Muzi Audio Tools]
[00:00.00]LyricSync - synced lyrics display
[00:04.00]Drop a .lrc file into this window
[00:08.50]Move the playhead anywhere
[00:12.00]The lyrics follow instantly
[00:15.50]Stop, rewind or loop - never out of sync
[00:20.00]It reads the host project time
[00:24.50]Not a timer inside the plugin
[00:29.00]Try jumping to 00:40
[00:33.00]The next line appears right away
[00:37.00]Jump back to 00:12, still works
[00:40.00]You reached 40 seconds - this line is lit
[00:45.00]Fine-tune the offset below
[00:50.00]Or paste your own lyrics text
[00:55.00]LRC / word-timed LRC / SRT / plain text
[01:00.00]Happy recording
"""


def cpp_string(text):
    """Return a concatenation of pure-ASCII C++ string literals for `text`."""
    parts, buf = [], ""

    def flush():
        nonlocal buf
        if buf:
            parts.append('"' + buf + '"')
            buf = ""

    for ch in text:
        raw = ch.encode("utf-8")
        if len(raw) == 1 and 0x20 <= raw[0] < 0x7f and ch not in ('"', "\\"):
            buf += ch
            continue
        if ch == "\n":
            flush()
            parts.append('"\\n"')
            continue
        flush()
        parts.append('"' + "".join("\\x%02x" % b for b in raw) + '"')
    flush()
    return " ".join(parts) if parts else '""'


ROOT = Path(__file__).resolve().parent.parent

header = [
    "// AUTO-GENERATED by tools/gen_i18n.py -- do not edit by hand.",
    "#pragma once",
    "",
    "#include <juce_core/juce_core.h>",
    "",
    "//==============================================================================",
    "/** All localised UI text. The .cpp stores pure-ASCII UTF-8 escape literals.",
    " *  Language: 0 = Chinese, 1 = English.",
    " */",
    "namespace Lang",
    "{",
    "    enum class TextId",
    "    {",
]
for i, (key, _zh, _en) in enumerate(TABLE):
    header.append("        %s%s" % (key, "," if i < len(TABLE) - 1 else ""))
header += [
    "    };",
    "",
    "    /** Decode a UTF-8 byte literal into a real juce::String. */",
    "    juce::String u8 (const char* utf8Bytes);",
    "",
    "    /** Look up a localised string. language: 0 = Chinese, 1 = English. */",
    "    juce::String tr (TextId id, int language);",
    "",
    "    /** Built-in demonstration LRC, in the requested language. */",
    "    juce::String demoLyrics (int language);",
    "}",
    "",
]
(ROOT / "Source" / "I18n.h").write_text("\n".join(header), encoding="ascii")

cpp = [
    "// AUTO-GENERATED by tools/gen_i18n.py -- do not edit by hand.",
    "#include \"I18n.h\"",
    "",
    "namespace Lang",
    "{",
    "namespace",
    "{",
    "    // { Chinese, English }. All entries are plain ASCII C++ literals;",
    "    // non-ASCII characters are UTF-8 byte escapes.",
    "    static const char* const kStrings[][2] =",
    "    {",
]
for key, zh, en in TABLE:
    cpp.append("        /* %-18s */ { %s, %s }," % (key, cpp_string(zh), cpp_string(en)))
cpp += [
    "    };",
    "",
    "    static const char* const kDemo[2] =",
    "    {",
    "        %s," % cpp_string(DEMO_ZH),
    "        %s" % cpp_string(DEMO_EN),
    "    };",
    "}",
    "",
    "juce::String u8 (const char* utf8Bytes)",
    "{",
    "    return juce::String::fromUTF8 (utf8Bytes);",
    "}",
    "",
    "juce::String tr (TextId id, int language)",
    "{",
    "    const int index = static_cast<int> (id);",
    "    const int count = static_cast<int> (sizeof (kStrings) / sizeof (kStrings[0]));",
    "",
    "    if (index < 0 || index >= count)",
    "        return {};",
    "",
    "    return juce::String::fromUTF8 (kStrings[index][language == 1 ? 1 : 0]);",
    "}",
    "",
    "juce::String demoLyrics (int language)",
    "{",
    "    return juce::String::fromUTF8 (kDemo[language == 1 ? 1 : 0]);",
    "}",
    "",
    "} // namespace Lang",
    "",
]
(ROOT / "Source" / "I18n.cpp").write_text("\n".join(cpp), encoding="ascii")

print("generated I18n.h / I18n.cpp with %d strings" % len(TABLE))
for f in ("Source/I18n.h", "Source/I18n.cpp"):
    p = ROOT / f
    bad = [b for b in p.read_bytes() if b > 127]
    print("  %-20s %6d bytes, non-ASCII bytes: %d" % (f, p.stat().st_size, len(bad)))
