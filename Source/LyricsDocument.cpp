#include "LyricsDocument.h"
#include "I18n.h"

#include <algorithm>
#include <cmath>

namespace
{
    /** 解析 12.34 / 01:23.45 / 1:02:03,400 之类时间戳，返回秒。 */
    bool parseClock (juce::String s, double& out)
    {
        s = s.trim().replaceCharacter (',', '.');
        if (s.isEmpty())
            return false;

        for (int i = 0; i < s.length(); ++i)
        {
            auto c = s[i];
            const bool ok = juce::CharacterFunctions::isDigit (c)
                         || c == ':' || c == '.' || c == '+' || c == '-' || c == ' ';
            if (! ok)
                return false;
        }

        auto parts = juce::StringArray::fromTokens (s, ":", "");
        double v = 0.0;

        if (parts.size() == 3)
            v = parts[0].getDoubleValue() * 3600.0
              + parts[1].getDoubleValue() *   60.0
              + parts[2].getDoubleValue();
        else if (parts.size() == 2)
            v = parts[0].getDoubleValue() * 60.0 + parts[1].getDoubleValue();
        else if (parts.size() == 1)
            v = parts[0].getDoubleValue();
        else
            return false;

        if (! std::isfinite (v) || v < 0.0)
            return false;

        out = v;
        return true;
    }

    juce::StringArray splitLines (const juce::String& text)
    {
        juce::StringArray out;
        int start = 0;

        for (int i = 0; i <= text.length(); ++i)
        {
            if (i == text.length() || text[i] == '\n')
            {
                out.add (text.substring (start, i));
                start = i + 1;
            }
        }

        return out;
    }

    bool isKnownMetaTag (const juce::String& tag)
    {
        auto t = tag.toLowerCase();
        return t.startsWith ("ar:")     || t.startsWith ("ti:")
            || t.startsWith ("al:")     || t.startsWith ("by:")
            || t.startsWith ("offset:") || t.startsWith ("length:")
            || t.startsWith ("re:")     || t.startsWith ("ve:")
            || t.startsWith ("total:")  || t.startsWith ("tool:");
    }

    bool isPlainNumber (const juce::String& s)
    {
        if (s.isEmpty())
            return false;

        for (int i = 0; i < s.length(); ++i)
            if (! juce::CharacterFunctions::isDigit (s[i]))
                return false;

        return true;
    }

    /** 去掉字幕里的斜体/位置标签，但保留数字型逐字时间戳。 */
    juce::String stripSubtitleTags (const juce::String& in)
    {
        juce::String out;
        for (int i = 0; i < in.length();)
        {
            if (in[i] == '<')
            {
                auto tail = in.substring (i + 1);
                auto close = tail.indexOfChar ('>');
                if (close >= 0)
                {
                    auto inner = tail.substring (0, close);
                    double dummy = 0.0;
                    if (! parseClock (inner, dummy))   // 非时间戳标签 -> 丢弃
                    {
                        i = i + 1 + close + 1;
                        continue;
                    }
                }
            }
            out += in[i];
            ++i;
        }
        return out;
    }

    struct WordAnchor
    {
        int    charIndex = 0;
        double seconds   = 0.0;
    };

    /**
     *  从 "文<00:12.60>字" 形式的文本里抽出逐字锚点，并把每个字
     *  线性插值成相对行首的起始时间，让高亮可以平滑推进。
     */
    juce::String extractKaraoke (const juce::String& src,
                                 double lineTime,
                                 std::vector<double>& charOffsets)
    {
        juce::String clean;
        std::vector<WordAnchor> anchors;

        for (int i = 0; i < src.length();)
        {
            if (src[i] == '<')
            {
                auto tail  = src.substring (i + 1);
                auto close = tail.indexOfChar ('>');
                if (close >= 0)
                {
                    double t = 0.0;
                    if (parseClock (tail.substring (0, close), t))
                    {
                        anchors.push_back ({ clean.length(), t });
                        i = i + 1 + close + 1;
                        continue;
                    }
                }
            }
            clean += src[i];
            ++i;
        }

        if (anchors.empty() || clean.isEmpty())
            return clean;

        // 行首本身是第一个锚点；过滤掉越界/重复的锚点
        std::vector<WordAnchor> a;
        a.push_back ({ 0, lineTime });
        for (const auto& an : anchors)
        {
            if (an.charIndex <= 0 || an.charIndex > clean.length())
                continue;
            if (an.charIndex <= static_cast<int> (a.back().charIndex))
                continue;
            if (an.seconds <= a.back().seconds)
                continue;
            a.push_back (an);
        }

        if (a.size() < 2)
            return clean;

        const int n = clean.length();
        charOffsets.assign (static_cast<size_t> (n), 0.0);

        for (int ci = 0; ci < n; ++ci)
        {
            size_t k = 0;
            while (k + 1 < a.size() && a[k + 1].charIndex <= ci)
                ++k;

            double absT = a[k].seconds;

            if (k + 1 < a.size())
            {
                const double spanIdx = static_cast<double> (a[k + 1].charIndex - a[k].charIndex);
                const double spanT   = a[k + 1].seconds - a[k].seconds;
                if (spanIdx > 0.0)
                    absT += spanT * static_cast<double> (ci - a[k].charIndex) / spanIdx;
            }
            else
            {
                const auto& prev = a[a.size() - 2];
                const auto& last = a[a.size() - 1];
                const double perChar = last.charIndex > prev.charIndex
                                     ? (last.seconds - prev.seconds)
                                       / static_cast<double> (last.charIndex - prev.charIndex)
                                     : 0.08;
                absT = last.seconds + perChar * static_cast<double> (ci - last.charIndex);
            }

            charOffsets[static_cast<size_t> (ci)] = juce::jmax (0.0, absT - lineTime);
        }

        return clean;
    }
}

//==============================================================================
void LyricsDocument::clear()
{
    lines.clear();
    lastInfo = {};
}

void LyricsDocument::finish (juce::String format, bool plainFallback, juce::String warning)
{
    // LRC 的 [offset:xxx] 以毫秒为单位修正整份歌词的时间戳
    if (std::abs (lastInfo.offsetMs) > 0.001)
    {
        const double shift = lastInfo.offsetMs / 1000.0;
        for (auto& l : lines)
            l.time = juce::jmax (0.0, l.time + shift);
    }

    lastInfo.formatName         = std::move (format);
    lastInfo.plainTextFallback  = plainFallback;
    lastInfo.warning            = std::move (warning);
    lastInfo.lineCount          = size();
    lastInfo.karaokeLines       = 0;

    for (const auto& l : lines)
        if (l.hasKaraoke())
            ++lastInfo.karaokeLines;
}

LyricsInfo LyricsDocument::loadText (const juce::String& rawText, double fallbackInterval,
                                     int language)
{
    clear();

    auto text = rawText;
    if (text.startsWithChar (0xfeff))          // BOM
        text = text.substring (1);
    text = text.replace ("\r\n", "\n").replaceCharacter ('\r', '\n');

    if (text.trim().isEmpty())
    {
        finish ("-", false);
        return lastInfo;
    }

    LyricsInfo meta;
    const bool srtMode = text.contains ("-->");
    juce::StringArray plainLines;
    int karaokeCount = 0;

    //--------------------------------------------------------------------------
    //  SRT / WebVTT 风格字幕
    //--------------------------------------------------------------------------
    if (srtMode)
    {
        double pendingTime = -1.0;
        juce::String pendingText;

        auto flush = [&]()
        {
            if (pendingTime >= 0.0 && pendingText.isNotEmpty())
            {
                LyricLine l;
                l.time = pendingTime;
                l.text = pendingText.trim();
                lines.push_back (std::move (l));
            }
            pendingTime = -1.0;
            pendingText.clear();
        };

        for (const auto& rawLine : splitLines (text))
        {
            auto ln = rawLine.trim();

            if (ln.contains ("-->"))
            {
                flush();
                auto lhs = ln.upToFirstOccurrenceOf ("-->", false, false).trim();
                double t = 0.0;
                if (parseClock (lhs, t))
                    pendingTime = t;
                continue;
            }

            if (ln.isEmpty() || isPlainNumber (ln) || ln.startsWith ("WEBVTT") || ln.startsWith ("NOTE"))
            {
                flush();
                continue;
            }

            auto cleaned = stripSubtitleTags (ln).trim();
            if (cleaned.isNotEmpty())
                pendingText = pendingText.isEmpty() ? cleaned : pendingText + "  " + cleaned;
        }
        flush();

        std::stable_sort (lines.begin(), lines.end(),
                          [] (const LyricLine& a, const LyricLine& b) { return a.time < b.time; });
        finish ("SRT", false);
        return lastInfo;
    }

    //--------------------------------------------------------------------------
    //  LRC / 增强 LRC / 纯文本
    //--------------------------------------------------------------------------
    for (const auto& rawLine : juce::StringArray::fromLines (text))
    {
        juce::String s = rawLine.trim();
        if (s.isEmpty() || s.startsWith ("#"))
            continue;

        juce::Array<double> stamps;
        bool consumedAnyTag = false;

        // 行首可能有多个 [mm:ss.xx] 或 [ar:xxx] 这样的标签
        while (s.startsWithChar ('['))
        {
            const int close = s.indexOfChar (']');
            if (close < 0)
                break;

            auto tag  = s.substring (1, close).trim();
            auto rest = s.substring (close + 1);

            double t = 0.0;
            if (parseClock (tag, t))
            {
                stamps.add (t);
            }
            else if (isKnownMetaTag (tag))
            {
                auto lower = tag.toLowerCase();
                auto value = tag.substring (tag.indexOfChar (':') + 1).trim();

                if (lower.startsWith ("ti:"))        meta.title    = value;
                else if (lower.startsWith ("ar:"))   meta.artist   = value;
                else if (lower.startsWith ("al:"))   meta.album    = value;
                else if (lower.startsWith ("offset:")) meta.offsetMs = value.getDoubleValue();
            }
            else
            {
                break;   // 形如 "[合唱] 歌词" -> 当作正文，不吞掉
            }

            s = rest;
            consumedAnyTag = true;
        }

        if (stamps.isEmpty())
        {
            if (s.trim().isNotEmpty())
                plainLines.add (s.trim());      // 无时间戳的歌词正文
            else if (! consumedAnyTag)
                plainLines.add (rawLine.trim());
            continue;
        }

        s = s.trim();
        if (s.isEmpty())
            continue;

        std::vector<double> offsets;
        auto clean = extractKaraoke (s, stamps[0], offsets);
        if (! offsets.empty())
            ++karaokeCount;

        for (auto t : stamps)
        {
            LyricLine l;
            l.time        = t;
            l.text        = clean;
            l.charOffsets = offsets;
            lines.push_back (std::move (l));
        }
    }

    //--------------------------------------------------------------------------
    //  完全没有时间戳 -> 纯文本按固定间隔排布（方便快速打草稿）
    //--------------------------------------------------------------------------
    if (lines.empty() && ! plainLines.isEmpty())
    {
        const double gap = juce::jlimit (0.5, 30.0, fallbackInterval);
        for (int i = 0; i < plainLines.size(); ++i)
        {
            LyricLine l;
            l.time = static_cast<double> (i) * gap;
            l.text = plainLines[i];
            lines.push_back (std::move (l));
        }

        lastInfo = meta;
        finish ("TXT", false);
        lastInfo.warning = Lang::tr (Lang::TextId::warnNoTimestamps1, language)
                         + juce::String (gap, 1)
                         + Lang::tr (Lang::TextId::warnNoTimestamps2, language);
        lastInfo.plainTextFallback = true;
        return lastInfo;
    }

    //--------------------------------------------------------------------------
    //  排序、双语合并、去重
    //--------------------------------------------------------------------------
    std::stable_sort (lines.begin(), lines.end(),
                      [] (const LyricLine& a, const LyricLine& b) { return a.time < b.time; });

    // 同一时间戳连续两行视为「原文 + 译文」
    // （原文可以是逐字行：KRC 的翻译就是挂在逐字行下面的，所以不排除 hasKaraoke）
    for (size_t i = 1; i < lines.size();)
    {
        auto& prev = lines[i - 1];
        auto& cur  = lines[i];

        if (std::abs (cur.time - prev.time) < 0.004
            && prev.translation.isEmpty()
            && ! cur.hasKaraoke()
            && cur.text != prev.text)
        {
            prev.translation = cur.text;
            lines.erase (lines.begin() + static_cast<std::ptrdiff_t> (i));
        }
        else
        {
            ++i;
        }
    }

    // 去掉完全重复的行
    lines.erase (std::unique (lines.begin(), lines.end(),
                              [] (const LyricLine& a, const LyricLine& b)
                              {
                                  return std::abs (a.time - b.time) < 0.001 && a.text == b.text;
                              }),
                 lines.end());

    lastInfo = meta;
    juce::String format = karaokeCount > 0 ? "Enhanced LRC" : "LRC";
    juce::String warning;

    if (! lines.empty() && lines.front().time > 5.0)
        warning = Lang::tr (Lang::TextId::warnFirstLine1, language)
                + juce::String (lines.front().time, 1)
                + Lang::tr (Lang::TextId::warnFirstLine2, language);

    finish (format, false, warning);
    return lastInfo;
}

//==============================================================================
int LyricsDocument::findIndexAt (double t) const noexcept
{
    if (lines.empty())
        return -1;

    auto it = std::upper_bound (lines.begin(), lines.end(), t,
                                [] (double value, const LyricLine& l) { return value < l.time; });

    if (it == lines.begin())
        return -1;

    return static_cast<int> (std::distance (lines.begin(), it)) - 1;
}

double LyricsDocument::lineEnd (int index, double defaultHold) const noexcept
{
    if (index < 0 || index >= size())
        return 0.0;

    if (index + 1 < size())
    {
        auto next = lines[static_cast<size_t> (index + 1)].time;
        if (next > lines[static_cast<size_t> (index)].time + 0.01)
            return next;
    }

    return lines[static_cast<size_t> (index)].time + defaultHold;
}

double LyricsDocument::totalDuration() const noexcept
{
    if (lines.empty())
        return 60.0;

    return juce::jmax (10.0, lineEnd (size() - 1));
}

int LyricsDocument::sungCharsAt (const LyricLine& l, double t) const noexcept
{
    if (l.charOffsets.empty())
        return -1;

    const double rel = t - l.time;
    int count = 0;

    for (auto off : l.charOffsets)
    {
        if (rel >= off)
            ++count;
        else
            break;
    }

    return juce::jlimit (0, l.text.length(), count);
}

double LyricsDocument::sungProgressAt (const LyricLine& l, double t) const noexcept
{
    if (l.charOffsets.empty() || l.text.isEmpty())
        return -1.0;

    const double rel = t - l.time;
    const int n = static_cast<int> (l.charOffsets.size());

    int count = 0;

    for (auto off : l.charOffsets)
    {
        if (rel >= off)
            ++count;
        else
            break;
    }

    if (count <= 0)
        return rel < 0.0 ? -1.0 : 0.0;

    const int idx = count - 1;                       // 正在唱的那个字
    const double start = l.charOffsets[static_cast<size_t> (idx)];
    // 最后一个字没有"下一个起点"，用 0.4 s 兜底（大多数歌曲单字在 0.2–0.5 s）
    const double end = (idx + 1 < n) ? l.charOffsets[static_cast<size_t> (idx + 1)]
                                     : start + 0.4;

    const double span = juce::jmax (0.02, end - start);
    const double frac = juce::jlimit (0.0, 1.0, (rel - start) / span);

    return static_cast<double> (idx) + frac;
}

