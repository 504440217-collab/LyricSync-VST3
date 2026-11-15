#include "OnlineLyrics.h"
#include "KrcLyrics.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <utility>

namespace OnlineLyrics
{
namespace
{
    const char* const kKugouHeaders =
        "User-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64)\r\n"
        "Referer: https://www.kugou.com/\r\n";

    const char* const kLrclibHeaders =
        "User-Agent: LyricSync/1.9.0 (local VST3 lyrics plugin)\r\n"
        "Accept: application/json\r\n";

    /** 自己按 UTF-8 字节做 percent-encode，避免 JUCE addEscapeChars 的版本差异。 */
    juce::String urlEncode (const juce::String& s)
    {
        static const char* hex = "0123456789ABCDEF";
        std::string out;

        const auto* bytes = s.toRawUTF8();
        const int n = static_cast<int> (s.getNumBytesAsUTF8());

        for (int i = 0; i < n; ++i)
        {
            const unsigned char c = static_cast<unsigned char> (bytes[i]);

            if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')
                || (c >= '0' && c <= '9')
                || c == '-' || c == '_' || c == '.' || c == '~')
            {
                out += static_cast<char> (c);
            }
            else
            {
                out += '%';
                out += hex[c >> 4];
                out += hex[c & 15];
            }
        }

        return juce::String::fromUTF8 (out.c_str(), static_cast<int> (out.size()));
    }

    juce::String httpGet (const juce::String& url, const juce::String& headers, int& status)
    {
        status = 0;

        const auto options =
            juce::URL::InputStreamOptions (juce::URL::ParameterHandling::inAddress)
                .withConnectionTimeoutMs (7000)
                .withNumRedirectsToFollow (3)
                .withExtraHeaders (headers)
                .withStatusCode (&status);

        auto stream = juce::URL (url).createInputStream (options);

        if (stream == nullptr)
            return {};

        return stream->readEntireStreamAsString();
    }

    juce::var field (const juce::var& obj, const char* name)
    {
        if (auto* o = obj.getDynamicObject())
            return o->getProperty (name);

        return {};
    }

    juce::String str (const juce::var& v)
    {
        return v.isString() ? v.toString() : juce::String();
    }

    juce::String str (const juce::var& obj, const char* name)
    {
        return str (field (obj, name));
    }

    int intField (const juce::var& obj, const char* name)
    {
        return field (obj, name).toString().getIntValue();
    }

    juce::String shortPreview (const juce::String& text)
    {
        juce::StringArray parts;

        for (const auto& raw : juce::StringArray::fromLines (text))
        {
            auto line = raw.trim();

            // 去掉 [mm:ss.xx] / [ti:xxx] 这类头部，只留正文
            while (line.startsWith ("["))
            {
                const int close = line.indexOfChar (']');
                if (close < 0)
                    break;
                line = line.substring (close + 1).trim();
            }

            if (line.isEmpty())
                continue;

            parts.add (line);

            if (parts.size() >= 2)
                break;
        }

        return parts.joinIntoString ("  /  ");
    }

    std::vector<Result> parseKugou (const juce::String& body)
    {
        std::vector<Result> out;
        const auto root = juce::JSON::parse (body);
        const auto candidates = field (root, "candidates");
        const auto* arr = candidates.getArray();

        if (arr == nullptr)
            return out;

        for (const auto& v : *arr)
        {
            Result r;
            r.id        = str (v, "id");
            r.accessKey = str (v, "accesskey");
            r.title     = str (v, "song");
            r.artist    = str (v, "singer");
            r.album     = str (v, "album");
            r.durationSec = intField (v, "duration") / 1000;   // 酷狗给的是毫秒
            r.source    = "Kugou";
            r.wordSync  = true;                                  // 优先尝试 KRC 逐字

            if (r.id.isEmpty() || r.accessKey.isEmpty())
                continue;

            if (r.title.isEmpty())
                r.title = str (root, "keyword");

            out.push_back (r);
        }

        return out;
    }

    std::vector<Result> parseLrclib (const juce::String& body, const char* source)
    {
        std::vector<Result> out;
        const auto root = juce::JSON::parse (body);
        const auto* arr = root.getArray();

        if (arr == nullptr)
            return out;

        for (const auto& v : *arr)
        {
            Result r;
            r.title   = str (v, "trackName");
            if (r.title.isEmpty()) r.title = str (v, "name");
            r.artist  = str (v, "artistName");
            r.album   = str (v, "albumName");
            r.durationSec = static_cast<int> (field (v, "duration").toString().getDoubleValue());
            r.source  = source;
            r.lrc     = str (v, "syncedLyrics");
            r.wordSync = false;

            if (r.lrc.isEmpty())
                r.lrc = str (v, "plainLyrics");

            if (r.lrc.trim().isEmpty())      // 纯音乐 / 没有歌词
                continue;

            r.preview = shortPreview (r.lrc);
            out.push_back (r);
        }

        return out;
    }

    /** 自定义服务：纯 LRC 文本、JSON 数组（LRCLIB 形状）、或带 content 的 JSON 对象。 */
    std::vector<Result> parseCustom (const juce::String& body, const juce::String& title)
    {
        std::vector<Result> out;
        const auto t = body.trim();

        if (t.isEmpty())
            return out;

        if (t.startsWith ("[") && ! t.startsWith ("[{") && t.contains ("]"))
        {
            Result r;
            r.title  = title;
            r.source = "Custom";
            r.lrc    = body;
            r.preview = shortPreview (body);
            out.push_back (r);
            return out;
        }

        const auto root = juce::JSON::parse (t);

        if (root.getArray() != nullptr)
            return parseLrclib (body, "Custom");

        if (root.getDynamicObject() != nullptr)
        {
            juce::String text = str (root, "syncedLyrics");
            if (text.isEmpty()) text = str (root, "content");
            if (text.isEmpty()) text = str (root, "lrc");
            if (text.isEmpty()) return out;

            // content 有可能是 base64 文本
            if (! text.contains ("[") && ! text.contains ("\n"))
            {
                juce::MemoryOutputStream mo;
                if (juce::Base64::convertFromBase64 (mo, text) && mo.getDataSize() > 0)
                    text = juce::String::fromUTF8 (static_cast<const char*> (mo.getData()),
                                                  static_cast<int> (mo.getDataSize()));
            }

            Result r;
            r.title   = str (root, "trackName");
            if (r.title.isEmpty()) r.title = title;
            r.artist  = str (root, "artistName");
            r.source  = "Custom";
            r.lrc     = text;
            r.preview = shortPreview (text);
            out.push_back (r);
            return out;
        }

        // 既不是 JSON 也不是 LRC：当成纯文本歌词（解析器会给它默认间隔）
        Result r;
        r.title   = title;
        r.source  = "Custom";
        r.lrc     = body;
        r.preview = shortPreview (body);
        out.push_back (r);
        return out;
    }

    juce::String expandTemplate (juce::String tpl, const juce::String& title,
                                 const juce::String& artist, int durationSec)
    {
        tpl = tpl.trim();

        if (! tpl.contains ("{q}"))
            tpl += (tpl.contains ("?") ? "&q=" : "?q=") + juce::String ("{q}");

        return tpl.replace ("{q}", urlEncode (title))
                  .replace ("{artist}", urlEncode (artist))
                  .replace ("{duration}", juce::String (durationSec));
    }

    void sortByDuration (std::vector<Result>& results, int targetSec)
    {
        if (targetSec <= 0)
            return;

        std::stable_sort (results.begin(), results.end(),
            [targetSec] (const Result& a, const Result& b)
            {
                const int da = a.durationSec > 0 ? std::abs (a.durationSec - targetSec) : 999999;
                const int db = b.durationSec > 0 ? std::abs (b.durationSec - targetSec) : 999999;
                return da < db;
            });
    }
}

//==============================================================================
Outcome search (juce::String title, juce::String artist,
                int durationSec, juce::String customUrl)
{
    Outcome outcome;
    title  = title.trim();
    artist = artist.trim();

    if (title.isEmpty())
    {
        outcome.error = "empty";
        return outcome;
    }

    //---- 1. 自定义服务（填了就优先，失败再自动回退） -------------------------
    if (customUrl.trim().isNotEmpty())
    {
        int status = 0;
        const auto body = httpGet (expandTemplate (customUrl, title, artist, durationSec),
                                   kLrclibHeaders, status);

        if (status == 200 && body.isNotEmpty())
        {
            auto found = parseCustom (body, title);

            if (! found.empty())
            {
                outcome.results = std::move (found);
                outcome.via = "Custom";
                return outcome;
            }
        }
    }

    //---- 2. 酷狗直连 ---------------------------------------------------------
    juce::StringArray keywords;

    if (artist.isNotEmpty())
        keywords.add (title + " " + artist);

    keywords.add (title);

    for (const auto& kw : keywords)
    {
        int status = 0;
        const auto body = httpGet (
            "https://lyrics.kugou.com/search?ver=1&man=yes&client=pc&keyword="
            + urlEncode (kw), kKugouHeaders, status);

        if (status != 200 || body.isEmpty())
            continue;

        auto found = parseKugou (body);

        if (found.empty())
            continue;

        sortByDuration (found, durationSec);

        if (found.size() > 20)
            found.resize (20);

        outcome.results = std::move (found);
        outcome.via = "Kugou";
        return outcome;
    }

    //---- 3. LRCLIB 兜底 ------------------------------------------------------
    {
        const auto q = artist.isNotEmpty() ? title + " " + artist : title;
        int status = 0;
        const auto body = httpGet ("https://lrclib.net/api/search?q=" + urlEncode (q),
                                   kLrclibHeaders, status);

        if (status == 200 && body.isNotEmpty())
        {
            auto found = parseLrclib (body, "LRCLIB");

            if (! found.empty())
            {
                if (found.size() > 20)
                    found.resize (20);

                outcome.results = std::move (found);
                outcome.via = "LRCLIB";
                return outcome;
            }
        }
    }

    outcome.error = "notfound";
    return outcome;
}

juce::String download (const Result& result)
{
    if (result.lrc.isNotEmpty())
        return result.lrc;

    if (result.id.isEmpty() || result.accessKey.isEmpty())
        return {};

    const juce::String base = "https://lyrics.kugou.com/download?ver=1&client=pc&id="
                            + urlEncode (result.id)
                            + "&accesskey=" + urlEncode (result.accessKey)
                            + "&charset=utf8&fmt=";

    // 先要逐字 KRC，拿不到再退普通 LRC
    for (const char* fmt : { "krc", "lrc" })
    {
        int status = 0;
        const auto body = httpGet (base + fmt, kKugouHeaders, status);

        if (status != 200 || body.isEmpty())
            continue;

        const auto root = juce::JSON::parse (body);
        const auto b64 = str (root, "content");

        if (b64.isEmpty())
            continue;

        juce::MemoryOutputStream decoded;

        if (! juce::Base64::convertFromBase64 (decoded, b64) || decoded.getDataSize() == 0)
            continue;

        juce::MemoryBlock raw (decoded.getData(), decoded.getDataSize());

        if (juce::String (fmt) == "krc")
        {
            if (! Krc::isKrcData (raw.getData(), static_cast<int> (raw.getSize())))
                continue;

            const auto plain = Krc::decrypt (raw);
            const auto lrc = Krc::toEnhancedLrc (plain);

            if (lrc.isNotEmpty())
                return lrc;
        }
        else
        {
            const auto text = juce::String::fromUTF8 (
                static_cast<const char*> (raw.getData()),
                static_cast<int> (raw.getSize()));

            if (text.isNotEmpty())
                return text;
        }
    }

    return {};
}
}
