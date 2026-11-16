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
    ("appSubtitle",      "DAW 歌词同步",
                         "DAW lyrics"),
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

    ("hint",             "提示：歌词跟随宿主播放头 | 任意跳转 / 循环 / 倒退都会立即对齐 | 支持 .lrc / .krc / .srt / .txt",
                         "Tip: lyrics follow the host playhead | seeking, looping and rewinding align instantly | .lrc / .krc / .srt / .txt"),
    ("demoFileDesc",     "示例歌词  |  拖入文件即可替换",
                         "Demo lyrics  |  drop a file to replace"),
    ("savedFileDesc",    "宿主工程中保存的歌词", "Lyrics saved in host project"),
    ("emptyStage",       "把 .lrc / .krc / .srt / .txt 拖进窗口\n或点击左下角的 载入歌词 按钮",
                         "Drop a .lrc / .krc / .srt / .txt file here\nor click Load Lyrics below"),
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

    #--- 打轴（Tap timing） -------------------------------------------------
    ("tapStart",         "开始打轴", "Tap Timing"),
    ("tapRecordThis",    "记录本句", "Stamp"),
    ("tapTitle",         "编辑歌词", "EDIT LYRICS"),
    ("tapHint",          "播放到这一句开始时：点击画面空白处，或按空格",
                         "When this line starts: click the stage, or press Space"),
    ("tapSpaceWarn",     "若宿主抢占了空格键，请直接用鼠标点击",
                         "If the host captures Space, just click with the mouse"),
    ("tapPrev",          "已记录", "STAMPED"),
    ("tapNext",          "下一句", "NEXT"),
    ("tapWillStamp",     "本句时间码", "Stamp at"),
    ("tapDoneLabel",     "打轴结果", "Tap timing result"),
    ("tapUndo",          "撤销", "Undo"),
    ("tapRestart",       "重打本句", "Redo line"),
    ("tapNudgeBack",     "−10 ms", "−10 ms"),
    ("tapNudgeFwd",      "+10 ms", "+10 ms"),
    ("tapPause",         "暂停", "Pause"),
    ("tapHostPlay",      "宿主播放", "Host play"),
    ("tapHostPause",     "宿主暂停", "Host pause"),
    ("tipTapHost",       "向宿主发送播放/暂停（空格键），免得出插件窗口去按。若宿主改过快捷键则不生效",
                         "Sends a play/pause keystroke to the host so you do not have to leave the window"),
    ("tapHostKeyHint",   "已向宿主发送空格键；没有反应说明宿主改过播放快捷键",
                         "Sent Space to the host - no reaction means its shortcut differs"),
    ("tapEditTitle",     "编辑这一句", "Edit line"),
    ("tapSpaceHint",     "空格 = 记录本句", "Space = stamp"),
    ("tipStamp",         "把当前播放位置记录为本句歌词（等同于按空格）", "Stamp the current playhead position to this line (same as Space)"),
    ("tapRowText",       "歌词文字", "Text"),
    ("tapRowTime",       "时间码", "Timecode"),
    ("tapRowRestamp",    "重新打点", "Stamp now"),
    ("tapRowApply",      "应用", "Apply"),
    ("menuCut",          "剪切", "Cut"),
    ("menuCopy",         "复制", "Copy"),
    ("menuPaste",        "粘贴", "Paste"),
    ("menuDelete",       "删除", "Delete"),
    ("menuSelectAll",    "全选", "Select all"),
    ("menuUndo",         "撤销", "Undo"),
    ("tapBadTime",       "时间码格式不对，请写成 01:23.45",
                         "Bad timecode - use 01:23.45"),
    ("tapTimeOrder",     "时间必须晚于上一句", "Time must be later than the previous line"),
    ("tipTapRowRestamp", "把这一句的时间改成当前播放位置", "Set this line's time to the current playhead"),
    ("tapResume",        "继续", "Resume"),
    ("tapListTitle",     "点一行，直接改时间码和文字", "Click a line to edit its time and text"),
    ("tapPausedWarn",    "已暂停：调整好后点「继续」或直接播放，再点画面继续打轴",
                         "Paused: press Resume or start playback, then tap again"),
    ("tapHostRewind",   "已回到该句起点：请把宿主播放头拖到此处再继续打轴",
                         "Rewound to that line - drag the host playhead there to continue"),
    ("tapTooClose",      "与上一句间隔太短，可能播放已暂停，已忽略",
                         "Too close to the previous line - is playback paused?"),
    ("tapNudged",        "已调整上一句时间", "Previous line nudged"),
    ("tipTapRestart",    "预备重打这一句：不必先重打上一句，唱到本句开头点一下即可",
                         "Arm this line for re-stamping: no need to redo the line before it"),
    ("tapArmTitle",      "预备重打", "Re-stamp armed"),
    ("tapArmLine",       "第 %d 句", "line %d"),
    ("tapArmHint",       "唱到本句开头时按空格 / 点画面，只写这一句",
                         "Tap or press Space at this line's start - only this line is written"),
    ("tapArmTooEarly",   "还在上一句里，等本句开头再点",
                         "Still inside the previous line - tap when this one starts"),
    ("tapArmDone",       "已重打第 ", "Re-stamped line "),
    ("tapArmDemo",       "手动预览已倒回 3 秒并暂停，按播放顺着唱过来",
                         "Manual preview rewound 3 s and paused - press play and follow"),
    ("tapArmSeek",       "先把播放头拖到 ", "Seek before "),
    ("tapArmSeek2",      " 之前再播放，唱到本句开头点一下",
                         " and play, then tap when this line starts"),
    ("tapArmAhead",      "已越过本句 ", "Already past this line by "),
    ("tapArmCancel",     "已取消预备", "Arm cancelled"),
    ("tapUnarm",         "取消预备", "Cancel arm"),

    #--- 设起点 / 撤销 -------------------------------------------------------
    ("setStart",        "设起点", "Set start"),
    ("undoSetStart",    "撤销", "Undo"),
    ("setStartDone",    "已把当前播放位置设为歌词起点", "Lyrics start re-anchored to the playhead"),
    ("setStartUndo",    "已撤销上一次起点调整", "Previous anchor change undone"),
    ("setStartNone",    "没有可撤销的起点调整", "Nothing to undo"),
    ("tipSetStart",     "把播放头放到音频的新开头，再点这里：歌词时间轴会整体重设起点（音频块被拖动后用它对齐）",
                        "Put the playhead at the clip's new start, then click: the lyric timeline is re-anchored (use after moving the audio clip)"),
    ("tipUndoSetStart", "撤销上一次「设起点」或偏移调整",
                        "Undo the last anchor change or offset nudge"),

    #--- 逐句颜色与备注 -----------------------------------------------------
    ("annotate",        "着色备注", "Style"),
    ("tipAnnotate",     "刷子图标：点开后光标变 I 形，在原歌词界面拖选句子，右键在鼠标旁设置；点颜色立即生效，备注需点应用",
                        "Brush: I-beam cursor, drag to select lines in the lyric view, right-click for the box; colours apply instantly, notes need Apply") ,
    ("annotateTitle",   "着色与备注", "Colour and note"),
    ("annotateLine",    "第 ", "Line "),
    ("annotateColour",  "颜色", "Colour"),
    ("annotateNote",    "备注", "Note"),
    ("annotateDefault", "默认", "Default"),
    ("annotateClear",   "清除", "Clear"),
    ("annotateApplyNote", "应用备注", "Apply note"),
    ("annotateHint",    "拖选歌词（可跨行）→ 右键打开小选择框；点颜色立即生效，备注需点应用；滚轮翻歌词，Esc 退出",
                        "Drag to select lines, right-click for the box; colours apply instantly, notes need Apply; wheel scrolls, Esc exits"),
    ("annotateNotePlaceholder", "例如：李三", "e.g. Li San"),
    ("tipTapNudge",      "微调刚打好的上一句时间（10 毫秒一步）",
                         "Nudge the last stamped line by 10 ms"),
    ("tipTapPause",      "手动预览模式下暂停/继续，方便停下来重打某句",
                         "Pause/resume in manual preview so you can fix a line"),
    ("tapFinish",        "完成", "Finish"),
    ("tapExit",          "退出", "Exit"),
    ("tapNoLines",       "没有可打轴的歌词行，请先输入歌词文本",
                         "No lyric lines to time - enter some lyrics first"),

    #--- 音频长度（Progress bar follows the real audio, not the project） ----
    ("audioPrefix",      "音频 ", "Audio "),
    ("audioUnknown",     "音频长度：播放一遍即可自动测量",
                         "Audio length: play once to measure"),
    ("measureAudio",     "重测音频", "Re-measure"),
    ("audioMeasured",    "已测量音频长度，进度条按这首歌的长度显示",
                         "Audio length measured - the progress bar now follows this song"),

    #--- Tooltips -----------------------------------------------------------
    #--- KRC (酷狗逐字歌词) ---------------------------------------------------
    ("krcFormat",    "KRC 酷狗逐字", "KRC (Kugou)"),
    ("krcLoaded",    "KRC 已转成逐字歌词；若整体偏移请用 - / + 微调",
                     "KRC converted to word-timed lyrics; use -/+ to nudge the offset"),
    ("tipLoad",     "载入 .lrc / .krc / .srt / .txt 歌词文件", "Load a .lrc / .krc / .srt / .txt lyrics file"),
    ("tipPaste",    "编辑歌词：点一行就能直接改时间码和文字", "Edit lyrics: click a line to change its timecode and text"),
    ("tipDemo",     "载入内置示例歌词", "Load the built-in demo lyrics"),
    ("tipTheme",    "切换深色 / 浅色界面", "Switch dark / light theme"),
    ("tipMinus",    "歌词整体提前 100 毫秒", "Shift all lyrics 100 ms earlier"),
    ("tipPlus",     "歌词整体延后 100 毫秒", "Shift all lyrics 100 ms later"),
    ("tipReset",    "把时间偏移归零", "Reset the offset to zero"),
    ("tipFont",     "调整歌词字号大小", "Adjust the lyric font size"),
    ("tipPreview",  "手动预览：用进度条拖动时间，不依赖宿主播放",
                    "Manual preview: drag the bar instead of the host playhead"),
    ("tipPlay",     "开始手动预览播放", "Start manual preview playback"),
    ("tipStop",     "停止手动预览播放", "Stop manual preview playback"),
    ("tipApply",    "应用编辑器里的歌词文本", "Apply the lyrics in the editor"),
    ("tipCancel",   "放弃本次修改", "Discard these changes"),
    ("tipTap",      "打轴：边播放边点击画面或按空格，逐句记录时间码",
                    "Tap timing: play and click/press Space to stamp each line"),
    ("tipUndo",     "撤销上一句已记录的时间码", "Undo the last stamped line"),
    ("tipFinish",   "结束打轴并生成带时间码的歌词", "Finish and build the timed lyrics"),
    ("tipExit",     "退出打轴，不保存", "Exit tap mode without saving"),
    ("tipMeasure",  "重新测量音频长度（播放时自动测量，进度条按它显示）",
                    "Re-measure the audio length (measured automatically while playing)"),
    ("tipSeek",     "播放进度：按音频实际长度显示，可拖动定位",
                    "Playback position: follows the real audio length, drag to seek"),

    #--- 设置面板（右上角齿轮） ---------------------------------------------
    ("settingsTitle",  "设置", "Settings"),
    ("setLanguage",    "界面语言", "Language"),
    ("setZoom",        "窗口比例", "Window size"),
    ("setLayout",      "窗口布局", "Layout"),
    ("orientH",        "横版", "Landscape"),
    ("orientV",        "竖版", "Portrait"),
    ("setSkin",        "皮肤", "Skin"),
    ("setFont",        "字体", "Font"),
    ("setColor",       "歌词颜色", "Lyric colour"),
    ("setAdvance",     "歌词推进", "Advance"),
    ("advLine",        "逐句跳动", "By line"),
    ("advChar",        "逐字跳动", "By word"),
    ("setBackground",  "自定义背景", "Background"),
    ("pickImage",      "选择图片", "Choose image"),
    ("clearImage",     "清除", "Clear"),
    ("closeLabel",     "关闭", "Close"),

    ("skinDark",       "深色", "Dark"),
    ("skinLight",      "浅色", "Light"),
    ("skinMidnight",   "午夜蓝", "Midnight"),
    ("skinWarm",       "暖阳", "Warm"),
    ("skinForest",     "森林", "Forest"),
    ("skinCustom",     "自定义图片", "Custom image"),

    ("colorAuto",      "跟随皮肤", "Theme"),
    ("colorWhite",     "白色", "White"),
    ("colorAmber",     "淡黄", "Amber"),
    ("colorMint",      "青绿", "Mint"),
    ("colorSky",       "天蓝", "Sky"),
    ("colorPink",      "粉色", "Pink"),

    #--- 关于窗口（点左上角 logo） -------------------------------------------
    ("aboutTitle",     "关于 LyricSync", "About LyricSync"),
    ("aboutVersion",   "版本", "Version"),
    ("aboutAuthor",    "创作者", "Author"),
    ("aboutAuthorValue", "李宾 @ 沐兹音乐工作室（Muzi Audio Tool）",
                        "Li Bin @ Muzi Audio Studio (Muzi Audio Tool)"),
    ("aboutDesc",      "在 DAW 里显示与宿主播放头精确同步的歌词\n支持 LRC / 增强 LRC / 酷狗 KRC / SRT / TXT\n任意跳转、循环、倒退都会立即对齐\n卡拉OK 多行歌词 + 点击打轴 + 逐字高亮",
                       "Lyrics that follow the host playhead exactly\nLRC / enhanced LRC / Kugou KRC / SRT / TXT\nSeeking, looping and rewinding align instantly\nKaraoke stack + tap timing + word highlighting"),
    ("aboutHint",      "点击任意处关闭", "Click anywhere to close"),

    #--- 歌词存档 -----------------------------------------------------------
    ("savedToLibrary", "已存档到歌词库", "Saved to lyric library"),
    ("libraryNote",    "歌词已存入本机歌词库，重新打开工程会自动读取",
                       "Lyrics are archived locally and reloaded with the project"),

    #--- Tooltips: 设置 / 关于 ----------------------------------------------
    ("tipGear",     "设置：窗口比例 / 横竖版 / 皮肤 / 字体 / 语言",
                    "Settings: window size, layout, skin, font, language"),
    ("tipLogo",     "关于 LyricSync：版本、创作者、功能介绍",
                    "About LyricSync: version, author, features"),
    ("tipZoom",     "插件窗口大小（下次打开工程会记住）",
                    "Plugin window size (remembered for this project)"),
    ("tipLayout",   "横版 / 竖版布局，控件会跟着重排", "Landscape / portrait layout"),
    ("tipSkin",     "选择界面皮肤，或用自己的图片做背景", "Pick a skin, or use your own image"),
    ("tipTypeface", "歌词字体（typeface）", "Lyrics typeface"),

    #--- 字体显示名（下拉框用中文） ------------------------------------------
    ("fontYaHei",    "微软雅黑", "Microsoft YaHei"),
    ("fontDengXian", "等线",     "DengXian"),
    ("fontSimHei",   "黑体",     "SimHei"),
    ("fontSimSun",   "宋体",     "SimSun"),
    ("fontKaiTi",    "楷体",     "KaiTi"),
    ("fontFangSong", "仿宋",     "FangSong"),
    ("fontYouYuan",  "幼圆",     "YouYuan"),
    ("fontLiSu",     "隶书",     "LiSu"),
    ("tipColor",    "歌词文字颜色", "Lyrics text colour"),
    ("tipAdvance",  "逐句：整句一起亮；逐字：歌词从第一个字扫到最后一个字",
                    "By line: the whole line lights up; by word: the highlight sweeps across"),
    ("previewBadge", "预览", "Preview"),
    ("exportLrc",    "导出 LRC", "Export LRC"),
    ("exportTitle",  "导出歌词", "Export lyrics"),
    ("tipExport",    "按当前状态重新生成一份新的 .lrc：包含改过的歌词文字、最新时间码，以及已折算进时间码的提前/延后偏移",
                     "Rebuild a fresh .lrc from the current state: edited text, latest timestamps and the offset baked into every timestamp"),
    ("exportEmpty",  "当前没有可导出的歌词", "No lyrics to export"),
    ("exportDone",   "已生成新的 LRC 文件", "New LRC file created"),
    ("exportFailed", "写入失败，请检查保存位置或文件权限", "Could not write the file; check the location and permissions"),
    ("zenExit",      "双击返回", "Double-click to exit"),
    ("bpmPrefix",    "测速 ", "Tempo "),
    ("retempo",      "重新测速", "Re-detect"),
    ("tipRetempo",   "清空已采集的音频，重新分析节拍（播放 10 秒以上最准）",
                     "Discard the captured audio and analyse the tempo again (best after 10 s of playback)"),
    ("wordFormat",   "Word 文档", "Word document"),
    ("showHints",    "界面提示", "On-screen tips"),
    ("onLabel",      "开", "On"),
    ("offLabel",     "关", "Off"),

    #--- 左下角轮播提示 -----------------------------------------------------
    ("hintA", "双击歌词区 = 纯享全屏，再双击返回", "Double-click the lyrics for full-screen, again to return"),
    ("hintClick", "点这里换下一条提示", "Click for the next tip"),
    ("hintB", "歌词区按住上下拖动，可以翻看前后歌词", "Drag the lyrics up/down to preview other lines"),
    ("hintC", "齿轮里可换皮肤 / 字体 / 歌词颜色 / 窗口比例", "The gear holds skin, font, colour and window size"),
    ("hintD", "支持 LRC / 酷狗 KRC / SRT / TXT / Word 歌词", "LRC / Kugou KRC / SRT / TXT / Word lyrics"),
    ("hintE", "「导出 LRC」可把当前歌词存成文件", "Export LRC saves the current lyrics to a file"),
    ("hintF", "前奏结尾会出现 4 拍圆点倒数", "A 4-beat count-in appears before the first line"),
    ("tipPickImage", "上传一张图片作为自定义皮肤背景", "Use your own image as the skin background"),
    ("tipClearImage", "清除自定义背景图片", "Remove the custom background image"),
    ("tipClose",    "关闭设置", "Close settings"),

    # Parser warnings (used by LyricsDocument)
    ("warnNoTimestamps1", "未检测到时间戳，已按每句 ",
                          "No timestamps found; arranged at "),
    ("warnNoTimestamps2", " 秒自动排布；建议改用带时间戳的 LRC 文件。",
                          " s per line. Use a timed LRC for accuracy."),
    ("warnFirstLine1",    "第一句在 ", "First line starts at "),
    ("warnFirstLine2",    " 秒之后，若歌词整体错位请检查偏移或 LRC 的 [offset:] 标签。",
                          " s. If lyrics are offset, check Offset or the LRC [offset:] tag."),

    #--- 在线搜索歌词 -------------------------------------------------------
    ("onlineLyrics",   "在线搜词", "Search online"),
    ("tipOnline",      "按歌名 / 歌手在线搜索歌词，选一条直接加载进插件（酷狗直连，LRCLIB 兜底）",
                       "Search lyrics online by title / artist and load one straight into the plugin (Kugou, LRCLIB fallback)"),
    ("onlineTitle",    "在线搜索歌词", "Search lyrics online"),
    ("onlineSong",     "歌名", "Title"),
    ("onlineArtist",   "歌手", "Artist"),
    ("onlineSearch",   "搜索", "Search"),
    ("onlineSearching","正在搜索，请稍候…", "Searching, please wait…"),
    ("onlineNotFound", "没有找到歌词，试试更完整的歌名，或检查网络后重试",
                       "No lyrics found. Try a fuller title, or check your connection"),
    ("onlineFound",    "找到", "Found"),
    ("onlineEnterTitle","请先输入歌名", "Enter a title first"),
    ("onlineLoading",  "正在下载歌词…", "Downloading lyrics…"),
    ("onlineLoadFailed","下载失败，换一条结果或稍后重试", "Download failed; try another result"),
    ("onlineLoad",     "加载歌词", "Load lyrics"),
    ("onlineLoaded",   "在线歌词", "Online lyrics"),
    ("onlineWordSync", "逐字", "Word sync"),
    ("onlineNoResult", "（暂无结果，输入歌名后点「搜索」）", "(no results yet; enter a title and press Search)"),
    ("onlineStartHint","输入歌名（可加歌手）后点「搜索」", "Enter a title (artist optional), then press Search"),
    ("onlineSelectHint","结果来自酷狗歌曲库（带准确歌手），点选一条再点「加载歌词」；滚轮可翻页；也可用 LX Music 直接取当前播放歌词",
                       "Results come from the Kugou song database with accurate artist info. Pick one, then Load; wheel scrolls; or fetch from LX Music"),
    ("onlineSourceHint","酷狗按歌曲 hash 精确取词，LRCLIB 兜底；结果标有行数与「片段」提示；无网络时用本地载入或 LX Music",
                       "Kugou matches by song hash, LRCLIB as fallback; results show line counts and partial flags; use local files or LX Music offline"),
    ("onlineSongPlaceholder",   "例如：海阔天空", "e.g. Yesterday"),
    ("onlineArtistPlaceholder", "例如：Beyond（可留空）", "e.g. The Beatles (optional)"),
    ("onlineUrlPlaceholder",    "留空即用内置源", "Leave empty for built-in sources"),
    ("setCustomUrl",   "自定义服务", "Custom service"),
    ("onlineUrlHint",  "可填自建中转地址，支持 {q}（歌名）、{artist}（歌手）、{duration}（秒）占位符；留空则用酷狗 + LRCLIB",
                       "Optional proxy URL. Use {q} (title), {artist}, {duration} (sec); empty = Kugou + LRCLIB"),

    #--- 精确匹配 / 繁简 / LX Music -------------------------------------------
    ("onlineExactHint","已按歌曲库精确匹配", "Matched via song database"),
    ("onlinePartial",  "歌词行数偏少，可能不完整，可换一条结果重试", "This lyric looks incomplete; try another result"),
    ("onlinePartialTag","片段", "partial"),
    ("onlineLinesSuffix"," 行", " lines"),
    ("onlineLx",       "从 LX Music 取词", "From LX Music"),
    ("onlineLxLoading","正在从 LX Music 读取当前播放歌词…", "Reading the current track's lyrics from LX Music…"),
    ("lxNoService",    "连不上 LX Music：请在它的「设置 → 开放 API」启用服务（默认 127.0.0.1:23330）",
                       "Cannot reach LX Music: enable its Open API in Settings (default 127.0.0.1:23330)"),
    ("lxNoLyric",      "LX Music 当前没有播放歌曲或没有歌词，请先在它里面播放目标歌曲",
                       "Nothing playing in LX Music, or no lyric available. Play the target song there first"),
    ("lxLoaded",       "LX Music", "LX Music"),
    ("lxUrlHint",      "LX Music 桌面版 v2.7.0+ 的开放 API 地址，默认 http://127.0.0.1:23330",
                       "Open API address of LX Music desktop v2.7.0+, default http://127.0.0.1:23330"),
    ("lxButtonHint",   "从 LX Music 当前播放的歌曲直接取歌词（你在它里面选对歌，最不容易串歌）",
                       "Grab lyrics for the song currently playing in LX Music (you pick the exact track there)"),
    ("lxUrlPlaceholder","http://127.0.0.1:23330", "http://127.0.0.1:23330"),
    ("setLxUrl",       "LX Music 地址", "LX Music URL"),
    ("setSimplify",    "繁体转简体", "Trad. to Simp."),
    ("onlineSimplifyOn","繁转简：开", "To Simplified: ON"),
    ("onlineSimplifyOff","繁转简：关", "To Simplified: OFF"),
    ("onlineSimplifyHint","在线获取的歌词自动把繁体转成简体；想保留粤语原词可关掉",
                       "Automatically convert traditional Chinese to simplified; turn off to keep original Cantonese text"),
    ("onlineDiag",      "诊断", "Diag"),
    ("onlineFailed",    "请求失败，请检查网络或稍后重试", "Request failed; check your connection"),
    ("onlineTimeout",   "请求超时，已自动解锁，请再点一次「搜索」", "Timed out; press Search again"),
    ("sidSource",       "来源", "Source"),
    ("sidTime",         "时间", "Time"),
    ("sidDisplay",      "显示", "Display"),
    ("netDirect",       "网络：直连", "Net: direct"),
    ("netProxy",        "网络：跟随系统代理", "Net: system proxy"),
    ("netHint",         "直连适合普通国内网络；如果你靠 v2rayN / Clash 等代理上网，改成「跟随系统代理」再开代理",
                        "Direct works on ordinary networks; choose system proxy if you reach the internet through v2rayN / Clash"),

    #--- 独立版走带 -----------------------------------------------------------
    ("standalonePlay",  "播放", "Play"),
    ("standalonePause", "暂停", "Pause"),
    ("tipStandalone",   "独立版专用：暂停 / 继续播放歌词；点右边的进度条可拖到任意位置",
                        "Standalone only: pause / resume; click or drag the progress bar to seek"),

    #--- 侧栏（内联搜索 / 步进器 / 分段开关） ---------------------------------
    ("loadShort",       "载入", "Load"),
    ("editShort",       "编辑", "Edit"),
    ("searchPlaceholder", "搜索歌名或歌手", "Search title or artist"),
    ("orientSwitchTip", "横版 / 竖版切换", "Switch horizontal / vertical"),
    ("lyricsGroup",     "歌词", "Lyrics"),
    ("offsetLabel",     "偏移", "Offset"),
    ("annotateShort",   "着色", "Style"),
    ("exportShort",     "导出", "Export"),
    ("searchRefreshTip",  "换一个歌词源：从 LX Music 取当前播放歌词",
                          "Switch source: take the playing track from LX Music"),
    ("startPrefix",     "起点 ", "Start "),
    ("offsetHint",      "±100 ms", "±100 ms"),
    ("fontHint",        "Ctrl + 滚轮可微调", "Ctrl + scroll to fine-tune"),
    ("segLine",         "逐行", "By line"),
    ("segWord",         "逐字", "By word"),
    ("cast",            "投屏", "Cast"),
    ("castSoon",        "投屏（局域网二维码）正在做，下个版本给你",
                        "Casting (LAN QR code) is on the way in the next build"),
    ("fontPx",          " px", " px"),
    ("searchResultsEmpty", "输入歌名后回车搜索", "Type a title, then press Enter"),
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


_keys = [k for k, _zh, _en in TABLE]
_dups = sorted({k for k in _keys if _keys.count(k) > 1})
if _dups:
    raise SystemExit("duplicate i18n keys: %s" % ", ".join(_dups))

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
