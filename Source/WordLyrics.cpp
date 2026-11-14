#include "WordLyrics.h"

#include <cstring>

#if JUCE_WINDOWS
 #include <windows.h>
#endif

namespace
{
    bool isCjkChar (juce::juce_wchar c)
    {
        return (c >= 0x2e80 && c <= 0x9fff)      // CJK 部首 / 汉字
            || (c >= 0xf900 && c <= 0xfaff)      // 兼容汉字
            || (c >= 0xff00 && c <= 0xffef)      // 全角标点
            || (c >= 0x3000 && c <= 0x303f);     // 中文标点
    }

    //==========================================================================
    //  docx
    //==========================================================================
    void collectParagraphs (const juce::XmlElement& el, juce::String& out)
    {
        const auto& tag = el.getTagName();

        if (tag == "w:p")
            out << "\n";
        else if (tag == "w:br")
            out << "\n";
        else if (tag == "w:tab")
            out << "  ";
        else if (tag == "w:t")
            out << el.getAllSubText();

        for (auto* child : el.getChildIterator())
            collectParagraphs (*child, out);
    }

    juce::String extractFromDocx (const juce::File& file)
    {
        juce::ZipFile zip (file);

        auto* entry = zip.getEntry ("word/document.xml");

        if (entry == nullptr)
            return {};

        std::unique_ptr<juce::InputStream> stream (zip.createStreamForEntry (*entry));

        if (stream == nullptr)
            return {};

        const auto xmlText = stream->readEntireStreamAsString();
        auto xml = juce::parseXML (xmlText);

        if (xml == nullptr)
            return {};

        juce::String out;
        collectParagraphs (*xml, out);

        // 段落里可能残留连续空行，收一下
        juce::StringArray lines;

        for (const auto& line : juce::StringArray::fromLines (out))
        {
            auto t = line.trim();

            if (t.isNotEmpty())
                lines.add (t);
        }

        return lines.joinIntoString ("\n");
    }

    //==========================================================================
    //  RTF（很多"另存为 .doc"其实是 RTF）
    //==========================================================================
    juce::String stripRtf (const juce::String& rtf)
    {
        juce::MemoryOutputStream bytes;
        const int n = rtf.length();

        for (int i = 0; i < n;)
        {
            const auto c = rtf[i];

            if (c == '{' || c == '}')
            {
                ++i;
                continue;
            }

            if (c != '\\')
            {
                if (c == '\r' || c == '\n')
                {
                    ++i;
                    continue;
                }

                bytes.writeByte (static_cast<char> (c));
                ++i;
                continue;
            }

            // 反斜杠：控制字或转义
            if (i + 1 >= n)
                break;

            const auto next = rtf[i + 1];

            if (next == '\\' || next == '{' || next == '}')
            {
                bytes.writeByte (static_cast<char> (next));
                i += 2;
                continue;
            }

            if (next == '\'')           // \'hh —— 一字节（通常是 GBK）
            {
                if (i + 3 < n)
                {
                    const auto hex = rtf.substring (i + 2, i + 4);
                    bytes.writeByte (static_cast<char> (hex.getHexValue32()));
                    i += 4;
                    continue;
                }

                break;
            }

            if (next == '*')            // {\* ...} 忽略整组
            {
                int depth = 1;
                i += 2;

                while (i < n && depth > 0)
                {
                    if (rtf[i] == '{') ++depth;
                    else if (rtf[i] == '}') --depth;
                    ++i;
                }

                continue;
            }

            // 控制字
            int j = i + 1;

            while (j < n && juce::CharacterFunctions::isLetter (rtf[j]))
                ++j;

            const auto word = rtf.substring (i + 1, j);

            while (j < n && (juce::CharacterFunctions::isDigit (rtf[j]) || rtf[j] == '-'))
                ++j;

            if (word == "par" || word == "line" || word == "row")
                bytes.writeByte ('\n');
            else if (word == "tab")
                bytes.writeByte ('\t');

            if (j < n && rtf[j] == ' ')
                ++j;

            i = j;
        }

        auto raw = bytes.getMemoryBlock();
        const auto* text = static_cast<const char*> (raw.getData());
        const int size = static_cast<int> (raw.getSize());

        if (size <= 0)
            return {};

        if (juce::CharPointer_UTF8::isValidString (text, size))
            return juce::String::fromUTF8 (text, size);

       #if JUCE_WINDOWS
        const int needed = ::MultiByteToWideChar (936, 0, text, size, nullptr, 0);

        if (needed > 0)
        {
            std::wstring wide (static_cast<size_t> (needed), L'\0');
            ::MultiByteToWideChar (936, 0, text, size, wide.data(), needed);
            return juce::String (wide.c_str());
        }
       #endif

        return juce::String::fromUTF8 (text, size);
    }

    //==========================================================================
    //  二进制 .doc：扫"可打印 run"
    //==========================================================================
    juce::String runsFromWide (const juce::String& text, int& cjkCount)
    {
        juce::StringArray lines;
        juce::String run;
        cjkCount = 0;

        for (auto c : text)
        {
            const bool printable = (c >= 0x20 && c <= 0x7e) || isCjkChar (c);

            if (printable)
            {
                if (isCjkChar (c))
                    ++cjkCount;

                run << c;
            }
            else
            {
                if (run.trim().length() >= 4)
                    lines.add (run.trim());

                run.clear();
            }
        }

        if (run.trim().length() >= 4)
            lines.add (run.trim());

        return lines.joinIntoString ("\n");
    }

    juce::String extractFromBinaryDoc (const juce::MemoryBlock& data)
    {
        const auto* bytes = static_cast<const unsigned char*> (data.getData());
        const int size = static_cast<int> (data.getSize());

        if (size < 512)
            return {};

        // 尝试 1：按 UTF-16LE 解
        juce::String wide;
        wide.preallocateBytes (static_cast<size_t> (size));

        for (int i = 0; i + 1 < size; i += 2)
        {
            const auto lo = static_cast<juce::juce_wchar> (bytes[i]);
            const auto hi = static_cast<juce::juce_wchar> (bytes[i + 1]);
            wide << static_cast<juce::juce_wchar> (lo | (hi << 8));
        }

        int cjkWide = 0;
        const auto wideText = runsFromWide (wide, cjkWide);

        // 尝试 2：按 GBK 解
       #if JUCE_WINDOWS
        const int needed = ::MultiByteToWideChar (936, 0, reinterpret_cast<const char*> (bytes),
                                                  size, nullptr, 0);

        if (needed > 0)
        {
            std::wstring w (static_cast<size_t> (needed), L'\0');
            ::MultiByteToWideChar (936, 0, reinterpret_cast<const char*> (bytes), size,
                                   w.data(), needed);

            int cjkGbk = 0;
            const auto gbkText = runsFromWide (juce::String (w.c_str()), cjkGbk);

            if (cjkGbk > cjkWide)
                return gbkText;
        }
       #endif

        return wideText;
    }
}

//==============================================================================
namespace WordDoc
{
    bool looksLikeWordFile (const juce::String& path)
    {
        const auto p = path.toLowerCase();
        return p.endsWith (".doc") || p.endsWith (".docx");
    }

    juce::String extractText (const juce::File& file, const juce::MemoryBlock& data)
    {
        const auto ext = file.getFileExtension().toLowerCase();

        if (ext == ".docx" || ext == ".docx/")
        {
            auto text = extractFromDocx (file);

            if (text.isNotEmpty())
                return text;
        }

        // 二进制/RTF 走内存判断（.doc 也可能是 RTF 或 docx 改名）
        const auto* bytes = static_cast<const char*> (data.getData());
        const int size = static_cast<int> (data.getSize());

        if (size >= 8 && std::memcmp (bytes, "PK\x03\x04", 4) == 0)
        {
            auto text = extractFromDocx (file);

            if (text.isNotEmpty())
                return text;
        }

        if (size >= 5 && juce::String::fromUTF8 (bytes, juce::jmin (size, 5)) == "{\\rtf")
            return stripRtf (juce::String::fromUTF8 (bytes, size));

        return extractFromBinaryDoc (data);
    }
}
