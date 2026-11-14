#include "KrcLyrics.h"

#include <cmath>
#include <cstring>
#include <utility>
#include <vector>

namespace
{
    // 酷狗固定的 16 字节循环异或密钥（与文件头的 4 字节无关）。
    const unsigned char kKrcKey[16] =
    {
        0x40, 0x47, 0x61, 0x77, 0x5e, 0x32, 0x74, 0x47,
        0x51, 0x36, 0x31, 0x2d, 0xce, 0xd2, 0x6e, 0x69
    };

    /** mm:ss.xx，正好是插件解析器能识别的 <...> 锚点格式。 */
    juce::String stampFor (double seconds)
    {
        seconds = juce::jmax (0.0, seconds);
        const int mins = static_cast<int> (seconds / 60.0);
        const double secs = seconds - static_cast<double> (mins) * 60.0;
        return juce::String::formatted ("%02d:%05.2f", mins, secs);
    }

    struct TranslationLine
    {
        double time = 0.0;
        juce::String text;
    };

    /**
     *  [language:BASE64] 里装着 JSON：
     *      {"content":[{"type":1,"lyricContent":[[startMs,durMs,"译文"],...]}],"version":1}
     *  type 1 = 翻译。解析失败就静默跳过（不能让翻译影响主歌词）。
     */
    void parseLanguageTag (const juce::String& line, std::vector<TranslationLine>& out)
    {
        const int colon = line.indexOfChar (0, ':');
        const int close = line.lastIndexOfChar (']');
        if (colon < 0 || close <= colon)
            return;

        const auto b64 = line.substring (colon + 1, close).trim();
        if (b64.isEmpty())
            return;

        juce::MemoryOutputStream decoded;
        if (! juce::Base64::convertFromBase64 (decoded, b64))
            return;

        const int decodedSize = static_cast<int> (decoded.getDataSize());
        if (decodedSize <= 0)
            return;

        const auto json = juce::String::fromUTF8 (static_cast<const char*> (decoded.getData()),
                                                  decodedSize);

        auto root = juce::JSON::parse (json);
        auto* rootObj = root.getDynamicObject();
        if (rootObj == nullptr)
            return;

        auto* items = rootObj->getProperty ("content").getArray();
        if (items == nullptr)
            return;

        for (const auto& item : *items)
        {
            auto* itemObj = item.getDynamicObject();
            if (itemObj == nullptr)
                continue;

            if (static_cast<int> (itemObj->getProperty ("type")) != 1)
                continue;

            auto* lines = itemObj->getProperty ("lyricContent").getArray();
            if (lines == nullptr)
                continue;

            for (const auto& entry : *lines)
            {
                auto* fields = entry.getArray();
                if (fields == nullptr || fields->size() < 3)
                    continue;

                TranslationLine tl;
                tl.time = static_cast<double> ((*fields)[0]) / 1000.0;
                tl.text = (*fields)[2].toString().trim();

                if (tl.text.isNotEmpty() && std::isfinite (tl.time))
                    out.push_back (std::move (tl));
            }
        }
    }

    /** 取 [total:...] / [offset:...] 里的毫秒值。 */
    double tagMilliseconds (const juce::String& plainText, const juce::String& tag)
    {
        for (const auto& raw : juce::StringArray::fromLines (plainText))
        {
            const auto line = raw.trim();

            if (! line.startsWith (tag))
                continue;

            const int close = line.indexOfChar (0, ']');
            if (close < 0)
                continue;

            const auto value = line.substring (tag.length(), close).trim().getDoubleValue();

            if (std::isfinite (value))
                return value;
        }

        return -1.0;
    }
}

//==============================================================================
namespace Krc
{
    bool isKrcData (const void* data, int sizeInBytes)
    {
        return data != nullptr && sizeInBytes >= 8
            && std::memcmp (data, "krc1", 4) == 0;
    }

    juce::String decrypt (const juce::MemoryBlock& raw)
    {
        const auto* bytes = static_cast<const unsigned char*> (raw.getData());
        const int size = static_cast<int> (raw.getSize());

        if (! isKrcData (bytes, size))
            return {};

        // 跳过 4 字节 "krc1"，剩下的按 16 字节密钥循环异或
        juce::MemoryBlock cipher (static_cast<size_t> (size - 4));
        auto* dst = static_cast<unsigned char*> (cipher.getData());

        for (int i = 4; i < size; ++i)
            dst[i - 4] = static_cast<unsigned char> (bytes[i] ^ kKrcKey[(i - 4) % 16]);

        // 明文是标准 zlib 流（78 9c ...），JUCE 自带解压器可直接吃。
        juce::MemoryInputStream compressed (cipher, false);
        juce::GZIPDecompressorInputStream inflater (
            &compressed, false, juce::GZIPDecompressorInputStream::zlibFormat);

        return inflater.readEntireStreamAsString();
    }

    juce::String toEnhancedLrc (const juce::String& plainText)
    {
        auto text = plainText;

        if (text.startsWithChar (juce::juce_wchar (0xfeff)))
            text = text.substring (1);

        std::vector<std::pair<double, juce::String>> lines;
        std::vector<TranslationLine> translations;

        for (const auto& rawLine : juce::StringArray::fromLines (text))
        {
            const auto line = rawLine.trim();

            if (line.length() < 4 || ! line.startsWithChar ('['))
                continue;

            const int close = line.indexOfChar (0, ']');
            if (close <= 1)
                continue;

            const auto head = line.substring (1, close);

            if (head.containsChar (':'))
            {
                // 元信息：[ti:..] [ar:..] [total:..] [language:..] ...
                if (head.startsWithIgnoreCase ("language"))
                    parseLanguageTag (line, translations);

                continue;
            }

            // [行起始毫秒,行持续毫秒]
            const auto timing = juce::StringArray::fromTokens (head, ",", "");
            if (timing.isEmpty())
                continue;

            const double startMs = timing[0].getDoubleValue();
            if (! std::isfinite (startMs) || startMs < 0.0)
                continue;

            const auto body = line.substring (close + 1);
            juce::String out;

            for (int i = 0; i < body.length();)
            {
                if (body[i] == '<')
                {
                    const int end = body.indexOfChar (i, '>');

                    if (end > i)
                    {
                        const auto inner = body.substring (i + 1, end);
                        const auto fields = juce::StringArray::fromTokens (inner, ",", "");

                        if (! fields.isEmpty())
                        {
                            const double offsetMs = fields[0].getDoubleValue();

                            if (std::isfinite (offsetMs))
                                out << "<" << stampFor ((startMs + offsetMs) / 1000.0) << ">";
                        }

                        i = end + 1;
                        continue;
                    }
                }

                out << body[i];
                ++i;
            }

            out = out.trim();

            if (out.isNotEmpty())
                lines.emplace_back (startMs / 1000.0, out);
        }

        if (lines.empty())
            return {};

        // 翻译行插在对应原文行之后：同一时间戳，解析器会合并成双语。
        for (const auto& tr : translations)
        {
            int best = -1;
            double bestDelta = 0.55;

            for (int i = 0; i < static_cast<int> (lines.size()); ++i)
            {
                const double delta = std::abs (lines[static_cast<size_t> (i)].first - tr.time);

                if (delta < bestDelta)
                {
                    bestDelta = delta;
                    best = i;
                }
            }

            if (best >= 0)
            {
                const double at = lines[static_cast<size_t> (best)].first;
                lines.insert (lines.begin() + best + 1, std::make_pair (at, tr.text));
            }
        }

        juce::StringArray out;

        for (const auto& line : lines)
            out.add ("[" + stampFor (line.first) + "]" + line.second);

        return out.joinIntoString ("\n");
    }

    double totalSecondsFromPlain (const juce::String& plainText)
    {
        const double ms = tagMilliseconds (plainText, "[total:");

        return ms > 1000.0 ? ms / 1000.0 : -1.0;
    }

    double offsetMsFromPlain (const juce::String& plainText)
    {
        const double ms = tagMilliseconds (plainText, "[offset:");

        return ms > -1.0 ? ms : 0.0;
    }
}
