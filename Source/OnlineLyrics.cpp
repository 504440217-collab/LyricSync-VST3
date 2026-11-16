#include "OnlineLyrics.h"
#include "KrcLyrics.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <string>
#include <utility>

#if JUCE_WINDOWS
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #include <windows.h>
 #include <winhttp.h>
#endif

namespace OnlineLyrics
{
namespace
{
    // 0 = 直连（WinHTTP NO_PROXY），1 = 跟随系统代理。GUI 线程写，后台线程读。
    std::atomic<int> gUseProxy { 0 };

    const char* const kKugouHeaders =
        "User-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64)\r\n"
        "Referer: https://www.kugou.com/\r\n";

    const char* const kLrclibHeaders =
        "User-Agent: LyricSync/1.9.4 (local VST3 lyrics plugin)\r\n"
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

    juce::String stepDiag (const char* tag, int status, int err)
    {
        juce::String s (tag);
        s << status;

        if (err != 0)
            s << "e" << err;

        return s;
    }

#if JUCE_WINDOWS
    struct HttpTarget
    {
        bool secure = false;
        int  port = 80;
        juce::String host, path;

        /** 自己拆 http(s) URL：不让 JUCE 的 URL 再编码一次已经 %XX 化的查询串。 */
        bool parse (const juce::String& url)
        {
            auto s = url.trim();

            if (s.startsWithIgnoreCase ("https://"))      { secure = true;  s = s.substring (8); port = 443; }
            else if (s.startsWithIgnoreCase ("http://"))  { secure = false; s = s.substring (7); port = 80; }
            else return false;

            const int slash = s.indexOfChar ('/');
            auto authority = slash >= 0 ? s.substring (0, slash) : s;
            path = slash >= 0 ? s.substring (slash) : juce::String ("/");

            const int colon = authority.indexOfChar (':');

            if (colon >= 0)
            {
                port = authority.substring (colon + 1).getIntValue();
                authority = authority.substring (0, colon);
            }

            host = authority;
            return host.isNotEmpty() && port > 0;
        }
    };

    /**
     *  WinHTTP 直连实现。
     *
     *  为什么不用 juce::URL：它走 WinINet 的 PRECONFIG，会继承系统/IE 的代理与 PAC 设置。
     *  机器上装过代理软件、残留了 127.0.0.1:10808 之类的配置后，插件进程里
     *  所有请求都会静默失败（HTTP 状态一直是 0，看不出原因）。这里显式 NO_PROXY
     *  直连，并把 WinHTTP 的错误码带出来（12007 解析失败 / 12029 连不上 / 时间超时等）。
     */
    juce::String winHttpGet (const juce::String& url, const juce::String& headers,
                             int& status, int& err, int timeoutMs, bool useProxy)
    {
        status = 0;
        err = 0;

        HttpTarget target;

        if (! target.parse (url))
        {
            err = -1;
            return {};
        }

        HINTERNET session = WinHttpOpen (L"LyricSync/1.9.5 (VST3 lyrics plugin)",
                                         useProxy ? WINHTTP_ACCESS_TYPE_DEFAULT_PROXY
                                                  : WINHTTP_ACCESS_TYPE_NO_PROXY,
                                         WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);

        if (session == nullptr)
        {
            err = static_cast<int> (GetLastError());
            return {};
        }

        WinHttpSetTimeouts (session, timeoutMs, timeoutMs, timeoutMs, timeoutMs);

        {
            DWORD protocols = 0x00000800 | 0x00002000;      // TLS 1.2 + TLS 1.3
            WinHttpSetOption (session, WINHTTP_OPTION_SECURE_PROTOCOLS,
                              &protocols, sizeof (protocols));
        }

        std::string bytes;

        HINTERNET connect = WinHttpConnect (session, target.host.toWideCharPointer(),
                                            static_cast<INTERNET_PORT> (target.port), 0);

        if (connect != nullptr)
        {
            HINTERNET request = WinHttpOpenRequest (connect, L"GET",
                                                    target.path.toWideCharPointer(),
                                                    nullptr, WINHTTP_NO_REFERER,
                                                    WINHTTP_DEFAULT_ACCEPT_TYPES,
                                                    target.secure ? WINHTTP_FLAG_SECURE : 0);

            if (request != nullptr)
            {
                if (headers.isNotEmpty())
                    WinHttpAddRequestHeaders (request, headers.toWideCharPointer(),
                                              static_cast<DWORD> (-1),
                                              WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE);

                if (WinHttpSendRequest (request, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                        WINHTTP_NO_REQUEST_DATA, 0, 0, 0)
                    && WinHttpReceiveResponse (request, nullptr))
                {
                    DWORD code = 0, size = sizeof (code);

                    if (WinHttpQueryHeaders (request,
                                             WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                             WINHTTP_HEADER_NAME_BY_INDEX, &code, &size,
                                             WINHTTP_NO_HEADER_INDEX))
                        status = static_cast<int> (code);

                    for (;;)
                    {
                        DWORD avail = 0;

                        if (! WinHttpQueryDataAvailable (request, &avail) || avail == 0)
                            break;

                        std::vector<char> buf (static_cast<size_t> (avail));
                        DWORD got = 0;

                        if (! WinHttpReadData (request, buf.data(), avail, &got) || got == 0)
                            break;

                        // 先把字节攒起来，最后统一解码：分段 fromUTF8 会切断多字节汉字
                        bytes.append (buf.data(), static_cast<size_t> (got));
                    }
                }
                else
                {
                    err = static_cast<int> (GetLastError());
                }

                WinHttpCloseHandle (request);
            }
            else
            {
                err = static_cast<int> (GetLastError());
            }

            WinHttpCloseHandle (connect);
        }
        else
        {
            err = static_cast<int> (GetLastError());
        }

        WinHttpCloseHandle (session);

        if (bytes.empty())
            return {};

        return juce::String::fromUTF8 (bytes.data(), static_cast<int> (bytes.size()));
    }
#endif

    /** WinHTTP 直连优先；失败再回落到 juce::URL（会走系统代理，适合公司代理环境）。 */
    juce::String httpGet (const juce::String& url, const juce::String& headers,
                          int& status, int timeoutMs = 7000, int* errorCode = nullptr)
    {
        status = 0;

        if (errorCode != nullptr)
            *errorCode = 0;

       #if JUCE_WINDOWS
        // 只有「直连」模式用 WinHTTP。WinHTTP 的 DEFAULT_PROXY 读的是
        // netsh winhttp 的配置，而不是 v2rayN / Clash 设置的 IE 系统代理，
        // 所以「跟随系统代理」必须交给下面的 WinINet（juce::URL）。
        if (gUseProxy.load() == 0)
        {
            int winErr = 0;
            auto viaWinHttp = winHttpGet (url, headers, status, winErr, timeoutMs, false);

            if (status > 0)
            {
                // HTTP 层已经通：即使 4xx/5xx 也把内容交给调用方判断，不重复请求
                return viaWinHttp;
            }

            if (errorCode != nullptr)
                *errorCode = winErr;

            status = 0;
        }
       #endif

        const auto options =
            juce::URL::InputStreamOptions (juce::URL::ParameterHandling::inAddress)
                .withConnectionTimeoutMs (timeoutMs)
                .withNumRedirectsToFollow (3)
                .withExtraHeaders (headers)
                .withStatusCode (&status);

        auto stream = juce::URL (url).createInputStream (options);

        if (stream == nullptr)
            return {};

        return stream->readEntireStreamAsString();
    }

    bool usable (const juce::String& body, int status)
    {
        // 有些节点会返回 206 之类，2xx 都当成功；status 0 表示平台没填，也放行
        return body.isNotEmpty() && (status == 0 || (status >= 200 && status < 300));
    }

    juce::String diag (const char* tag, int status, int items)
    {
        return juce::String (tag) + "=" + juce::String (status) + "/" + juce::String (items) + " ";
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

    juce::String normalise (const juce::String& s)
    {
        // 全角标点用 UTF-8 字节转义构造：源码保持纯 ASCII，避免 MSVC 代码页乱码
        const auto cjkPunct = juce::String::fromUTF8 (
            "\xef\xbc\x88\xef\xbc\x89\xe3\x80\x90\xe3\x80\x91"
            "\xe3\x80\x81\xc2\xb7\xef\xbc\x9a\xe3\x80\x8a\xe3\x80\x8b");

        return s.toLowerCase().removeCharacters (
            juce::String (" ()[]-.,:'\"") + cjkPunct);
    }

    juce::String shortPreview (const juce::String& text)
    {
        juce::StringArray parts;

        for (const auto& raw : juce::StringArray::fromLines (text))
        {
            auto line = raw.trim();

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

    /** 最终文本处理：去 BOM、按开关繁转简。 */
    juce::String postProcess (const juce::String& text, bool simplify)
    {
        auto out = simplify ? toSimplified (text) : text;

        if (out.isNotEmpty() && out[0] == juce::juce_wchar (0xfeff))
            out = out.substring (1);

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

            if (r.lrc.trim().isEmpty())
                continue;

            r.lineCount = countTimedLines (r.lrc);
            r.preview = shortPreview (r.lrc);
            out.push_back (r);
        }

        return out;
    }

    /** 自定义服务：纯 LRC、JSON 数组、或带 content 的 JSON 对象。 */
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
            r.lineCount = countTimedLines (body);
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
            r.lineCount = countTimedLines (text);
            out.push_back (r);
            return out;
        }

        Result r;
        r.title   = title;
        r.source  = "Custom";
        r.lrc     = body;
        r.preview = shortPreview (body);
        r.lineCount = countTimedLines (body);
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

    //--------------------------------------------------------------------------
    /** 酷狗「歌曲库」搜索：返回带准确歌手、专辑、时长与 hash 的歌曲。 */
    int songScore (const Result& r, const juce::String& title,
                   const juce::String& artist, int targetSec)
    {
        const auto nt = normalise (title), na = normalise (artist);
        const auto rt = normalise (r.title), ra = normalise (r.artist);

        int score = 0;

        if (rt == nt)                                   score += 100;
        else if (rt.contains (nt) || nt.contains (rt))  score += 55;
        else                                             score -= 40;

        if (na.isNotEmpty())
        {
            if (ra == na)                                   score += 90;
            else if (ra.contains (na) || na.contains (ra))  score += 70;
            else                                             score -= 120;
        }

        if (targetSec > 0 && r.durationSec > 0)
            score -= juce::jmin (30, std::abs (r.durationSec - targetSec) / 8);

        // 现场版 / 伴奏之类排到录音室原版后面（中文词同样用字节转义）
        const auto liveCn = juce::String::fromUTF8 ("\xe7\x8e\xb0\xe5\x9c\xba");
        const auto instCn = juce::String::fromUTF8 ("\xe4\xbc\xb4\xe5\xa5\x8f");

        if (r.title.contains ("Live") || r.title.contains ("live")
            || r.title.contains (liveCn) || r.title.contains (instCn))
            score -= 12;

        return score;
    }

    std::vector<Result> searchKugouSongs (juce::String title, juce::String artist,
                                          int targetSec, juce::String& diag)
    {
        // 用户常把「歌名-歌手」一起粘进歌名框，这里拆开
        if (artist.isEmpty())
        {
            const int dash = title.indexOfChar ('-');
            if (dash > 0)
            {
                artist = title.substring (dash + 1).trim();
                title  = title.substring (0, dash).trim();
            }
        }

        const auto keyword = artist.isNotEmpty() ? title + " " + artist : title;
        const juce::String tail = "/api/v3/search/song?format=json&keyword="
                                + urlEncode (keyword)
                                + "&page=1&pagesize=12&showtype=1";

        // 该 CDN 的 HTTPS 证书在部分网络下 hostname 不匹配，明文 HTTP 优先
        // mobiles.kugou.com 的 HTTPS 证书正常，优先用它；
        // mobilecdn 两个域名 HTTPS 证书 hostname 不匹配，只能走明文，放后面兜底。
        static const char* hosts[] =
        {
            "https://mobiles.kugou.com",
            "http://mobilecdn.kugou.com",
            "http://mobilecdnbj.kugou.com",
            "http://mobiles.kugou.com"
        };

        std::vector<Result> out;

        for (const auto* host : hosts)
        {
            int status = 0, err = 0;
            const auto body = httpGet (juce::String (host) + tail, kKugouHeaders,
                                       status, 5000, &err);

            if (! usable (body, status))
            {
                diag << stepDiag ("song", status, err) << " ";
                continue;
            }

            const auto root = juce::JSON::parse (body);
            const auto data = field (root, "data");
            const auto info = field (data, "info");
            const auto* arr = info.getArray();

            if (arr == nullptr)
            {
                diag << "songBADJSON ";
                continue;
            }

            diag << "song" << status << "/" << static_cast<int> (arr->size()) << " ";

            for (const auto& v : *arr)
            {
                Result r;
                r.title       = str (v, "songname");
                r.artist      = str (v, "singername");
                r.album       = str (v, "album_name");
                r.hash        = str (v, "hash");
                r.durationSec = intField (v, "duration");
                r.source      = "Kugou";
                r.wordSync    = true;

                if (r.title.isEmpty() || r.hash.isEmpty())
                    continue;

                r.preview = r.artist;
                if (r.album.isNotEmpty())
                    r.preview += "  |  " + r.album;

                out.push_back (r);
            }

            if (! out.empty())
                break;
        }

        if (out.empty())
            return out;

        std::stable_sort (out.begin(), out.end(),
            [&] (const Result& x, const Result& y)
            {
                return songScore (x, title, artist, targetSec)
                     > songScore (y, title, artist, targetSec);
            });

        // 指定了歌手时，明显不匹配的翻唱/同名曲直接剔掉；但至少留 2 条以免空手
        if (artist.isNotEmpty())
        {
            std::vector<Result> kept;

            for (const auto& r : out)
                if (songScore (r, title, artist, targetSec) >= 20 || kept.size() < 2)
                    kept.push_back (r);

            out = std::move (kept);
        }

        if (out.size() > 12)
            out.resize (12);

        return out;
    }

    /** 旧的关键词歌词搜索（歌曲库不可用时的兜底，歌手信息不可靠）。 */
    std::vector<Result> searchKugouByKeyword (const juce::String& title,
                                              const juce::String& artist,
                                              int targetSec, juce::String& diag)
    {
        std::vector<Result> all;
        juce::StringArray keywords;

        if (artist.isNotEmpty())
            keywords.add (title + " " + artist);

        keywords.add (title);

        for (const auto& kw : keywords)
        {
            int status = 0, err = 0;
            const auto body = httpGet (
                "https://lyrics.kugou.com/search?ver=1&man=yes&client=pc&keyword="
                + urlEncode (kw), kKugouHeaders, status, 6000, &err);

            if (! usable (body, status))
            {
                diag << stepDiag ("kw", status, err) << " ";
                continue;
            }

            const auto root = juce::JSON::parse (body);
            const auto candidates = field (root, "candidates");
            const auto* arr = candidates.getArray();

            if (arr == nullptr)
            {
                diag << "kwBADJSON ";
                continue;
            }

            diag << "kw" << status << "/" << static_cast<int> (arr->size()) << " ";

            for (const auto& v : *arr)
            {
                Result r;
                r.id          = str (v, "id");
                r.accessKey   = str (v, "accesskey");
                r.title       = str (v, "song");
                r.artist      = str (v, "singer");
                r.durationSec = intField (v, "duration") / 1000;
                r.source      = "Kugou";
                r.wordSync    = true;

                if (r.id.isEmpty() || r.accessKey.isEmpty())
                    continue;

                if (r.title.isEmpty())
                    r.title = str (root, "keyword");

                all.push_back (r);
            }

            if (! all.empty())
                break;
        }

        if (targetSec > 0)
        {
            std::stable_sort (all.begin(), all.end(),
                [targetSec] (const Result& a, const Result& b)
                {
                    const int da = a.durationSec > 0 ? std::abs (a.durationSec - targetSec) : 999999;
                    const int db = b.durationSec > 0 ? std::abs (b.durationSec - targetSec) : 999999;
                    return da < db;
                });
        }

        if (all.size() > 20)
            all.resize (20);

        return all;
    }

    /** 用歌曲 hash 精确查歌词候选，并按歌曲时长挑最接近的几条。 */
    std::vector<Result> lyricsByHash (const Result& song)
    {
        std::vector<Result> out;
        const int ms = song.durationSec > 0 ? song.durationSec * 1000 : 0;

        juce::String url = "https://lyrics.kugou.com/search?ver=1&man=yes&client=pc&keyword="
                         + urlEncode (song.title)
                         + "&hash=" + urlEncode (song.hash);

        if (ms > 0)
            url += "&duration=" + juce::String (ms);

        int status = 0;
        const auto body = httpGet (url, kKugouHeaders, status, 6000);

        if (! usable (body, status))
            return out;

        const auto root = juce::JSON::parse (body);
        const auto candidates = field (root, "candidates");
        const auto* arr = candidates.getArray();

        if (arr == nullptr)
            return out;

        for (const auto& v : *arr)
        {
            Result c;
            c.id          = str (v, "id");
            c.accessKey   = str (v, "accesskey");
            c.title       = str (v, "song");
            c.artist      = str (v, "singer");
            c.durationSec = intField (v, "duration") / 1000;
            c.source      = "Kugou";
            c.wordSync    = true;

            if (c.id.isEmpty() || c.accessKey.isEmpty())
                continue;

            out.push_back (c);
        }

        if (ms > 0)
        {
            std::stable_sort (out.begin(), out.end(),
                [ms] (const Result& a, const Result& b)
                {
                    return std::abs (a.durationSec * 1000 - ms)
                         < std::abs (b.durationSec * 1000 - ms);
                });
        }

        if (out.size() > 4)
            out.resize (4);

        return out;
    }

    /** 下载一条酷狗歌词候选：KRC 逐字优先，失败退普通 LRC。 */
    juce::String fetchKugouLyrics (const Result& c)
    {
        if (c.id.isEmpty() || c.accessKey.isEmpty())
            return {};

        const juce::String base = "https://lyrics.kugou.com/download?ver=1&client=pc&id="
                                + urlEncode (c.id)
                                + "&accesskey=" + urlEncode (c.accessKey)
                                + "&charset=utf8&fmt=";

        for (const char* fmt : { "krc", "lrc" })
        {
            int status = 0;
            const auto body = httpGet (base + fmt, kKugouHeaders, status, 6000);

            if (! usable (body, status))
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

//==============================================================================
int countTimedLines (const juce::String& text)
{
    int n = 0;

    for (const auto& raw : juce::StringArray::fromLines (text))
    {
        const auto t = raw.trim();

        if (! t.startsWith ("["))
            continue;

        const int close = t.indexOfChar (']');
        if (close < 4)
            continue;

        const auto tag = t.substring (1, close);
        const int colon = tag.indexOfChar (':');

        if (colon > 0 && tag.substring (0, colon).containsOnly ("0123456789"))
            ++n;
    }

    return n;
}

juce::String toSimplified (const juce::String& text)
{
   #if JUCE_WINDOWS
    if (text.isEmpty())
        return text;

    const auto* utf8 = text.toRawUTF8();
    const int bytes = text.getNumBytesAsUTF8();

    const int wlen = MultiByteToWideChar (CP_UTF8, 0, utf8, bytes, nullptr, 0);
    if (wlen <= 0)
        return text;

    std::wstring wide (static_cast<size_t> (wlen), L'\0');
    MultiByteToWideChar (CP_UTF8, 0, utf8, bytes, wide.data(), wlen);

    std::wstring mapped (static_cast<size_t> (wlen) + 8, L'\0');
    const int n = LCMapStringEx (LOCALE_NAME_USER_DEFAULT, LCMAP_SIMPLIFIED_CHINESE,
                                 wide.c_str(), wlen, mapped.data(),
                                 static_cast<int> (mapped.size()),
                                 nullptr, nullptr, 0);
    if (n <= 0)
        return text;

    const int need = WideCharToMultiByte (CP_UTF8, 0, mapped.c_str(), n,
                                          nullptr, 0, nullptr, nullptr);
    if (need <= 0)
        return text;

    std::string result (static_cast<size_t> (need), '\0');
    WideCharToMultiByte (CP_UTF8, 0, mapped.c_str(), n, result.data(), need,
                         nullptr, nullptr);

    return juce::String::fromUTF8 (result.data(), need);
   #else
    return text;
   #endif
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

    juce::String diag;
    diag << (gUseProxy.load() != 0 ? "proxy " : "direct ");

    //---- 1. 自定义服务（填了就优先，失败再自动回退） -------------------------
    if (customUrl.trim().isNotEmpty())
    {
        int status = 0;
        const auto body = httpGet (expandTemplate (customUrl, title, artist, durationSec),
                                   kLrclibHeaders, status);

        if (usable (body, status))
        {
            auto found = parseCustom (body, title);

            if (! found.empty())
            {
                outcome.results = std::move (found);
                outcome.via = "Custom";
                outcome.detail = "custom" + juce::String (status);
                return outcome;
            }
        }

        diag << "custom" << status << " ";
    }

    //---- 2. 酷狗歌曲库（带准确歌手与 hash，避免同名串歌） -------------------
    {
        auto found = searchKugouSongs (title, artist, durationSec, diag);

        if (! found.empty())
        {
            outcome.results = std::move (found);
            outcome.via = "Kugou";
            outcome.detail = diag.trim();
            return outcome;
        }
    }

    //---- 3. 酷狗旧关键词接口（歌曲库不可用时的兜底） -------------------------
    {
        auto found = searchKugouByKeyword (title, artist, durationSec, diag);

        if (! found.empty())
        {
            outcome.results = std::move (found);
            outcome.via = "Kugou (keyword)";
            outcome.detail = diag.trim();
            return outcome;
        }
    }

    //---- 4. LRCLIB 兜底 ------------------------------------------------------
    {
        int status = 0, err = 0;
        juce::String body;

        // 有歌手时先用精确参数，命中率比模糊 q= 高
        if (artist.isNotEmpty())
        {
            body = httpGet ("https://lrclib.net/api/search?track_name=" + urlEncode (title)
                            + "&artist_name=" + urlEncode (artist),
                            kLrclibHeaders, status, 7000, &err);
        }

        if (! usable (body, status))
        {
            status = 0;
            err = 0;
            const auto q = artist.isNotEmpty() ? title + " " + artist : title;
            body = httpGet ("https://lrclib.net/api/search?q=" + urlEncode (q),
                            kLrclibHeaders, status, 7000, &err);
        }

        if (usable (body, status))
        {
            auto found = parseLrclib (body, "LRCLIB");

            // 行数多的排前面，残缺条目自然沉底
            std::stable_sort (found.begin(), found.end(),
                [] (const Result& a, const Result& b)
                { return a.lineCount > b.lineCount; });

            diag << "lrc" << status << "/" << static_cast<int> (found.size()) << " ";

            if (! found.empty())
            {
                if (found.size() > 20)
                    found.resize (20);

                outcome.results = std::move (found);
                outcome.via = "LRCLIB";
                outcome.detail = diag.trim();
                return outcome;
            }
        }
        else
        {
            diag << stepDiag ("lrc", status, err) << " ";
        }
    }

    outcome.error = "notfound";
    outcome.detail = diag.trim();
    return outcome;
}

void setUseSystemProxy (bool shouldUse)
{
    gUseProxy.store (shouldUse ? 1 : 0);
}

bool isUsingSystemProxy()
{
    return gUseProxy.load() != 0;
}

juce::String download (const Result& result, bool simplify)
{
    if (result.lrc.isNotEmpty())
        return postProcess (result.lrc, simplify);

    // 歌曲库结果：按 hash 精确查歌词，多条候选中挑时间码行数最多的一条
    if (result.hash.isNotEmpty())
    {
        const auto candidates = lyricsByHash (result);
        juce::String best;
        int bestLines = 0;

        for (const auto& c : candidates)
        {
            const auto text = fetchKugouLyrics (c);

            if (text.isEmpty())
                continue;

            const auto converted = postProcess (text, simplify);
            const int lines = countTimedLines (converted);

            if (lines > bestLines)
            {
                best = converted;
                bestLines = lines;
            }

            if (bestLines >= 10)
                break;
        }

        return best;
    }

    if (result.id.isNotEmpty() && result.accessKey.isNotEmpty())
        return postProcess (fetchKugouLyrics (result), simplify);

    return {};
}

LxResult fetchFromLx (juce::String baseUrl, bool simplify)
{
    LxResult out;
    auto base = baseUrl.trim();

    if (base.isEmpty())
        base = "http://127.0.0.1:23330";

    while (base.endsWith ("/"))
        base = base.dropLastCharacters (1);

    int status = 0;
    const auto statusBody = httpGet (
        base + "/status?filter=name,singer,duration,lyric", juce::String(), status, 2500);

    if (usable (statusBody, status))
    {
        const auto root = juce::JSON::parse (statusBody);
        out.title  = str (root, "name");
        out.artist = str (root, "singer");
        const auto lyric = str (root, "lyric");

        if (lyric.isNotEmpty())
            out.lrc = lyric;
    }

    if (out.lrc.isEmpty())
    {
        status = 0;
        const auto body = httpGet (base + "/lyric", juce::String(), status, 2500);

        if (usable (body, status))
            out.lrc = body;
    }

    if (out.lrc.trim().isEmpty())
    {
        out.error = "connect";
        return out;
    }

    out.lrc = postProcess (out.lrc, simplify);
    out.lineCount = countTimedLines (out.lrc);

    if (out.lineCount == 0)
        out.error = "noLyric";

    return out;
}
}
