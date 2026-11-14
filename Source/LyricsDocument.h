#pragma once

#include <juce_core/juce_core.h>
#include <vector>

//==============================================================================
/** 一行歌词。time 是"绝对项目时间"（秒），与 LRC/SRT 的时间戳对应。 */
struct LyricLine
{
    double       time = 0.0;
    juce::String text;
    juce::String translation;              // 双语 LRC 的第二行（可选）
    std::vector<double> charOffsets;       // 每个字相对本行开始的秒数；为空表示无逐字信息

    bool hasKaraoke() const noexcept { return ! charOffsets.empty(); }
};

/** 解析结果摘要，供界面显示。 */
struct LyricsInfo
{
    juce::String title;
    juce::String artist;
    juce::String album;
    juce::String formatName;               // LRC / Enhanced LRC / SRT / TXT
    juce::String warning;
    double       offsetMs = 0.0;           // 文件内 [offset:] 标签
    int          lineCount = 0;
    int          karaokeLines = 0;
    bool         plainTextFallback = false;
};

//==============================================================================
/**
 *  歌词文档：解析 LRC / 增强 LRC(逐字) / SRT / 纯文本，提供按时间二分查找。
 *
 *  设计要点：所有时间戳都是"宿主项目绝对时间"，查找时与 DAW 的
 *  ProcessContext.projectTimeSamples / sampleRate 直接比较，因此播放头
 *  任意跳转、循环、倒退都能在下一帧立即得到正确的句子，不需要内部计时器。
 */
class LyricsDocument
{
public:
    /** 解析文本。fallbackInterval 用于无时间戳的纯文本歌词（每句秒数）。
     *  language 只影响解析警告文案：0 = 中文, 1 = English。
     */
    LyricsInfo loadText (const juce::String& rawText, double fallbackInterval = 4.0,
                         int language = 0);

    void clear();

    bool isEmpty() const noexcept               { return lines.empty(); }
    int  size()   const noexcept               { return static_cast<int> (lines.size()); }
    const LyricLine& line (int index) const    { return lines[static_cast<size_t> (index)]; }

    /** 返回 t 时刻正在唱的行索引；第一句之前返回 -1。O(log n)。 */
    int findIndexAt (double t) const noexcept;

    /** 本行结束时间 = 下一行开始时间（最后一行补 defaultHold 秒）。 */
    double lineEnd (int index, double defaultHold = 4.0) const noexcept;

    /** 整份歌词的总时长（用于进度条、手动预览滑杆）。 */
    double totalDuration() const noexcept;

    /** 返回 t 时刻已唱过的字数（0..text.length()）；无逐字信息时返回 -1。 */
    int sungCharsAt (const LyricLine& l, double t) const noexcept;

    /**
     *  连续版的"唱到第几个字"：返回值可以是 3.42，表示前 3 个字已唱完、
     *  第 4 个字唱了 42%。界面用它做字内渐变，避免整字硬跳。
     *  没有逐字时间戳时返回 -1。
     */
    double sungProgressAt (const LyricLine& l, double t) const noexcept;

    const LyricsInfo& info() const noexcept   { return lastInfo; }

private:
    std::vector<LyricLine> lines;
    LyricsInfo lastInfo;

    void finish (juce::String format, bool plainFallback, juce::String warning = {});
};
