#include "PluginEditor.h"
#include "DemoLyrics.h"

#include <juce_core/juce_core.h>   // FileOutputStream：导出 LRC 时显式按 UTF-8 写
#include <juce_events/juce_events.h>   // JUCEApplicationBase：运行时判断是否为独立版

#include <cmath>
#include <string>
#include <thread>

#if JUCE_WINDOWS
 #include <windows.h>
#endif

namespace
{
    const char* const kVersion = "2.0.7";

    // 常见字体（Windows 自带或系统常见），显示名直接用字体名，跨语言一致。
    // 只放含汉字的字体：以前放了 Segoe UI / Arial，选中后所有中文变成方块，
    // 干脆换成两个中文手写体，界面和歌词都一定有字形。
    const char* const kFontNames[] =
    {
        "Microsoft YaHei UI", "DengXian", "SimHei", "SimSun",
        "KaiTi", "FangSong", "YouYuan", "LiSu"
    };
    const int kNumFonts = static_cast<int> (sizeof (kFontNames) / sizeof (kFontNames[0]));

    const Lang::TextId kFontLabelIds[] =
    {
        Lang::TextId::fontYaHei, Lang::TextId::fontDengXian, Lang::TextId::fontSimHei,
        Lang::TextId::fontSimSun, Lang::TextId::fontKaiTi, Lang::TextId::fontFangSong,
        Lang::TextId::fontYouYuan, Lang::TextId::fontLiSu
    };

    // 0 = 跟随皮肤（用 accent）
    const juce::uint32 kLyricColours[] =
    {
        0x00000000, 0xfff2f5ff, 0xffffe9a3, 0xff5ce1c1, 0xff7cc6ff, 0xffff9ec4
    };
    const int kNumLyricColours = static_cast<int> (sizeof (kLyricColours) / sizeof (kLyricColours[0]));

    // 逐句着色用的调色板：红 / 橙 / 黄 / 绿 / 青 / 蓝 / 紫 / 粉
    const juce::uint32 kLineColours[] =
    {
        0xffff5555, 0xffff9f43, 0xffffd93d, 0xff51cf66,
        0xff22d3ee, 0xff4d9fff, 0xffa78bfa, 0xffff7ab6
    };
    const int kNumLineColours = static_cast<int> (sizeof (kLineColours) / sizeof (kLineColours[0]));

    /**
     *  判断一行是不是「作词 / 作曲 / 编曲 …」这类制作信息，而不是真正的歌词。
     *  在线搜来的 LRC 常常前面挂十几行这种信息，倒数 4 拍必须落在正式唱词前，
     *  而不是落在第一行（歌名或制作名单）上。
     *  中文关键词用 UTF-8 字节转义写，源码保持纯 ASCII，避免 MSVC 代码页问题。
     */
    bool looksLikeCredit (const juce::String& text)
    {
        const auto t = text.trim();

        if (t.isEmpty())
            return true;

        static const juce::StringArray cn = juce::StringArray::fromLines (
            juce::String::fromUTF8 (
                "\xe4\xbd\x9c\xe8\xaf\x8d\n\xe4\xbd\x9c\xe6\x9b\xb2\n"
                "\xe7\xbc\x96\xe6\x9b\xb2\n\xe5\x88\xb6\xe4\xbd\x9c\xe4\xba\xba\n"
                "\xe5\x87\xba\xe5\x93\x81\n\xe7\x9b\x91\xe5\x88\xb6\n"
                "\xe6\xb7\xb7\xe9\x9f\xb3\n\xe6\xaf\x8d\xe5\xb8\xa6\n"
                "\xe5\xbd\x95\xe9\x9f\xb3\n\xe5\x92\x8c\xe5\xa3\xb0\n"
                "\xe5\x8e\x9f\xe5\x94\xb1\n\xe7\xbf\xbb\xe5\x94\xb1\n"
                "\xe7\xad\x96\xe5\x88\x92\n\xe7\xbb\x9f\xe7\xad\xb9\n"
                "\xe5\x90\x8e\xe6\x9c\x9f\n\xe5\xad\x97\xe5\xb9\x95\n"
                "\xe7\xbf\xbb\xe8\xaf\x91\n\xe6\xa0\xa1\xe5\xaf\xb9\n"
                "\xe5\xa1\xab\xe8\xaf\x8d\n\xe8\xb0\xb1\xe6\x9b\xb2\n"
                "\xe6\xbc\x94\xe5\x94\xb1\n\xe8\xaf\x8d\xef\xbc\x9a\n"
                "\xe6\x9b\xb2\xef\xbc\x9a"));

        static const juce::StringArray en = juce::StringArray::fromLines (
            "lyrics by\nlyricist\ncomposed by\ncomposer\narranged by\narranger\n"
            "producer\nproduced by\nmixing\nmix by\nmastering\nmastered by\n"
            "recorded\nrecording\nwritten by\nmusic by\nvocal\nguitar\nbass\n"
            "drums\nkeyboard\npiano\nstrings\nchoir");

        const auto lower = t.toLowerCase();

        for (const auto& w : cn)
            if (w.isNotEmpty() && t.contains (w))
                return true;

        for (const auto& w : en)
            if (w.isNotEmpty() && lower.contains (w))
                return true;

        return false;
    }

    /** 刷子图标：斜笔杆 + 笔头，纯 Path 绘制，不依赖字体里有没有该字形。 */
    void drawBrushIcon (juce::Graphics& g, juce::Rectangle<float> b, juce::Colour colour)
    {
        const float x = b.getX(), y = b.getY();
        const float w = b.getWidth(), h = b.getHeight();

        // 笔杆
        juce::Path handle;
        handle.startNewSubPath (x + w * 0.62f, y + h * 0.52f);
        handle.lineTo (x + w * 0.86f, y + h * 0.12f);
        g.setColour (colour);
        g.strokePath (handle, juce::PathStrokeType (juce::jmax (1.6f, w * 0.11f),
                                                    juce::PathStrokeType::curved,
                                                    juce::PathStrokeType::rounded));

        // 笔头（刷毛）
        juce::Path bristles;
        bristles.startNewSubPath (x + w * 0.62f, y + h * 0.50f);
        bristles.lineTo (x + w * 0.72f, y + h * 0.66f);
        bristles.lineTo (x + w * 0.50f, y + h * 0.92f);
        bristles.lineTo (x + w * 0.10f, y + h * 0.86f);
        bristles.lineTo (x + w * 0.16f, y + h * 0.52f);
        bristles.closeSubPath();
        g.setColour (colour.withAlpha (0.92f));
        g.fillPath (bristles);

        // 笔杆与刷毛之间的金属箍
        juce::Path ferrule;
        ferrule.startNewSubPath (x + w * 0.54f, y + h * 0.45f);
        ferrule.lineTo (x + w * 0.68f, y + h * 0.58f);
        g.setColour (colour.withAlpha (0.55f));
        g.strokePath (ferrule, juce::PathStrokeType (juce::jmax (1.2f, w * 0.08f),
                                                     juce::PathStrokeType::curved,
                                                     juce::PathStrokeType::rounded));
    }

    const int kZoomSteps[] = { 50, 75, 100, 125, 150, 200 };
    const int kNumZoomSteps = static_cast<int> (sizeof (kZoomSteps) / sizeof (kZoomSteps[0]));

    // 字号倍率：滑块、Ctrl+滚轮、全局偏好文件共用同一组上下限
    constexpr double kFontScaleMin = 0.70;
    constexpr double kFontScaleMax = 2.40;

    /** 纯 ASCII 手工拼时间码（不走 printf，避免任何格式化/编码意外）。 */
    juce::String formatTimecode (double seconds)
    {
        const bool negative = seconds < 0.0;
        seconds = std::abs (seconds);

        int mins = static_cast<int> (seconds / 60.0);
        double secs = seconds - static_cast<double> (mins) * 60.0;

        int whole = static_cast<int> (std::floor (secs));
        int milli = static_cast<int> (std::lround ((secs - static_cast<double> (whole)) * 1000.0));

        if (milli >= 1000)
        {
            milli -= 1000;
            ++whole;
        }

        if (whole >= 60)
        {
            whole -= 60;
            ++mins;
        }

        juce::String s;

        if (negative)
            s << "-";

        if (mins < 10)
            s << "0";

        s << juce::String (mins) << ":";

        if (whole < 10)
            s << "0";

        s << juce::String (whole) << ".";

        if (milli < 10)       s << "00";
        else if (milli < 100) s << "0";

        s << juce::String (milli);

        return s;
    }

    /** 去掉增强 LRC 的 <mm:ss.xx> 逐字锚点（编辑时显示用，保留整行时间戳）。 */
    juce::String stripWordAnchors (const juce::String& in)
    {
        juce::String out;

        for (int i = 0; i < in.length();)
        {
            if (in[i] == '<')
            {
                const int close = in.indexOfChar (i, '>');

                if (close > i)
                {
                    i = close + 1;
                    continue;
                }
            }

            out += in[i];
            ++i;
        }

        return out;
    }

    juce::String formatShort (double seconds)
    {
        seconds = juce::jmax (0.0, seconds);
        const int total = static_cast<int> (std::lround (seconds));
        return juce::String::formatted ("%d:%02d", total / 60, total % 60);
    }

    /** LRC timestamp: mm:ss.xx */
    juce::String formatStamp (double seconds)
    {
        seconds = juce::jmax (0.0, seconds);
        const int mins = static_cast<int> (seconds / 60.0);
        const double secs = seconds - static_cast<double> (mins) * 60.0;
        return juce::String::formatted ("%02d:%05.2f", mins, secs);
    }

    /** Many Chinese LRC files are GBK encoded: accept UTF-8 / UTF-16 / GB18030. */
    juce::String decodeTextFile (const juce::MemoryBlock& data)
    {
        if (data.getSize() == 0)
            return {};

        auto* bytes = static_cast<const char*> (data.getData());
        const int size = static_cast<int> (data.getSize());

        if (size >= 2
            && static_cast<unsigned char> (bytes[0]) == 0xff
            && static_cast<unsigned char> (bytes[1]) == 0xfe)
        {
           #if JUCE_WINDOWS
            const int count = (size - 2) / 2;
            std::wstring wide (reinterpret_cast<const wchar_t*> (bytes + 2),
                               static_cast<size_t> (juce::jmax (0, count)));
            return juce::String (wide.c_str());
           #else
            return {};
           #endif
        }

        if (juce::CharPointer_UTF8::isValidString (bytes, size))
            return juce::String::fromUTF8 (bytes, size);

       #if JUCE_WINDOWS
        const int needed = ::MultiByteToWideChar (936, 0, bytes, size, nullptr, 0);
        if (needed > 0)
        {
            std::wstring wide (static_cast<size_t> (needed), L'\0');
            ::MultiByteToWideChar (936, 0, bytes, size, wide.data(), needed);
            return juce::String (wide.c_str());
        }
       #endif

        return juce::String::fromUTF8 (bytes, size);
    }

    bool looksLikeLyricsFile (const juce::String& path)
    {
        auto p = path.toLowerCase();
        return p.endsWith (".lrc") || p.endsWith (".krc") || p.endsWith (".txt")
            || p.endsWith (".srt") || p.endsWith (".vtt") || p.endsWith (".ass")
            || p.endsWith (".ssa") || p.endsWith (".doc") || p.endsWith (".docx");
    }

    bool looksLikeImageFile (const juce::String& path)
    {
        auto p = path.toLowerCase();
        return p.endsWith (".png") || p.endsWith (".jpg") || p.endsWith (".jpeg")
            || p.endsWith (".bmp") || p.endsWith (".webp") || p.endsWith (".gif");
    }

    /** Remove [mm:ss.xx] / <mm:ss.xx> stamps, keeping only the lyric text. */
    juce::String stripTimeTags (const juce::String& in)
    {
        juce::String out;

        for (int i = 0; i < in.length();)
        {
            const auto c = in[i];

            if (c == '[' || c == '<')
            {
                const auto close = in.indexOfChar (i, c == '[' ? ']' : '>');
                if (close > i)
                {
                    i = close + 1;
                    continue;
                }
            }

            out += c;
            ++i;
        }

        return out.trim();
    }

    bool isCjkChar (juce::juce_wchar c)
    {
        return (c >= 0x2e80 && c <= 0x9fff)
            || (c >= 0xf900 && c <= 0xfaff)
            || (c >= 0xff00 && c <= 0xffef)
            || (c >= 0x3000 && c <= 0x303f);
    }

    /** 现在字体表里全是中文字体；保留这个判断，防止以后又混进纯拉丁字体。 */
    bool typefaceNeedsCjkFallback (int index)
    {
        juce::ignoreUnused (index);
        return false;
    }

    /** 自定义皮肤图片的存档目录（复制一份，避免原图被移动后失效）。 */
    juce::File skinsDir()
    {
        return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
                 .getChildFile ("LyricSync")
                 .getChildFile ("Skins");
    }

    /** 歌词库目录：文档/LyricSync/Lyrics */
    juce::File lyricsLibraryDir()
    {
        return juce::File::getSpecialLocation (juce::File::userDocumentsDirectory)
                 .getChildFile ("LyricSync")
                 .getChildFile ("Lyrics");
    }

    /** 全局界面偏好：只作为新工程新实例的默认值，工程状态里的值优先。 */
    juce::File uiPrefsFile()
    {
        return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
                 .getChildFile ("LyricSync")
                 .getChildFile ("ui-prefs.txt");
    }

    juce::String sanitiseFilePart (const juce::String& s)
    {
        juce::String out;

        for (auto c : s)
        {
            if (juce::CharacterFunctions::isLetterOrDigit (c) || c == ' '
                || c == '-' || c == '_' || c > 127)
                out << c;
            else
                out << '_';
        }

        return out.trim().substring (0, 60);
    }
}

//==============================================================================
juce::Font ScaledLookAndFeel::getTextButtonFont (juce::TextButton&, int)
{
    return juce::Font (typeface, 12.0f * scale, juce::Font::plain);
}

juce::Font ScaledLookAndFeel::getComboBoxFont (juce::ComboBox&)
{
    return juce::Font (typeface, 12.0f * scale, juce::Font::plain);
}

juce::Font ScaledLookAndFeel::getLabelFont (juce::Label&)
{
    return juce::Font (typeface, 12.0f * scale, juce::Font::plain);
}

juce::Font ScaledLookAndFeel::getPopupMenuFont()
{
    return juce::Font (typeface, 13.0f * scale, juce::Font::plain);
}

juce::Font ScaledLookAndFeel::getSliderPopupFont (juce::Slider&)
{
    return juce::Font (typeface, 12.0f * scale, juce::Font::plain);
}


//==============================================================================
void ScaledLookAndFeel::drawButtonBackground (juce::Graphics& g, juce::Button& b,
                                             const juce::Colour& backgroundColour,
                                             bool over, bool down)
{
    const auto r = b.getLocalBounds().toFloat().reduced (0.5f);
    const float corner = juce::jmax (5.0f, 10.0f * scale);

    // 用按钮自己的文字色做"边缘色"：深色主题→淡白边，浅色主题→淡黑边
    const auto edge = b.findColour (juce::TextButton::textColourOffId);

    //---- 方案里的扁平按钮：透明底 + 1px 细边，只有悬停/选中才上色 ----------
    const bool flat = static_cast<bool> (b.getProperties().getWithDefault ("flat", false));
    const bool bare = static_cast<bool> (b.getProperties().getWithDefault ("bare", false));

    if (flat || bare)
    {
        const bool on = b.getToggleState();
        const auto fillOn = b.findColour (juce::TextButton::buttonOnColourId);
        const bool darkBg = backgroundColour.getPerceivedBrightness() < 0.45f;

        if (on)
        {
            g.setColour (fillOn);
            g.fillRoundedRectangle (r, juce::jmax (4.0f, 8.0f * scale));
        }
        else if (over || down)
        {
            g.setColour (darkBg ? juce::Colours::white.withAlpha (0.06f)
                                : juce::Colours::black.withAlpha (0.04f));
            g.fillRoundedRectangle (r, juce::jmax (4.0f, 8.0f * scale));
        }

        if (! bare)
        {
            const auto line = on ? edge.withAlpha (0.55f)
                                 : edge.withAlpha (over ? (darkBg ? 0.50f : 0.26f)
                                                        : (darkBg ? 0.32f : 0.15f));
            g.setColour (line);
            g.drawRoundedRectangle (r.reduced (0.4f), juce::jmax (4.0f, 8.0f * scale),
                                    juce::jmax (1.0f, 1.1f * scale));
        }

        return;
    }

    auto fill = backgroundColour;

    if (b.getToggleState())
        fill = findColour (juce::TextButton::buttonOnColourId);

    // 上亮下暗的极轻渐变，让平面色块有玻璃的体积
    juce::ColourGradient glass (fill.brighter (down ? 0.22f : (over ? 0.14f : 0.06f)),
                                r.getX(), r.getY(),
                                fill.darker (0.05f),
                                r.getX(), r.getBottom(), false);
    g.setGradientFill (glass);
    g.fillRoundedRectangle (r, corner);

    // 边缘：常态 26%，hover 45%
    g.setColour (edge.withAlpha (over ? 0.45f : 0.26f));
    g.drawRoundedRectangle (r.reduced (0.4f), corner, juce::jmax (1.0f, 1.1f * scale));

    // 顶部一道更亮的高光，只有上沿，像玻璃反光
    {
        juce::Path top;
        top.addRoundedRectangle (r.getX() + corner * 0.8f, r.getY() + 0.9f,
                                 r.getWidth() - corner * 1.6f, juce::jmax (2.0f, 3.0f * scale),
                                 corner * 0.4f);
        g.setColour (juce::Colours::white.withAlpha (b.getToggleState() ? 0.22f : 0.10f));
        g.fillPath (top);
    }
}

void ScaledLookAndFeel::drawComboBox (juce::Graphics& g, int width, int height, bool,
                                      int, int, int, int, juce::ComboBox& box)
{
    const auto r = juce::Rectangle<float> (0.0f, 0.0f,
                                           static_cast<float> (width),
                                           static_cast<float> (height)).reduced (0.5f);
    const float corner = juce::jmax (4.0f, 7.0f * scale);

    g.setColour (box.findColour (juce::ComboBox::backgroundColourId));
    g.fillRoundedRectangle (r, corner);

    g.setColour (box.findColour (juce::ComboBox::textColourId).withAlpha (0.22f));
    g.drawRoundedRectangle (r.reduced (0.4f), corner, juce::jmax (1.0f, 1.1f * scale));

    // 下拉箭头（自绘，避免依赖字体字形）
    const float cx = static_cast<float> (width) - 16.0f * juce::jmax (0.85f, scale);
    const float cy = static_cast<float> (height) * 0.5f;
    const float s  = juce::jmax (3.0f, 3.4f * juce::jmax (0.85f, scale));

    juce::Path arrow;
    arrow.startNewSubPath (cx - s, cy - s * 0.5f);
    arrow.lineTo (cx, cy + s * 0.7f);
    arrow.lineTo (cx + s, cy - s * 0.5f);

    g.setColour (box.findColour (juce::ComboBox::arrowColourId));
    g.strokePath (arrow, juce::PathStrokeType (juce::jmax (1.0f, 1.4f * scale),
                                               juce::PathStrokeType::curved,
                                               juce::PathStrokeType::rounded));
}

//==============================================================================
void IconButton::paintButton (juce::Graphics& g, bool over, bool down)
{
    if (icon == Icon::none)
        return;

    const auto outer = getLocalBounds().toFloat().reduced (0.5f);
    const auto b = outer.reduced (1.0f);
    const auto colour = (over || down) ? hoverColour : normalColour;

    if (boxed)
    {
        const bool darkBg = boxFill.getPerceivedBrightness() < 0.45f;

        if (over || down)
        {
            g.setColour (darkBg ? juce::Colours::white.withAlpha (0.06f)
                                : juce::Colours::black.withAlpha (0.04f));
            g.fillRoundedRectangle (outer, 8.0f);
        }

        g.setColour (normalColour.withAlpha (over ? (darkBg ? 0.50f : 0.26f)
                                                  : (darkBg ? 0.32f : 0.15f)));
        g.drawRoundedRectangle (outer.reduced (0.4f), 8.0f, 1.1f);
    }
    else if (over || down)
    {
        g.setColour (normalColour.withAlpha (0.10f));
        g.fillRoundedRectangle (b, 8.0f);
    }

    if (icon == Icon::orient)
    {
        // 横 / 竖：两片叠起来的圆角矩形（前片用侧栏底色挡住后片）
        const float s = juce::jmin (outer.getWidth(), outer.getHeight());
        const auto box = outer.withSizeKeepingCentre (s, s).reduced (s * 0.20f);

        const auto back  = juce::Rectangle<float> (box.getX(), box.getY() + box.getHeight() * 0.52f,
                                                   box.getWidth() * 0.58f, box.getHeight() * 0.40f);
        const auto front = juce::Rectangle<float> (box.getX() + box.getWidth() * 0.45f,
                                                   box.getY(), box.getWidth() * 0.55f,
                                                   box.getHeight() * 0.66f);

        g.setColour (colour);
        g.drawRoundedRectangle (back.reduced (0.5f), 1.6f, 1.15f);
        g.fillRoundedRectangle (front, 1.6f);
        g.setColour (boxFill.isTransparent() ? juce::Colours::black.withAlpha (0.55f) : boxFill);
        g.fillRoundedRectangle (front.reduced (1.6f), 1.0f);
        g.setColour (colour);
        g.drawRoundedRectangle (front.reduced (0.5f), 1.6f, 1.15f);
        return;
    }

    if (icon == Icon::undo)
    {
        // 撤销：一圈带缺口的弧 + 顶部小箭头（纯 Path，不依赖字体里有没有 ↺）
        const float cx = outer.getCentreX();
        const float cy = outer.getCentreY();
        const float rr = juce::jmin (outer.getWidth(), outer.getHeight()) * 0.5f - 6.0f;

        juce::Path arc;
        arc.addCentredArc (cx, cy, rr, rr, 0.0f,
                           juce::MathConstants<float>::pi * -0.34f,
                           juce::MathConstants<float>::pi * 1.46f, true);

        g.setColour (colour);
        g.strokePath (arc, juce::PathStrokeType (1.4f,
                        juce::PathStrokeType::curved,
                        juce::PathStrokeType::rounded));

        const float tipX = cx - rr * 0.16f;
        const float tipY = cy - rr * 0.99f;

        juce::Path head;
        head.addTriangle (tipX - 3.4f, tipY + 0.4f,
                          tipX + 3.6f, tipY - 3.6f,
                          tipX + 3.4f, tipY + 4.2f);
        g.fillPath (head);
        return;
    }

    const float cx = b.getCentreX();
    const float cy = b.getCentreY();
    const float r  = juce::jmin (b.getWidth(), b.getHeight()) * 0.5f - 7.0f;

    if (icon == Icon::gear)
    {
        const int teeth = 8;
        juce::Path p;

        for (int i = 0; i < teeth * 2; ++i)
        {
            const float angle = juce::MathConstants<float>::twoPi * static_cast<float> (i)
                              / static_cast<float> (teeth * 2);
            const float rad = (i % 2 == 0) ? r : r * 0.74f;
            const float x = cx + std::cos (angle) * rad;
            const float y = cy + std::sin (angle) * rad;

            if (i == 0) p.startNewSubPath (x, y);
            else        p.lineTo (x, y);
        }

        p.closeSubPath();
        p.addEllipse (cx - r * 0.30f, cy - r * 0.30f, r * 0.60f, r * 0.60f);
        p.setUsingNonZeroWinding (false);

        g.setColour (colour);
        g.fillPath (p);
    }
    else
    {
        juce::Path p;
        const float d = r * 0.72f;
        p.startNewSubPath (cx - d, cy - d);
        p.lineTo (cx + d, cy + d);
        p.startNewSubPath (cx + d, cy - d);
        p.lineTo (cx - d, cy + d);

        g.setColour (colour);
        g.strokePath (p, juce::PathStrokeType (2.0f));
    }
}

//==============================================================================
LyricSyncEditor::LyricSyncEditor (LyricSyncProcessor& owner)
    : juce::AudioProcessorEditor (owner),
      processor (owner),
      palette (UiPalette::dark())
{
    setLookAndFeel (&lookAndFeel);
    setOpaque (true);
    setWantsKeyboardFocus (true);

    // 独立版（Standalone）没有宿主时间轴，JUCE 自带的假播放头只会一直往前跑。
    // 用 JUCEApplicationBase 运行时判断（VST3 里为 false），启用内置时钟与走带控件。
    standaloneMode = juce::JUCEApplicationBase::isStandaloneApp()
                  || juce::JUCEApplicationBase::getInstance() != nullptr;

    if (standaloneMode)
        processor.enableStandaloneTransport();

    // 全局默认值先铺一遍，工程状态恢复后会覆盖它（见 syncControlsFromSettings）。
    loadUiPrefs();
    syncControlsFromSettings();
    loadLineStyles();

    // 右下角缩放柄回来了：自由拖拽即"无极缩放"，画布始终按比例整体缩放。
    setResizable (true, true);
    setResizeLimits (280, 280, 2600, 2600);

    applyWindowSize();

    //--------------------------------------------------------------------------
    for (auto* b : { &loadButton, &pasteButton, &demoButton,
                     &minusButton, &plusButton, &resetButton,
                     &demoPlayButton, &demoStopButton,
                     &applyTextButton, &cancelTextButton,
                     &tapStartButton, &padStampButton,
                     &tapUndoButton, &tapFinishButton, &tapExitButton,
                     &tapRestartButton, &tapNudgeBackButton, &tapNudgeFwdButton, &tapPauseButton,
                     &tapHostTransportButton,
                     &tapRowRestampButton, &tapRowApplyButton, &tapRowCancelButton,
                     &measureButton, &exportButton, &retempoButton, &demoClockToggle,
                     &orientHButton, &orientVButton, &pickImageButton, &clearImageButton,
                     &hintToggle,
                     &setStartButton, &annotateButton,
                     &styleApplyButton, &styleClearButton, &styleCancelButton,
                     &onlineLyricsButton, &searchGoButton, &searchLoadButton,
                     &searchLxButton, &simplifyToggle, &standalonePlayButton,
                     &gearButton, &castButton, &fontMinusButton, &fontPlusButton,
                     &segLineButton, &segWordButton })
    {
        b->addListener (this);
        addAndMakeVisible (*b);
    }

    for (auto* b : { &orientButton, &undoSetStartButton, &logoButton,
                     &closeSettingsButton, &closeAboutButton, &closeSearchButton })
    {
        b->addListener (this);
        addAndMakeVisible (*b);
    }

    // 逐行 / 逐字：互斥分段开关
    for (auto* b : { &segLineButton, &segWordButton })
    {
        b->setClickingTogglesState (true);
        b->setRadioGroupId (0x5301);
        b->setColour (juce::TextButton::buttonOnColourId, juce::Colour (0xfff3f3f5));
        b->setColour (juce::TextButton::textColourOnId,   juce::Colour (0xff101013));
        b->getProperties().set ("bare", true);
    }

    segLineButton.setToggleState (advanceMode == 0, juce::dontSendNotification);
    segWordButton.setToggleState (advanceMode == 1, juce::dontSendNotification);

    demoClockToggle.setClickingTogglesState (true);
    hintToggle.setClickingTogglesState (true);

    for (auto* box : { &languageBox, &zoomBox, &skinBox, &fontBox, &colourBox, &advanceBox,
                       &networkBox })
    {
        box->setEditableText (false);
        box->setJustificationType (juce::Justification::centred);
        box->addListener (this);
        addAndMakeVisible (*box);
    }

    fontSlider.setRange (kFontScaleMin, kFontScaleMax, 0.01);
    fontSlider.setValue (fontScale, juce::dontSendNotification);
    fontSlider.addListener (this);
    addAndMakeVisible (fontSlider);

    const auto addLabel = [this] (juce::Label& l, float size, bool bold,
                                  juce::Justification just)
    {
        l.setFont (uiFont (size, bold));
        l.setJustificationType (just);
        addAndMakeVisible (l);
    };

    addLabel (offsetLabel, 11.5f, false, juce::Justification::centred);
    addLabel (fileLabel,   11.5f, false, juce::Justification::centredLeft);
    addLabel (hintLabel,   11.0f, false, juce::Justification::centredLeft);
    hintLabel.addMouseListener (this, false);      // 点提示词 -> 换下一条
    addLabel (fontLabel,   11.5f, false, juce::Justification::centred);
    addLabel (audioLabel,  11.5f, false, juce::Justification::centredRight);

    for (auto* ed : { &tapTextEditor, &tapTimeEditor })
    {
        ed->setMultiLine (false);
        ed->setReturnKeyStartsNewLine (false);
        ed->setScrollbarsShown (false);
        ed->setJustification (juce::Justification::centredLeft);
        ed->onReturnKey = [this] { applyTapRow(); };
        ed->onEscapeKey = [this] { closeTapRowEditing(); };
        ed->setPopupMenuEnabled (false);          // 自带菜单是英文，自己画
        ed->addMouseListener (this, false);
        addChildComponent (*ed);
    }

    styleNoteEditor.setMultiLine (false);
    styleNoteEditor.setReturnKeyStartsNewLine (false);
    styleNoteEditor.setScrollbarsShown (false);
    styleNoteEditor.setJustification (juce::Justification::centredLeft);
    styleNoteEditor.setPopupMenuEnabled (false);
    styleNoteEditor.addMouseListener (this, false);
    styleNoteEditor.onReturnKey = [this] { applyLineStyle(); };
    styleNoteEditor.onEscapeKey = [this] { closeLineStyle(); };
    addChildComponent (styleNoteEditor);

    //---- 在线搜歌词的输入框（面板打开时才显示） -----------------------------
    for (auto* ed : { &searchTitleEditor, &searchArtistEditor, &customUrlEditor,
                      &lxUrlEditor })
    {
        ed->setMultiLine (false);
        ed->setReturnKeyStartsNewLine (false);
        ed->setScrollbarsShown (false);
        ed->setJustification (juce::Justification::centredLeft);
        ed->setPopupMenuEnabled (false);
        addChildComponent (*ed);
    }

    searchTitleEditor.onReturnKey  = [this] { startSearch(); };
    searchArtistEditor.onReturnKey = [this] { startSearch(); };
    customUrlEditor.onReturnKey    = [this] { commitCustomUrl(); };
    lxUrlEditor.onReturnKey        = [this] { commitCustomUrl(); };
    customUrlEditor.setText (customSearchUrl, false);
    lxUrlEditor.setText (lxMusicUrl, false);
    simplifyToggle.setClickingTogglesState (true);
    simplifyToggle.setToggleState (simplifyChinese, juce::dontSendNotification);
    OnlineLyrics::setUseSystemProxy (useSystemProxy);

    tapTextEditor.setTextToShowWhenEmpty (tr (Lang::TextId::tapRowText), juce::Colours::grey);
    tapTimeEditor.setTextToShowWhenEmpty ("00:00.00", juce::Colours::grey);

    editorBox.setMultiLine (true, true);
    editorBox.setReturnKeyStartsNewLine (true);
    editorBox.setScrollbarsShown (true);
    editorBox.setFont (uiFont (14.0f));
    editorBox.setPopupMenuEnabled (false);        // 自带菜单是英文，自己画
    editorBox.addMouseListener (this, false);
    addChildComponent (editorBox);

    // 工程里存的是不是内置示例？只有"没存过"或"正好是示例"才用当前语言重载，
    // 否则绝不覆盖用户歌词（这是"关掉插件窗口再打开，歌词没了"的根因）。
    const auto storedLyrics = processor.getSetting (Settings::lyricsText, "").toString();
    usingDemoLyrics = storedLyrics.trim().isEmpty()
                   || storedLyrics == demoLyricsText (0)
                   || storedLyrics == demoLyricsText (1);

    if (! usingDemoLyrics)
        processor.reloadFromSettings (language);

    fileDescription = tr (usingDemoLyrics ? Lang::TextId::demoFileDesc
                                          : Lang::TextId::savedFileDesc);

    applyUiText (true);
    refreshLookAndFeel();
    updateVisibility();
    OnlineLyrics::resetCancel();      // 编辑器重建后恢复正常联网

    startTimerHz (30);
    hintRotateMs = juce::Time::getMillisecondCounterHiRes();

    tooltipWindow = std::make_unique<juce::TooltipWindow> (this, 550);
}

LyricSyncEditor::~LyricSyncEditor()
{
    stopTimer();

    // 关键：先把在途的网络请求掐掉，再等后台线程退出。
    // 之前用的是 std::thread(...).detach()，宿主关工程时如果还有请求在跑，
    // Cubase 卸载插件 DLL 就会被这条线程拖住 —— 表现为「卸载 MixConsole」卡死。
    OnlineLyrics::cancelAll();
    networkPool.removeAllJobs (true, 3000);

    tooltipWindow.reset();
    chooser.reset();
    setLookAndFeel (nullptr);
}

//==============================================================================
void LyricSyncEditor::loadUiPrefs()
{
    auto f = uiPrefsFile();

    if (! f.existsAsFile())
        return;

    prefsLoaded = true;

    for (const auto& raw : juce::StringArray::fromLines (f.loadFileAsString()))
    {
        const auto line = raw.trim();
        const int eq = line.indexOfChar ('=');

        if (eq <= 0)
            continue;

        const auto key = line.substring (0, eq).trim();
        const auto value = line.substring (eq + 1).trim();

        if      (key == "zoom")        zoomPercent = juce::jlimit (50, 200, value.getIntValue());
        else if (key == "orientation") orientation = value.getIntValue() == 1 ? Orientation::vertical
                                                                             : Orientation::horizontal;
        else if (key == "skin")        skinIndex   = value.getIntValue();
        else if (key == "font")        fontIndex   = value.getIntValue();
        else if (key == "colour")      colourIndex = value.getIntValue();
        else if (key == "fontScale")   fontScale   = juce::jlimit (kFontScaleMin, kFontScaleMax, value.getDoubleValue());
        else if (key == "customUrl")   customSearchUrl = value;
        else if (key == "lxUrl")       lxMusicUrl      = value;
        else if (key == "simplify")    simplifyChinese = value.getIntValue() != 0;
        else if (key == "netProxy")    useSystemProxy   = value.getIntValue() != 0;
        else if (key == "railCollapsed") sidebarCollapsed = value.getIntValue() != 0;
        else if (key == "bg" && value.isNotEmpty())
        {
            const juce::File bgFile (value);

            if (bgFile.existsAsFile())
            {
                customBg = juce::ImageFileFormat::loadFrom (bgFile);
                customBgPath = value;
            }
        }
    }

    skinIndex   = juce::jlimit (0, UiPalette::numSkins() - 1, skinIndex);
    fontIndex   = juce::jlimit (0, kNumFonts - 1, fontIndex);
    colourIndex = juce::jlimit (0, kNumLyricColours - 1, colourIndex);

    // 「自定义图片」皮肤但图片已经找不到了：退回深色，避免看起来像"皮肤丢了"
    if (skinIndex == 5 && ! customBg.isValid())
        skinIndex = 0;
}

void LyricSyncEditor::saveUiPrefs()
{
    auto f = uiPrefsFile();
    f.getParentDirectory().createDirectory();

    juce::StringArray lines;
    lines.add ("zoom=" + juce::String (zoomPercent));
    lines.add ("orientation=" + juce::String (orientation == Orientation::vertical ? 1 : 0));
    lines.add ("skin=" + juce::String (skinIndex));
    lines.add ("font=" + juce::String (fontIndex));
    lines.add ("colour=" + juce::String (colourIndex));
    lines.add ("fontScale=" + juce::String (fontScale, 3));
    lines.add ("bg=" + customBgPath);
    lines.add ("customUrl=" + customSearchUrl);
    lines.add ("lxUrl=" + lxMusicUrl);
    lines.add ("simplify=" + juce::String (simplifyChinese ? 1 : 0));
    lines.add ("netProxy=" + juce::String (useSystemProxy ? 1 : 0));
    lines.add ("railCollapsed=" + juce::String (sidebarCollapsed ? 1 : 0));

    f.replaceWithText (lines.joinIntoString ("\n"));
}

//==============================================================================
juce::String LyricSyncEditor::uiTypefaceName() const
{
    const int idx = juce::jlimit (0, kNumFonts - 1, fontIndex);

    if (typefaceNeedsCjkFallback (idx))
        return "Microsoft YaHei UI";

    return juce::String (kFontNames[idx]);
}

juce::Font LyricSyncEditor::uiFont (float size, bool bold) const
{
    return juce::Font (uiTypefaceName(), size, bold ? juce::Font::bold : juce::Font::plain);
}

juce::Colour LyricSyncEditor::lyricColour() const
{
    if (colourIndex <= 0 || colourIndex >= kNumLyricColours)
        return palette.accent;

    return juce::Colour (kLyricColours[colourIndex]);
}

void LyricSyncEditor::syncControlsFromSettings()
{
    offsetMs  = static_cast<double> (processor.getSetting (Settings::offsetMs, 0.0));
    fontScale = juce::jlimit (kFontScaleMin, kFontScaleMax,
                   static_cast<double> (processor.getSetting (Settings::fontScale, 1.0)));
    language  = juce::jlimit (0, 1, static_cast<int> (processor.getSetting (Settings::language, 0)));
    // 「手动预览」入口已从主界面移除，内部模拟播放头不再启用，
    // 避免旧工程里残留的 demoClock 设置让插件一直停在预览模式。
    demoMode = false;
    processor.setSetting (Settings::demoClock, false);

    // 外观（皮肤 / 背景图 / 字体 / 歌词颜色 / 比例 / 版型）以全局偏好为准。
    // 工程状态里也存了这些，但只要偏好文件存在就优先用它 —— 否则删掉插件重新加载、
    // 或者换个工程，外观就会被工程状态里的旧值（或默认值）顶掉。
    if (! prefsLoaded)
    {
        const int skin = static_cast<int> (processor.getSetting (Settings::skin, -1));
        if (skin >= 0)
            skinIndex = juce::jlimit (0, UiPalette::numSkins() - 1, skin);

        const int zoom = static_cast<int> (processor.getSetting (Settings::zoom, -1));
        if (zoom > 0)
            zoomPercent = juce::jlimit (50, 200, zoom);

        const int orient = static_cast<int> (processor.getSetting (Settings::orientation, -1));
        if (orient >= 0)
            orientation = (orient == 1) ? Orientation::vertical : Orientation::horizontal;

        const int font = static_cast<int> (processor.getSetting (Settings::font, -1));
        if (font >= 0)
            fontIndex = juce::jlimit (0, kNumFonts - 1, font);

        const int col = static_cast<int> (processor.getSetting (Settings::colour, -1));
        if (col >= 0)
            colourIndex = juce::jlimit (0, kNumLyricColours - 1, col);
    }

    const int adv = static_cast<int> (processor.getSetting (Settings::advance, -1));
    if (adv >= 0)
        advanceMode = juce::jlimit (0, 1, adv);

    const int hints = static_cast<int> (processor.getSetting (Settings::showHints, 1));
    showHints = hints != 0;

    const auto bgPath = processor.getSetting (Settings::customBg, "").toString();

    if (! customBg.isValid() && bgPath.isNotEmpty() && juce::File (bgPath).existsAsFile())
    {
        customBg = juce::ImageFileFormat::loadFrom (juce::File (bgPath));
        customBgPath = bgPath;
    }

    const double s = static_cast<double> (processor.getSetting (Settings::audioStart, -1.0));
    const double e = static_cast<double> (processor.getSetting (Settings::audioEnd,   -1.0));

    if (s >= 0.0 && e > s + 1.0)
    {
        processor.audioStartSeconds.store (s);
        processor.audioEndSeconds.store (e);
        savedAudioStart = s;
        savedAudioEnd   = e;
    }

    audioTotalSeconds = static_cast<double> (processor.getSetting (Settings::audioTotal, -1.0));

    if (skinIndex == 5 && ! customBg.isValid())
        skinIndex = 0;

    palette = UiPalette::forSkin (skinIndex);
    lookAndFeel.typeface = uiTypefaceName();
    fontSlider.setValue (fontScale, juce::dontSendNotification);
    demoClockToggle.setToggleState (demoMode, juce::dontSendNotification);
}

/** Re-apply every localised widget text / tooltip. */
void LyricSyncEditor::applyUiText (bool reloadDemoLyrics)
{
    loadButton  .setButtonText (tr (Lang::TextId::loadShort));
    pasteButton .setButtonText (tr (Lang::TextId::editShort));
    demoButton  .setButtonText (tr (Lang::TextId::demoLyrics));

    gearButton      .setButtonText (tr (Lang::TextId::settingsTitle));
    castButton      .setButtonText (tr (Lang::TextId::cast));
    fontMinusButton .setButtonText ("A-");
    fontPlusButton  .setButtonText ("A+");
    segLineButton   .setButtonText (tr (Lang::TextId::segLine));
    segWordButton   .setButtonText (tr (Lang::TextId::segWord));

    minusButton     .setButtonText (juce::String());
    plusButton      .setButtonText (juce::String());
    resetButton     .setButtonText (tr (Lang::TextId::reset));
    demoPlayButton  .setButtonText (tr (Lang::TextId::playSymbol));
    demoStopButton  .setButtonText (tr (Lang::TextId::stopSymbol));
    applyTextButton .setButtonText (tr (Lang::TextId::applyLyrics));
    cancelTextButton.setButtonText (tr (Lang::TextId::cancel));
    demoClockToggle .setButtonText (tr (Lang::TextId::manualPreview));

    tapStartButton  .setButtonText (tr (Lang::TextId::tapStart));
    padStampButton  .setButtonText (tr (Lang::TextId::tapRecordThis));
    tapUndoButton   .setButtonText (tr (Lang::TextId::tapUndo));
    tapFinishButton .setButtonText (tr (Lang::TextId::tapFinish));
    tapExitButton   .setButtonText (tr (Lang::TextId::tapExit));
    tapRestartButton.setButtonText (tr (Lang::TextId::tapRestart));
    tapNudgeBackButton.setButtonText (tr (Lang::TextId::tapNudgeBack));
    tapNudgeFwdButton .setButtonText (tr (Lang::TextId::tapNudgeFwd));
    tapPauseButton    .setButtonText (tr (demoPlaying ? Lang::TextId::tapPause
                                                     : Lang::TextId::tapResume));
    tapHostTransportButton.setButtonText (tr (processor.isPlaying.load() ? Lang::TextId::tapHostPause
                                                                       : Lang::TextId::tapHostPlay));
    tapRowRestampButton.setButtonText (tr (Lang::TextId::tapRowRestamp));
    tapRowApplyButton .setButtonText (tr (Lang::TextId::tapRowApply));
    tapRowCancelButton.setButtonText (tr (Lang::TextId::cancel));
    measureButton   .setButtonText (tr (Lang::TextId::measureAudio));
    exportButton    .setButtonText (tr (Lang::TextId::exportShort));
    retempoButton   .setButtonText (tr (Lang::TextId::retempo));

    orientHButton   .setButtonText (tr (Lang::TextId::orientH));
    orientVButton   .setButtonText (tr (Lang::TextId::orientV));
    pickImageButton .setButtonText (tr (Lang::TextId::pickImage));
    clearImageButton.setButtonText (tr (Lang::TextId::clearImage));

    setStartButton   .setButtonText (tr (Lang::TextId::setStart));
    annotateButton   .setButtonText (tr (annotateMode ? Lang::TextId::tapFinish
                                                      : Lang::TextId::annotateShort));
    onlineLyricsButton.setButtonText (tr (Lang::TextId::onlineLyrics));
    standalonePlayButton.setButtonText (tr (processor.standalonePlaying.load()
                                                ? Lang::TextId::standalonePause
                                                : Lang::TextId::standalonePlay));
    searchGoButton    .setButtonText (tr (Lang::TextId::onlineSearch));
    searchLoadButton  .setButtonText (tr (Lang::TextId::onlineLoad));
    searchLxButton    .setButtonText (tr (Lang::TextId::onlineLx));
    simplifyToggle    .setButtonText (tr (simplifyChinese ? Lang::TextId::onlineSimplifyOn
                                                         : Lang::TextId::onlineSimplifyOff));
    styleApplyButton .setButtonText (tr (Lang::TextId::annotateApplyNote));
    styleClearButton .setButtonText (tr (Lang::TextId::annotateClear));
    styleCancelButton.setButtonText (tr (Lang::TextId::cancel));

    hintToggle.setButtonText (showHints ? tr (Lang::TextId::onLabel)
                                        : tr (Lang::TextId::offLabel));
    hintToggle.setToggleState (showHints, juce::dontSendNotification);
    updateHint (true);

    //---- Combo contents -----------------------------------------------------
    updatingCombos = true;

    languageBox.clear (juce::dontSendNotification);
    languageBox.addItem (Lang::tr (Lang::TextId::languageChinese, 0), 1);
    languageBox.addItem (Lang::tr (Lang::TextId::languageEnglish, 0), 2);
    languageBox.setSelectedId (language + 1, juce::dontSendNotification);

    zoomBox.clear (juce::dontSendNotification);
    int zoomId = 3;
    for (int i = 0; i < kNumZoomSteps; ++i)
    {
        zoomBox.addItem (juce::String (kZoomSteps[i]) + " %", i + 1);

        if (kZoomSteps[i] == zoomPercent)
            zoomId = i + 1;
    }
    zoomBox.setSelectedId (zoomId, juce::dontSendNotification);

    skinBox.clear (juce::dontSendNotification);
    for (int i = 0; i < UiPalette::numSkins(); ++i)
        skinBox.addItem (tr (static_cast<Lang::TextId> (static_cast<int> (Lang::TextId::skinDark) + i)),
                         i + 1);
    skinBox.setSelectedId (skinIndex + 1, juce::dontSendNotification);

    fontBox.clear (juce::dontSendNotification);
    for (int i = 0; i < kNumFonts; ++i)
        fontBox.addItem (tr (kFontLabelIds[i]), i + 1);
    fontBox.setSelectedId (fontIndex + 1, juce::dontSendNotification);

    colourBox.clear (juce::dontSendNotification);
    for (int i = 0; i < kNumLyricColours; ++i)
        colourBox.addItem (tr (static_cast<Lang::TextId> (static_cast<int> (Lang::TextId::colorAuto) + i)),
                           i + 1);
    colourBox.setSelectedId (colourIndex + 1, juce::dontSendNotification);

    advanceBox.clear (juce::dontSendNotification);
    advanceBox.addItem (tr (Lang::TextId::advLine), 1);
    advanceBox.addItem (tr (Lang::TextId::advChar), 2);
    advanceBox.setSelectedId (advanceMode + 1, juce::dontSendNotification);

    networkBox.clear (juce::dontSendNotification);
    networkBox.addItem (tr (Lang::TextId::netDirect), 1);
    networkBox.addItem (tr (Lang::TextId::netProxy), 2);
    networkBox.setSelectedId (useSystemProxy ? 2 : 1, juce::dontSendNotification);

    updatingCombos = false;

    //---- Hover explanations -------------------------------------------------
    loadButton      .setTooltip (tr (Lang::TextId::tipLoad));
    pasteButton     .setTooltip (tr (Lang::TextId::tipPaste));
    orientButton    .setTooltip (tr (Lang::TextId::orientSwitchTip));
    castButton      .setTooltip (tr (Lang::TextId::cast));
    fontMinusButton .setTooltip (tr (Lang::TextId::tipFont));
    fontPlusButton  .setTooltip (tr (Lang::TextId::tipFont));    demoButton      .setTooltip (tr (Lang::TextId::tipDemo));
    minusButton     .setTooltip (tr (Lang::TextId::tipMinus));
    plusButton      .setTooltip (tr (Lang::TextId::tipPlus));
    resetButton     .setTooltip (tr (Lang::TextId::tipReset));
    fontSlider      .setTooltip (tr (Lang::TextId::tipFont));
    fontLabel       .setTooltip (tr (Lang::TextId::tipFont));
    demoClockToggle .setTooltip (tr (Lang::TextId::tipPreview));
    demoPlayButton  .setTooltip (tr (Lang::TextId::tipPlay));
    demoStopButton  .setTooltip (tr (Lang::TextId::tipStop));
    applyTextButton .setTooltip (tr (Lang::TextId::tipApply));
    cancelTextButton.setTooltip (tr (Lang::TextId::tipCancel));
    tapStartButton  .setTooltip (tr (Lang::TextId::tipTap));
    padStampButton  .setTooltip (tr (Lang::TextId::tipStamp));
    tapUndoButton   .setTooltip (tr (Lang::TextId::tipUndo));
    tapFinishButton .setTooltip (tr (Lang::TextId::tipFinish));
    tapExitButton   .setTooltip (tr (Lang::TextId::tipExit));
    tapRestartButton.setTooltip (tr (Lang::TextId::tipTapRestart));
    tapNudgeBackButton.setTooltip (tr (Lang::TextId::tipTapNudge));
    tapNudgeFwdButton .setTooltip (tr (Lang::TextId::tipTapNudge));
    tapPauseButton    .setTooltip (tr (Lang::TextId::tipTapPause));
    tapHostTransportButton.setTooltip (tr (Lang::TextId::tipTapHost));
    tapRowRestampButton.setTooltip (tr (Lang::TextId::tipTapRowRestamp));
    measureButton   .setTooltip (tr (Lang::TextId::tipMeasure));
    exportButton    .setTooltip (tr (Lang::TextId::tipExport));
    retempoButton   .setTooltip (tr (Lang::TextId::tipRetempo));
    audioLabel      .setTooltip (tr (Lang::TextId::audioUnknown));
    editorBox       .setTooltip (tr (Lang::TextId::tipPaste));

    gearButton      .setTooltip (tr (Lang::TextId::tipGear));
    logoButton      .setTooltip (tr (Lang::TextId::tipLogo));
    zoomBox         .setTooltip (tr (Lang::TextId::tipZoom));
    skinBox         .setTooltip (tr (Lang::TextId::tipSkin));
    fontBox         .setTooltip (tr (Lang::TextId::tipTypeface));
    colourBox       .setTooltip (tr (Lang::TextId::tipColor));
    advanceBox      .setTooltip (tr (Lang::TextId::tipAdvance));
    languageBox     .setTooltip (tr (Lang::TextId::uiLanguage));
    orientHButton   .setTooltip (tr (Lang::TextId::tipLayout));
    orientVButton   .setTooltip (tr (Lang::TextId::tipLayout));
    pickImageButton .setTooltip (tr (Lang::TextId::tipPickImage));
    clearImageButton.setTooltip (tr (Lang::TextId::tipClearImage));
    closeSettingsButton.setTooltip (tr (Lang::TextId::tipClose));
    hintToggle      .setTooltip (tr (Lang::TextId::showHints));
    setStartButton  .setTooltip (tr (Lang::TextId::tipSetStart));
    undoSetStartButton.setTooltip (tr (Lang::TextId::tipUndoSetStart));
    annotateButton  .setTooltip (tr (Lang::TextId::tipAnnotate));
    onlineLyricsButton.setTooltip (tr (Lang::TextId::tipOnline));
    standalonePlayButton.setTooltip (tr (Lang::TextId::tipStandalone));
    closeSearchButton .setTooltip (tr (Lang::TextId::tipClose));
    customUrlEditor   .setTooltip (tr (Lang::TextId::onlineUrlHint));
    lxUrlEditor       .setTooltip (tr (Lang::TextId::lxUrlHint));
    simplifyToggle    .setTooltip (tr (Lang::TextId::onlineSimplifyHint));
    networkBox        .setTooltip (tr (Lang::TextId::netHint));
    searchLxButton    .setTooltip (tr (Lang::TextId::lxButtonHint));
    searchTitleEditor .setTooltip (tr (Lang::TextId::searchRefreshTip));
    hintLabel       .setTooltip (tr (Lang::TextId::hintClick));

    if (reloadDemoLyrics && usingDemoLyrics)
    {
        loadLyricsText (demoLyricsText (language), tr (Lang::TextId::demoLyrics), true);
        fileDescription = tr (Lang::TextId::demoFileDesc);
    }

    updateLabels (displayTime);
    repaint();
}

void LyricSyncEditor::refreshLookAndFeel()
{
    styleSidebarControls();

    auto& l = lookAndFeel;

    // 极夜玻璃：按钮本身几乎透明，只靠 1px 高光边和一层薄色区分
    l.setColour (juce::TextButton::buttonColourId,   palette.text.withAlpha (0.105f));
    l.setColour (juce::TextButton::buttonOnColourId, palette.accent.withAlpha (0.26f));
    l.setColour (juce::TextButton::textColourOffId,  palette.dim);
    l.setColour (juce::TextButton::textColourOnId,   palette.text);

    l.setColour (juce::Label::textColourId,          palette.dim);
    l.setColour (juce::Label::backgroundColourId,    juce::Colours::transparentBlack);

    l.setColour (juce::Slider::thumbColourId,        palette.accent);
    l.setColour (juce::Slider::trackColourId,        palette.accentSoft);
    l.setColour (juce::Slider::backgroundColourId,   palette.track);

    l.setColour (juce::TextEditor::backgroundColourId,      palette.card);
    l.setColour (juce::TextEditor::textColourId,            palette.text);
    l.setColour (juce::TextEditor::outlineColourId,         palette.panelEdge);
    l.setColour (juce::TextEditor::focusedOutlineColourId,  palette.accent);
    l.setColour (juce::TextEditor::highlightColourId,       palette.accentSoft.withAlpha (0.35f));

    l.setColour (juce::ComboBox::backgroundColourId,  palette.text.withAlpha (0.085f));
    l.setColour (juce::ComboBox::textColourId,        palette.text);
    l.setColour (juce::ComboBox::outlineColourId,     palette.panelEdge.withAlpha (0.55f));
    l.setColour (juce::ComboBox::arrowColourId,       palette.dim);
    l.setColour (juce::ComboBox::buttonColourId,      palette.panelEdge);
    l.setColour (juce::PopupMenu::backgroundColourId, palette.panel);
    l.setColour (juce::PopupMenu::textColourId,       palette.text);
    l.setColour (juce::PopupMenu::highlightedBackgroundColourId, palette.accentSoft);
    l.setColour (juce::PopupMenu::highlightedTextColourId,       palette.text);

    l.setColour (juce::TooltipWindow::backgroundColourId, palette.panel);
    l.setColour (juce::TooltipWindow::textColourId,       palette.text);
    l.setColour (juce::TooltipWindow::outlineColourId,    palette.panelEdge);

    // 步进器里的数值：白字、无底色（底色由 paintStepperBox 统一画）
    offsetLabel.setColour (juce::Label::textColourId, palette.text);
    offsetLabel.setColour (juce::Label::backgroundColourId, juce::Colours::transparentBlack);
    offsetLabel.addMouseListener (this, false);
    offsetLabel.setMouseCursor (juce::MouseCursor::LeftRightResizeCursor);
    fontLabel.addMouseListener (this, false);
    fontLabel.setMouseCursor (juce::MouseCursor::LeftRightResizeCursor);
    hintLabel.setColour (juce::Label::textColourId, palette.faint);
    fileLabel.setColour (juce::Label::textColourId, palette.dim);
    fontLabel.setColour (juce::Label::textColourId, palette.text);
    fontLabel.setColour (juce::Label::backgroundColourId, juce::Colours::transparentBlack);
    audioLabel.setColour (juce::Label::textColourId, palette.dim);

    closeSettingsButton.normalColour = palette.dim;
    closeSettingsButton.hoverColour  = palette.text;
    closeAboutButton.normalColour = palette.dim;
    closeAboutButton.hoverColour  = palette.text;
    // 搜索面板是浅色/白色底：X 不能再用默认的白色，否则浅色皮肤里几乎看不见
    closeSearchButton.normalColour = palette.text;
    closeSearchButton.hoverColour  = palette.accent;

    orientHButton.setToggleState (orientation == Orientation::horizontal,
                                  juce::dontSendNotification);
    orientVButton.setToggleState (orientation == Orientation::vertical,
                                  juce::dontSendNotification);

    editorBox.applyFontToAllText (uiFont (14.0f * uiScale));

    // 让所有子控件重新读一次颜色（ComboBox / Label 会缓存文字色，换皮肤后必须刷新）
    sendLookAndFeelChange();

    repaint();
}

void LyricSyncEditor::updateVisibility()
{
    if (zenMode)
    {
        for (auto* child : getChildren())
            if (child != tooltipWindow.get())
                child->setVisible (false);

        return;
    }

    const bool overlay = settingsOpen || aboutOpen || searchOpen;

    // 侧栏收起后整列控件一起隐藏（竖版仍走底部布局，不受影响）
    const bool railOpen = (orientation == Orientation::vertical) || railWidth() > 60.0f;
    // 着色是"就地"操作：主界面的按钮一个都不隐藏
    const bool normal  = ! editing && ! tapMode && ! overlay;

    for (auto* c : { static_cast<juce::Component*> (&loadButton),
                     static_cast<juce::Component*> (&pasteButton),
                     static_cast<juce::Component*> (&orientButton),
                     static_cast<juce::Component*> (&minusButton),
                     static_cast<juce::Component*> (&plusButton),
                     static_cast<juce::Component*> (&resetButton),
                     static_cast<juce::Component*> (&offsetLabel),
                     static_cast<juce::Component*> (&fontLabel),
                     static_cast<juce::Component*> (&fontMinusButton),
                     static_cast<juce::Component*> (&fontPlusButton),
                     static_cast<juce::Component*> (&fontSlider),
                     static_cast<juce::Component*> (&segLineButton),
                     static_cast<juce::Component*> (&segWordButton),
                     static_cast<juce::Component*> (&measureButton),
                     static_cast<juce::Component*> (&exportButton),
                     static_cast<juce::Component*> (&retempoButton),
                     static_cast<juce::Component*> (&gearButton),
                     static_cast<juce::Component*> (&castButton),
                     static_cast<juce::Component*> (&onlineLyricsButton),
                     static_cast<juce::Component*> (&searchTitleEditor) })
        c->setVisible (normal && railOpen);

    // 内联搜索框在窄轨（收起态）里放不下，横版收起时隐藏
    searchTitleEditor.setVisible (normal && railOpen
                                  && (orientation == Orientation::vertical || railWidth() > 60.0f));

    // 已废掉的控件：字号滑块换成步进器，在线搜词按钮换成内联搜索框
    fontSlider.setVisible (false);
    resetButton.setVisible (false);
    onlineLyricsButton.setVisible (false);

    // 右下角读数条接管了这些内容：重测音频废弃；测速 / 撤销 / 音频长度都挪进读数条
    measureButton.setVisible (false);
    retempoButton.setVisible (false);
    audioLabel   .setVisible (false);

    // 已下线的「示例歌词」与「手动预览」：一律不显示
    demoButton      .setVisible (false);
    demoClockToggle .setVisible (false);
    demoPlayButton  .setVisible (false);
    demoStopButton  .setVisible (false);

    setStartButton   .setVisible (normal);
    undoSetStartButton.setVisible (normal);
    annotateButton   .setVisible (normal);
    standalonePlayButton.setVisible (normal && standaloneMode);

    // 样式卡片
    const bool styleEdit = annotateMode && ! overlay && styleEditIndex >= 0;

    styleNoteEditor  .setVisible (styleEdit);
    styleApplyButton .setVisible (styleEdit);
    styleClearButton .setVisible (styleEdit);
    styleCancelButton.setVisible (styleEdit);

    // 纯享模式把上面这些都隐藏过，回来必须逐个恢复；
    // 这三个 Label 不在上面那批里，之前漏掉，导致退出全屏后提示词永久消失。
    fileLabel .setVisible (normal);
    audioLabel.setVisible (normal);
    hintLabel .setVisible (false);   // 左下角功能介绍按用户要求移除

    applyTextButton .setVisible (editing && ! overlay);
    cancelTextButton.setVisible (editing && ! overlay);
    tapStartButton  .setVisible (editing && ! overlay);

    const bool tapMain = tapMode && ! overlay && tapEditIndex < 0;

    padStampButton  .setVisible (tapMode && ! overlay);
    tapUndoButton   .setVisible (tapMain);
    tapRestartButton.setVisible (tapMain);
    tapNudgeBackButton.setVisible (tapMain);
    tapNudgeFwdButton .setVisible (tapMain);
    tapPauseButton    .setVisible (tapMain && demoMode);
    tapHostTransportButton.setVisible (tapMain);
    tapFinishButton .setVisible (tapMain);
    tapExitButton   .setVisible (tapMain);

    const bool rowEdit = tapMode && ! overlay && tapEditIndex >= 0;

    tapTextEditor   .setVisible (rowEdit);
    tapTimeEditor   .setVisible (rowEdit);
    tapRowRestampButton.setVisible (rowEdit);
    tapRowApplyButton  .setVisible (rowEdit);
    tapRowCancelButton .setVisible (rowEdit);
    for (auto* c : { static_cast<juce::Component*> (&languageBox),
                     static_cast<juce::Component*> (&advanceBox),
                     static_cast<juce::Component*> (&zoomBox),
                     static_cast<juce::Component*> (&skinBox),
                     static_cast<juce::Component*> (&fontBox),
                     static_cast<juce::Component*> (&colourBox),
                     static_cast<juce::Component*> (&orientHButton),
                     static_cast<juce::Component*> (&orientVButton),
                     static_cast<juce::Component*> (&pickImageButton),
                     static_cast<juce::Component*> (&clearImageButton),
                     static_cast<juce::Component*> (&closeSettingsButton),
                     static_cast<juce::Component*> (&hintToggle) })
        c->setVisible (settingsOpen);

    closeAboutButton.setVisible (aboutOpen);
    logoButton.setVisible (false);      // 左下角「关于」入口已移除，功能说明改为悬停提示

    // 旧版弹窗里的搜索控件：内联化之后全部不再显示
    for (auto* c : { static_cast<juce::Component*> (&searchArtistEditor),
                     static_cast<juce::Component*> (&searchGoButton),
                     static_cast<juce::Component*> (&searchLoadButton),
                     static_cast<juce::Component*> (&searchLxButton),
                     static_cast<juce::Component*> (&closeSearchButton) })
        c->setVisible (false);

    customUrlEditor.setVisible (settingsOpen);
    lxUrlEditor.setVisible (settingsOpen);
    simplifyToggle.setVisible (settingsOpen);
    networkBox.setVisible (settingsOpen);

    clearImageButton.setEnabled (customBg.isValid());
}

//==============================================================================
juce::Rectangle<int> LyricSyncEditor::scaledRect (float x, float y, float w, float h) const
{
    return juce::Rectangle<int> (
        juce::roundToInt (originX + x * uiScaleX),
        juce::roundToInt (originY + y * uiScaleY),
        juce::jmax (1, juce::roundToInt (w * uiScaleX)),
        juce::jmax (1, juce::roundToInt (h * uiScaleY)));
}

float LyricSyncEditor::railWidth() const
{
    // 竖版放不下 248 的侧栏，恒用窄轨
    if (orientation == Orientation::vertical)
        return 40.0f;

    return sidebarCollapsed ? 40.0f : 248.0f;
}

juce::Rectangle<float> LyricSyncEditor::railToggleRect() const
{
    const float w = railWidth();

    return { w - 9.0f, baseH() * 0.5f - 26.0f, 18.0f, 52.0f };
}

//==============================================================================
//  侧栏 / 底部面板几何：横版是一列 248 宽的侧栏，竖版把这些控件排到底部两条带里。
//==============================================================================
juce::Rectangle<float> LyricSyncEditor::sideTopRow() const
{
    if (orientation == Orientation::vertical)
        return { vShift() + 22.0f, 566.0f, 556.0f, 28.0f };

    return { 12.0f, 54.0f, 224.0f, 27.0f };
}

juce::Rectangle<float> LyricSyncEditor::sideSearchBox() const
{
    if (orientation == Orientation::vertical)
        return { vShift() + 22.0f, 600.0f, 556.0f, 32.0f };

    return { 12.0f, 89.0f, 224.0f, 32.0f };
}

/** 搜索框右侧的「换一个歌词源」小圆钮。 */
juce::Rectangle<float> LyricSyncEditor::sideSearchRefresh() const
{
    const auto box = sideSearchBox();

    return { box.getRight() - 30.0f, box.getY() + 4.0f, 26.0f, box.getHeight() - 8.0f };
}

juce::Rectangle<float> LyricSyncEditor::sideStartRow() const{
    if (orientation == Orientation::vertical)
        return { vShift() + 22.0f, 730.0f, 556.0f, 26.0f };

    return { 12.0f, 388.0f, 224.0f, 27.0f };
}

juce::Rectangle<float> LyricSyncEditor::sideOffsetStepper() const
{
    if (orientation == Orientation::vertical)
        return { vShift() + 86.0f, 762.0f, 492.0f, 26.0f };

    return { 12.0f, 430.0f, 224.0f, 27.0f };
}

juce::Rectangle<float> LyricSyncEditor::sideFontStepper() const
{
    if (orientation == Orientation::vertical)
        return { vShift() + 86.0f, 794.0f, 492.0f, 26.0f };

    return { 12.0f, 486.0f, 224.0f, 27.0f };
}

juce::Rectangle<float> LyricSyncEditor::sideSegArea() const
{
    if (orientation == Orientation::vertical)
        return { vShift() + 22.0f, 826.0f, 556.0f, 26.0f };

    return { 12.0f, 524.0f, 224.0f, 28.0f };
}

juce::Rectangle<float> LyricSyncEditor::sideToolsRow() const
{
    if (orientation == Orientation::vertical)
        return { vShift() + 22.0f, 860.0f, 556.0f, 26.0f };

    return { 12.0f, 560.0f, 224.0f, 27.0f };
}

juce::Rectangle<float> LyricSyncEditor::sideResultsArea() const
{
    const auto searchBox = sideSearchBox();
    const auto top = searchBox.getBottom() + 8.0f;
    const auto bottom = sideStartRow().getY() - 8.0f;

    if (orientation == Orientation::vertical)
        return { searchBox.getX(), top, searchBox.getWidth(),
                 juce::jmax (0.0f, bottom - top) };

    return { 12.0f, top, 224.0f, juce::jmax (0.0f, bottom - top) };
}

std::vector<juce::Rectangle<float>> LyricSyncEditor::sideResultRows() const
{
    std::vector<juce::Rectangle<float>> rows;

    const auto area = sideResultsArea();
    const float rowH = orientation == Orientation::vertical ? 38.0f : 46.0f;
    const float gap  = 4.0f;

    const int total = static_cast<int> (searchResults.size());
    const int fit   = juce::jmax (1, static_cast<int> ((area.getHeight() + gap) / (rowH + gap)));
    const int maxScroll = juce::jmax (0, total - fit);
    const int first = juce::jlimit (0, maxScroll, searchScroll);

    for (int i = 0; i < fit && first + i < total; ++i)
        rows.push_back ({ area.getX(), area.getY() + static_cast<float> (i) * (rowH + gap),
                          area.getWidth(), rowH });

    return rows;
}

int LyricSyncEditor::sideResultAt (juce::Point<float> logical) const
{
    const auto rows = sideResultRows();

    for (int i = 0; i < static_cast<int> (rows.size()); ++i)
        if (rows[(size_t) i].contains (logical))
            return searchScroll + i;

    return -1;
}

juce::Rectangle<float> LyricSyncEditor::stageArea() const
{
    if (zenMode)
        return { 10.0f, 10.0f, baseW() - 20.0f, baseH() - 20.0f };

    const float rail = railWidth();

    if (orientation == Orientation::vertical)
        return { rail + 10.0f, 72.0f, baseW() - rail - 20.0f, 486.0f };

    return { rail + 14.0f, 56.0f, hBaseW - rail - 28.0f, hBaseH - 56.0f - 84.0f };
}

void LyricSyncEditor::toggleSidebar()
{
    sidebarCollapsed = ! sidebarCollapsed;
    saveUiPrefs();
    updateVisibility();
    resized();
    repaint();
}

juce::Rectangle<float> LyricSyncEditor::settingsPanelArea() const
{
    if (orientation == Orientation::vertical)
        return { juce::jmax (20.0f, (baseW() - 560.0f) * 0.5f), 100.0f, 560.0f, 620.0f };

    // 12 行设置，行距收到 40
    return { 200.0f, 10.0f, 560.0f, 580.0f };
}

juce::Rectangle<float> LyricSyncEditor::aboutPanelArea() const
{
    if (orientation == Orientation::vertical)
        return { 44.0f, 210.0f, 512.0f, 460.0f };

    return { 230.0f, 120.0f, 500.0f, 370.0f };
}

juce::Rectangle<float> LyricSyncEditor::searchPanelArea() const
{
    if (orientation == Orientation::vertical)
        return { juce::jmax (20.0f, (baseW() - 560.0f) * 0.5f), 140.0f, 560.0f, 540.0f };

    return { 200.0f, 30.0f, 560.0f, 540.0f };
}

std::vector<juce::Rectangle<float>> LyricSyncEditor::searchResultRows (
    juce::Rectangle<float> panel) const
{
    std::vector<juce::Rectangle<float>> rows;
    const float x = panel.getX() + 24.0f;
    const float y = panel.getY() + 200.0f;
    const float w = panel.getWidth() - 48.0f;

    for (int i = 0; i < 6; ++i)
        rows.push_back ({ x, y + static_cast<float> (i) * 30.0f, w, 28.0f });

    return rows;
}

/** 侧栏控件统一成方案里的扁平样式：透明底 + 1px 细灰边、灰字，选中态才用强调色。 */
void LyricSyncEditor::styleSidebarControls()
{
    const bool light = palette.text.getPerceivedBrightness() < 0.5f;

    const auto bg   = light ? juce::Colour (0xfff2f4f8) : juce::Colour (0xff0e0e11);
    const auto fg   = light ? juce::Colour (0xff4a5160) : juce::Colour (0xffa1a1aa);
    const auto onBg = light ? juce::Colour (0xffd9f2ea) : juce::Colour (0xff1b3330);
    const auto onFg = light ? juce::Colour (0xff0e9e86) : juce::Colour (0xff5ce1c1);

    for (auto* b : { &loadButton, &pasteButton, &onlineLyricsButton,
                     &setStartButton,
                     &annotateButton, &exportButton, &gearButton, &castButton,
                     &resetButton })
    {
        b->setColour (juce::TextButton::buttonColourId, bg);
        b->setColour (juce::TextButton::buttonOnColourId, onBg);
        b->setColour (juce::TextButton::textColourOffId, fg);
        b->setColour (juce::TextButton::textColourOnId, onFg);
        b->getProperties().set ("flat", true);
    }

    // 步进器内部的 ‹ › / A- A+：没有自己的边框（外框由 paintStepperBox 画）
    for (auto* b : { &minusButton, &plusButton, &fontMinusButton, &fontPlusButton })
    {
        b->setColour (juce::TextButton::buttonColourId, bg);
        b->setColour (juce::TextButton::buttonOnColourId, onBg);
        b->setColour (juce::TextButton::textColourOffId,
                      light ? juce::Colour (0xff8b94a6) : juce::Colour (0xff7a7a85));
        b->setColour (juce::TextButton::textColourOnId, onFg);
        b->getProperties().set ("bare", true);
    }

    // 横竖切换：图标按钮也做成一格按钮，前片用侧栏底色遮挡
    orientButton.normalColour = fg;
    orientButton.hoverColour  = light ? juce::Colour (0xff2b3242) : juce::Colour (0xfff2f2f4);
    orientButton.boxed = true;
    orientButton.boxFill = bg;

    undoSetStartButton.normalColour = light ? juce::Colour (0xff8b94a6)
                                            : juce::Colour (0xff8e8e99);
    undoSetStartButton.hoverColour  = light ? juce::Colour (0xff2b3242)
                                            : juce::Colour (0xfff2f2f4);

    // 分段开关：底槽自己画，两个按钮裸色块
    for (auto* b : { &segLineButton, &segWordButton })
    {
        b->setColour (juce::TextButton::buttonColourId, bg);
        b->setColour (juce::TextButton::buttonOnColourId,
                      light ? juce::Colour (0xffffffff) : juce::Colour (0xfff3f3f5));
        b->setColour (juce::TextButton::textColourOffId,
                      light ? juce::Colour (0xff5d6883) : juce::Colour (0xff63636d));
        b->setColour (juce::TextButton::textColourOnId, juce::Colour (0xff101013));
        b->getProperties().set ("bare", true);
    }

    // 内联搜索框：框体在 paintSidebarSearch 里画，编辑器本身全透明
    searchTitleEditor.setColour (juce::TextEditor::backgroundColourId, juce::Colours::transparentBlack);
    searchTitleEditor.setColour (juce::TextEditor::outlineColourId, juce::Colours::transparentBlack);
    searchTitleEditor.setColour (juce::TextEditor::focusedOutlineColourId, juce::Colours::transparentBlack);
    searchTitleEditor.setColour (juce::TextEditor::textColourId, light ? juce::Colour (0xff1a2233)
                                                                       : juce::Colour (0xfff2f2f4));
    searchTitleEditor.setColour (juce::TextEditor::highlightColourId,
                                 palette.accent.withAlpha (0.22f));
    searchTitleEditor.setColour (juce::CaretComponent::caretColourId, palette.accent);
    searchTitleEditor.setTextToShowWhenEmpty (tr (Lang::TextId::searchPlaceholder),
                                              light ? juce::Colour (0xff98a2b8)
                                                    : juce::Colour (0xff63636d));

    // 「记录本句」：强调色描边 + 淡青底
    padStampButton.setColour (juce::TextButton::buttonColourId, onBg);
    padStampButton.setColour (juce::TextButton::buttonOnColourId, onBg);
    padStampButton.setColour (juce::TextButton::textColourOffId, onFg);
    padStampButton.setColour (juce::TextButton::textColourOnId, onFg);
    padStampButton.getProperties().set ("flat", true);

    offsetLabel.setColour (juce::Label::backgroundColourId, juce::Colours::transparentBlack);
    offsetLabel.setColour (juce::Label::textColourId, light ? juce::Colour (0xff2b3242)
                                                            : juce::Colour (0xfff2f2f4));
}

void LyricSyncEditor::updateScaledFonts()
{
    offsetLabel.setFont (uiFont (11.5f * uiScale));
    fileLabel  .setFont (uiFont (11.5f * uiScale));
    hintLabel  .setFont (uiFont (11.0f  * uiScale));
    fontLabel  .setFont (uiFont (11.5f * uiScale));
    audioLabel .setFont (uiFont (11.5f * uiScale));
    editorBox  .setFont (uiFont (14.0f  * uiScale));
    editorBox.applyFontToAllText (uiFont (14.0f * uiScale));

    tapTextEditor.applyFontToAllText (uiFont (13.0f * uiScale));
    tapTimeEditor.applyFontToAllText (uiFont (13.0f * uiScale));
    styleNoteEditor.applyFontToAllText (uiFont (13.0f * uiScale));
    for (auto* ed : { &searchTitleEditor, &searchArtistEditor, &customUrlEditor,
                      &lxUrlEditor })
        ed->applyFontToAllText (uiFont (12.0f * uiScale));

    searchTitleEditor.setTextToShowWhenEmpty (tr (Lang::TextId::onlineSongPlaceholder),
                                              palette.faint.withAlpha (0.6f));
    searchArtistEditor.setTextToShowWhenEmpty (tr (Lang::TextId::onlineArtistPlaceholder),
                                               palette.faint.withAlpha (0.6f));
    customUrlEditor.setTextToShowWhenEmpty (tr (Lang::TextId::onlineUrlPlaceholder),
                                            palette.faint.withAlpha (0.6f));
    lxUrlEditor.setTextToShowWhenEmpty (tr (Lang::TextId::lxUrlPlaceholder),
                                        palette.faint.withAlpha (0.6f));
    styleNoteEditor.setTextToShowWhenEmpty (tr (Lang::TextId::annotateNotePlaceholder),
                                            palette.faint.withAlpha (0.6f));
}

void LyricSyncEditor::applyWindowSize()
{
    // 全屏纯享时窗口尺寸由宿主/用户控制，不要在这里重设。
    if (zenMode)
        return;

    // 用版型的「基准尺寸」定窗口，而不是拿当前窗口比例去反推 baseW()。
    // 否则从横版切到竖版时会沿用 16:10 的宽比例，竖版看起来只是放大的横版。
    // 横版 960×600，竖版 600×900（2:3 竖长），再乘缩放档位。
    const float bw = (orientation == Orientation::vertical) ? vBaseW : hBaseW;
    const float bh = (orientation == Orientation::vertical) ? vBaseH : hBaseH;
    const int w = juce::roundToInt (bw * static_cast<float> (zoomPercent) / 100.0f);
    const int h = juce::roundToInt (bh * static_cast<float> (zoomPercent) / 100.0f);
    setSize (w, h);
}

void LyricSyncEditor::resized()
{
    const float w = static_cast<float> (getWidth());
    const float h = static_cast<float> (getHeight());

    float sx = w / baseW();
    float sy = h / baseH();

    // 允许拖上下左右边框做横向 / 纵向拉伸；但如果比例差太多会严重变形，
    // 这时退回等比（居中留边），保证文字始终好读。
    const float ratio = sx / juce::jmax (0.01f, sy);
    if (! zenMode && (ratio > 1.35f || ratio < 0.74f))
    {
        const float s = juce::jmin (sx, sy);
        sx = sy = s;
    }

    uiScaleX = juce::jmax (0.1f, sx);
    uiScaleY = juce::jmax (0.1f, sy);
    uiScale  = std::sqrt (uiScaleX * uiScaleY);

    originX = (w - baseW() * uiScaleX) * 0.5f;
    originY = (h - baseH() * uiScaleY) * 0.5f;
    uiTransform = juce::AffineTransform (uiScaleX, 0.0f, originX,
                                         0.0f, uiScaleY, originY);
    lookAndFeel.scale = uiScale;
    lookAndFeel.typeface = uiTypefaceName();

    updateScaledFonts();

    const auto stage = stageArea();

    logoButton.setBounds (scaledRect (14, 4,
                                      orientation == Orientation::vertical ? 250.0f : 310.0f,
                                      54));

    if (orientation == Orientation::horizontal)
    {
        const float bottom = baseH();

        editorBox.setBounds (scaledRect (stage.getX() + 2.0f, stage.getY() + 2.0f,
                                         stage.getWidth() - 4.0f, stage.getHeight() - 4.0f));

        //---- 顶部：载入 / 编辑 / 横竖切换（三格等宽）----
        {
            const auto row = sideTopRow();
            const float thirdW = juce::jmax (8.0f, (row.getWidth() - 8.0f) / 3.0f);

            loadButton .setBounds (scaledRect (row.getX(), row.getY(), thirdW, row.getHeight()));
            pasteButton.setBounds (scaledRect (row.getX() + thirdW + 4.0f, row.getY(),
                                               thirdW, row.getHeight()));
            orientButton.setBounds (scaledRect (row.getX() + (thirdW + 4.0f) * 2.0f, row.getY(),
                                                thirdW, row.getHeight()));
        }

        //---- 内联搜索框 ----
        {
            const auto box = sideSearchBox();
            const auto refresh = sideSearchRefresh();

            searchTitleEditor.setBounds (scaledRect (box.getX() + 30.0f, box.getY() + 5.0f,
                                                     refresh.getX() - box.getX() - 36.0f,
                                                     box.getHeight() - 10.0f));
        }

        //---- 起点：文字 + 设起点 + 撤销 ----
        {
            const auto row = sideStartRow();
            const float undoW = 26.0f;
            const float setW  = 52.0f;

            setStartButton.setBounds (scaledRect (row.getRight() - setW - 4.0f - undoW, row.getY(),
                                                  setW, row.getHeight()));
            undoSetStartButton.setBounds (scaledRect (row.getRight() - undoW, row.getY(),
                                                      undoW, row.getHeight()));
        }

        //---- 偏移 / 字号步进器 ----
        {
            const auto off = sideOffsetStepper();
            const float btnW = 27.0f;
            const float midW = juce::jmax (20.0f, off.getWidth() - btnW * 2.0f);

            minusButton.setBounds (scaledRect (off.getX(), off.getY(), btnW, off.getHeight()));
            offsetLabel.setBounds (scaledRect (off.getX() + btnW, off.getY(), midW, off.getHeight()));
            plusButton .setBounds (scaledRect (off.getRight() - btnW, off.getY(),
                                               btnW, off.getHeight()));

            const auto fnt = sideFontStepper();

            fontMinusButton.setBounds (scaledRect (fnt.getX(), fnt.getY(), 30.0f, fnt.getHeight()));
            fontLabel      .setBounds (scaledRect (fnt.getX() + 30.0f, fnt.getY(),
                                                   juce::jmax (20.0f, fnt.getWidth() - 60.0f),
                                                   fnt.getHeight()));
            fontPlusButton .setBounds (scaledRect (fnt.getRight() - 30.0f, fnt.getY(),
                                                   30.0f, fnt.getHeight()));
        }

        //---- 逐行 / 逐字 ----
        {
            const auto seg = sideSegArea();
            const float halfW = (seg.getWidth() - 4.0f) * 0.5f;

            segLineButton.setBounds (scaledRect (seg.getX() + 2.0f, seg.getY() + 2.0f,
                                                 halfW - 2.0f, seg.getHeight() - 4.0f));
            segWordButton.setBounds (scaledRect (seg.getX() + halfW + 2.0f, seg.getY() + 2.0f,
                                                 halfW - 2.0f, seg.getHeight() - 4.0f));
        }

        //---- 底部：设置 / 着色 / 导出 / 投屏 ----
        {
            const auto row = sideToolsRow();
            const float w4 = juce::jmax (8.0f, (row.getWidth() - 12.0f) / 4.0f);

            gearButton    .setBounds (scaledRect (row.getX(), row.getY(), w4, row.getHeight()));
            annotateButton.setBounds (scaledRect (row.getX() + w4 + 4.0f, row.getY(), w4, row.getHeight()));
            exportButton  .setBounds (scaledRect (row.getX() + (w4 + 4.0f) * 2.0f, row.getY(),
                                                  w4, row.getHeight()));
            castButton    .setBounds (scaledRect (row.getX() + (w4 + 4.0f) * 3.0f, row.getY(),
                                                  w4, row.getHeight()));
        }

        //---- 编辑 / 打轴模式的按钮：放到舞台下方那条空白带 ----
        if (editing)
        {
            applyTextButton .setBounds (scaledRect (stage.getX(), bottom - 78.0f, 96.0f, 28.0f));
            cancelTextButton.setBounds (scaledRect (stage.getX() + 104.0f, bottom - 78.0f, 76.0f, 28.0f));
            tapStartButton  .setBounds (scaledRect (stage.getX() + 190.0f, bottom - 78.0f, 104.0f, 28.0f));
        }
        else if (tapMode)
        {
            float tx = stage.getX();

            auto put = [&] (juce::Component& c, float w2)
            {
                c.setBounds (scaledRect (tx, bottom - 78.0f, w2, 28.0f));
                tx += w2 + 6.0f;
            };

            // 侧栏展开时「记录本句」放侧栏底部；收起后才挤进舞台按钮行
            if (railWidth() > 60.0f)
            {
                padStampButton.setBounds (scaledRect (12.0f, baseH() - 66.0f,
                                                      railWidth() - 24.0f, 30.0f));
            }
            else
            {
                put (padStampButton, 96.0f);
            }

            put (tapUndoButton, 74.0f);
            put (tapRestartButton, 96.0f);
            put (tapNudgeBackButton, 62.0f);
            put (tapNudgeFwdButton, 62.0f);
            put (tapHostTransportButton, 92.0f);
            put (tapPauseButton, 84.0f);
            put (tapFinishButton, 84.0f);
            put (tapExitButton, 74.0f);
        }

        fileLabel .setBounds (scaledRect (stage.getX(), 32.0f, stage.getWidth() - 8.0f, 16.0f));
        hintLabel .setBounds (scaledRect (stage.getX(), bottom - 22.0f,
                                          stage.getWidth() * 0.78f, 18.0f));

    }
    else
    {
        const float ox = vShift();

        gearButton   .setBounds (scaledRect (baseW() - 68.0f, 12, 46, 46));

        editorBox.setBounds (scaledRect (stage.getX() + 2.0f, stage.getY() + 2.0f,
                                         stage.getWidth() - 4.0f, stage.getHeight() - 4.0f));

        if (editing)
        {
            applyTextButton .setBounds (scaledRect (ox + 22, 566, 176, 30));
            cancelTextButton.setBounds (scaledRect (ox + 206, 566, 176, 30));
            tapStartButton  .setBounds (scaledRect (ox + 390, 566, 176, 30));
        }
        else if (tapMode)
        {
            padStampButton     .setBounds (scaledRect (ox + 22, 566, 150, 30));
            tapUndoButton      .setBounds (scaledRect (ox + 180, 566, 120, 30));
            tapRestartButton   .setBounds (scaledRect (ox + 308, 566, 120, 30));
            tapFinishButton    .setBounds (scaledRect (ox + 436, 566, 120, 30));

            tapNudgeBackButton .setBounds (scaledRect (ox + 22, 602, 96, 30));
            tapNudgeFwdButton  .setBounds (scaledRect (ox + 126, 602, 96, 30));
            tapPauseButton     .setBounds (scaledRect (ox + 230, 602, 104, 30));
            tapHostTransportButton.setBounds (scaledRect (ox + 342, 602, 124, 30));
            tapExitButton      .setBounds (scaledRect (ox + 474, 602, 84, 30));
        }
        else
        {
            // 顶部：载入 / 编辑 / 横竖
            const auto row = sideTopRow();
            const float thirdW = juce::jmax (8.0f, (row.getWidth() - 8.0f) / 3.0f);

            loadButton .setBounds (scaledRect (row.getX(), row.getY(), thirdW, row.getHeight()));
            pasteButton.setBounds (scaledRect (row.getX() + thirdW + 4.0f, row.getY(),
                                               thirdW, row.getHeight()));
            orientButton.setBounds (scaledRect (row.getX() + (thirdW + 4.0f) * 2.0f, row.getY(),
                                                thirdW, row.getHeight()));

            const auto box = sideSearchBox();
            const auto refresh = sideSearchRefresh();

            searchTitleEditor.setBounds (scaledRect (box.getX() + 30.0f, box.getY() + 5.0f,
                                                     refresh.getX() - box.getX() - 36.0f,
                                                     box.getHeight() - 10.0f));

            const auto start = sideStartRow();
            const float undoW = 26.0f;
            const float setW  = 52.0f;

            setStartButton.setBounds (scaledRect (start.getRight() - setW - 4.0f - undoW,
                                                  start.getY(), setW, start.getHeight()));
            undoSetStartButton.setBounds (scaledRect (start.getRight() - undoW, start.getY(),
                                                      undoW, start.getHeight()));

            const auto off = sideOffsetStepper();
            const float btnW = 27.0f;

            minusButton.setBounds (scaledRect (off.getX(), off.getY(), btnW, off.getHeight()));
            offsetLabel.setBounds (scaledRect (off.getX() + btnW, off.getY(),
                                               juce::jmax (20.0f, off.getWidth() - btnW * 2.0f),
                                               off.getHeight()));
            plusButton .setBounds (scaledRect (off.getRight() - btnW, off.getY(),
                                               btnW, off.getHeight()));

            const auto fnt = sideFontStepper();

            fontMinusButton.setBounds (scaledRect (fnt.getX(), fnt.getY(), 40.0f, fnt.getHeight()));
            fontLabel      .setBounds (scaledRect (fnt.getX() + 40.0f, fnt.getY(),
                                                   juce::jmax (20.0f, fnt.getWidth() - 90.0f),
                                                   fnt.getHeight()));
            fontPlusButton .setBounds (scaledRect (fnt.getRight() - 50.0f, fnt.getY(),
                                                   50.0f, fnt.getHeight()));

            const auto seg = sideSegArea();
            const float segHalf = (seg.getWidth() - 4.0f) * 0.5f;

            segLineButton.setBounds (scaledRect (seg.getX() + 2.0f, seg.getY() + 2.0f,
                                                 segHalf - 2.0f, seg.getHeight() - 4.0f));
            segWordButton.setBounds (scaledRect (seg.getX() + segHalf + 2.0f, seg.getY() + 2.0f,
                                                 segHalf - 2.0f, seg.getHeight() - 4.0f));

            const auto tools = sideToolsRow();
            const float w4 = juce::jmax (8.0f, (tools.getWidth() - 12.0f) / 4.0f);

            gearButton    .setBounds (scaledRect (tools.getX(), tools.getY(), w4, tools.getHeight()));
            annotateButton.setBounds (scaledRect (tools.getX() + w4 + 4.0f, tools.getY(),
                                                  w4, tools.getHeight()));
            exportButton  .setBounds (scaledRect (tools.getX() + (w4 + 4.0f) * 2.0f, tools.getY(),
                                                  w4, tools.getHeight()));
            castButton    .setBounds (scaledRect (tools.getX() + (w4 + 4.0f) * 3.0f, tools.getY(),
                                                  w4, tools.getHeight()));

            standalonePlayButton.setBounds (scaledRect (ox + 22, 566, 88, 30));
            standaloneSeekBar = { ox + 120.0f, 566.0f, 458.0f, 30.0f };
        }

        fileLabel .setBounds (scaledRect (22, 40, baseW() - 44.0f, 18));
        hintLabel .setBounds (scaledRect (22, 56, baseW() - 44.0f, 16));
        audioLabel.setBounds (scaledRect (ox + 22, 40, 240, 18));

    }

    // 浮动小选择框里的控件：横竖版同一套坐标，跟着右键位置走
    {
        const auto card = styleCard();
        styleNoteEditor  .setBounds (scaledRect (card.getX() + 58.0f, card.getY() + 90.0f,
                                                 card.getWidth() - 82.0f, 26.0f));
        styleApplyButton .setBounds (scaledRect (card.getX() + 24.0f,  card.getY() + 136.0f, 96.0f, 28.0f));
        styleClearButton .setBounds (scaledRect (card.getX() + 130.0f, card.getY() + 136.0f, 96.0f, 28.0f));
        styleCancelButton.setBounds (scaledRect (card.getX() + 236.0f, card.getY() + 136.0f, 96.0f, 28.0f));
    }

    //---- Settings panel -----------------------------------------------------
    {
        const auto panel = settingsPanelArea();
        const bool v = (orientation == Orientation::vertical);
        const float ctrlX = panel.getX() + (v ? 168.0f : 190.0f);
        const float ctrlW = panel.getRight() - 24.0f - ctrlX;
        const float rowH = 40.0f;

        closeSettingsButton.setBounds (scaledRect (panel.getRight() - 46.0f,
                                                   panel.getY() + 12.0f, 34.0f, 34.0f));

        float y = panel.getY() + 92.0f;

        languageBox.setBounds (scaledRect (ctrlX, y, ctrlW, 28));  y += rowH;
        zoomBox    .setBounds (scaledRect (ctrlX, y, ctrlW, 28));  y += rowH;

        orientHButton.setBounds (scaledRect (ctrlX, y, ctrlW * 0.48f, 28));
        orientVButton.setBounds (scaledRect (ctrlX + ctrlW * 0.52f, y, ctrlW * 0.48f, 28));
        y += rowH;

        skinBox.setBounds (scaledRect (ctrlX, y, ctrlW, 28));  y += rowH;

        pickImageButton .setBounds (scaledRect (ctrlX, y, ctrlW * 0.57f - 4.0f, 28));
        clearImageButton.setBounds (scaledRect (ctrlX + ctrlW * 0.57f + 4.0f, y,
                                                ctrlW * 0.43f - 4.0f, 28));
        y += rowH;

        fontBox  .setBounds (scaledRect (ctrlX, y, ctrlW, 28));  y += rowH;
        colourBox.setBounds (scaledRect (ctrlX, y, ctrlW, 28));  y += rowH;
        advanceBox.setBounds (scaledRect (ctrlX, y, ctrlW, 28)); y += rowH;
        hintToggle.setBounds (scaledRect (ctrlX, y, ctrlW * 0.4f, 28));
        customUrlEditor.setBounds (scaledRect (ctrlX, y + rowH, ctrlW, 28));
        lxUrlEditor.setBounds (scaledRect (ctrlX, y + 2.0f * rowH, ctrlW, 28));
        simplifyToggle.setBounds (scaledRect (ctrlX, y + 3.0f * rowH, ctrlW * 0.46f, 28));
        networkBox.setBounds (scaledRect (ctrlX + ctrlW * 0.5f + 6.0f, y + 3.0f * rowH,
                                          ctrlW * 0.5f - 6.0f, 28));
    }

    //---- 在线搜词面板已改为侧栏内联，这里不再布局（控件全部隐藏） ----------

    //---- Tap row editor -----------------------------------------------------
    if (tapEditIndex >= 0)
    {
        if (orientation == Orientation::vertical)
        {
            const float cx = 40.0f, cy = 300.0f, cw = baseW() - 80.0f;

            tapTextEditor.setBounds (scaledRect (cx + 20, cy + 50, cw - 40, 30));
            tapTimeEditor.setBounds (scaledRect (cx + 84, cy + 92, 150, 26));
            tapRowRestampButton.setBounds (scaledRect (cx + 244, cy + 90, 120, 30));
            tapRowApplyButton  .setBounds (scaledRect (cx + 20, cy + 140, 150, 30));
            tapRowCancelButton .setBounds (scaledRect (cx + 180, cy + 140, 150, 30));
        }
        else
        {
            const auto rr = tapEditRowRect();

            if (! rr.isEmpty())
            {
                // 侧栏展开：时间码 / 歌词上下两行，直接盖在那一行的位置上
                if (railWidth() > 60.0f)
                {
                    tapTimeEditor.setBounds (scaledRect (rr.getX() + 24.0f, rr.getY() + 1.0f,
                                                         rr.getWidth() - 58.0f, 19.0f));
                    tapTextEditor.setBounds (scaledRect (rr.getX() + 24.0f, rr.getY() + 20.0f,
                                                         rr.getWidth() - 58.0f, 19.0f));
                }
                else
                {
                    // 收起态：列表还在舞台右侧，维持原来的单行排布
                    const float rowH = rr.getHeight() - 2.0f;

                    tapTimeEditor.setBounds (scaledRect (rr.getX() + 24.0f, rr.getY() + 1.0f,
                                                         92.0f, rowH));
                    tapTextEditor.setBounds (scaledRect (rr.getX() + 122.0f, rr.getY() + 1.0f,
                                                         juce::jmax (60.0f, rr.getWidth() - 166.0f),
                                                         rowH));
                }

                // 三个动作按钮落在列表下方那一条
                const float by = stage.getBottom() - 32.0f;

                tapRowRestampButton.setBounds (scaledRect (stage.getX(), by, 108.0f, 26.0f));
                tapRowApplyButton  .setBounds (scaledRect (stage.getX() + 116.0f, by, 108.0f, 26.0f));
                tapRowCancelButton .setBounds (scaledRect (stage.getX() + 232.0f, by, 108.0f, 26.0f));
            }
            else
            {
                const float cx = 60.0f, cy = 250.0f, cw = 540.0f;

                tapTextEditor.setBounds (scaledRect (cx + 24, cy + 52, cw - 48, 30));
                tapTimeEditor.setBounds (scaledRect (cx + 88, cy + 92, 160, 26));
                tapRowRestampButton.setBounds (scaledRect (cx + 312, cy + 90, 110, 30));
                tapRowApplyButton  .setBounds (scaledRect (cx + 24, cy + 140, 120, 30));
                tapRowCancelButton .setBounds (scaledRect (cx + 156, cy + 140, 120, 30));
            }
        }
    }

    //---- About panel --------------------------------------------------------
    {
        const auto panel = aboutPanelArea();
        closeAboutButton.setBounds (scaledRect (panel.getRight() - 46.0f,
                                                panel.getY() + 12.0f, 34.0f, 34.0f));
    }

    processor.setSetting (Settings::windowW, getWidth());
    processor.setSetting (Settings::windowH, getHeight());
}

//==============================================================================
juce::Range<double> LyricSyncEditor::audioRange() const
{
    const double s = processor.audioStartSeconds.load();
    const double e = processor.audioEndSeconds.load();

    if (s >= 0.0 && e > s + 1.0)
        return { s, e };

    if (audioTotalSeconds > 1.0)
        return { 0.0, audioTotalSeconds };

    return { 0.0, juce::jmax (5.0, processor.lyrics().totalDuration()) };
}

//==============================================================================
void LyricSyncEditor::paint (juce::Graphics& g)
{
    const auto bounds = getLocalBounds().toFloat();

    if (skinIndex == 5 && customBg.isValid())
    {
        // 图片按"填满窗口"等比裁剪
        const float scale = juce::jmax (bounds.getWidth() / static_cast<float> (customBg.getWidth()),
                                        bounds.getHeight() / static_cast<float> (customBg.getHeight()));
        const float iw = static_cast<float> (customBg.getWidth()) * scale;
        const float ih = static_cast<float> (customBg.getHeight()) * scale;

        g.drawImage (customBg, juce::Rectangle<float> ((bounds.getWidth() - iw) * 0.5f,
                                                       (bounds.getHeight() - ih) * 0.5f, iw, ih));

        g.setColour (palette.bgTop.withAlpha (0.68f));
        g.fillAll();
    }
    else
    {
        // 深色皮肤：整块近黑，不做任何柔光 / 圈圈，保持极简
        if (palette.text.getPerceivedBrightness() > 0.5f)
        {
            g.setColour (juce::Colour (0xff08080a));
            g.fillAll();
        }
        else
        {
            juce::ColourGradient gradient (palette.bgTop, 0.0f, 0.0f,
                                           palette.bgBottom, 0.0f, bounds.getHeight(), false);
            g.setGradientFill (gradient);
            g.fillAll();
        }
    }

    // 逻辑画布
    const juce::Graphics::ScopedSaveState state (g);
    g.addTransform (uiTransform);

    if (! zenMode)
        paintSidebar (g);

    // 着色模式不切换界面：歌词舞台照常绘制，只叠一层选区高亮与浮动小框
    paintStage (g, stageArea());

    if (annotateMode)
        paintAnnotateOverlay (g, stageArea());

    if (zenMode)
        drawText (g, juce::Rectangle<float> (0.0f, baseH() - 34.0f, baseW(), 20.0f),
                  tr (Lang::TextId::zenExit), 11.0f,
                  palette.faint.withAlpha (0.45f), false);
    else
        paintInfoLine (g);

    if (standaloneMode)
        paintStandaloneTransport (g);

    // 走带读数条：编辑 / 打轴 / 弹面板时让位，其余时候常驻
    if (! zenMode && ! editing && ! tapMode && ! annotateMode
        && ! settingsOpen && ! aboutOpen && ! searchOpen)
        paintHud (g);

    if (settingsOpen)
        paintSettings (g, settingsPanelArea());

    if (aboutOpen)
        paintAbout (g, aboutPanelArea());

    if (searchOpen)
        paintSearch (g, searchPanelArea());
}

void LyricSyncEditor::paintOverChildren (juce::Graphics& g)
{
    // 「着色」按钮只保留文字，不再在前面画刷子图标；
    // 进入着色模式时用一圈强调色描边提示状态。
    if (! annotateButton.isVisible() || ! annotateMode)
        return;

    const auto b = annotateButton.getBounds().toFloat();

    g.setColour (palette.accent.withAlpha (0.75f));
    g.drawRoundedRectangle (b.reduced (0.8f), 7.0f, 1.4f);
}

void LyricSyncEditor::paintHeader (juce::Graphics& g)
{
    g.setColour (palette.accent);
    g.fillRoundedRectangle (22.0f, 20.0f, 7.0f, 25.0f, 3.5f);
    g.setColour (palette.accent.withAlpha (0.35f));
    g.fillRoundedRectangle (33.0f, 27.0f, 5.0f, 11.0f, 2.5f);

    drawText (g, juce::Rectangle<float> (48.0f, 10.0f, 250.0f, 28.0f),
              "LyricSync", 20.0f, palette.text, true, -1, {}, 1.0f,
              juce::Justification::centredLeft);

    g.setColour (palette.faint.withAlpha (0.85f));
    g.setFont (uiFont (10.0f));
    g.drawText (juce::String ("v") + kVersion,
                juce::Rectangle<float> (160.0f, 17.0f, 80.0f, 16.0f),
                juce::Justification::centredLeft, false);

    if (orientation == Orientation::horizontal)
        drawText (g, juce::Rectangle<float> (48.0f, 35.0f, 330.0f, 18.0f),
                  tr (Lang::TextId::appSubtitle), 11.0f, palette.faint, false, -1, {}, 1.0f,
                  juce::Justification::centredLeft);

    // 时间码 + 状态：直接用 Graphics 画（不用 Label 组件），
    // 这样右上方永远只有这两个字符串，不会出现任何标签叠加/残影。
    {
        const bool v = (orientation == Orientation::vertical);
        const float rightEdge = baseW() - 100.0f;
        const float textW = v ? 236.0f : 350.0f;
        const auto area = juce::Rectangle<float> (rightEdge - textW, 0.0f, textW, 56.0f);

        g.setColour (palette.text);
        g.setFont (uiFont (21.0f, true));
        g.drawText (timecodeText (displayTime),
                    area.withHeight (34.0f).translated (0.0f, 3.0f),
                    juce::Justification::centredRight, false);

        g.setColour (palette.dim);
        g.setFont (uiFont (10.5f));
        g.drawText (statusText(),
                    area.withTrimmedTop (38.0f).withHeight (17.0f),
                    juce::Justification::centredRight, true);
    }

    const bool live = ! demoMode && processor.hasHostTime.load()
                   && (juce::Time::getMillisecondCounterHiRes() - lastHostUpdateMs < 1800.0);

    g.setColour (demoMode ? palette.warn
                          : (live ? (processor.isPlaying.load() ? palette.ok : palette.dim)
                                  : palette.faint));
    g.fillEllipse (baseW() - 92.0f, 31.0f, 8.0f, 8.0f);

    g.setColour (palette.panelEdge.withAlpha (0.4f));
    g.drawLine (22.0f, 64.0f, baseW() - 22.0f, 64.0f, 1.0f);
}

juce::Rectangle<float> LyricSyncEditor::railIconRect (int index) const
{
    const float x = (railWidth() - 30.0f) * 0.5f;

    return { x, 54.0f + static_cast<float> (index) * 36.0f, 30.0f, 30.0f };
}

int LyricSyncEditor::railIconAt (juce::Point<float> logical) const
{
    if (! sidebarCollapsed || orientation != Orientation::horizontal)
        return -1;

    for (int i = 0; i < 5; ++i)
        if (railIconRect (i).expanded (2.0f).contains (logical))
            return i;

    return -1;
}

/** 收起态那一列：载入 / 编辑 / 着色 / 导出 / 设置，纯 Path，不依赖字体。 */
void LyricSyncEditor::paintRailIcons (juce::Graphics& g)
{
    for (int i = 0; i < 5; ++i)
    {
        const auto r = railIconRect (i);
        const float cx = r.getCentreX();
        const float cy = r.getCentreY();

        const bool active = (i == 2 && annotateMode);

        g.setColour (active ? palette.accent.withAlpha (0.14f)
                            : palette.text.withAlpha (0.04f));
        g.fillRoundedRectangle (r, 6.0f);

        if (active)
        {
            g.setColour (palette.accent.withAlpha (0.55f));
            g.drawRoundedRectangle (r.reduced (0.5f), 6.0f, 1.0f);
        }

        g.setColour (active ? palette.accent : palette.dim);
        g.setFont (uiFont (11.0f));

        if (i == 0 || i == 3)
        {
            // 载入 = 箭头向下进托盘；导出 = 箭头向上出托盘
            const float dir = (i == 0) ? 1.0f : -1.0f;
            const float sx = cx, sy = cy - 6.0f * dir;

            juce::Path p;
            p.startNewSubPath (sx, sy);
            p.lineTo (sx, sy + 7.0f * dir);
            p.startNewSubPath (sx - 3.2f, sy + 4.2f * dir);
            p.lineTo (sx, sy + 7.2f * dir);
            p.lineTo (sx + 3.2f, sy + 4.2f * dir);
            p.startNewSubPath (cx - 5.5f, cy + 3.4f);
            p.lineTo (cx - 5.5f, cy + 6.0f);
            p.lineTo (cx + 5.5f, cy + 6.0f);
            p.lineTo (cx + 5.5f, cy + 3.4f);

            g.strokePath (p, juce::PathStrokeType (1.3f,
                            juce::PathStrokeType::curved,
                            juce::PathStrokeType::rounded));
        }
        else if (i == 1)
        {
            // 编辑 = 三行文字（歌词文本）
            juce::Path p;

            for (int k = 0; k < 3; ++k)
            {
                const float w = (k == 2) ? 4.0f : 5.5f;
                p.startNewSubPath (cx - 5.5f, cy - 4.4f + static_cast<float> (k) * 4.4f);
                p.lineTo (cx + (k == 2 ? -1.5f : w), cy - 4.4f + static_cast<float> (k) * 4.4f);
            }

            g.strokePath (p, juce::PathStrokeType (1.3f,
                            juce::PathStrokeType::curved,
                            juce::PathStrokeType::rounded));
        }
        else if (i == 2)
        {
            drawBrushIcon (g, r.withSizeKeepingCentre (17.0f, 17.0f),
                           active ? palette.accent : palette.dim);
        }
        else
        {
            // 设置 = 齿轮
            juce::Path p;
            p.addEllipse (cx - 2.2f, cy - 2.2f, 4.4f, 4.4f);
            g.strokePath (p, juce::PathStrokeType (1.2f));

            p.clear();
            p.addEllipse (cx - 5.2f, cy - 5.2f, 10.4f, 10.4f);
            g.strokePath (p, juce::PathStrokeType (1.2f));

            g.setColour ((i == 4) ? (active ? palette.accent : palette.dim) : palette.dim);

            juce::Path ticks;

            for (int k = 0; k < 8; ++k)
            {
                const float ang = juce::MathConstants<float>::twoPi
                                * static_cast<float> (k) / 8.0f;
                const float x1 = cx + std::cos (ang) * 5.2f;
                const float y1 = cy + std::sin (ang) * 5.2f;
                const float x2 = cx + std::cos (ang) * 6.6f;
                const float y2 = cy + std::sin (ang) * 6.6f;

                ticks.startNewSubPath (x1, y1);
                ticks.lineTo (x2, y2);
            }

            g.strokePath (ticks, juce::PathStrokeType (1.2f,
                            juce::PathStrokeType::curved,
                            juce::PathStrokeType::rounded));
        }
    }
}

//==============================================================================
//  方案 v4 · 侧栏：近黑底、1px 细边、内联搜索、步进器、四格工具条
//==============================================================================
void LyricSyncEditor::paintStepperBox (juce::Graphics& g, juce::Rectangle<float> box,
                                       const juce::String&, bool active, bool chevrons)
{
    const bool light = palette.text.getPerceivedBrightness() < 0.5f;
    const auto line = light ? juce::Colour (0xffd4dbea) : juce::Colour (0xff33333a);

    g.setColour (active ? palette.accent : line);
    g.drawRoundedRectangle (box.reduced (0.5f), 8.0f, active ? 1.4f : 1.0f);

    // ‹ › 自己画成折线，不用字体里的字符（有些系统字体没有这两个字形）
    if (chevrons)
    {
        const float cy = box.getCentreY();
        const auto arrowCol = light ? juce::Colour (0xff8b94a6) : juce::Colour (0xff7a7a85);

        g.setColour (arrowCol);

        juce::Path left;
        left.startNewSubPath (box.getX() + 15.0f, cy - 3.4f);
        left.lineTo (box.getX() + 11.6f, cy);
        left.lineTo (box.getX() + 15.0f, cy + 3.4f);
        g.strokePath (left, juce::PathStrokeType (1.4f,
                        juce::PathStrokeType::curved,
                        juce::PathStrokeType::rounded));

        juce::Path right;
        right.startNewSubPath (box.getRight() - 15.0f, cy - 3.4f);
        right.lineTo (box.getRight() - 11.6f, cy);
        right.lineTo (box.getRight() - 15.0f, cy + 3.4f);
        g.strokePath (right, juce::PathStrokeType (1.4f,
                        juce::PathStrokeType::curved,
                        juce::PathStrokeType::rounded));
    }
}

void LyricSyncEditor::paintSidebarSearch (juce::Graphics& g)
{
    const bool light = palette.text.getPerceivedBrightness() < 0.5f;

    const auto txt2 = light ? juce::Colour (0xff5d6883) : juce::Colour (0xffa1a1aa);
    const auto txt3 = light ? juce::Colour (0xff98a2b8) : juce::Colour (0xff63636d);
    const auto line = light ? juce::Colour (0xffd4dbea) : juce::Colour (0xff33333a);

    const bool focused = searchTitleEditor.hasKeyboardFocus (true);
    const auto box = sideSearchBox();

    // 框体
    g.setColour (light ? juce::Colour (0xffffffff) : juce::Colour (0xff0d0d0f));
    g.fillRoundedRectangle (box, 8.0f);

    g.setColour (focused ? palette.accent : line);
    g.drawRoundedRectangle (box.reduced (0.5f), 8.0f, focused ? 1.4f : 1.0f);

    // 放大镜
    {
        const float cx = box.getX() + 16.0f;
        const float cy = box.getCentreY();

        juce::Path p;
        p.addEllipse (cx - 4.6f, cy - 4.6f, 6.6f, 6.6f);
        p.startNewSubPath (cx + 1.4f, cy + 1.4f);
        p.lineTo (cx + 4.4f, cy + 4.4f);

        g.setColour (txt3);
        g.strokePath (p, juce::PathStrokeType (1.3f,
                        juce::PathStrokeType::curved,
                        juce::PathStrokeType::rounded));
    }

    // 右侧「换一个歌词源」：圆弧 + 箭头
    {
        const auto r = sideSearchRefresh();
        const float cx = r.getCentreX();
        const float cy = r.getCentreY();

        juce::Path arc;
        arc.addCentredArc (cx, cy, 4.2f, 4.2f, 0.0f,
                           juce::MathConstants<float>::pi * -0.35f,
                           juce::MathConstants<float>::pi * 1.15f, true);

        g.setColour (searchBusy ? palette.accent.withAlpha (0.6f) : txt3);
        g.strokePath (arc, juce::PathStrokeType (1.3f,
                        juce::PathStrokeType::curved,
                        juce::PathStrokeType::rounded));

        juce::Path head;
        head.addTriangle (cx + 1.4f, cy - 6.4f, cx + 5.4f, cy - 4.4f, cx + 1.6f, cy - 2.6f);
        g.fillPath (head);
    }

    //---- 结果列表 ----------------------------------------------------------
    const auto area = sideResultsArea();

    if (area.getHeight() < 20.0f)
        return;

    if (searchResults.empty())
    {
        juce::String hint = searchStatus.isNotEmpty() ? searchStatus
                                                      : tr (Lang::TextId::searchResultsEmpty);

        g.setColour (searchBusy ? txt2 : txt3);
        g.setFont (uiFont (10.8f));
        g.drawFittedText (hint, area.reduced (4.0f, 2.0f).toNearestInt(),
                          juce::Justification::topLeft, 8, 1.0f);
        return;
    }

    const auto rows = sideResultRows();
    const auto titleCol = light ? juce::Colour (0xff1a2233) : juce::Colour (0xfff2f2f4);
    const auto metaCol  = light ? juce::Colour (0xff98a2b8) : juce::Colour (0xff63636d);
    const auto selBg    = light ? juce::Colour (0xffeef2f9) : juce::Colour (0xff1c1c20);

    for (int i = 0; i < static_cast<int> (rows.size()); ++i)
    {
        const int idx = searchScroll + i;

        if (idx < 0 || idx >= static_cast<int> (searchResults.size()))
            break;

        const auto& res = searchResults[(size_t) idx];
        const auto row = rows[(size_t) i];
        const bool sel = (idx == searchSel);

        if (sel)
        {
            g.setColour (selBg);
            g.fillRoundedRectangle (row, 10.0f);
            g.setColour (line.brighter (0.25f));
            g.drawRoundedRectangle (row.reduced (0.5f), 10.0f, 1.0f);
        }

        // 逐字角标
        float badgeW = 0.0f;

        if (res.wordSync)
        {
            const float bw = 34.0f;
            const juce::Rectangle<float> badge (row.getRight() - bw - 9.0f,
                                                row.getCentreY() - 9.0f, bw, 18.0f);

            g.setColour (palette.accent.withAlpha (0.10f));
            g.fillRoundedRectangle (badge, 9.0f);
            g.setColour (palette.accent.withAlpha (0.55f));
            g.drawRoundedRectangle (badge.reduced (0.5f), 9.0f, 1.0f);

            g.setColour (palette.accent);
            g.setFont (uiFont (9.5f));
            g.drawText (tr (Lang::TextId::onlineWordSync), badge,
                        juce::Justification::centred, false);

            badgeW = bw + 12.0f;
        }

        const float textW = juce::jmax (20.0f, row.getWidth() - 18.0f - badgeW);

        g.setColour (titleCol);
        g.setFont (uiFont (12.5f, true));
        g.drawText (res.title.isNotEmpty() ? res.title : juce::String ("-"),
                    juce::Rectangle<float> (row.getX() + 9.0f,
                                            row.getCentreY() - 17.0f, textW, 17.0f),
                    juce::Justification::centredLeft, true);

        // 歌手 · 时长 · 行数
        juce::String meta = res.artist.isNotEmpty() ? res.artist : res.source;

        if (res.durationSec > 0)
            meta += juce::String::fromUTF8 ("  \xc2\xb7  ") + juce::String (res.durationSec / 60) + ":"
                  + juce::String (res.durationSec % 60).paddedLeft ('0', 2);

        if (res.lineCount > 0)
            meta += juce::String::fromUTF8 ("  \xc2\xb7  ") + juce::String (res.lineCount) + tr (Lang::TextId::lineCountSuffix);

        g.setColour (sel ? metaCol.brighter (0.25f) : metaCol);
        g.setFont (uiFont (10.5f));
        g.drawText (meta,
                    juce::Rectangle<float> (row.getX() + 9.0f,
                                            row.getCentreY() + 1.0f, textW, 15.0f),
                    juce::Justification::centredLeft, true);
    }

    // 搜索中：列表顶部一条流光细线
    if (searchBusy)
    {
        g.setColour (palette.accent.withAlpha (0.65f));
        g.fillRoundedRectangle (area.getX(), area.getY() - 3.0f, area.getWidth(), 1.5f, 0.75f);
    }
}

void LyricSyncEditor::paintSidebar (juce::Graphics& g)
{
    const float railW = railWidth();
    const bool wide = railW > 60.0f;
    const bool vertical = (orientation == Orientation::vertical);

    const bool lightTheme = palette.text.getPerceivedBrightness() < 0.5f;
    const auto sideBg   = lightTheme ? juce::Colour (0xfff2f4f8) : juce::Colour (0xff0e0e11);
    const auto lineCol  = lightTheme ? juce::Colour (0xffd4dbea) : juce::Colour (0xff232327);
    const auto txt2     = lightTheme ? juce::Colour (0xff5d6883) : juce::Colour (0xffa1a1aa);
    const auto txt3     = lightTheme ? juce::Colour (0xff98a2b8) : juce::Colour (0xff63636d);
    const auto titleCol = lightTheme ? juce::Colour (0xff98a2b8) : juce::Colour (0xff55555e);

    // 编辑 / 打轴 / 弹层时侧栏只留品牌行，避免和舞台上的控件打架
    const bool fullSide = ! editing && ! tapMode && ! settingsOpen && ! aboutOpen;

    // 竖版没有侧栏，把这些控件画在底部面板上
    if (vertical)
    {
        const auto ox = vShift();
        const auto top = sideTopRow();

        g.setColour (sideBg);
        g.fillRect (juce::Rectangle<float> (ox + 8.0f, top.getY() - 14.0f,
                                            baseW() - ox * 2.0f - 16.0f,
                                            baseH() - top.getY() + 6.0f));
        g.setColour (lineCol);
        g.drawLine (ox + 22.0f, top.getY() - 8.0f, ox + 22.0f + 556.0f, top.getY() - 8.0f, 1.0f);

        if (! fullSide)
            return;

        // 起点 / 偏移 / 字号 三个小标题
        g.setColour (txt2);
        g.setFont (uiFont (11.0f));
        g.drawText (tr (Lang::TextId::fontSize),
                    juce::Rectangle<float> (ox + 22.0f, sideFontStepper().getY(), 60.0f, 26.0f),
                    juce::Justification::centredLeft, false);
        g.drawText (tr (Lang::TextId::offsetLabel),
                    juce::Rectangle<float> (ox + 22.0f, sideOffsetStepper().getY(), 60.0f, 26.0f),
                    juce::Justification::centredLeft, false);

        paintStepperBox (g, sideOffsetStepper(), {}, offsetScrub, true);
        paintStepperBox (g, sideFontStepper(), {}, fontScrub, false);

        // 逐行 / 逐字 底槽
        {
            const auto seg = sideSegArea();
            g.setColour (lightTheme ? juce::Colour (0xffe8edf6) : juce::Colour (0xff1c1c20));
            g.fillRoundedRectangle (seg, 8.0f);
        }

        {
            const auto tools = sideToolsRow();
            g.setColour (lineCol.withAlpha (0.6f));
            g.drawLine (tools.getX(), tools.getY() - 6.0f, tools.getRight(), tools.getY() - 6.0f, 1.0f);
        }

        paintSidebarSearch (g);

        // 起点读数
        {
            const auto row = sideStartRow();
            g.setColour (txt2);
            g.setFont (uiFont (11.5f));
            g.drawText (tr (Lang::TextId::startPrefix) + timecodeText (-offsetMs / 1000.0),
                        row.withTrimmedRight (110.0f),
                        juce::Justification::centredLeft, false);
        }

        return;
    }

    g.setColour (sideBg);
    g.fillRect (juce::Rectangle<float> (0.0f, 0.0f, railW, baseH()));

    g.setColour (lineCol);
    g.drawLine (railW - 0.5f, 0.0f, railW - 0.5f, baseH(), 1.0f);

    //---- 编辑歌词：侧栏换成打轴面板的头 + 操作说明 ------------------------
    if (wide && tapMode)
    {
        g.setColour (palette.accent);
        g.fillRoundedRectangle (12.0f, 14.0f, 7.0f, 7.0f, 3.5f);

        drawText (g, juce::Rectangle<float> (27.0f, 10.0f, 140.0f, 18.0f),
                  "LyricSync", 13.0f, palette.text, false, -1, {}, 1.0f,
                  juce::Justification::centredLeft);

        const float barY = 44.0f;

        g.setColour (palette.accent);
        g.fillRoundedRectangle (12.0f, barY, 3.0f, 18.0f, 1.5f);

        drawText (g, juce::Rectangle<float> (21.0f, barY - 1.0f, 130.0f, 20.0f),
                  tr (Lang::TextId::tapTitle), 12.0f, palette.accent, false, -1, {}, 1.0f,
                  juce::Justification::centredLeft);

        g.setColour (txt3);
        g.setFont (uiFont (10.5f));
        g.drawText (juce::String (juce::jmin (tapIndex + 1, (int) tapLines.size()))
                      + " / " + juce::String ((int) tapLines.size()),
                    juce::Rectangle<float> (railW - 106.0f, barY + 1.0f, 94.0f, 16.0f),
                    juce::Justification::centredRight, false);

        g.setColour (txt3);
        g.setFont (uiFont (10.5f));
        g.drawFittedText (tr (Lang::TextId::tapListTitle),
                          juce::Rectangle<float> (14.0f, 74.0f, railW - 28.0f, 32.0f)
                              .toNearestInt(),
                          juce::Justification::topLeft, 2, 1.0f);

        g.setColour (lineCol.withAlpha (0.7f));
        g.drawLine (12.0f, 116.0f, railW - 12.0f, 116.0f, 1.0f);

        // 底部小字：空格同效（按钮本体在 resized() 里布局）
        drawText (g, juce::Rectangle<float> (12.0f, baseH() - 84.0f, railW - 24.0f, 16.0f),
                  tr (Lang::TextId::tapSpaceHint), 10.0f, txt3, false, -1.0f, {}, 1.0f,
                  juce::Justification::centredLeft);

        return;
    }

    if (wide && fullSide)
    {
        // 品牌行
        g.setColour (palette.accent);
        g.fillEllipse (14.0f, 15.0f, 7.0f, 7.0f);

        drawText (g, juce::Rectangle<float> (27.0f, 11.0f, 130.0f, 18.0f),
                  "LyricSync", 13.0f, palette.text, false, -1, {}, 1.0f,
                  juce::Justification::centredLeft);

        g.setColour (txt3);
        g.setFont (uiFont (10.5f));
        g.drawText (juce::String ("v") + kVersion,
                    juce::Rectangle<float> (140.0f, 12.0f, 94.0f, 16.0f),
                    juce::Justification::centredRight, false);

        // 分组标题：歌词 / 来源
        g.setColour (titleCol);
        g.setFont (uiFont (10.0f));
        g.drawText (tr (Lang::TextId::lyricsGroup),
                    juce::Rectangle<float> (16.0f, 38.0f, 120.0f, 12.0f),
                    juce::Justification::centredLeft, false);
        g.drawText (tr (Lang::TextId::sidSource),
                    juce::Rectangle<float> (railW - 136.0f, 38.0f, 120.0f, 12.0f),
                    juce::Justification::centredRight, false);

        paintSidebarSearch (g);

        // 起点：读数 + 两个小按钮（按钮由子控件画）
        {
            const auto row = sideStartRow();

            g.setColour (txt2);
            g.setFont (uiFont (11.5f));
            g.drawText (tr (Lang::TextId::startPrefix) + timecodeText (-offsetMs / 1000.0),
                        row.withTrimmedRight (92.0f),
                        juce::Justification::centredLeft, false);
        }

        // 偏移 / 字号：小标题 + 右侧说明
        {
            const auto off = sideOffsetStepper();
            const auto fnt = sideFontStepper();

            g.setColour (titleCol);
            g.setFont (uiFont (10.0f));
            g.drawText (tr (Lang::TextId::offsetLabel),
                        juce::Rectangle<float> (16.0f, off.getY() - 14.0f, 80.0f, 12.0f),
                        juce::Justification::centredLeft, false);
            g.drawText (tr (Lang::TextId::offsetHint),
                        juce::Rectangle<float> (railW - 116.0f, off.getY() - 14.0f, 100.0f, 12.0f),
                        juce::Justification::centredRight, false);

            g.setColour (titleCol);
            g.setFont (uiFont (10.0f));
            g.drawText (tr (Lang::TextId::fontSize),
                        juce::Rectangle<float> (16.0f, fnt.getY() - 14.0f, 80.0f, 12.0f),
                        juce::Justification::centredLeft, false);
            g.drawText (tr (Lang::TextId::fontHint),
                        juce::Rectangle<float> (railW - 156.0f, fnt.getY() - 14.0f, 140.0f, 12.0f),
                        juce::Justification::centredRight, false);

            paintStepperBox (g, off, {}, offsetScrub, true);
            paintStepperBox (g, fnt, {}, fontScrub, false);
        }

        // 逐行 / 逐字 底槽
        {
            const auto seg = sideSegArea();
            g.setColour (lightTheme ? juce::Colour (0xffe8edf6) : juce::Colour (0xff1c1c20));
            g.fillRoundedRectangle (seg, 8.0f);
        }
    }

    // 收起态：只用图标代替整列按钮
    if (! wide)
        paintRailIcons (g);

    // 折叠键：贴在侧栏右缘的垂直中点
    {
        const auto t = railToggleRect();
        const float cx = t.getCentreX();
        const float cy = t.getCentreY();

        g.setColour (lineCol.withAlpha (0.5f));
        g.fillRoundedRectangle (t.reduced (1.0f, 8.0f), 6.0f);

        g.setColour (txt3);

        juce::Path chev;
        const float dir = sidebarCollapsed ? 1.0f : -1.0f;   // 收起时朝右

        chev.startNewSubPath (cx - 2.0f * dir, cy - 5.0f);
        chev.lineTo (cx + 2.0f * dir, cy);
        chev.lineTo (cx - 2.0f * dir, cy + 5.0f);

        g.strokePath (chev, juce::PathStrokeType (1.5f,
                        juce::PathStrokeType::curved,
                        juce::PathStrokeType::rounded));
    }
}

void LyricSyncEditor::paintStage (juce::Graphics& g, juce::Rectangle<float> area)
{
    const bool darkSkin = palette.text.getPerceivedBrightness() > 0.5f;

    if (darkSkin)
    {
        // 深色：舞台就是一块比画布更黑的平面，没有卡片、没有描边、没有光晕
        g.setColour (juce::Colour (0xff030304));
        g.fillRect (area);
    }
    else
    {
        // 浅色皮肤保留一点玻璃质感，否则白底上什么都看不见
        g.setColour (palette.text.withAlpha (0.055f));
        g.fillRoundedRectangle (area, 22.0f);

        juce::ColourGradient thick (juce::Colours::transparentBlack,
                                    area.getX(), area.getCentreY(),
                                    juce::Colours::black.withAlpha (0.10f),
                                    area.getX(), area.getBottom(), false);
        g.setGradientFill (thick);
        g.fillRoundedRectangle (area, 22.0f);

        g.setColour (palette.text.withAlpha (0.16f));
        g.drawRoundedRectangle (area.reduced (0.5f), 22.0f, 1.2f);
    }

    if (tapMode)
    {
        paintTapStage (g, area);
        return;
    }

    if (editing)
        return;

    auto& doc = processor.lyrics();

    g.setColour (palette.faint);
    g.setFont (uiFont (10.5f, true));

    if (! zenMode && ! annotateMode)
        g.drawText (tr (Lang::TextId::currentLine),
                    static_cast<int> (area.getX() + 30.0f),
                    static_cast<int> (area.getY() + 15.0f), 200, 16,
                    juce::Justification::centredLeft);

    if (! doc.isEmpty() && ! zenMode && ! annotateMode)
    {
        g.setFont (uiFont (10.5f));
        g.drawText (juce::String (juce::jmax (0, currentIndex + 1)) + " / "
                      + juce::String (doc.size()),
                    static_cast<int> (area.getRight() - 160.0f),
                    static_cast<int> (area.getY() + 15.0f), 130, 16,
                    juce::Justification::centredRight);
    }

    if (doc.isEmpty())
    {
        drawText (g, area.reduced (60.0f, 80.0f), tr (Lang::TextId::emptyStage),
                  20.0f, palette.dim, true);
        return;
    }

    //---- Karaoke stack ------------------------------------------------------
    const float refW = (orientation == Orientation::vertical ? 520.0f : 900.0f);
    const float widthScale = juce::jlimit (0.80f, 1.70f, area.getWidth() / refW)
                           * static_cast<float> (fontScale);

    const float innerH = area.getHeight() - 78.0f;

    // 行距必须跟着字号走，否则字号调大后相邻行会叠在一起（竖版最明显）。
    const float maxLineSize = 34.0f * widthScale;
    const float minGap = maxLineSize * 1.16f;

    int radius = static_cast<int> ((innerH / juce::jmax (1.0f, minGap) - 1.0f) * 0.5f);
    radius = juce::jlimit (1, 4, radius);

    const auto inner = juce::Rectangle<float> (area.getX() + 26.0f, area.getY() + 36.0f,
                                               area.getWidth() - 52.0f, innerH);
    const float spacing = inner.getHeight() / static_cast<float> (2 * radius + 1);
    const float centreY = inner.getCentreY();

    const bool hasCurrent = currentIndex >= 0 && currentIndex < doc.size();
    const bool hasTrans   = hasCurrent && doc.line (currentIndex).translation.isNotEmpty();
    const float lift      = hasTrans ? 9.0f * widthScale : 0.0f;

    const auto currentTint = lyricColour();

    //---- 前奏倒数：首个正式唱词前 4 拍，4 个圆点按拍点亮 --------------------
    //  圆点直径跟歌词正文一样大（v1.9.8），比原来的小点更醒目。
    //  BPM 优先用自动测速结果，测不出来就用宿主/默认速度；纯算术，零延迟。
    const int vocalStart = firstVocalLineIndex();
    int countInLit = -1;          // >=0 表示正在倒数，值 = 已点亮的点数
    double countInBpm = 0.0;

    if (! doc.isEmpty() && vocalStart >= 0 && vocalStart < doc.size())
    {
        const double vocalTime = doc.line (vocalStart).time;
        const double bpmNow = detectedBpm > 0.0 ? detectedBpm : processor.bpm.load();
        const double spb = bpmNow > 20.0 ? 60.0 / bpmNow : 0.0;

        if (spb > 0.0)
        {
            const double beatsLeft = (vocalTime - displayTime) / spb;

            if (beatsLeft > -0.30 && beatsLeft <= 4.0)
            {
                countInLit = juce::jlimit (0, 4,
                    4 - static_cast<int> (std::ceil (juce::jmax (0.0, beatsLeft))) + 1);
                countInBpm = bpmNow;

                // 圆点直径与正文字号一致；间距留足，四点整齐排在文字上方，不重叠
                const float r    = 17.0f * widthScale;
                const float gap  = r * 3.2f;              // 相邻圆心距 > 2r，绝不重叠
                const float dotY = centreY - 52.0f * widthScale;

                for (int i = 0; i < 4; ++i)
                {
                    const float cx = area.getCentreX()
                                   + (static_cast<float> (i) - 1.5f) * gap;
                    const bool on = i < countInLit;

                    g.setColour (on ? currentTint : palette.track);
                    g.fillEllipse (cx - r, dotY - r, r * 2.0f, r * 2.0f);
                }

                drawText (g, juce::Rectangle<float> (area.getX() + 60.0f,
                                                     dotY + r + 10.0f,
                                                     area.getWidth() - 120.0f, 18.0f),
                          tr (Lang::TextId::bpmPrefix) + juce::String (countInBpm, 1),
                          11.0f * widthScale, palette.faint, false);
            }
        }
    }

    for (int pass = 0; pass < 2; ++pass)
    {
        const int lo = static_cast<int> (std::floor (displayIndex)) - radius - 1;
        const int hi = static_cast<int> (std::ceil  (displayIndex)) + radius + 1;

        for (int i = hi; i >= lo; --i)
        {
            if (i < 0 || i >= doc.size())
                continue;

            const float d  = static_cast<float> (i) - displayIndex;
            const float ad = std::abs (d);

            if (ad > static_cast<float> (radius) + 0.95f)
                continue;

            const bool isCurrent = (i == currentIndex);
            if (pass == 0 && ad < 0.45f) continue;
            if (pass == 1 && ad >= 0.45f) continue;

            const float size  = juce::jmax (17.0f, 34.0f - 14.0f * ad) * widthScale;
            const float alpha = juce::jmax (0.10f, 1.0f - 0.34f * ad);

            const float y = centreY + d * spacing - size * 0.70f
                          - (isCurrent ? lift : 0.0f)
                          + (countInLit >= 0 ? 54.0f * widthScale : 0.0f);

            const auto lineRect = juce::Rectangle<float> (area.getX() + 40.0f, y,
                                                          area.getWidth() - 80.0f,
                                                          size * 1.42f);

            // 着色模式：被拖选的句子加一条淡色选区底，像文本被选中一样
            if (annotateMode && annotateSelLo >= 0
                && i >= annotateSelLo && i <= annotateSelHi)
            {
                const auto sel = lineRect.reduced (8.0f, 2.0f);
                g.setColour (palette.accent.withAlpha (0.18f));
                g.fillRoundedRectangle (sel, 8.0f);
                g.setColour (palette.accent.withAlpha (0.55f));
                g.drawRoundedRectangle (sel, 8.0f, 1.2f);
            }

            const auto& line = doc.line (i);

            if (isCurrent)
            {
                // 逐句模式：整行一起亮；逐字模式：连续扫光（0..N 的浮点数）
                float sung = -1.0f;

                if (advanceMode == 1)
                {
                    if (line.hasKaraoke())
                    {
                        sung = static_cast<float> (doc.sungProgressAt (line, displayTime));
                    }
                    else if (line.text.isNotEmpty())
                    {
                        const double s0 = line.time;
                        const double s1 = doc.lineEnd (i);
                        const double p = juce::jlimit (0.0, 1.0,
                                                       (displayTime - s0) / juce::jmax (0.05, s1 - s0));
                        sung = static_cast<float> (p * static_cast<double> (line.text.length()));
                    }
                }

                // 时间码是按音频块来的（约 20 ms 一跳），再叠一层轻度平滑，
                // 视觉上就是"一路扫过去"而不是一格一格爬。
                if (sung >= 0.0f)
                {
                    if (smoothedSung < 0.0f || std::abs (sung - smoothedSung) > 2.5f)
                        smoothedSung = sung;               // 跳转/回退：直接吸附
                    else
                        smoothedSung += (sung - smoothedSung) * 0.55f;

                    sung = smoothedSung;
                }
                else
                {
                    smoothedSung = -1.0f;
                }

                const float fade = 0.55f + 0.45f * transition;

                // 这一句如果单独设了颜色，就用它代替主题色
                const auto lineTint = lineColourFor (i, currentTint);
                const auto note = lineNoteFor (i);

                // 逐字模式里，未唱的字原本固定用白色，导致单独着色的句子
                // 只有"唱到的那几个字"变色，预览/未开唱时看不出改过色。
                // 给这一句上过色时，未唱部分也用该颜色的浅一档，整句都看得出来。
                const auto styleIt = lineStyles.find (i);
                const bool hasLineTint = (styleIt != lineStyles.end()
                                          && styleIt->second.colourIndex >= 0);
                const auto unsungColour = hasLineTint
                    ? lineTint.interpolatedWith (palette.text, 0.35f)
                    : palette.text;

                if (advanceMode == 1)
                {
                    // 逐字：未唱的字用底色，唱到的字渐变为该行颜色（字内渐变）
                    drawText (g, lineRect, line.text, size, unsungColour, true,
                              sung, lineTint, fade, juce::Justification::centred, note);
                }
                else
                {
                    // 逐句：整行在换句时由白渐变为该行颜色
                    const auto lineFade = palette.text.interpolatedWith (lineTint,
                                                                         juce::jlimit (0.0f, 1.0f, transition));
                    drawText (g, lineRect, line.text, size, lineFade, true,
                              -1.0f, palette.text, fade, juce::Justification::centred, note);
                }

                if (line.translation.isNotEmpty())
                    drawText (g, juce::Rectangle<float> (lineRect.getX(),
                                                         lineRect.getBottom() - 3.0f * widthScale,
                                                         lineRect.getWidth(),
                                                         20.0f * widthScale),
                              line.translation, 13.0f * widthScale,
                              palette.dim.withAlpha (0.85f), false);
            }
            else
            {
                // 刚被替换下来的那一句：从主题色慢慢淡回普通颜色
                auto colour = lineColourFor (i, palette.text);

                if (i == previousIndex && transition < 0.999f)
                    colour = lineColourFor (i, currentTint)
                               .interpolatedWith (colour, juce::jlimit (0.0f, 1.0f, transition));

                drawText (g, lineRect, line.text, size, colour,
                          ad < 1.5f, -1.0f, {}, alpha,
                          juce::Justification::centred, lineNoteFor (i));
            }
        }
    }

    if (std::abs (previewOffset) > 0.02)
    {
        const juce::String badge = tr (Lang::TextId::previewBadge) + "  "
                                 + (previewOffset >= 0.0 ? "+" : "-")
                                 + juce::String (std::abs (previewOffset), 1) + " s";

        drawText (g, juce::Rectangle<float> (area.getRight() - 220.0f, area.getY() + 10.0f,
                                             190.0f, 18.0f),
                  badge, 11.0f, palette.warn, false, -1, {}, 1.0f,
                  juce::Justification::centredRight);
    }

    if (! hasCurrent)
    {
        // 正式唱词的时间（可能被前面的作词/作曲信息推后）
        const double vocalTime = (vocalStart >= 0 && vocalStart < doc.size())
                               ? doc.line (vocalStart).time
                               : (doc.isEmpty() ? 0.0 : doc.line (0).time);

        // 倒数时「前奏」字样移到四个圆点上方，互不遮挡
        const float introY = (countInLit >= 0)
                           ? centreY - 96.0f * static_cast<float> (fontScale)
                           : centreY - 13.0f;

        drawText (g, juce::Rectangle<float> (area.getX() + 60.0f, introY,
                                             area.getWidth() - 120.0f, 26.0f),
                  tr (Lang::TextId::intro), 17.0f * widthScale,
                  palette.faint, false);

        if (countInLit < 0)
        {
            drawText (g, juce::Rectangle<float> (area.getX() + 60.0f, centreY + 17.0f,
                                                 area.getWidth() - 120.0f, 20.0f),
                      tr (Lang::TextId::nextLineIn)
                        + juce::String (juce::jmax (0.0, vocalTime - displayTime), 1)
                        + tr (Lang::TextId::secondsSuffix),
                      11.5f * widthScale, palette.faint, false);
        }
    }
}

//==============================================================================
void LyricSyncEditor::paintAnnotateOverlay (juce::Graphics& g, juce::Rectangle<float> area)
{
    // 舞台仍是原来的歌词界面，只在顶部给一行使用提示
    drawText (g, juce::Rectangle<float> (area.getX() + 30.0f, area.getY() + 11.0f,
                                         area.getWidth() - 60.0f, 20.0f),
              tr (Lang::TextId::annotateHint), 11.0f, palette.accent, true,
              -1.0f, {}, 1.0f, juce::Justification::centredLeft);

    if (styleEditIndex < 0)
        return;

    const auto card = styleCard();

    // 浮在鼠标旁的小选择框：磨砂玻璃 + 一圈细边，不是新窗口
    g.setColour (palette.card.withAlpha (0.98f));
    g.fillRoundedRectangle (card, 14.0f);
    g.setColour (palette.accent.withAlpha (0.42f));
    g.drawRoundedRectangle (card.reduced (0.6f), 14.0f, 1.2f);

    const int lo = (annotateSelLo >= 0 ? annotateSelLo : styleEditIndex);
    const int hi = (annotateSelHi >= lo ? annotateSelHi : lo);

    juce::String title = tr (Lang::TextId::annotateTitle) + "   "
                       + tr (Lang::TextId::annotateLine) + juce::String (lo + 1);

    if (hi > lo)
        title += juce::String ("-") + juce::String (hi + 1);

    drawText (g, juce::Rectangle<float> (card.getX() + 20.0f, card.getY() + 10.0f,
                                         card.getWidth() - 40.0f, 22.0f),
              title, 13.5f, palette.accent, true, -1.0f, {}, 1.0f,
              juce::Justification::centredLeft);

    // 被选中那一句的原文，作为参照
    juce::String preview = processor.lyrics().line (lo).text;

    if (hi > lo)
        preview += "  ...";

    drawText (g, juce::Rectangle<float> (card.getX() + 20.0f, card.getY() + 32.0f,
                                         card.getWidth() - 40.0f, 16.0f),
              preview, 10.5f, palette.faint, false, -1.0f, {}, 1.0f,
              juce::Justification::centredLeft);

    drawText (g, juce::Rectangle<float> (card.getX() + 18.0f, card.getY() + 54.0f,
                                         40.0f, 22.0f),
              tr (Lang::TextId::annotateColour), 10.0f, palette.faint, false,
              -1.0f, {}, 1.0f, juce::Justification::centredLeft);

    drawText (g, juce::Rectangle<float> (card.getX() + 18.0f, card.getY() + 90.0f,
                                         40.0f, 26.0f),
              tr (Lang::TextId::annotateNote), 10.0f, palette.faint, false,
              -1.0f, {}, 1.0f, juce::Justification::centredLeft);

    // 颜色圆点
    const auto swatches = styleSwatches (card);

    for (int i = 0; i < static_cast<int> (swatches.size()); ++i)
    {
        const auto sw = swatches[(size_t) i];
        const int  colourIndex = i - 1;          // 0 = 默认
        const bool sel = (styleEditColour == colourIndex);

        if (colourIndex < 0)
        {
            g.setColour (palette.text.withAlpha (0.10f));
            g.fillEllipse (sw);

            g.setColour (palette.faint);
            g.drawEllipse (sw.reduced (0.5f), 1.0f);
            g.drawLine (sw.getX() + 4.0f, sw.getBottom() - 4.0f,
                        sw.getRight() - 4.0f, sw.getY() + 4.0f, 1.4f);
        }
        else
        {
            g.setColour (juce::Colour (kLineColours[colourIndex]));
            g.fillEllipse (sw);
        }

        if (sel)
        {
            g.setColour (palette.text.withAlpha (0.85f));
            g.drawEllipse (sw.expanded (2.5f), 1.6f);
        }
    }
}

//==============================================================================
void LyricSyncEditor::paintTapStage (juce::Graphics& g, juce::Rectangle<float> area)
{
    //---- 行编辑：横版直接在被点的那一行上编辑；竖版或行已滚出视区才用卡片 -------
    const auto editRow = tapEditRowRect();

    if (tapEditIndex >= 0 && (orientation == Orientation::vertical || editRow.isEmpty()))
    {
        const bool v = (orientation == Orientation::vertical);

        const juce::Rectangle<float> card = v
            ? juce::Rectangle<float> (40.0f, 300.0f, baseW() - 80.0f, 200.0f)
            : juce::Rectangle<float> (60.0f, 250.0f, 540.0f, 190.0f);

        g.setColour (palette.card.withAlpha (0.94f));
        g.fillRoundedRectangle (card, 16.0f);
        g.setColour (palette.text.withAlpha (0.16f));
        g.drawRoundedRectangle (card, 16.0f, 1.2f);

        drawText (g, juce::Rectangle<float> (card.getX() + 24.0f, card.getY() + 12.0f,
                                             card.getWidth() - 48.0f, 26.0f),
                  tr (Lang::TextId::tapEditTitle) + "   #"
                    + juce::String (tapEditIndex + 1),
                  15.0f, palette.accent, true, -1.0f, {}, 1.0f,
                  juce::Justification::centredLeft);

        drawText (g, juce::Rectangle<float> (card.getX() + 24.0f, card.getY() + 94.0f,
                                             60.0f, 24.0f),
                  tr (Lang::TextId::tapRowTime), 10.5f, palette.faint, false, -1.0f, {}, 1.0f,
                  juce::Justification::centredLeft);

        return;
    }

    const int total = static_cast<int> (tapLines.size());
    const int idx   = juce::jlimit (0, juce::jmax (0, total - 1), tapIndex);
    const bool v    = (orientation == Orientation::vertical);

    // 预备重打时，界面展示的是"被预备的那一句"，而不是指针所指的下一句
    const bool armed = (tapArmIndex >= 0 && tapArmIndex < total);
    const int  shown = juce::jlimit (0, juce::jmax (0, total - 1), armed ? tapArmIndex : idx);

    g.setColour (palette.accent);
    g.setFont (uiFont (10.5f, true));
    g.drawText (tr (Lang::TextId::tapTitle),
                static_cast<int> (area.getX() + 30.0f),
                static_cast<int> (area.getY() + 15.0f), 240, 16,
                juce::Justification::centredLeft);

    g.setColour (palette.faint);
    g.setFont (uiFont (10.5f));
    g.drawText (juce::String (juce::jmin (shown + 1, total)) + " / " + juce::String (total),
                static_cast<int> (area.getRight() - 160.0f),
                static_cast<int> (area.getY() + 15.0f), 130, 16,
                juce::Justification::centredRight);

    if (armed)
    {
        drawText (g, juce::Rectangle<float> (area.getX() + 150.0f, area.getY() + 13.0f,
                                             240.0f, 18.0f),
                  tr (Lang::TextId::tapArmTitle) + "  #" + juce::String (tapArmIndex + 1),
                  11.0f, palette.warn, true, -1.0f, {}, 1.0f,
                  juce::Justification::centredLeft);
    }

    const float refW = (v ? 520.0f : 900.0f);
    const float widthScale = juce::jlimit (0.80f, 1.70f, area.getWidth() / refW)
                           * static_cast<float> (fontScale);

    const auto rows = tapListRows (area);
    const bool hasList = ! rows.empty();

    float mainX = area.getX() + 40.0f;
    float mainW = area.getWidth() - 80.0f;

    // 列表挪进侧栏后不再挤占舞台，主内容铺满
    const bool listInSide = (! v && railWidth() > 60.0f);

    if (! v && hasList && ! listInSide)
        mainW = rows[0].getX() - mainX - 24.0f;

    const float cx = mainX;
    const float cw = mainW;

    // 竖版把列表放在上方，主内容顺着列表往下排；横版仍然左右分栏
    const float listBottom = rows.empty() ? area.getY() + 48.0f : rows.back().getBottom();
    const float centreY = v ? listBottom + 74.0f : area.getCentreY() - 8.0f;

    if (! v && shown > 0 && shown - 1 < total)
    {
        drawText (g, juce::Rectangle<float> (cx, centreY - 96.0f, cw, 20.0f),
                  tr (Lang::TextId::tapPrev) + "  "
                    + (tapTimes[(size_t) (shown - 1)] >= 0.0
                         ? formatTimecode (tapTimes[(size_t) (shown - 1)])
                         : juce::String ("--:--.--")),
                  10.5f * widthScale, palette.faint, false);

        drawText (g, juce::Rectangle<float> (cx, centreY - 74.0f, cw, 30.0f),
                  tapLines[(size_t) (shown - 1)], 16.0f * widthScale,
                  palette.dim, false);
    }

    if (armed || tapIndex < total)
    {
        drawText (g, juce::Rectangle<float> (cx, centreY - 24.0f, cw, 52.0f),
                  tapLines[(size_t) shown], 30.0f * widthScale,
                  armed ? palette.warn : lyricColour(), true);

        // 预备重打时，下面一行显示"这一句原时间 / 距现在多久"
        if (armed)
        {
            juce::String sub;

            if (tapTimes[(size_t) shown] >= 0.0)
            {
                const double diff = tapTimes[(size_t) shown] - displayTime;

                sub = formatTimecode (tapTimes[(size_t) shown]) + "   "
                    + (diff >= 0.0 ? tr (Lang::TextId::nextLineIn)
                                     + juce::String (diff, 1)
                                   : tr (Lang::TextId::tapArmAhead)
                                     + juce::String (-diff, 1)
                                     + tr (Lang::TextId::secondsSuffix));
            }
            else
            {
                sub = tr (Lang::TextId::tapArmHint);
            }

            drawText (g, juce::Rectangle<float> (cx, centreY + 34.0f, cw, 22.0f),
                      sub, 11.5f * widthScale, palette.warn, false);
        }
        else if (shown + 1 < total)
        {
            drawText (g, juce::Rectangle<float> (cx, centreY + 36.0f, cw, 18.0f),
                      tr (Lang::TextId::tapNext), 10.5f * widthScale, palette.faint, false);

            drawText (g, juce::Rectangle<float> (cx, centreY + 54.0f, cw, 26.0f),
                      tapLines[(size_t) (shown + 1)], 15.0f * widthScale,
                      palette.dim.withAlpha (0.55f), false);
        }
    }
    else
    {
        drawText (g, juce::Rectangle<float> (cx, centreY - 24.0f, cw, 52.0f),
                  tr (Lang::TextId::tapDoneLabel), 23.0f * widthScale, palette.ok, true);
    }

    //---- 已记录列表（可点，点一行就从那一句重打）-------------------------
    if (hasList)
    {
        const auto ids = tapListIndices();

        if (! listInSide)
        {
            drawText (g, juce::Rectangle<float> (rows[0].getX(), rows[0].getY() - 20.0f,
                                                 rows[0].getWidth(), 16.0f),
                      tr (Lang::TextId::tapListTitle), 10.0f * widthScale,
                      palette.faint, false, -1.0f, {}, 1.0f,
                      juce::Justification::centredLeft);
        }

        for (int i = 0; i < static_cast<int> (rows.size()); ++i)
        {
            const auto r  = rows[(size_t) i];
            const int  li = ids[(size_t) i];

            const bool thisEditing = (li == tapEditIndex);

            g.setColour (thisEditing ? palette.accent.withAlpha (0.10f)
                                     : palette.text.withAlpha (0.05f));
            g.fillRoundedRectangle (r, 5.0f);

            g.setColour (thisEditing ? palette.accent.withAlpha (0.55f)
                                     : palette.text.withAlpha (0.10f));
            g.drawRoundedRectangle (r, 5.0f, 1.0f);

            if (thisEditing)
            {
                g.setColour (palette.accent);
                g.fillRoundedRectangle (r.getX() - 3.0f, r.getY() + 3.0f,
                                        2.5f, r.getHeight() - 6.0f, 1.2f);
            }

            auto ri = r.toNearestInt();
            const auto armRect = tapRowArmButton (r);
            const bool thisArmed = (tapArmIndex == li);

            g.setColour (palette.faint);
            g.setFont (uiFont (9.5f));
            g.drawText (juce::String (li + 1), ri.removeFromLeft (24),
                        juce::Justification::centredRight, false);

            if (listInSide)
            {
                // 侧栏两行：第一行时间码，第二行歌词，右边留给环形重打钮
                const auto textW = ri.getWidth() - static_cast<int> (armRect.getWidth()) - 4;

                g.setColour (thisArmed ? palette.warn : palette.accent);
                g.setFont (uiFont (10.5f, true));
                g.drawText (formatTimecode (tapTimes[(size_t) li]),
                            juce::Rectangle<int> (ri.getX(), ri.getY(), textW, 19),
                            juce::Justification::centredLeft, false);

                g.setColour (thisArmed ? palette.text : palette.dim);
                g.setFont (uiFont (10.5f));
                g.drawText (tapLines[(size_t) li],
                            juce::Rectangle<int> (ri.getX(), ri.getY() + 19, textW, 19),
                            juce::Justification::centredLeft, true);
            }
            else
            {
                g.setColour (thisArmed ? palette.warn : palette.accent);
                g.setFont (uiFont (10.5f, true));
                g.drawText (formatTimecode (tapTimes[(size_t) li]),
                            ri.removeFromLeft (92),
                            juce::Justification::centredLeft, false);

                g.setColour (thisArmed ? palette.text : palette.dim);
                g.setFont (uiFont (10.5f));
                g.drawText (tapLines[(size_t) li],
                            ri.withTrimmedLeft (6).withTrimmedRight (static_cast<int> (armRect.getWidth()) + 4),
                            juce::Justification::centredLeft, true);
            }

            // 右侧「重打这一句」：一个环形箭头，点了就进入预备状态
            {
                const auto af = armRect;
                const float rr = juce::jmax (4.0f, af.getWidth() * 0.27f);

                g.setColour (thisArmed ? palette.warn.withAlpha (0.22f)
                                       : palette.text.withAlpha (0.07f));
                g.fillEllipse (af);

                juce::Path arc;
                arc.addCentredArc (af.getCentreX(), af.getCentreY(), rr, rr,
                                   0.0f, 0.75f, 5.55f, true);
                g.setColour (thisArmed ? palette.warn : palette.accent.withAlpha (0.75f));
                g.strokePath (arc, juce::PathStrokeType (1.3f,
                                                         juce::PathStrokeType::curved,
                                                         juce::PathStrokeType::rounded));

                const float ax = af.getCentreX() + rr * std::cos (5.55f);
                const float ay = af.getCentreY() + rr * std::sin (5.55f);

                juce::Path head;
                head.startNewSubPath (ax - 3.4f, ay - 2.2f);
                head.lineTo (ax + 2.2f, ay - 0.2f);
                head.lineTo (ax - 3.0f, ay + 2.6f);
                g.strokePath (head, juce::PathStrokeType (1.3f,
                                                          juce::PathStrokeType::curved,
                                                          juce::PathStrokeType::rounded));
            }
        }
    }

    //---- 列表滚动条（滑块可点、可上下拖） ---------------------------------
    {
        const auto track = tapListTrack (area);

        if (! track.isEmpty())
        {
            const auto thumb = tapListThumb (area);

            g.setColour (palette.text.withAlpha (0.10f));
            g.fillRoundedRectangle (track, 4.0f);

            g.setColour (palette.accent.withAlpha (tapListDragging ? 0.95f : 0.65f));
            g.fillRoundedRectangle (thumb, 4.0f);

            // 滑块里两道细纹，提示"这里可以拖"
            g.setColour (palette.text.withAlpha (0.35f));
            const float cx = thumb.getCentreX();
            const float cy = thumb.getCentreY();
            g.drawLine (cx - 2.0f, cy - 3.0f, cx + 2.0f, cy - 3.0f, 1.0f);
            g.drawLine (cx - 2.0f, cy + 3.0f, cx + 2.0f, cy + 3.0f, 1.0f);
        }
    }

    //---- 底部提示 -------------------------------------------------------
    if (armed)
    {
        const double at = tapTimes[(size_t) shown];
        const juce::String seekHint = at >= 0.0
            ? tr (Lang::TextId::tapArmSeek) + formatTimecode (juce::jmax (0.0, at - 3.0))
              + tr (Lang::TextId::tapArmSeek2)
            : tr (Lang::TextId::tapArmHint);

        drawText (g, juce::Rectangle<float> (cx, area.getBottom() - (v ? 66.0f : 92.0f), cw, 20.0f),
                  seekHint, 11.0f * widthScale, palette.warn, false);

        drawText (g, juce::Rectangle<float> (cx, area.getBottom() - (v ? 44.0f : 68.0f), cw, 24.0f),
                  tr (Lang::TextId::tapArmTitle) + "  #" + juce::String (shown + 1)
                    + "   " + tr (Lang::TextId::tapHint) + "   (Esc)",
                  (v ? 13.5f : 15.0f) * widthScale, palette.accent, true);
    }
    else
    {
        drawText (g, juce::Rectangle<float> (cx, area.getBottom() - (v ? 66.0f : 92.0f), cw, 20.0f),
                  tr (Lang::TextId::tapHint), 11.0f * widthScale, palette.dim, false);

        drawText (g, juce::Rectangle<float> (cx, area.getBottom() - (v ? 44.0f : 68.0f), cw, 24.0f),
                  tr (Lang::TextId::tapWillStamp) + "  " + formatTimecode (displayTime),
                  (v ? 14.0f : 16.0f) * widthScale, palette.accent, true);
    }

    const bool paused = tapMode && demoMode && ! demoPlaying;

    drawText (g, juce::Rectangle<float> (cx, area.getBottom() - (v ? 22.0f : 38.0f), cw, 18.0f),
              paused ? tr (Lang::TextId::tapPausedWarn) : tr (Lang::TextId::tapSpaceWarn),
              10.0f * widthScale, paused ? palette.warn : palette.faint, false);
}

//==============================================================================
void LyricSyncEditor::paintInfoLine (juce::Graphics&)
{
    // BPM / 时间码 / 测速已全部收进右下角的读数条（paintHud），这里不再重复绘制。
}

void LyricSyncEditor::paintSettings (juce::Graphics& g, juce::Rectangle<float> panel)
{
    g.setColour (juce::Colours::black.withAlpha (0.55f));
    g.fillRect (juce::Rectangle<float> (0.0f, 0.0f, baseW(), baseH()));

    g.setColour (palette.panel.withAlpha (0.96f));
    g.fillRoundedRectangle (panel, 18.0f);
    g.setColour (juce::Colours::white.withAlpha (0.10f));
    g.drawRoundedRectangle (panel.reduced (0.5f), 18.0f, 1.0f);

    drawText (g, juce::Rectangle<float> (panel.getX() + 24.0f, panel.getY() + 18.0f,
                                         panel.getWidth() - 100.0f, 28.0f),
              tr (Lang::TextId::settingsTitle), 19.0f, palette.text, true, -1, {}, 1.0f,
              juce::Justification::centredLeft);

    g.setColour (palette.panelEdge.withAlpha (0.7f));
    g.drawLine (panel.getX() + 24.0f, panel.getY() + 62.0f,
                panel.getRight() - 24.0f, panel.getY() + 62.0f, 1.0f);

    const bool v = (orientation == Orientation::vertical);
    const float labelX = panel.getX() + 24.0f;
    const float ctrlX  = panel.getX() + (v ? 168.0f : 190.0f);
    const float labelW = ctrlX - labelX - 10.0f;

    float y = panel.getY() + 92.0f;
    const float rowH = 40.0f;

    const Lang::TextId labels[] =
    {
        Lang::TextId::setLanguage,
        Lang::TextId::setZoom,
        Lang::TextId::setLayout,
        Lang::TextId::setSkin,
        Lang::TextId::setBackground,
        Lang::TextId::setFont,
        Lang::TextId::setColor,
        Lang::TextId::setAdvance,
        Lang::TextId::showHints,
        Lang::TextId::setCustomUrl,
        Lang::TextId::setLxUrl,
        Lang::TextId::setSimplify
    };

    for (auto id : labels)
    {
        drawText (g, juce::Rectangle<float> (labelX, y, labelW, 28.0f),
                  tr (id), 13.0f, palette.dim, false, -1, {}, 1.0f,
                  juce::Justification::centredLeft);
        y += rowH;
    }
}

void LyricSyncEditor::paintSearch (juce::Graphics& g, juce::Rectangle<float> panel)
{
    g.setColour (juce::Colours::black.withAlpha (0.58f));
    g.fillRect (juce::Rectangle<float> (0.0f, 0.0f, baseW(), baseH()));

    g.setColour (palette.panel.withAlpha (0.97f));
    g.fillRoundedRectangle (panel, 18.0f);
    g.setColour (palette.panelEdge.withAlpha (0.7f));
    g.drawRoundedRectangle (panel.reduced (0.5f), 18.0f, 1.0f);

    const float x = panel.getX() + 24.0f;
    const float w = panel.getWidth() - 48.0f;

    drawText (g, juce::Rectangle<float> (x, panel.getY() + 16.0f, w - 60.0f, 28.0f),
              tr (Lang::TextId::onlineTitle), 19.0f, palette.text, true, -1, {}, 1.0f,
              juce::Justification::centredLeft);

    g.setColour (palette.panelEdge.withAlpha (0.7f));
    g.drawLine (x, panel.getY() + 62.0f, panel.getRight() - 24.0f,
                panel.getY() + 62.0f, 1.0f);

    drawText (g, juce::Rectangle<float> (x, panel.getY() + 78.0f, 60.0f, 24.0f),
              tr (Lang::TextId::onlineSong), 12.5f, palette.dim, false, -1, {}, 1.0f,
              juce::Justification::centredLeft);
    drawText (g, juce::Rectangle<float> (x, panel.getY() + 118.0f, 60.0f, 24.0f),
              tr (Lang::TextId::onlineArtist), 12.5f, palette.dim, false, -1, {}, 1.0f,
              juce::Justification::centredLeft);

    // 状态行
    drawText (g, juce::Rectangle<float> (x + 122.0f, panel.getY() + 156.0f,
                                         w - 122.0f, 28.0f),
              searchStatus, 11.5f,
              searchBusy ? palette.warn : palette.faint, false, -1, {}, 1.0f,
              juce::Justification::centredLeft);

    //---- 结果列表 -----------------------------------------------------------
    const auto rows = searchResultRows (panel);
    const int total = static_cast<int> (searchResults.size());
    const int maxScroll = juce::jmax (0, total - static_cast<int> (rows.size()));
    const int first = juce::jlimit (0, maxScroll, searchScroll);

    if (total == 0)
    {
        g.setColour (palette.text.withAlpha (0.04f));
        g.fillRoundedRectangle (juce::Rectangle<float> (x, panel.getY() + 200.0f,
                                                        w, 180.0f), 8.0f);
        drawText (g, juce::Rectangle<float> (x + 12.0f, panel.getY() + 200.0f,
                                             w - 24.0f, 180.0f),
                  tr (Lang::TextId::onlineNoResult), 12.0f, palette.faint, false,
                  -1, {}, 1.0f, juce::Justification::centred);
    }

    for (int i = 0; i < static_cast<int> (rows.size()) && first + i < total; ++i)
    {
        const int idx = first + i;
        const auto r = rows[(size_t) i];
        const auto& res = searchResults[(size_t) idx];
        const bool selected = (idx == searchSel);

        g.setColour (selected ? palette.accent.withAlpha (0.18f)
                              : palette.text.withAlpha (i % 2 == 0 ? 0.05f : 0.025f));
        g.fillRoundedRectangle (r, 5.0f);

        if (selected)
        {
            g.setColour (palette.accent.withAlpha (0.55f));
            g.drawRoundedRectangle (r, 5.0f, 1.0f);
        }

        juce::String left = res.title;
        if (res.artist.isNotEmpty())
            left += juce::String ("  -  ") + res.artist;

        drawText (g, juce::Rectangle<float> (r.getX() + 10.0f, r.getY(),
                                             r.getWidth() - 150.0f, r.getHeight()),
                  left, 12.0f, selected ? palette.accent : palette.text, selected,
                  -1, {}, 1.0f, juce::Justification::centredLeft);

        juce::String right = res.source;

        if (res.wordSync)
            right += juce::String ("  ") + tr (Lang::TextId::onlineWordSync);

        if (res.lineCount > 0)
        {
            right += juce::String ("  ") + juce::String (res.lineCount)
                   + tr (Lang::TextId::onlineLinesSuffix);

            if (res.lineCount < 8)
                right += juce::String ("  ") + tr (Lang::TextId::onlinePartialTag);
        }

        if (res.durationSec > 0)
            right += juce::String ("  ") + formatShort (static_cast<double> (res.durationSec));

        drawText (g, juce::Rectangle<float> (r.getRight() - 140.0f, r.getY(),
                                             130.0f, r.getHeight()),
                  right, 10.0f, palette.dim, false, -1, {}, 1.0f,
                  juce::Justification::centredRight);
    }

    // 结果列表右侧滚动条
    if (total > static_cast<int> (rows.size()))
    {
        const float trackH = 180.0f;
        const float trackY = panel.getY() + 200.0f;
        const float thumbH = juce::jmax (20.0f, trackH * static_cast<float> (rows.size())
                                                 / static_cast<float> (total));
        const float t01 = static_cast<float> (first) / static_cast<float> (maxScroll);
        const float ty = trackY + (trackH - thumbH) * t01;

        g.setColour (palette.text.withAlpha (0.10f));
        g.fillRoundedRectangle (panel.getRight() - 20.0f, trackY, 5.0f, trackH, 3.0f);
        g.setColour (palette.accent.withAlpha (0.65f));
        g.fillRoundedRectangle (panel.getRight() - 20.0f, ty, 5.0f, thumbH, 3.0f);
    }

    //---- 选中结果的预览 -----------------------------------------------------
    const auto preview = juce::Rectangle<float> (x, panel.getY() + 386.0f, w, 46.0f);
    g.setColour (palette.text.withAlpha (0.045f));
    g.fillRoundedRectangle (preview, 8.0f);

    if (searchSel >= 0 && searchSel < total)
    {
        const auto& res = searchResults[(size_t) searchSel];
        juce::String text = res.preview;

        if (text.isEmpty())
            text = res.source + (res.wordSync
                                 ? juce::String ("  /  ") + tr (Lang::TextId::onlineWordSync)
                                 : juce::String());

        drawText (g, preview.reduced (10.0f, 4.0f), text, 11.0f, palette.dim, false,
                  -1, {}, 1.0f, juce::Justification::centredLeft);
    }
    else if (searchDiag.isNotEmpty() && ! searchBusy)
    {
        // 搜不到时把每一步的 HTTP 状态摆出来，方便判断是网络还是接口问题
        drawText (g, preview.reduced (10.0f, 4.0f),
                  tr (Lang::TextId::onlineDiag) + "  " + searchDiag,
                  10.0f, palette.warn, false, -1, {}, 1.0f,
                  juce::Justification::centredLeft);
    }

    drawText (g, juce::Rectangle<float> (x, panel.getY() + 478.0f, w, 18.0f),
              tr (Lang::TextId::onlineSelectHint), 10.5f, palette.faint, false,
              -1, {}, 1.0f, juce::Justification::centredLeft);

    drawText (g, juce::Rectangle<float> (x, panel.getY() + 500.0f, w, 18.0f),
              tr (Lang::TextId::onlineSourceHint), 10.0f, palette.faint, false,
              -1, {}, 1.0f, juce::Justification::centredLeft);
}

juce::Point<float> LyricSyncEditor::toLogicalPoint (juce::Point<float> physical) const
{
    return { (physical.x - originX) / juce::jmax (0.01f, uiScaleX),
             (physical.y - originY) / juce::jmax (0.01f, uiScaleY) };
}

void LyricSyncEditor::seekStandaloneAt (juce::Point<float> logical)
{
    if (standaloneSeekBar.isEmpty())
        return;

    const double t01 = juce::jlimit (0.0, 1.0,
        static_cast<double> ((logical.x - standaloneSeekBar.getX())
                             / standaloneSeekBar.getWidth()));
    const auto range = audioRange();
    const double target = range.getStart() + t01 * juce::jmax (1.0, range.getLength());

    previewOffset = 0.0;
    // displayTime = 播放位置 + 偏移，所以定位时要把偏移减掉
    processor.seekStandalone (juce::jmax (0.0, target - offsetMs / 1000.0));
    repaint();
}

/** 独立版专属：跟随内置时钟的播放进度条（可点、可拖）。 */
//==============================================================================
juce::Rectangle<float> LyricSyncEditor::hudBounds() const
{
    const auto s = stageArea();

    return { s.getX() + 18.0f, s.getBottom() - 30.0f,
             s.getWidth() - 36.0f, 24.0f };
}

juce::Rectangle<float> LyricSyncEditor::hudPlayRect() const
{
    const auto r = hudBounds();
    return { r.getX(), r.getY(), 24.0f, 24.0f };
}

juce::Rectangle<float> LyricSyncEditor::hudUndoRect() const
{
    const auto r = hudBounds();
    return { r.getRight() - 24.0f, r.getY(), 24.0f, 24.0f };
}

juce::Rectangle<float> LyricSyncEditor::hudTempoRect() const
{
    const auto r = hudBounds();
    return { r.getRight() - 24.0f - 8.0f - 26.0f, r.getY(), 26.0f, 24.0f };
}

juce::Rectangle<float> LyricSyncEditor::hudBarRect() const
{
    const auto r = hudBounds();

    // 右侧依次让出：撤销 24 + 8、测速 26 + 8、BPM 78 + 8、分隔 1 + 8、时间码 104
    const float rightEdge = r.getRight() - 24.0f - 8.0f - 26.0f - 8.0f
                          - 78.0f - 8.0f - 1.0f - 8.0f - 104.0f;

    const float left = hudPlayRect().getRight() + 10.0f;

    return { left, r.getCentreY() - 1.5f,
             juce::jmax (40.0f, rightEdge - 10.0f - left), 3.0f };
}

/** 拖进度条：独立版真的定位；VST3 里只挪歌词（方案 A），2 秒后回到跟随宿主。 */
void LyricSyncEditor::hudSeekTo (juce::Point<float> logical)
{
    const auto bar = hudBarRect();

    if (bar.getWidth() <= 1.0f)
        return;

    const double t01 = juce::jlimit (0.0, 1.0,
        static_cast<double> ((logical.x - bar.getX()) / bar.getWidth()));

    const auto range = audioRange();
    const double target = range.getStart() + t01 * juce::jmax (1.0, range.getLength());

    if (standaloneMode)
    {
        previewOffset = 0.0;
        processor.seekStandalone (juce::jmax (0.0, target - offsetMs / 1000.0));
    }
    else
    {
        // displayTime = 播放位置 + 偏移 + 预览偏移
        previewOffset = target - (processor.positionSeconds.load() + offsetMs / 1000.0);
        hudHoldUntilMs = juce::Time::getMillisecondCounterHiRes() + 2000.0;
    }

    repaint();
}

void LyricSyncEditor::paintHud (juce::Graphics& g)
{
    const auto r = hudBounds();

    if (r.getWidth() < 220.0f)
        return;

    // 淡底只在自定义背景图 / 浅色皮肤上出现，纯黑底不需要
    if (palette.text.getPerceivedBrightness() < 0.5f || skinIndex == 5)
    {
        g.setColour (juce::Colours::black.withAlpha (0.30f));
        g.fillRoundedRectangle (r.expanded (8.0f, 5.0f), 8.0f);
    }

    const bool playing = standaloneMode ? processor.standalonePlaying.load()
                                        : processor.isPlaying.load();

    //---- 播放 / 暂停 ----
    {
        const auto b = hudPlayRect();

        g.setColour (palette.text.withAlpha (0.06f));
        g.fillRoundedRectangle (b, 6.0f);

        g.setColour (palette.text.withAlpha (0.85f));

        if (playing)
        {
            g.fillRoundedRectangle (b.getCentreX() - 4.6f, b.getY() + 7.0f, 2.6f, 10.0f, 1.0f);
            g.fillRoundedRectangle (b.getCentreX() + 2.0f, b.getY() + 7.0f, 2.6f, 10.0f, 1.0f);
        }
        else
        {
            juce::Path tri;
            tri.addTriangle (b.getCentreX() - 3.0f, b.getY() + 6.5f,
                             b.getCentreX() - 3.0f, b.getBottom() - 6.5f,
                             b.getCentreX() + 5.4f, b.getCentreY());
            g.fillPath (tri);
        }
    }

    //---- 进度条 ----
    const auto bar = hudBarRect();
    const auto range = audioRange();
    const float t01 = static_cast<float> (juce::jlimit (0.0, 1.0,
        (displayTime - range.getStart()) / juce::jmax (1.0, range.getLength())));

    g.setColour (palette.track.withAlpha (0.85f));
    g.fillRoundedRectangle (bar, 1.5f);

    const float filled = bar.getWidth() * t01;

    if (filled > 0.5f)
    {
        g.setColour (palette.accent.withAlpha (hudDrag ? 1.0f : 0.88f));
        g.fillRoundedRectangle (bar.withWidth (filled), 1.5f);
    }

    {
        const float cx = bar.getX() + juce::jlimit (0.0f, bar.getWidth(), filled);
        g.setColour (palette.text);
        g.fillEllipse (cx - 5.0f, bar.getCentreY() - 5.0f, 10.0f, 10.0f);

        g.setColour (palette.bgTop.withAlpha (0.55f));
        g.drawEllipse (cx - 5.0f, bar.getCentreY() - 5.0f, 10.0f, 10.0f, 1.0f);
    }

    //---- 时间码（预览定位时前面点一个小圆点提示）----
    const float gap = 8.0f;
    const auto tcRect = juce::Rectangle<float> (bar.getRight() + 10.0f, r.getY(),
                                                104.0f, r.getHeight());

    if (std::abs (previewOffset) > 0.02)
    {
        g.setColour (palette.accent);
        g.fillEllipse (tcRect.getX() - 7.0f, r.getCentreY() - 2.0f, 4.0f, 4.0f);
    }

    drawText (g, tcRect,
              timecodeText (displayTime) + " / " + timecodeText (range.getEnd()),
              11.0f, palette.dim, false, -1.0f, {}, 1.0f, juce::Justification::centredLeft);

    //---- 分隔线 ----
    const float sepX = tcRect.getRight() + gap;

    g.setColour (palette.panelEdge.withAlpha (0.55f));
    g.drawLine (sepX, r.getY() + 7.0f, sepX, r.getBottom() - 7.0f, 1.0f);

    //---- BPM ----
    const auto bpmRect = juce::Rectangle<float> (sepX + gap, r.getY(), 78.0f, r.getHeight());
    const juce::String bpmText = (detectedBpm > 0.0 ? juce::String (detectedBpm, 1)
                                                    : juce::String ("--"))
                               + " BPM";

    drawText (g, bpmRect, bpmText, 11.0f, palette.dim, false, -1.0f, {}, 1.0f,
              juce::Justification::centredLeft);

    //---- 测速（循环箭头）----
    {
        const auto b = hudTempoRect();

        g.setColour (palette.text.withAlpha (0.05f));
        g.fillRoundedRectangle (b, 6.0f);

        const float cx = b.getCentreX(), cy = b.getCentreY();

        // 测速 = 秒表：完整圆环 + 从中心指向右上的指针 + 顶部按钮
        g.setColour (palette.dim);
        g.drawEllipse (cx - 5.2f, cy - 5.2f, 10.4f, 10.4f, 1.3f);
        g.drawLine (cx, cy, cx + 3.4f, cy - 3.0f, 1.3f);
        g.fillEllipse (cx - 1.0f, cy - 1.0f, 2.0f, 2.0f);
        g.drawLine (cx, cy - 5.2f, cx, cy - 7.0f, 1.2f);
    }

    //---- 撤销（↺）----
    {
        const auto b = hudUndoRect();

        g.setColour (palette.text.withAlpha (0.05f));
        g.fillRoundedRectangle (b, 6.0f);

        const float cx = b.getCentreX(), cy = b.getCentreY();

        // 撤销 = ↺：大缺口圆弧（开口朝上）+ 箭头落在左上
        juce::Path arc;
        arc.addCentredArc (cx, cy, 4.8f, 4.8f, 0.0f,
                           juce::MathConstants<float>::pi * 0.20f,
                           juce::MathConstants<float>::pi * 1.86f, true);

        g.setColour (palette.dim);
        g.strokePath (arc, juce::PathStrokeType (1.4f,
                        juce::PathStrokeType::curved,
                        juce::PathStrokeType::rounded));

        juce::Path head;
        head.addTriangle (cx - 5.6f, cy - 4.6f,
                          cx - 2.0f, cy - 6.4f,
                          cx - 3.6f, cy - 1.8f);
        g.fillPath (head);
    }
}

void LyricSyncEditor::paintStandaloneTransport (juce::Graphics& g)
{
    if (! standaloneMode || zenMode || editing || tapMode || standaloneSeekBar.isEmpty())
        return;

    const auto r = standaloneSeekBar;
    const auto range = audioRange();
    const double span = juce::jmax (1.0, range.getLength());
    const double t01 = juce::jlimit (0.0, 1.0,
                                     (displayTime - range.getStart()) / span);

    const auto label = juce::Rectangle<float> (r.getX(), r.getY() - 1.0f,
                                               r.getWidth(), 13.0f);
    g.setColour (palette.dim);
    g.setFont (uiFont (9.5f));
    g.drawText (timecodeText (displayTime), label, juce::Justification::centredLeft, false);
    g.drawText (timecodeText (range.getEnd()), label, juce::Justification::centredRight, false);

    const auto track = juce::Rectangle<float> (r.getX(), r.getY() + 18.0f,
                                               r.getWidth(), 6.0f);
    g.setColour (palette.track);
    g.fillRoundedRectangle (track, 3.0f);

    const float filled = track.getWidth() * static_cast<float> (t01);

    if (filled > 1.0f)
    {
        g.setColour (standaloneSeekDragging ? palette.accent
                                            : palette.accent.withAlpha (0.85f));
        g.fillRoundedRectangle (track.withWidth (filled), 3.0f);
    }

    const float cx = track.getX() + juce::jlimit (3.0f, track.getWidth() - 3.0f, filled);
    g.setColour (palette.text);
    g.fillEllipse (cx - 5.0f, track.getCentreY() - 5.0f, 10.0f, 10.0f);
}

void LyricSyncEditor::paintAbout (juce::Graphics& g, juce::Rectangle<float> panel)
{
    g.setColour (juce::Colours::black.withAlpha (0.6f));
    g.fillRect (juce::Rectangle<float> (0.0f, 0.0f, baseW(), baseH()));

    g.setColour (palette.panel.withAlpha (0.96f));
    g.fillRoundedRectangle (panel, 18.0f);
    g.setColour (juce::Colours::white.withAlpha (0.10f));
    g.drawRoundedRectangle (panel.reduced (0.5f), 18.0f, 1.0f);

    const float x = panel.getX() + 30.0f;
    const float w = panel.getWidth() - 60.0f;

    g.setColour (palette.accent);
    g.fillRoundedRectangle (panel.getX() + 30.0f, panel.getY() + 30.0f, 6.0f, 22.0f, 3.0f);

    drawText (g, juce::Rectangle<float> (x + 16.0f, panel.getY() + 24.0f, w - 60.0f, 30.0f),
              "LyricSync", 22.0f, palette.text, true, -1, {}, 1.0f,
              juce::Justification::centredLeft);

    drawText (g, juce::Rectangle<float> (x, panel.getY() + 66.0f, w, 20.0f),
              tr (Lang::TextId::aboutVersion) + "  " + kVersion, 12.5f,
              palette.dim, false, -1, {}, 1.0f, juce::Justification::centredLeft);

    drawText (g, juce::Rectangle<float> (x, panel.getY() + 90.0f, w, 20.0f),
              tr (Lang::TextId::aboutAuthor) + "  " + tr (Lang::TextId::aboutAuthorValue),
              12.5f, palette.dim, false, -1, {}, 1.0f, juce::Justification::centredLeft);

    g.setColour (palette.panelEdge.withAlpha (0.7f));
    g.drawLine (x, panel.getY() + 124.0f, panel.getRight() - 30.0f, panel.getY() + 124.0f, 1.0f);

    drawText (g, juce::Rectangle<float> (x, panel.getY() + 138.0f, w,
                                         panel.getHeight() - 190.0f),
              tr (Lang::TextId::aboutDesc), 13.0f, palette.text, false, -1, {}, 0.9f,
              juce::Justification::centredLeft);

    drawText (g, juce::Rectangle<float> (x, panel.getBottom() - 34.0f, w, 18.0f),
              tr (Lang::TextId::aboutHint), 10.5f, palette.faint, false);
}

//==============================================================================
void LyricSyncEditor::drawText (juce::Graphics& g, juce::Rectangle<float> area,
                                const juce::String& text, float size,
                                juce::Colour colour, bool bold,
                                float sungProgress, juce::Colour sungColour, float alpha,
                                juce::Justification just, const juce::String& prefix)
{
    if (text.isEmpty() || area.isEmpty())
        return;

    juce::AttributedString s;
    s.setJustification (just);
    s.setWordWrap (juce::AttributedString::byChar);

    const int idx = juce::jlimit (0, kNumFonts - 1, fontIndex);
    const int style = bold ? juce::Font::bold : juce::Font::plain;

    // 前置备注（如演唱者名字）：小一号、半透明，和歌词连成一串一起排版
    const int prefixLen = prefix.isNotEmpty() ? prefix.length() + 2 : 0;

    if (prefixLen > 0)
        s.append (prefix + "  ", juce::Font (uiTypefaceName(),
                                             juce::jmax (9.0f, size * 0.62f), style));

    if (! typefaceNeedsCjkFallback (idx))
    {
        s.append (text, juce::Font (juce::String (kFontNames[idx]), size, style));
    }
    else
    {
        // 拉丁字体 + 雅黑混排：拉丁字符用所选字体，汉字交给雅黑
        const juce::Font latin (juce::String (kFontNames[idx]), size, style);
        const juce::Font cjk ("Microsoft YaHei UI", size, style);

        int start = 0;
        bool curCJK = isCjkChar (text[0]);

        for (int i = 1; i <= text.length(); ++i)
        {
            const bool last = (i >= text.length());
            const bool cjkNow = last ? curCJK : isCjkChar (text[i]);

            if (last || cjkNow != curCJK)
            {
                s.append (text.substring (start, i), curCJK ? cjk : latin);
                start = i;
                curCJK = cjkNow;
            }
        }
    }

    s.setColour (colour.withAlpha (alpha));

    if (prefixLen > 0)
        s.setColour (juce::Range<int> (0, prefix.length()),
                     colour.withAlpha (alpha * 0.50f));

    if (sungProgress > 0.0f)
    {
        const int total = text.length();
        const int full  = juce::jlimit (0, total, static_cast<int> (std::floor (sungProgress)));
        const float frac = juce::jlimit (0.0f, 1.0f, sungProgress - static_cast<float> (full));

        if (full > 0)
            s.setColour (juce::Range<int> (prefixLen, prefixLen + full),
                         sungColour.withAlpha (alpha));

        // 正在唱的这个字：底色 → 高亮色按字内进度混合，扫光就是连续的
        if (full < total && frac > 0.001f)
        {
            const auto blend = colour.interpolatedWith (sungColour, frac);
            s.setColour (juce::Range<int> (prefixLen + full, prefixLen + full + 1),
                         blend.withAlpha (alpha));
        }
    }

    const juce::Graphics::ScopedSaveState state (g);
    s.draw (g, area);
}

//==============================================================================
void LyricSyncEditor::timerCallback()
{
    if (processor.consumeStateReloadFlag())
    {
        processor.reloadFromSettings (language);
        syncControlsFromSettings();
        applyWindowSize();
        applyUiText (false);
        refreshLookAndFeel();
        updateVisibility();
        usingDemoLyrics = false;
        fileDescription = tr (Lang::TextId::savedFileDesc);
        currentIndex = -2;
        displayIndex = -1.0f;
    }

    const double now = juce::Time::getMillisecondCounterHiRes();
    const double dt  = lastTickMs <= 0.0 ? 0.0
                     : juce::jlimit (0.0, 0.25, (now - lastTickMs) / 1000.0);
    lastTickMs = now;

    watchSearchTimeout (now);

    auto updates = processor.timeUpdates.load();
    if (updates != lastTimeUpdates)
    {
        lastTimeUpdates = updates;
        lastHostUpdateMs = now;
    }

    auto& doc = processor.lyrics();
    const auto range = audioRange();

    // 一开始播放就把手动预览偏移清掉，歌词立刻回到播放头位置
    if (! previewDragging && ! hudDrag && previewOffset != 0.0
        && processor.isPlaying.load()
        && juce::Time::getMillisecondCounterHiRes() > hudHoldUntilMs)
        previewOffset = 0.0;

    if (demoMode)
    {
        if (demoPlaying)
            demoTime = juce::jlimit (range.getStart(), range.getEnd(), demoTime + dt);

        displayTime = demoTime + previewOffset;
    }
    else
    {
        displayTime = processor.positionSeconds.load() + offsetMs / 1000.0 + previewOffset;
    }

    const int idx = doc.findIndexAt (displayTime);
    if (idx != currentIndex)
    {
        previousIndex = currentIndex;      // 用来做"上一句淡出"的颜色过渡
        currentIndex = idx;
        transition = 0.0f;
        smoothedSung = -1.0f;
    }

    if (tapMode)
    {
        // 宿主里点时间轴 / 拖播放头时，displayTime 会突然跳一大截，
        // 而打轴指针只在打点时才动 —— 于是歌词停在上一次打点的位置。
        // 这里把"与正常推进量差得太多"判为跳转，跟着把指针挪到对应那一句。
        if (tapFollowTime >= 0.0 && std::abs ((displayTime - tapFollowTime) - dt) > 0.60)
            syncTapToPlayhead (displayTime);

        tapFollowTime = displayTime;

        const auto want = tr (processor.isPlaying.load() ? Lang::TextId::tapHostPause
                                                         : Lang::TextId::tapHostPlay);

        if (tapHostTransportButton.getButtonText() != want)
            tapHostTransportButton.setButtonText (want);

        const auto wantRestart = tr (tapArmIndex >= 0 ? Lang::TextId::tapUnarm
                                                      : Lang::TextId::tapRestart);

        if (tapRestartButton.getButtonText() != wantRestart)
            tapRestartButton.setButtonText (wantRestart);
    }
    else
    {
        tapFollowTime = -1.0;
    }

    // 独立版播放/暂停按钮文字跟着状态走（换语言、程序化改状态都不会脱节）
    if (standaloneMode)
    {
        const auto want = tr (processor.standalonePlaying.load()
                                  ? Lang::TextId::standalonePause
                                  : Lang::TextId::standalonePlay);

        if (standalonePlayButton.getButtonText() != want)
            standalonePlayButton.setButtonText (want);
    }

    // 左下角提示每 6 秒换一条
    if (showHints && now - hintRotateMs > 6000.0)
    {
        hintRotateMs = now;
        ++hintIndex;
        updateHint (false);
    }

    // 每秒测一次速度（只测不占音频线程）
    if (now - tempoAnalyseMs > 1000.0)
    {
        processor.analyseTempo();
        detectedBpm = processor.beatConfidence() > 0.18 ? processor.detectedBpm() : 0.0;
        tempoAnalyseMs = now;
    }

    const float target = static_cast<float> (currentIndex);
    if (std::abs (target - displayIndex) > 2.5f)
        displayIndex = target;
    else
        displayIndex += (target - displayIndex) * 0.30f;

    // 着色模式下把歌词视图钉住，滚轮自己翻；否则播放头一动，刚选中的句子就跑了
    if (annotateMode)
    {
        annotateViewIndex = juce::jlimit (0.0f,
                                          static_cast<float> (juce::jmax (0, doc.size() - 1)),
                                          annotateViewIndex);
        displayIndex = annotateViewIndex;
    }

    transition = juce::jmin (1.0f, transition + 0.16f);

    if (now - lastAudioSaveMs > 900.0)
    {
        const double s = processor.audioStartSeconds.load();
        const double e = processor.audioEndSeconds.load();
        bool changed = false;

        if (s >= 0.0 && std::abs (s - savedAudioStart) > 0.15)
        {
            savedAudioStart = s;
            changed = true;
        }

        if (e > 0.0 && std::abs (e - savedAudioEnd) > 0.15)
        {
            savedAudioEnd = e;
            changed = true;
        }

        if (changed)
            processor.storeAudioRange (savedAudioStart, savedAudioEnd);

        lastAudioSaveMs = now;
    }

    updateLabels (displayTime);
    refreshStepperColours();
    repaint();
}

void LyricSyncEditor::updateLabels (double t)
{
    juce::ignoreUnused (t);

    // 偏移按秒显示（设计稿：−1.8 s），字号按像素显示（140 px = 1.0 倍）
    {
        const double secs = offsetMs / 1000.0;
        const auto txt = (secs < -0.049 ? juce::String ("-") : juce::String())
                       + juce::String (std::abs (secs), 1) + " s";

        offsetLabel.setText (txt, juce::dontSendNotification);
        fontLabel  .setText (juce::String (juce::roundToInt (140.0 * fontScale))
                                + tr (Lang::TextId::fontPx), juce::dontSendNotification);
    }

    audioLabel .setText (audioRangeText(), juce::dontSendNotification);

    auto label = fileDescription;
    if (lastWarning.isNotEmpty())
        label += tr (Lang::TextId::warningPrefix) + lastWarning;
    fileLabel.setText (label, juce::dontSendNotification);
}

juce::String LyricSyncEditor::hintText() const
{
    static const Lang::TextId ids[] =
    {
        Lang::TextId::hintA, Lang::TextId::hintB, Lang::TextId::hintC,
        Lang::TextId::hintD, Lang::TextId::hintE, Lang::TextId::hintF
    };

    const int n = static_cast<int> (sizeof (ids) / sizeof (ids[0]));
    return tr (ids[juce::jlimit (0, n - 1, hintIndex % n)]);
}

void LyricSyncEditor::updateHint (bool force)
{
    juce::ignoreUnused (force);
    // 左下角提示已移除，不再更新文字

    // 「界面提示」关掉时，鼠标悬停说明也一起关掉（连 TooltipWindow 都销毁，
    // 只 setVisible(false) 不行 —— 它自己的定时器会再把自己显示出来）
    if (showHints)
    {
        if (tooltipWindow == nullptr)
            tooltipWindow = std::make_unique<juce::TooltipWindow> (this, 550);
    }
    else if (tooltipWindow != nullptr)
    {
        tooltipWindow.reset();
    }
}

juce::String LyricSyncEditor::timecodeText (double t) const
{
    return formatTimecode (t);
}

juce::String LyricSyncEditor::audioRangeText() const
{
    const auto range = audioRange();
    const double len = range.getEnd() - range.getStart();

    return tr (Lang::TextId::audioPrefix) + formatShort (len);
}

juce::String LyricSyncEditor::barBeatText() const
{
    const double ppq = juce::jmax (0.0, processor.ppqPosition.load());
    const int num = juce::jmax (1, processor.timeSigNum.load());
    const int den = juce::jmax (1, processor.timeSigDen.load());
    const double beatsPerBar = static_cast<double> (num) * 4.0 / static_cast<double> (den);

    const int bar = static_cast<int> (std::floor (ppq / beatsPerBar)) + 1;
    const double beat = ppq - static_cast<double> (bar - 1) * beatsPerBar + 1.0;
    const int tick = static_cast<int> ((beat - std::floor (beat)) * 960.0);

    return tr (Lang::TextId::barPrefix) + juce::String (bar) + "."
         + juce::String (static_cast<int> (std::floor (beat))) + "."
         + juce::String::formatted ("%03d", tick)
         + sep() + juce::String (processor.bpm.load(), 1)
         + " BPM" + sep() + juce::String (num) + "/" + juce::String (den)
         + (detectedBpm > 0.0
               ? sep() + tr (Lang::TextId::bpmPrefix) + juce::String (detectedBpm, 1)
               : juce::String());
}

juce::String LyricSyncEditor::statusText() const
{
    if (tapMode)
        return tr (Lang::TextId::tapTitle);

    if (demoMode)
        return tr (Lang::TextId::statusManual);

    if (! processor.hasHostTime.load())
        return tr (Lang::TextId::statusWaiting);

    juce::String s;
    if (processor.isRecording.load())    s = tr (Lang::TextId::statusRecording);
    else if (processor.isPlaying.load()) s = tr (Lang::TextId::statusPlaying);
    else                                 s = tr (Lang::TextId::statusStopped);

    if (processor.isLooping.load())
        s += tr (Lang::TextId::statusLoop);

    if (lastHostUpdateMs > 0.0
        && juce::Time::getMillisecondCounterHiRes() - lastHostUpdateMs > 1800.0)
        s += tr (Lang::TextId::statusIdle);

    return s;
}

//==============================================================================
void LyricSyncEditor::buttonClicked (juce::Button* button)
{
    if (button == &loadButton)
    {
        // 载入：默认开文件对话框，同时留一个「粘贴文本」入口
        juce::PopupMenu menu;
        menu.addItem (1, tr (Lang::TextId::loadLyrics));
        menu.addItem (2, tr (Lang::TextId::pasteEdit));

        juce::Component::SafePointer<LyricSyncEditor> safe (this);

        menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&loadButton),
                            [safe] (int chosen)
                            {
                                if (auto* ed = safe.getComponent())
                                {
                                    if (chosen == 1) ed->openFileDialog();
                                    else if (chosen == 2) ed->enterEditMode();
                                }
                            });
        return;
    }

    // 编辑 = 直接进「编辑歌词」：每一行的时间码和文字都能点开就地改，不再弹整块文本框
    if (button == &pasteButton)         { enterTapMode(); return; }
    if (button == &orientButton)
    {
        setOrientation (orientation == Orientation::horizontal ? Orientation::vertical
                                                               : Orientation::horizontal);
        return;
    }
    if (button == &castButton)
    {
        lastWarning = tr (Lang::TextId::castSoon);
        repaint();
        return;
    }
    if (button == &fontMinusButton)     { fontStep (-5); return; }
    if (button == &fontPlusButton)      { fontStep (5);  return; }
    if (button == &segLineButton)       { setAdvanceMode (0); return; }
    if (button == &segWordButton)       { setAdvanceMode (1); return; }
    if (button == &demoButton)
    {
        loadLyricsText (demoLyricsText (language), tr (Lang::TextId::demoLyrics), true);
        return;
    }
    if (button == &applyTextButton)     { leaveEditMode (true); return; }
    if (button == &cancelTextButton)    { leaveEditMode (false); return; }
    if (button == &demoPlayButton)      { demoPlaying = true; return; }
    if (button == &demoStopButton)      { demoPlaying = false; return; }
    if (button == &measureButton)       { resetAudioMeasurement(); return; }
    if (button == &exportButton)        { exportLyrics(); return; }

    if (button == &retempoButton)
    {
        processor.resetTempo();
        detectedBpm = 0.0;
        tempoAnalyseMs = 0.0;      // 下一帧立刻重新分析
        repaint();
        return;
    }

    if (button == &tapStartButton)      { enterTapMode(); return; }
    if (button == &padStampButton)      { stampTap(); return; }
    if (button == &tapUndoButton)       { undoStamp(); return; }
    if (button == &tapRestartButton)
    {
        if (tapArmIndex >= 0)
        {
            cancelArm();
            return;
        }

        armTapLine (juce::jmin (tapIndex, static_cast<int> (tapLines.size()) - 1));
        return;
    }
    if (button == &tapNudgeBackButton)  { nudgeTap (-10); return; }
    if (button == &tapNudgeFwdButton)   { nudgeTap (10); return; }
    if (button == &tapPauseButton)      { toggleTapPause(); return; }
    if (button == &tapHostTransportButton) { toggleHostTransport(); return; }
    if (button == &tapRowRestampButton) { restampTapRow(); return; }
    if (button == &tapRowApplyButton)   { applyTapRow(); return; }
    if (button == &tapRowCancelButton)  { closeTapRowEditing(); return; }
    if (button == &tapFinishButton)     { finishTapMode(); return; }
    if (button == &tapExitButton)       { exitTapMode(); return; }

    if (button == &gearButton)          { showSettings (true); return; }
    if (button == &logoButton)          { showAbout (true); return; }
    if (button == &closeSettingsButton) { showSettings (false); return; }
    if (button == &closeAboutButton)    { showAbout (false); return; }

    if (button == &orientHButton)       { setOrientation (Orientation::horizontal); return; }
    if (button == &orientVButton)       { setOrientation (Orientation::vertical); return; }
    if (button == &pickImageButton)     { pickBackgroundImage(); return; }
    if (button == &clearImageButton)    { clearBackgroundImage(); return; }

    if (button == &setStartButton)      { setLyricsStart(); return; }
    if (button == &undoSetStartButton)  { undoSetStart(); return; }

    if (button == &standalonePlayButton)
    {
        const bool shouldPlay = ! processor.standalonePlaying.load();
        processor.setStandalonePlaying (shouldPlay);
        standalonePlayButton.setButtonText (tr (shouldPlay ? Lang::TextId::standalonePause
                                                          : Lang::TextId::standalonePlay));
        repaint();
        return;
    }

    if (button == &onlineLyricsButton) { showSearch (true); return; }
    if (button == &closeSearchButton)  { showSearch (false); return; }
    if (button == &searchGoButton)     { startSearch(); return; }
    if (button == &searchLoadButton)   { loadSelectedResult(); return; }
    if (button == &searchLxButton)     { startLxFetch(); return; }

    if (button == &simplifyToggle)
    {
        simplifyChinese = simplifyToggle.getToggleState();
        simplifyToggle.setButtonText (tr (simplifyChinese ? Lang::TextId::onlineSimplifyOn
                                                         : Lang::TextId::onlineSimplifyOff));
        commitCustomUrl();
        repaint();
        return;
    }

    if (button == &annotateButton)
    {
        if (annotateMode) exitAnnotateMode();
        else              enterAnnotateMode();

        return;
    }

    if (button == &styleApplyButton)   { applyLineStyle(); return; }
    if (button == &styleClearButton)   { clearLineStyle(); return; }
    if (button == &styleCancelButton)  { closeLineStyle(); return; }

    if (button == &hintToggle)
    {
        showHints = hintToggle.getToggleState();
        hintToggle.setButtonText (showHints ? tr (Lang::TextId::onLabel)
                                            : tr (Lang::TextId::offLabel));
        showHints = hintToggle.getToggleState();
        processor.setSetting (Settings::showHints, showHints);
        updateHint (true);
        repaint();
        return;
    }

    if (button == &demoClockToggle)
    {
        demoMode = demoClockToggle.getToggleState();
        processor.setSetting (Settings::demoClock, demoMode);
        demoPlaying = false;

        const auto range = audioRange();
        demoTime = demoMode ? juce::jlimit (range.getStart(), range.getEnd(),
                                            processor.positionSeconds.load())
                            : range.getStart();

        updateVisibility();
        resized();
        repaint();
        return;
    }

    if (button == &minusButton || button == &plusButton || button == &resetButton)
    {
        offsetHistory.push_back (offsetMs);

        if (offsetHistory.size() > 60)
            offsetHistory.erase (offsetHistory.begin());

        if (button == &resetButton)  offsetMs = 0.0;
        if (button == &minusButton)  offsetMs -= 100.0;
        if (button == &plusButton)   offsetMs += 100.0;

        offsetMs = juce::jlimit (-1800000.0, 1800000.0, offsetMs);
        processor.setSetting (Settings::offsetMs, offsetMs);
        offsetFlashMs = juce::Time::getMillisecondCounterHiRes() + 900.0;
        refreshStepperColours();
        updateLabels (displayTime);
        repaint();
    }

    if (tapMode)
        grabKeyboardFocus();
}

void LyricSyncEditor::comboBoxChanged (juce::ComboBox* box)
{
    if (updatingCombos)
        return;

    if (box == &languageBox)  { setLanguage (languageBox.getSelectedId() - 1); return; }

    if (box == &zoomBox)
    {
        const int i = juce::jlimit (0, kNumZoomSteps - 1, zoomBox.getSelectedId() - 1);
        setZoomPercent (kZoomSteps[i]);
        return;
    }

    if (box == &skinBox)      { setSkin (skinBox.getSelectedId() - 1); return; }
    if (box == &fontBox)      { setFontIndex (fontBox.getSelectedId() - 1); return; }
    if (box == &colourBox)    { setLyricColour (colourBox.getSelectedId() - 1); return; }

    if (box == &networkBox)
    {
        useSystemProxy = (networkBox.getSelectedId() == 2);
        OnlineLyrics::setUseSystemProxy (useSystemProxy);
        saveUiPrefs();
        repaint();
        return;
    }

    if (box == &advanceBox)
    {
        advanceMode = juce::jlimit (0, 1, advanceBox.getSelectedId() - 1);
        processor.setSetting (Settings::advance, advanceMode);
        repaint();
        return;
    }
}

void LyricSyncEditor::sliderValueChanged (juce::Slider* slider)
{
    if (slider == &fontSlider)
    {
        fontScale = fontSlider.getValue();
        processor.setSetting (Settings::fontScale, fontScale);
        saveUiPrefs();
        repaint();
    }
}

//==============================================================================
/** 字号步进器：140 px 为 1.0 倍，A- / A+ 每次 5 px。 */
void LyricSyncEditor::fontStep (int px)
{
    const double next = juce::jlimit (kFontScaleMin, kFontScaleMax,
                                      fontScale + static_cast<double> (px) / 140.0);

    if (std::abs (next - fontScale) < 1.0e-4)
        return;

    fontScale = next;
    fontSlider.setValue (fontScale, juce::dontSendNotification);
    processor.setSetting (Settings::fontScale, fontScale);
    saveUiPrefs();
    fontFlashMs = juce::Time::getMillisecondCounterHiRes() + 900.0;
    refreshStepperColours();
    updateLabels (displayTime);
    repaint();
}

/** 步进器数值：平时白色，正在调（拖 / 点 ‹ › / A- A+）时转强调绿，松手后再亮 0.9 秒。 */
void LyricSyncEditor::refreshStepperColours()
{
    const double now = juce::Time::getMillisecondCounterHiRes();

    const bool offHot = offsetScrub || now < offsetFlashMs;
    const bool fntHot = fontScrub   || now < fontFlashMs;

    if (offHot != offsetHot)
    {
        offsetHot = offHot;
        offsetLabel.setColour (juce::Label::textColourId,
                               offHot ? palette.accent : palette.text);
        offsetLabel.repaint();
    }

    if (fntHot != fontHot)
    {
        fontHot = fntHot;
        fontLabel.setColour (juce::Label::textColourId,
                             fntHot ? palette.accent : palette.text);
        fontLabel.repaint();
    }
}

void LyricSyncEditor::setAdvanceMode (int mode)
{
    advanceMode = juce::jlimit (0, 1, mode);
    processor.setSetting (Settings::advance, advanceMode);
    advanceBox.setSelectedId (advanceMode + 1, juce::dontSendNotification);
    segLineButton.setToggleState (advanceMode == 0, juce::dontSendNotification);
    segWordButton.setToggleState (advanceMode == 1, juce::dontSendNotification);
    repaint();
}

//==============================================================================
void LyricSyncEditor::mouseDown (const juce::MouseEvent& e)
{
    // 文本框右键：显示跟随语言的菜单
    {
        juce::TextEditor* te = (e.eventComponent == &editorBox)  ? &editorBox
                             : (e.eventComponent == &tapTextEditor) ? &tapTextEditor
                             : (e.eventComponent == &tapTimeEditor) ? &tapTimeEditor
                             : nullptr;

        if (te != nullptr && e.mods.isPopupMenu())
        {
            showEditorMenu (*te, e.getScreenPosition());
            return;
        }
    }

    // 点左下角提示词：换下一条（不打断任何其它交互）
    if (e.eventComponent == &hintLabel)
    {
        nextHint();
        return;
    }

    const auto toLogical = [this] (juce::Point<float> p)
    {
        return juce::Point<float> ((p.x - originX) / juce::jmax (0.01f, uiScaleX),
                                   (p.y - originY) / juce::jmax (0.01f, uiScaleY));
    };

    // 独立版播放进度条：按住即可拖动定位
    if (standaloneMode && ! zenMode && ! editing && ! tapMode && ! annotateMode
        && ! settingsOpen && ! aboutOpen && ! searchOpen
        && e.eventComponent == this
        && standaloneSeekBar.expanded (3.0f, 7.0f).contains (toLogicalPoint (e.position)))
    {
        standaloneSeekDragging = true;
        seekStandaloneAt (toLogicalPoint (e.position));
        return;
    }

    // 收起态图标：直接转发给对应的按钮
    {
        const int icon = railIconAt (toLogicalPoint (e.position));

        if (icon >= 0)
        {
            switch (icon)
            {
                case 0:  loadButton.triggerClick();     break;
                case 1:  pasteButton.triggerClick();    break;
                case 2:  annotateButton.triggerClick(); break;
                case 3:  exportButton.triggerClick();   break;
                default: gearButton.triggerClick();     break;
            }

            return;
        }
    }

    // 侧栏中缝的折叠键
    if (orientation == Orientation::horizontal && ! zenMode
        && e.eventComponent == this
        && railToggleRect().expanded (2.0f).contains (toLogicalPoint (e.position)))
    {
        toggleSidebar();
        return;
    }

    // 按住「偏移」数值框拖动：每 10 逻辑像素走 100 ms
    if (e.eventComponent == &offsetLabel && ! editing && ! tapMode)
    {
        offsetHistory.push_back (offsetMs);

        if (offsetHistory.size() > 60)
            offsetHistory.erase (offsetHistory.begin());

        offsetScrub   = true;
        offsetScrubMs = offsetMs;
        return;
    }

    // 按住「字号」数值框拖动：无极调节，2 逻辑像素 = 1 px
    if (e.eventComponent == &fontLabel && ! editing && ! tapMode)
    {
        fontScrub   = true;
        fontScrubPx = 140.0 * fontScale;
        return;
    }

    // 搜索框右侧小圆钮：从 LX Music 取当前播放歌词
    if (e.eventComponent == this && ! editing && ! tapMode
        && sideSearchRefresh().expanded (2.0f).contains (toLogicalPoint (e.position)))
    {
        startLxFetch();
        return;
    }

    // 搜索结果：点一条就直接载入
    if (e.eventComponent == this && ! editing && ! tapMode && ! searchResults.empty()
        && (orientation == Orientation::vertical || railWidth() > 60.0f))
    {
        const auto p = toLogicalPoint (e.position);
        const int idx = sideResultAt (p);

        if (idx >= 0 && idx < static_cast<int> (searchResults.size()))
        {
            searchSel = idx;
            loadSelectedResult();
            return;
        }

        if (sideResultsArea().contains (p))
            return;
    }

    // 右下角读数条：播放/暂停、测速、撤销、拖动定位
    if (! zenMode && ! editing && ! tapMode && ! annotateMode
        && ! settingsOpen && ! aboutOpen && ! searchOpen
        && e.eventComponent == this)
    {
        const auto p = toLogicalPoint (e.position);

        if (hudPlayRect().expanded (3.0f).contains (p))
        {
            toggleHostTransport();
            return;
        }

        if (hudTempoRect().expanded (3.0f).contains (p))
        {
            retempoButton.triggerClick();
            return;
        }

        if (hudUndoRect().expanded (3.0f).contains (p))
        {
            undoSetStartButton.triggerClick();
            return;
        }

        if (hudBarRect().expanded (0.0f, 9.0f).contains (p))
        {
            hudDrag = true;
            hudSeekTo (p);
            return;
        }
    }

    if (aboutOpen)
    {
        if (! aboutPanelArea().contains (toLogical (e.position)))
            showAbout (false);

        return;
    }

    if (settingsOpen)
    {
        if (! settingsPanelArea().contains (toLogical (e.position)))
            showSettings (false);

        return;
    }

    if (searchOpen)
    {
        const auto p = toLogical (e.position);
        const auto panel = searchPanelArea();

        if (! panel.contains (p))
        {
            showSearch (false);
            return;
        }

        const auto rows = searchResultRows (panel);
        const int total = static_cast<int> (searchResults.size());
        const int maxScroll = juce::jmax (0, total - static_cast<int> (rows.size()));
        const int first = juce::jlimit (0, maxScroll, searchScroll);

        for (int i = 0; i < static_cast<int> (rows.size()) && first + i < total; ++i)
        {
            if (rows[(size_t) i].contains (p))
            {
                searchSel = first + i;
                repaint();
                return;
            }
        }

        return;
    }

    if (annotateMode && e.eventComponent == this)
    {
        const auto p = toLogical (e.position);
        const auto area = stageArea();

        if (styleEditIndex >= 0)
        {
            const auto card = styleCard();
            const auto swatches = styleSwatches (card);

            for (int i = 0; i < static_cast<int> (swatches.size()); ++i)
            {
                if (swatches[(size_t) i].expanded (3.0f).contains (p))
                {
                    styleEditColour = i - 1;       // 0 = 默认
                    applyColourToSelection (styleEditColour);
                    repaint();
                    return;
                }
            }

            // 点在卡片里（备注框 / 按钮区）交给子控件处理；点在外面则收起卡片，
            // 并继续按下面的规则重新选句 / 重新弹框
            if (card.expanded (6.0f).contains (p))
                return;

            closeLineStyle();
        }

        if (! area.contains (p))
            return;

        const int idx = stageLineAt (p);

        if (idx < 0)
            return;

        // 右键：就在鼠标旁弹出小选择框
        if (e.mods.isRightButtonDown() || e.mods.isPopupMenu())
        {
            if (annotateSelLo < 0 || idx < annotateSelLo || idx > annotateSelHi)
                annotateSelLo = annotateSelHi = idx;

            styleCardAnchor = p;
            openLineStyle (annotateSelLo);
            return;
        }

        // 左键：像选文字一样拖选歌词，可跨行
        annotateAnchor = idx;
        annotateSelLo = annotateSelHi = idx;
        annotateDragging = true;
        setMouseCursor (juce::MouseCursor::IBeamCursor);
        repaint();
        return;
    }

    // 子控件（备注框等）转发来的事件，在着色模式下不要再落到拖动预览上
    if (annotateMode)
        return;

    if (tapMode)
    {
        if (tapEditIndex >= 0)
            return;                       // 正在编辑某一行，画布点击不记录

        grabKeyboardFocus();

        // 点「已记录」里的某一行：打开这一行的编辑器
        const auto rows = tapListRows (stageArea());
        const auto ids  = tapListIndices();
        const auto p = toLogical (e.position);

        // 点滚动条：抓住滑块就能上下拖；点轨道则先把滑块移到该处再拖
        const auto track = tapListTrack (stageArea());
        // 向右扩出一段好点的热区，但别越过行的右缘去抢「重打」按钮的点击
        const auto hit = juce::Rectangle<float> (track.getX() - 2.0f, track.getY(),
                                                 track.getWidth() + 12.0f,
                                                 track.getHeight());

        if (! track.isEmpty() && hit.contains (p))
        {
            const auto thumb = tapListThumb (stageArea());
            const int  maxScroll = tapListMaxScroll();
            const float usable = juce::jmax (1.0f, track.getHeight() - thumb.getHeight());

            if (! thumb.expanded (4.0f, 0.0f).contains (p))
            {
                // 轨道点击：让滑块中心对准鼠标
                const float centre = p.y - track.getY() - thumb.getHeight() * 0.5f;
                const float t01 = juce::jlimit (0.0f, 1.0f, centre / usable);
                tapListScroll = juce::jlimit (0, maxScroll,
                    juce::roundToInt ((1.0f - t01) * static_cast<float> (maxScroll)));
            }

            tapListDragging = true;
            tapDragStartY = p.y;
            tapDragStartScroll = tapListScroll;
            setMouseCursor (juce::MouseCursor::UpDownResizeCursor);
            repaint();
            return;
        }

        for (int i = 0; i < static_cast<int> (rows.size()); ++i)
        {
            if (! rows[(size_t) i].contains (p))
                continue;

            // 点右侧的环形箭头 = 预备重打这一句；点行本身 = 编辑这一句
            if (tapRowArmButton (rows[(size_t) i]).contains (p))
                armTapLine (ids[(size_t) i]);
            else
                editTapRow (ids[(size_t) i]);

            return;
        }

        stampTap();
        return;
    }

    // 歌词区域：按住上下拖动 = 手动预览（不出声，只是把歌词往前/后翻看）
    if (! editing && stageArea().transformedBy (uiTransform).contains (e.position))
    {
        previewDragging = true;
        previewDragStartY = e.y;
        previewDragStartOffset = previewOffset;
        setMouseCursor (juce::MouseCursor::DraggingHandCursor);
    }
}

void LyricSyncEditor::mouseDrag (const juce::MouseEvent& e)
{
    if (hudDrag && e.eventComponent == this)
    {
        hudSeekTo (toLogicalPoint (e.position));
        return;
    }

    if (offsetScrub)
    {
        const double dx = e.getDistanceFromDragStartX() / juce::jmax (0.01f, uiScaleX);
        const double steps = std::round (dx / 10.0);          // 10px = 一格 = 0.1 秒
        offsetMs = juce::jlimit (-1800000.0, 1800000.0, offsetScrubMs + steps * 100.0);
        processor.setSetting (Settings::offsetMs, offsetMs);
        offsetFlashMs = juce::Time::getMillisecondCounterHiRes() + 900.0;
        refreshStepperColours();
        repaint();
        return;
    }

    if (fontScrub)
    {
        const double dx = e.getDistanceFromDragStartX() / juce::jmax (0.01f, uiScaleX);
        const double px = juce::jlimit (140.0 * kFontScaleMin, 140.0 * kFontScaleMax,
                                        fontScrubPx + dx * 0.5);
        fontScale = px / 140.0;
        processor.setSetting (Settings::fontScale, fontScale);
        fontFlashMs = juce::Time::getMillisecondCounterHiRes() + 900.0;
        refreshStepperColours();
        repaint();
        return;
    }

    if (standaloneSeekDragging && e.eventComponent == this)
    {
        seekStandaloneAt (toLogicalPoint (e.position));
        return;
    }

    if (tapListDragging && e.eventComponent == this)
    {
        const auto area = stageArea();
        const auto p = juce::Point<float> ((e.position.x - originX) / juce::jmax (0.01f, uiScaleX),
                                           (e.position.y - originY) / juce::jmax (0.01f, uiScaleY));
        const auto track = tapListTrack (area);
        const auto thumb = tapListThumb (area);
        const int maxScroll = tapListMaxScroll();
        const float usable = juce::jmax (1.0f, track.getHeight() - thumb.getHeight());

        // 往上拖 = 往回翻旧句子；位移换算成「行」
        const float deltaRows = (tapDragStartY - p.y) / usable
                              * static_cast<float> (maxScroll);
        const int next = juce::jlimit (0, maxScroll,
            juce::roundToInt (static_cast<float> (tapDragStartScroll) + deltaRows));

        if (next != tapListScroll)
        {
            tapListScroll = next;
            repaint();
        }

        return;
    }

    if (annotateDragging && e.eventComponent == this)
    {
        const auto p = juce::Point<float> ((e.position.x - originX) / juce::jmax (0.01f, uiScaleX),
                                           (e.position.y - originY) / juce::jmax (0.01f, uiScaleY));
        const int idx = stageLineAt (p);

        if (idx >= 0)
        {
            annotateSelLo = juce::jmin (annotateAnchor, idx);
            annotateSelHi = juce::jmax (annotateAnchor, idx);
            repaint();
        }

        return;
    }

    if (! previewDragging || e.eventComponent != this)
        return;

    // 往上拖 = 往后看；0.04 秒/像素，一句大约几十像素
    previewOffset = previewDragStartOffset
                  + static_cast<double> (previewDragStartY - e.y) * 0.04;

    repaint();
}

void LyricSyncEditor::nextHint()
{
    ++hintIndex;
    hintRotateMs = juce::Time::getMillisecondCounterHiRes();
    updateHint (true);
    repaint();
}

void LyricSyncEditor::mouseWheelMove (const juce::MouseEvent& e,
                                      const juce::MouseWheelDetails& wheel)
{
    const auto mods = juce::ModifierKeys::currentModifiers;
    const bool mod = mods.isCtrlDown() || mods.isCommandDown();

    // 在线搜词面板：滚轮翻结果列表
    if (searchOpen && e.eventComponent == this
        && searchPanelArea().transformedBy (uiTransform).contains (e.position))
    {
        const int total = static_cast<int> (searchResults.size());
        const int maxScroll = juce::jmax (0, total - 6);
        searchScroll = juce::jlimit (0, maxScroll,
                                     searchScroll + (wheel.deltaY > 0.0f ? -1 : 1));
        repaint();
        return;
    }

    // 着色模式：滚轮按"行"翻歌词，让每一句都能选中；视图不受播放头影响
    if (annotateMode && styleEditIndex < 0 && ! mod && e.eventComponent == this
        && stageArea().transformedBy (uiTransform).contains (e.position))
    {
        const int total = processor.lyrics().size();
        const float step = wheel.deltaY > 0.0f ? -1.0f : 1.0f;

        annotateViewIndex = juce::jlimit (0.0f,
                                          static_cast<float> (juce::jmax (0, total - 1)),
                                          annotateViewIndex + (wheel.isReversed ? -step : step));
        displayIndex = annotateViewIndex;
        repaint();
        return;
    }

    // 打轴界面：滚轮直接翻「已记录」列表（不按住 Ctrl）
    if (tapMode && tapEditIndex < 0 && ! mod && e.eventComponent == this
        && stageArea().transformedBy (uiTransform).contains (e.position))
    {
        scrollTapList (wheel.deltaY > 0.0f ? 1 : -1);
        return;
    }

    // 内联搜索结果：滚轮翻页（不按住 Ctrl，也不需要点进面板）
    if (! mod && e.eventComponent == this && ! searchResults.empty()
        && sideResultsArea().transformedBy (uiTransform).contains (e.position))
    {
        const int total = static_cast<int> (searchResults.size());
        const int fit   = static_cast<int> (sideResultRows().size());
        const int maxScroll = juce::jmax (0, total - fit);

        searchScroll = juce::jlimit (0, maxScroll,
                                     searchScroll + (wheel.deltaY > 0.0f ? -1 : 1));
        repaint();
        return;
    }

    // 只有「鼠标停在歌词区 + 按住 Ctrl」才改字号，其它情况照常把滚轮交给别处
    if (! mod || e.eventComponent != this
        || settingsOpen || aboutOpen || editing || tapMode
        || ! stageArea().transformedBy (uiTransform).contains (e.position))
    {
        juce::AudioProcessorEditor::mouseWheelMove (e, wheel);
        return;
    }

    double step = wheel.deltaY >= 0.0 ? 0.04 : -0.04;

    if (wheel.isReversed)
        step = -step;

    const double next = juce::jlimit (kFontScaleMin, kFontScaleMax, fontScale + step);

    if (std::abs (next - fontScale) < 1.0e-4)
        return;

    fontScale = next;
    fontSlider.setValue (fontScale, juce::dontSendNotification);
    processor.setSetting (Settings::fontScale, fontScale);

    // 滚轮可能连续触发，限制写文件频率
    const double now = juce::Time::getMillisecondCounterHiRes();

    if (now - wheelSaveMs > 400.0)
    {
        wheelSaveMs = now;
        saveUiPrefs();
    }

    repaint();
}

void LyricSyncEditor::mouseUp (const juce::MouseEvent&)
{
    if (hudDrag)
    {
        hudDrag = false;
        // 松手后先保持 2 秒预览，方便你看清楚跳到哪一句，再回到跟随宿主
        hudHoldUntilMs = juce::Time::getMillisecondCounterHiRes() + 2000.0;
        repaint();
    }

    if (offsetScrub)
    {
        offsetScrub = false;
        offsetFlashMs = juce::Time::getMillisecondCounterHiRes() + 900.0;
        saveUiPrefs();
        repaint();
    }

    if (fontScrub)
    {
        fontScrub = false;
        fontFlashMs = juce::Time::getMillisecondCounterHiRes() + 900.0;
        saveUiPrefs();
        repaint();
    }

    if (standaloneSeekDragging)
    {
        standaloneSeekDragging = false;
        repaint();
    }

    if (tapListDragging)
    {
        tapListDragging = false;
        setMouseCursor (juce::MouseCursor::NormalCursor);
        repaint();
    }

    if (annotateDragging)
    {
        annotateDragging = false;
        repaint();
    }

    if (! previewDragging)
        return;

    previewDragging = false;
    setMouseCursor (juce::MouseCursor::NormalCursor);
    repaint();
}

void LyricSyncEditor::mouseMove (const juce::MouseEvent& e)
{
    if (previewDragging || e.eventComponent != this)
        return;

    if (tapMode)
    {
        const auto area = stageArea();
        const auto rows = tapListRows (area);
        const auto p = juce::Point<float> ((e.position.x - originX) / juce::jmax (0.01f, uiScaleX),
                                           (e.position.y - originY) / juce::jmax (0.01f, uiScaleY));

        // 停在滚动条滑块/轨道上：给上下拖动的光标，提示它可以拖
        const auto track = tapListTrack (area);
        const auto hit = juce::Rectangle<float> (track.getX() - 2.0f, track.getY(),
                                                 track.getWidth() + 12.0f,
                                                 track.getHeight());

        if (! track.isEmpty() && hit.contains (p))
        {
            setMouseCursor (juce::MouseCursor::UpDownResizeCursor);
            return;
        }

        for (const auto& r : rows)
            if (r.contains (p))
            {
                setMouseCursor (juce::MouseCursor::PointingHandCursor);
                return;
            }

        setMouseCursor (juce::MouseCursor::NormalCursor);
        return;
    }

    if (annotateMode)
    {
        const auto p = juce::Point<float> ((e.position.x - originX) / juce::jmax (0.01f, uiScaleX),
                                           (e.position.y - originY) / juce::jmax (0.01f, uiScaleY));

        if (styleEditIndex >= 0)
        {
            bool over = false;

            for (const auto& sw : styleSwatches (styleCard()))
                if (sw.expanded (3.0f).contains (p))
                    over = true;

            setMouseCursor (over ? juce::MouseCursor::PointingHandCursor
                                 : juce::MouseCursor::NormalCursor);
            return;
        }

        // 歌词区里是 I 形文本选择光标，正好对应"拖选歌词"
        setMouseCursor (stageArea().contains (p) ? juce::MouseCursor::IBeamCursor
                                                  : juce::MouseCursor::NormalCursor);
        return;
    }

    // 走带条（进度条 / 播放暂停 / 测速 / 撤销 / 时间码）保持普通鼠标箭头，
    // 只有歌词区才是手形——用户明确要求，别在这里给手型光标。
    if (standaloneMode && ! editing && ! settingsOpen && ! aboutOpen && ! searchOpen
        && standaloneSeekBar.expanded (3.0f, 7.0f).contains (toLogicalPoint (e.position)))
    {
        setMouseCursor (juce::MouseCursor::NormalCursor);
        return;
    }

    const auto logical = toLogicalPoint (e.position);
    const bool overHud = ! zenMode && ! editing && ! tapMode && ! annotateMode
                      && ! settingsOpen && ! aboutOpen && ! searchOpen
                      && hudBounds().expanded (8.0f, 6.0f).contains (logical);

    const bool hand = ! overHud && ! editing && ! settingsOpen && ! aboutOpen
                   && stageArea().transformedBy (uiTransform).contains (e.position);

    setMouseCursor (hand ? juce::MouseCursor::DraggingHandCursor
                         : juce::MouseCursor::NormalCursor);
}

bool LyricSyncEditor::keyPressed (const juce::KeyPress& key)
{
    if (key.getKeyCode() == juce::KeyPress::escapeKey)
    {
        if (annotateMode)
        {
            if (styleEditIndex >= 0) closeLineStyle();
            else                     exitAnnotateMode();

            return true;
        }

        if (searchOpen)   { showSearch (false); return true; }
        if (zenMode)      { toggleZenMode(); return true; }
        if (aboutOpen)    { showAbout (false); return true; }
        if (settingsOpen) { showSettings (false); return true; }
        if (tapMode)      { exitTapMode(); return true; }
    }

    if (tapMode && ! settingsOpen && ! aboutOpen)
    {
        if (key.getKeyCode() == juce::KeyPress::spaceKey
            || key.getKeyCode() == juce::KeyPress::returnKey)
        {
            grabKeyboardFocus();
            stampTap();
            return true;
        }

        if (key.getKeyCode() == juce::KeyPress::backspaceKey)
        {
            undoStamp();
            return true;
        }

        if (key.getTextCharacter() == 'p' || key.getTextCharacter() == 'P')
        {
            toggleTapPause();
            return true;
        }

        if (key.getKeyCode() == juce::KeyPress::escapeKey && tapArmIndex >= 0)
        {
            cancelArm();
            return true;
        }
    }

    return false;
}

//==============================================================================
void LyricSyncEditor::showSettings (bool shouldShow)
{
    // 关闭设置时把「自定义服务」输入框里的内容收下来，
    // 避免用户没按回车就点面板外、随后打开搜索面板时输入被旧值覆盖。
    if (! shouldShow)
        commitCustomUrl();

    settingsOpen = shouldShow;

    if (settingsOpen)
        aboutOpen = false;

    updateVisibility();
    resized();
    repaint();
}

//==============================================================================
void LyricSyncEditor::showSearch (bool shouldShow)
{
    // 在线搜词已改为侧栏内联：这里只负责把焦点交给搜索框，不再有弹窗
    searchOpen = false;

    if (! shouldShow)
    {
        repaint();
        return;
    }

    if (annotateMode)
        exitAnnotateMode();

    settingsOpen = false;
    aboutOpen = false;
    searchStartedMs = 0.0;

    updateVisibility();
    resized();
    repaint();

    if (searchTitleEditor.isVisible())
        searchTitleEditor.grabKeyboardFocus();
}

void LyricSyncEditor::commitCustomUrl()
{
    customSearchUrl = customUrlEditor.getText().trim();
    lxMusicUrl      = lxUrlEditor.getText().trim();

    if (lxMusicUrl.isEmpty())
        lxMusicUrl = "http://127.0.0.1:23330";

    simplifyChinese = simplifyToggle.getToggleState();
    useSystemProxy  = (networkBox.getSelectedId() == 2);
    OnlineLyrics::setUseSystemProxy (useSystemProxy);
    saveUiPrefs();
}

void LyricSyncEditor::startSearch()
{
    if (searchBusy)
        return;

    auto typed = searchTitleEditor.getText().trim();

    if (typed.isEmpty())
    {
        searchStatus = tr (Lang::TextId::onlineEnterTitle);
        repaint();
        return;
    }

    commitCustomUrl();

    // 一个输入框要能吃下「晴天 周杰伦」/「晴天 - 周杰伦」这种写法：
    // 有 "-" 就按它切；否则按最后一个空格切（后半段不长时才算歌手）。
    juce::String artist = searchArtistEditor.getText().trim();

    if (artist.isEmpty())
    {
        const int dash = juce::jmax (typed.lastIndexOfChar ('-'),
                                     typed.lastIndexOfChar ((juce::juce_wchar) 0x2014));

        if (dash > 0 && dash < typed.length() - 1)
        {
            artist = typed.substring (dash + 1).trim();
            typed  = typed.substring (0, dash).trim();
        }
        else
        {
            const int sp = typed.lastIndexOfChar (' ');

            if (sp > 0 && typed.length() - sp > 1 && typed.length() - sp <= 17)
            {
                artist = typed.substring (sp + 1).trim();
                typed  = typed.substring (0, sp).trim();
            }
        }

        if (typed.isEmpty())
            typed = artist;
    }

    const auto title = typed;
    const int duration = audioTotalSeconds > 0.0
                       ? static_cast<int> (audioTotalSeconds) : 0;
    const auto url = customSearchUrl;

    ++searchToken;
    const int token = searchToken;

    searchBusy = true;
    searchStartedMs = juce::Time::getMillisecondCounterHiRes();
    searchResults.clear();
    searchSel = -1;
    searchScroll = 0;
    searchDiag = {};
    searchStatus = tr (Lang::TextId::onlineSearching);
    repaint();

    // 网络请求全部在后台线程，回调只带结果数据，不触碰已销毁的编辑器。
    // 整段 try/catch：任何异常都要回一次结果，否则 searchBusy 会永久卡死。
    juce::Component::SafePointer<LyricSyncEditor> safe (this);

    networkPool.addJob ([safe, title, artist, duration, url, token]()
    {
        OnlineLyrics::Outcome outcome;

        try
        {
            outcome = OnlineLyrics::search (title, artist, duration, url);
        }
        catch (...)
        {
            outcome.error = "exception";
            outcome.detail = "exception";
        }

        juce::MessageManager::callAsync ([safe, outcome, token]()
        {
            if (auto* ed = safe.getComponent())
                ed->searchFinished (outcome, token);
        });
    });
}

/** 连接超时管不住"连上后不返回数据"，这里给整次请求兜一个总时限。 */
void LyricSyncEditor::watchSearchTimeout (double nowMs)
{
    if (! searchBusy || searchStartedMs <= 0.0)
        return;

    if (nowMs - searchStartedMs < 40000.0)
        return;

    ++searchToken;                 // 让迟到的回调作废，不会覆盖下一次搜索
    searchBusy = false;
    searchStartedMs = 0.0;
    searchStatus = tr (Lang::TextId::onlineTimeout);
    repaint();
}

void LyricSyncEditor::searchFinished (OnlineLyrics::Outcome outcome, int token)
{
    // 看门狗已经判超时、或用户又发起了新搜索：这份过期结果直接丢掉
    if (token != searchToken)
        return;

    searchBusy = false;
    searchStartedMs = 0.0;
    searchDiag = outcome.detail;
    searchResults = std::move (outcome.results);
    searchSel = searchResults.empty() ? -1 : 0;
    searchScroll = 0;

    if (! searchResults.empty())
    {
        searchStatus = tr (Lang::TextId::onlineFound) + " "
                     + juce::String (static_cast<int> (searchResults.size()))
                     + "  |  " + outcome.via;

        if (outcome.via == "Kugou")
            searchStatus += "  |  " + tr (Lang::TextId::onlineExactHint);
    }
    else if (outcome.error == "empty")
    {
        searchStatus = tr (Lang::TextId::onlineEnterTitle);
    }
    else if (outcome.error == "exception")
    {
        searchStatus = tr (Lang::TextId::onlineFailed);
    }
    else
    {
        searchStatus = tr (Lang::TextId::onlineNotFound);
    }

    repaint();
}

void LyricSyncEditor::loadSelectedResult()
{
    if (searchBusy || searchSel < 0 || searchSel >= static_cast<int> (searchResults.size()))
        return;

    const auto result = searchResults[(size_t) searchSel];

    ++searchToken;
    const int token = searchToken;

    searchBusy = true;
    searchStartedMs = juce::Time::getMillisecondCounterHiRes();
    searchStatus = tr (Lang::TextId::onlineLoading);
    repaint();

    const bool simplify = simplifyChinese;

    juce::Component::SafePointer<LyricSyncEditor> safe (this);

    networkPool.addJob ([safe, result, simplify, token]()
    {
        juce::String lrc;

        try
        {
            lrc = OnlineLyrics::download (result, simplify);
        }
        catch (...)
        {
            lrc = {};
        }

        juce::MessageManager::callAsync ([safe, lrc, label = result.title, token]()
        {
            if (auto* ed = safe.getComponent())
                ed->downloadFinished (lrc, label, token);
        });
    });
}

void LyricSyncEditor::downloadFinished (juce::String lrc, juce::String label, int token)
{
    if (token != searchToken)
        return;

    searchBusy = false;

    if (lrc.trim().isEmpty())
    {
        searchStatus = tr (Lang::TextId::onlineLoadFailed);
        repaint();
        return;
    }

    const int lines = OnlineLyrics::countTimedLines (lrc);

    showSearch (false);
    loadLyricsText (lrc, tr (Lang::TextId::onlineLoaded) + "  " + label, false);

    if (lines > 0 && lines < 8)
        lastWarning = tr (Lang::TextId::onlinePartial);
}

void LyricSyncEditor::startLxFetch()
{
    if (searchBusy)
        return;

    commitCustomUrl();

    searchBusy = true;
    searchStatus = tr (Lang::TextId::onlineLxLoading);
    repaint();

    const auto url = lxMusicUrl;
    const bool simplify = simplifyChinese;

    juce::Component::SafePointer<LyricSyncEditor> safe (this);

    networkPool.addJob ([safe, url, simplify]()
    {
        OnlineLyrics::LxResult result;

        try
        {
            result = OnlineLyrics::fetchFromLx (url, simplify);
        }
        catch (...)
        {
            result.error = "connect";
        }

        juce::MessageManager::callAsync ([safe, result]()
        {
            if (auto* ed = safe.getComponent())
                ed->lxFinished (result);
        });
    });
}

void LyricSyncEditor::lxFinished (OnlineLyrics::LxResult result)
{
    searchBusy = false;

    if (result.error == "connect")
    {
        searchStatus = tr (Lang::TextId::lxNoService);
        repaint();
        return;
    }

    if (result.error == "noLyric" || result.lrc.trim().isEmpty())
    {
        searchStatus = tr (Lang::TextId::lxNoLyric);
        repaint();
        return;
    }

    juce::String label = tr (Lang::TextId::lxLoaded);

    if (result.title.isNotEmpty())
        label += "  " + result.title;

    if (result.artist.isNotEmpty())
        label += "  -  " + result.artist;

    showSearch (false);
    loadLyricsText (result.lrc, label, false);

    if (result.lineCount > 0 && result.lineCount < 8)
        lastWarning = tr (Lang::TextId::onlinePartial);
}

void LyricSyncEditor::showAbout (bool shouldShow)
{
    aboutOpen = shouldShow;

    if (aboutOpen)
        settingsOpen = false;

    updateVisibility();
    resized();
    repaint();
}

//==============================================================================
void LyricSyncEditor::setSkin (int index)
{
    skinIndex = juce::jlimit (0, UiPalette::numSkins() - 1, index);
    palette = UiPalette::forSkin (skinIndex);

    processor.setSetting (Settings::skin, skinIndex);
    processor.setSetting (Settings::customBg, customBgPath);
    saveUiPrefs();
    refreshLookAndFeel();
    updateVisibility();

    if (skinBox.getSelectedId() != skinIndex + 1)
    {
        updatingCombos = true;
        skinBox.setSelectedId (skinIndex + 1, juce::dontSendNotification);
        updatingCombos = false;
    }

    repaint();
}

void LyricSyncEditor::setFontIndex (int index)
{
    fontIndex = juce::jlimit (0, kNumFonts - 1, index);
    lookAndFeel.typeface = uiTypefaceName();

    processor.setSetting (Settings::font, fontIndex);
    saveUiPrefs();
    updateScaledFonts();
    resized();
    repaint();
}

void LyricSyncEditor::setLyricColour (int index)
{
    colourIndex = juce::jlimit (0, kNumLyricColours - 1, index);
    processor.setSetting (Settings::colour, colourIndex);
    saveUiPrefs();
    repaint();
}

void LyricSyncEditor::setZoomPercent (int percent)
{
    zoomPercent = juce::jlimit (50, 200, percent);
    processor.setSetting (Settings::zoom, zoomPercent);
    saveUiPrefs();
    applyWindowSize();
    resized();
    repaint();
}

void LyricSyncEditor::setOrientation (Orientation next)
{
    orientation = next;
    processor.setSetting (Settings::orientation,
                          orientation == Orientation::vertical ? 1 : 0);
    saveUiPrefs();

    applyWindowSize();
    refreshLookAndFeel();
    resized();
    repaint();
}

void LyricSyncEditor::pickBackgroundImage()
{
    chooser = std::make_unique<juce::FileChooser> (
        tr (Lang::TextId::setBackground),
        juce::File::getSpecialLocation (juce::File::userPicturesDirectory),
        "*.png;*.jpg;*.jpeg;*.bmp;*.webp");

    const int imageFlags = juce::FileBrowserComponent::openMode
                         | juce::FileBrowserComponent::canSelectFiles;

    chooser->launchAsync (imageFlags, [this] (const juce::FileChooser& fc)
    {
        auto file = fc.getResult();

        if (! file.existsAsFile() || ! looksLikeImageFile (file.getFullPathName()))
            return;

        auto img = juce::ImageFileFormat::loadFrom (file);

        if (! img.isValid())
            return;

        customBg = img;

        // 复制一份到应用目录：原图被移动/删除，皮肤也不会丢
        juce::File stored = file;
        auto dir = skinsDir();

        if (dir.createDirectory().wasOk() || dir.isDirectory())
        {
            auto dest = dir.getChildFile (file.getFileName());

            if (dest != file && (! dest.existsAsFile() || dest.deleteFile()))
            {
                if (file.copyFileTo (dest))
                    stored = dest;
            }
        }

        customBgPath = stored.getFullPathName();
        processor.setSetting (Settings::customBg, customBgPath);
        setSkin (5);                    // setSkin 里会把 bg 写进全局偏好
        updateVisibility();
        repaint();
    });
}

void LyricSyncEditor::clearBackgroundImage()
{
    customBg = juce::Image {};
    customBgPath.clear();
    processor.setSetting (Settings::customBg, "");

    if (skinIndex == 5)
        setSkin (0);

    updateVisibility();
    repaint();
}

void LyricSyncEditor::setLanguage (int next)
{
    next = juce::jlimit (0, 1, next);
    if (next == language)
        return;

    language = next;
    processor.setSetting (Settings::language, language);
    applyUiText (true);
}

//==============================================================================
void LyricSyncEditor::openFileDialog()
{
    chooser = std::make_unique<juce::FileChooser> (
        tr (Lang::TextId::fileChooserTitle),
        juce::File::getSpecialLocation (juce::File::userHomeDirectory),
        "*.lrc;*.krc;*.txt;*.srt;*.vtt;*.doc;*.docx");

    const int browserFlags = juce::FileBrowserComponent::openMode
                           | juce::FileBrowserComponent::canSelectFiles;

    chooser->launchAsync (browserFlags, [this] (const juce::FileChooser& fc)
    {
        auto file = fc.getResult();
        if (file.existsAsFile())
            loadLyricsFile (file);
    });
}

void LyricSyncEditor::loadLyricsFile (const juce::File& file)
{
    juce::MemoryBlock data;
    const bool loaded = file.loadFileAsData (data);

    juce::String text;
    bool isKrc = false;
    bool isWord = false;
    double krcTotal = -1.0;

    const auto ext = file.getFileExtension().toLowerCase();

    if (loaded && (ext == ".doc" || ext == ".docx"))
    {
        text = WordDoc::extractText (file, data);
        isWord = true;
    }
    else if (loaded && Krc::isKrcData (data.getData(), static_cast<int> (data.getSize())))
    {
        const auto plain = Krc::decrypt (data);
        text = Krc::toEnhancedLrc (plain);
        krcTotal = Krc::totalSecondsFromPlain (plain);
        isKrc = true;
    }
    else if (loaded)
    {
        text = decodeTextFile (data);
    }

    if (text.isEmpty())
    {
        fileDescription = tr (Lang::TextId::readFailed) + file.getFileName();
        lastWarning = tr (Lang::TextId::emptyFile);
        usingDemoLyrics = false;
        repaint();
        return;
    }

    const auto info = processor.setLyricsText (text, file.getFullPathName(), language);
    offsetMs = 0.0;
    processor.setSetting (Settings::offsetMs, 0.0);

    audioTotalSeconds = isKrc ? krcTotal : -1.0;
    processor.setSetting (Settings::audioTotal, audioTotalSeconds);

    fileDescription = file.getFileName() + sep()
                    + (isWord ? tr (Lang::TextId::wordFormat)
                              : (isKrc ? tr (Lang::TextId::krcFormat) : info.formatName))
                    + sep() + juce::String (info.lineCount)
                    + tr (Lang::TextId::lineCountSuffix);

    if (info.karaokeLines > 0)
        fileDescription += tr (Lang::TextId::karaokeSuffix);

    lastWarning = isKrc ? tr (Lang::TextId::krcLoaded) : info.warning;
    usingDemoLyrics = false;
    currentIndex = -2;
    displayIndex = -1.0f;
    demoTime = audioRange().getStart();

    archiveCurrentLyrics();
    resetAudioMeasurement();

    refreshLookAndFeel();
    repaint();
}

void LyricSyncEditor::loadLyricsText (const juce::String& text,
                                      const juce::String& label, bool isDemo)
{
    const auto info = processor.setLyricsText (text, {}, language);
    offsetMs = 0.0;
    processor.setSetting (Settings::offsetMs, 0.0);

    audioTotalSeconds = -1.0;
    processor.setSetting (Settings::audioTotal, -1.0);

    fileDescription = label + sep() + info.formatName
                    + sep() + juce::String (info.lineCount)
                    + tr (Lang::TextId::lineCountSuffix);
    lastWarning = info.warning;
    usingDemoLyrics = isDemo;
    currentIndex = -2;
    displayIndex = -1.0f;
    demoTime = audioRange().getStart();

    if (! isDemo)
        archiveCurrentLyrics();

    repaint();
}

/** 把当前歌词写进本机歌词库（文档/LyricSync/Lyrics），路径随工程保存。 */
void LyricSyncEditor::archiveCurrentLyrics()
{
    const auto text = processor.getSetting (Settings::lyricsText, "").toString();

    if (text.trim().isEmpty())
        return;

    auto dir = lyricsLibraryDir();

    if (dir.createDirectory().failed())
        return;

    juce::String title = "Lyrics";

    for (const auto& raw : juce::StringArray::fromLines (text))
    {
        const auto line = raw.trim();

        if (line.startsWithIgnoreCase ("[ti:"))
        {
            title = line.fromFirstOccurrenceOf (":", false, false)
                        .upToLastOccurrenceOf ("]", false, false).trim();
            break;
        }
    }

    if (title.isEmpty())
        title = "Lyrics";

    const auto hash = juce::String::toHexString (static_cast<juce::int64> (text.hashCode64()))
                        .substring (8).toLowerCase();

    auto file = dir.getChildFile (sanitiseFilePart (title) + "-" + hash + ".lrc");
    file.replaceWithText (text, false, false, "\n");

    archivedPath = file.getFullPathName();
    processor.setSetting (Settings::lyricsPath, archivedPath);

    if (! fileDescription.contains (tr (Lang::TextId::savedToLibrary)))
        fileDescription += sep() + tr (Lang::TextId::savedToLibrary);
}

void LyricSyncEditor::resetAudioMeasurement()
{
    processor.resetAudioRange();
    processor.storeAudioRange (-1.0, -1.0);
    savedAudioStart = -1.0;
    savedAudioEnd   = -1.0;
    lastAudioSaveMs = juce::Time::getMillisecondCounterHiRes();
    repaint();
}

//==============================================================================
void LyricSyncEditor::enterEditMode()
{
    editing = true;
    tapMode = false;

    // 增强 LRC / KRC 的 <mm:ss.xx> 逐字锚点在编辑框里非常乱，显示时去掉；
    // 原文留在 editOriginalText，没改动就原样保留（不丢逐字时间）。
    editOriginalText = processor.getSetting (Settings::lyricsText, "").toString();
    editShownText = stripWordAnchors (editOriginalText);

    editorBox.setText (editShownText);
    editorBox.setVisible (true);
    editorBox.toFront (true);
    refreshLookAndFeel();
    updateVisibility();
    resized();
    repaint();
}

void LyricSyncEditor::leaveEditMode (bool apply)
{
    if (apply)
    {
        const auto current = editorBox.getText();

        if (current.trim() != editShownText.trim())
            loadLyricsText (current, tr (Lang::TextId::pastedLabel));
    }

    editing = false;
    editorBox.setVisible (false);
    updateVisibility();
    resized();
    repaint();
}

//==============================================================================
void LyricSyncEditor::enterTapMode()
{
    const auto source = editing ? editorBox.getText()
                                : processor.getSetting (Settings::lyricsText, "").toString();

    // 行首的 [mm:ss.xx] / [mm:ss:xx] → 秒
    const auto parseLeadingStamp = [] (const juce::String& raw) -> double
    {
        auto s = raw.trimStart();

        if (! s.startsWithChar ('['))
            return -1.0;

        const int close = s.indexOfChar (']');

        if (close <= 1)
            return -1.0;

        auto head = s.substring (1, close).trim();

        if (! head.containsChar (':'))
            return -1.0;

        const int c1 = head.indexOfChar (':');
        const int mins = head.substring (0, c1).getIntValue();
        const double secs = head.substring (c1 + 1).replaceCharacter (':', '.').getDoubleValue();

        if (mins < 0 || secs < 0.0)
            return -1.0;

        return static_cast<double> (mins) * 60.0 + secs;
    };

    std::vector<juce::String> plain;
    std::vector<double>       times;

    for (const auto& raw : juce::StringArray::fromLines (source))
    {
        auto t = stripTimeTags (raw).trim();

        if (t.isEmpty())
            continue;

        plain.push_back (t);
        times.push_back (parseLeadingStamp (raw));
    }

    if (plain.empty())
    {
        // 没歌词就先打开大文本框让用户粘贴，别只给一句警告
        if (! editing)
        {
            lastWarning = tr (Lang::TextId::tapNoLines);
            enterEditMode();
        }
        else
        {
            lastWarning = tr (Lang::TextId::tapNoLines);
            repaint();
        }
        return;
    }

    tapLines = std::move (plain);
    tapTimes = std::move (times);
    tapEditIndex = -1;
    tapArmIndex = -1;
    tapListScroll = 0;
    tapFollowTime = demoMode ? demoTime : displayTime;

    // 已经打过轴的行保留时间，指针停到第一个还没打点的句子
    {
        int firstMissing = 0;

        while (firstMissing < static_cast<int> (tapTimes.size())
               && tapTimes[(size_t) firstMissing] >= 0.0)
            ++firstMissing;

        tapIndex = firstMissing;
    }

    tapMode = true;
    editing = false;
    editorBox.setVisible (false);
    lastWarning = {};

    if (demoMode)
    {
        const auto range = audioRange();

        demoTime = (tapIndex > 0 && tapTimes[(size_t) (tapIndex - 1)] >= 0.0)
                     ? juce::jlimit (range.getStart(), range.getEnd(), tapTimes[(size_t) (tapIndex - 1)])
                     : range.getStart();

        demoPlaying = tapIndex < static_cast<int> (tapLines.size());
    }

    updateVisibility();
    resized();
    grabKeyboardFocus();
    repaint();
}

void LyricSyncEditor::stampTap()
{
    if (! tapMode || tapLines.empty())
        return;

    // 用实时播放位置，而不是 30 Hz 刷新出来的 displayTime：
    // 缓存值最多慢一帧（33 ms），打点会系统性偏晚。
    double t = demoMode ? demoTime
                        : processor.positionSeconds.load() + offsetMs / 1000.0;

    if (! std::isfinite (t) || t < 0.0)
        t = 0.0;

    // 预备重打时只写被预备的那一句；否则写指针所指的下一句
    const int target = tapArmIndex >= 0 ? tapArmIndex : tapIndex;

    if (target < 0 || target >= static_cast<int> (tapLines.size()))
        return;

    if (target > 0)
    {
        const double prev = tapTimes[(size_t) (target - 1)];

        // 暂停时时间码不再走，连点两次会得到同一个值 —— 直接忽略，避免打出假时间
        if (prev >= 0.0 && t < prev + 0.10)
        {
            lastWarning = tr (tapArmIndex >= 0 ? Lang::TextId::tapArmTooEarly
                                               : Lang::TextId::tapTooClose);
            repaint();
            return;
        }
    }

    // 预备重打时如果点晚了、越过了下一句，仍然记录（这是用户的判断），
    // 但提示一下，方便接着用 −10 ms 或编辑卡修正。
    bool late = false;

    if (tapArmIndex >= 0 && target + 1 < static_cast<int> (tapLines.size()))
    {
        const double next = tapTimes[(size_t) (target + 1)];

        if (next >= 0.0 && t > next - 0.05)
            late = true;
    }

    tapTimes[(size_t) target] = t;

    if (tapArmIndex >= 0)
    {
        tapArmIndex = -1;
        tapIndex = juce::jmin (target + 1, static_cast<int> (tapLines.size()));
        tapListScroll = 0;
        lastWarning = tr (Lang::TextId::tapArmDone) + juce::String (target + 1);
    }
    else
    {
        ++tapIndex;

        if (tapIndex >= static_cast<int> (tapLines.size()))
        {
            finishTapMode();
            return;
        }
    }

    if (late)
        lastWarning = tr (Lang::TextId::tapTimeOrder) + "  (" + formatTimecode (t) + ")";

    repaint();
}

void LyricSyncEditor::undoStamp()
{
    if (! tapMode || tapIndex <= 0)
        return;

    --tapIndex;
    tapTimes[(size_t) tapIndex] = -1.0;
    repaint();
}

//==============================================================================
juce::Rectangle<float> LyricSyncEditor::styleCard() const
{
    const float w = 360.0f, h = 178.0f;
    const float maxX = juce::jmax (12.0f, baseW() - w - 12.0f);
    const float maxY = juce::jmax (12.0f, baseH() - h - 12.0f);

    // 贴着鼠标右下角浮出，但整张卡片一定留在画布里
    return { juce::jlimit (12.0f, maxX, styleCardAnchor.x + 14.0f),
             juce::jlimit (12.0f, maxY, styleCardAnchor.y - 28.0f), w, h };
}

std::vector<juce::Rectangle<float>> LyricSyncEditor::styleSwatches (juce::Rectangle<float> card) const
{
    std::vector<juce::Rectangle<float>> out;

    const float d   = 22.0f;
    const float gap = 27.0f;
    const int   n   = kNumLineColours + 1;            // 默认 + 8 色
    const float totalW = static_cast<float> (n - 1) * gap + d;
    float x = card.getCentreX() - totalW * 0.5f + 12.0f;
    const float y = card.getY() + 56.0f;

    for (int i = 0; i < n; ++i)
    {
        out.push_back ({ x, y, d, d });
        x += gap;
    }

    return out;
}

/** 与 paintStage 里 karaoke stack 完全一致的行几何，供鼠标命中测试复用。 */
juce::Rectangle<float> LyricSyncEditor::stageLineRect (int index,
                                                       juce::Rectangle<float> area) const
{
    const auto& doc = processor.lyrics();

    const float refW = (orientation == Orientation::vertical ? 520.0f : 900.0f);
    const float widthScale = juce::jlimit (0.80f, 1.70f, area.getWidth() / refW)
                           * static_cast<float> (fontScale);

    const float innerH = area.getHeight() - 78.0f;
    const float maxLineSize = 34.0f * widthScale;
    const float minGap = maxLineSize * 1.16f;

    int radius = static_cast<int> ((innerH / juce::jmax (1.0f, minGap) - 1.0f) * 0.5f);
    radius = juce::jlimit (1, 4, radius);

    const auto inner = juce::Rectangle<float> (area.getX() + 26.0f, area.getY() + 36.0f,
                                               area.getWidth() - 52.0f, innerH);
    const float spacing = inner.getHeight() / static_cast<float> (2 * radius + 1);
    const float centreY = inner.getCentreY();

    const bool hasCurrent = currentIndex >= 0 && currentIndex < doc.size();
    const bool hasTrans   = hasCurrent && doc.line (currentIndex).translation.isNotEmpty();
    const float lift      = hasTrans ? 9.0f * widthScale : 0.0f;

    const float d  = static_cast<float> (index) - displayIndex;
    const float size = juce::jmax (17.0f, 34.0f - 14.0f * std::abs (d)) * widthScale;
    const float y = centreY + d * spacing - size * 0.70f
                  - (index == currentIndex ? lift : 0.0f);

    return { area.getX() + 40.0f, y, area.getWidth() - 80.0f, size * 1.42f };
}

/** 点在歌词舞台上时返回最近的可见行；点在两行之间也照样能选中，拖选更顺手。 */
int LyricSyncEditor::stageLineAt (juce::Point<float> p) const
{
    const auto& doc = processor.lyrics();
    const int total = doc.size();
    const auto area = stageArea();

    if (total <= 0 || ! area.contains (p))
        return -1;

    const int lo = juce::jmax (0, static_cast<int> (std::floor (displayIndex)) - 5);
    const int hi = juce::jmin (total - 1, static_cast<int> (std::ceil (displayIndex)) + 5);

    // 只认屏幕上真正画出来的那几行（与 paintStage 的可见半径一致）
    const float widthScale = juce::jlimit (0.80f, 1.70f,
                                          area.getWidth() / (orientation == Orientation::vertical ? 520.0f : 900.0f))
                           * static_cast<float> (fontScale);
    const float minGap = 34.0f * widthScale * 1.16f;
    const int radius = juce::jlimit (1, 4,
        static_cast<int> (((area.getHeight() - 78.0f) / juce::jmax (1.0f, minGap) - 1.0f) * 0.5f));

    int best = -1;
    float bestDist = 1.0e9f;

    for (int i = lo; i <= hi; ++i)
    {
        if (std::abs (static_cast<float> (i) - displayIndex) > static_cast<float> (radius) + 0.95f)
            continue;

        const auto r = stageLineRect (i, area);

        if (r.contains (p))
            return i;

        const float dist = std::abs (p.y - r.getCentreY());

        if (dist < bestDist)
        {
            bestDist = dist;
            best = i;
        }
    }

    return best;
}

void LyricSyncEditor::enterAnnotateMode()
{
    if (editing || tapMode)
        return;

    annotateMode = true;
    styleEditIndex = -1;
    annotateSelLo = annotateSelHi = annotateAnchor = -1;
    annotateDragging = false;
    annotateViewIndex = static_cast<float> (juce::jmax (0, currentIndex));
    displayIndex = annotateViewIndex;      // 进入后视图固定，播放头不再把选中的句子推走
    lastWarning = {};

    annotateButton.setButtonText (tr (Lang::TextId::tapFinish));
    annotateButton.setToggleState (true, juce::dontSendNotification);
    setMouseCursor (juce::MouseCursor::IBeamCursor);

    updateVisibility();
    resized();
    repaint();
}

void LyricSyncEditor::exitAnnotateMode()
{
    annotateMode = false;
    styleEditIndex = -1;
    annotateSelLo = annotateSelHi = annotateAnchor = -1;
    annotateDragging = false;

    annotateButton.setButtonText (tr (Lang::TextId::annotate));
    annotateButton.setToggleState (false, juce::dontSendNotification);
    setMouseCursor (juce::MouseCursor::NormalCursor);

    updateVisibility();
    resized();
    repaint();
}

void LyricSyncEditor::openLineStyle (int index)
{
    const auto& doc = processor.lyrics();

    if (index < 0 || index >= doc.size())
        return;

    styleEditIndex = index;

    const auto it = lineStyles.find (index);
    styleEditColour = (it == lineStyles.end()) ? -1 : it->second.colourIndex;
    styleNoteEditor.setText (it == lineStyles.end() ? juce::String() : it->second.note, false);

    updateVisibility();
    resized();
    repaint();
}

void LyricSyncEditor::applyColourToSelection (int colourIndex)
{
    if (styleEditIndex < 0)
        return;

    const int lo = (annotateSelLo >= 0 ? annotateSelLo : styleEditIndex);
    const int hi = (annotateSelHi >= lo ? annotateSelHi : lo);
    const int total = processor.lyrics().size();

    // 颜色立刻写进歌词，但保留这一句原有的备注
    for (int i = lo; i <= hi && i < total; ++i)
    {
        const auto it = lineStyles.find (i);
        const juce::String note = (it == lineStyles.end()) ? juce::String()
                                                           : it->second.note;

        if (colourIndex < 0 && note.isEmpty())
        {
            lineStyles.erase (i);
        }
        else
        {
            LineStyle st;
            st.colourIndex = colourIndex;
            st.note = note;
            lineStyles[i] = st;
        }
    }

    saveLineStyles();
}

void LyricSyncEditor::applyLineStyle()
{
    if (styleEditIndex < 0)
        return;

    // 只有备注需要点「应用」；颜色在点色块时已经生效，这里保留原样。
    // 一次框选了多句时，备注只写在第一句前面，后面几句不重复显示。
    const juce::String note = styleNoteEditor.getText().trim();

    const int lo = (annotateSelLo >= 0 ? annotateSelLo : styleEditIndex);
    const int hi = (annotateSelHi >= lo ? annotateSelHi : lo);
    const int total = processor.lyrics().size();

    for (int i = lo; i <= hi && i < total; ++i)
    {
        const auto it = lineStyles.find (i);
        const int colourIndex = (it == lineStyles.end()) ? -1 : it->second.colourIndex;
        const juce::String lineNote = (i == lo) ? note : juce::String();

        if (colourIndex < 0 && lineNote.isEmpty())
        {
            lineStyles.erase (i);
        }
        else
        {
            LineStyle st;
            st.colourIndex = colourIndex;
            st.note = lineNote;
            lineStyles[i] = st;
        }
    }

    saveLineStyles();
    closeLineStyle();
}

void LyricSyncEditor::clearLineStyle()
{
    if (styleEditIndex < 0)
        return;

    const int lo = (annotateSelLo >= 0 ? annotateSelLo : styleEditIndex);
    const int hi = (annotateSelHi >= lo ? annotateSelHi : lo);
    const int total = processor.lyrics().size();

    for (int i = lo; i <= hi && i < total; ++i)
        lineStyles.erase (i);

    saveLineStyles();
    closeLineStyle();
}

void LyricSyncEditor::closeLineStyle()
{
    styleEditIndex = -1;
    styleNoteEditor.setText ({}, false);

    updateVisibility();
    resized();
    repaint();
}

void LyricSyncEditor::setLyricsStart()
{
    const double pos = demoMode ? demoTime
                                : processor.positionSeconds.load();

    if (! std::isfinite (pos) || pos < 0.0)
        return;

    offsetHistory.push_back (offsetMs);

    if (offsetHistory.size() > 60)
        offsetHistory.erase (offsetHistory.begin());

    // 把「当前播放位置」当成歌词时间轴的 0 秒
    offsetMs = juce::jlimit (-1800000.0, 1800000.0, -pos * 1000.0);
    processor.setSetting (Settings::offsetMs, offsetMs);

    // 音频块挪过位置，之前测到的音频长度已经不对了
    resetAudioMeasurement();

    lastWarning = tr (Lang::TextId::setStartDone) + "  " + formatTimecode (pos);
    repaint();
}

void LyricSyncEditor::undoSetStart()
{
    if (offsetHistory.empty())
    {
        lastWarning = tr (Lang::TextId::setStartNone);
        repaint();
        return;
    }

    offsetMs = offsetHistory.back();
    offsetHistory.pop_back();

    processor.setSetting (Settings::offsetMs, offsetMs);
    lastWarning = tr (Lang::TextId::setStartUndo) + "  " + juce::String (offsetMs, 0) + " ms";
    repaint();
}

//==============================================================================
/** 第一个「正式唱词」行：跳过开头的作词、作曲、编曲这类制作信息。 */
int LyricSyncEditor::firstVocalLineIndex() const
{
    const auto& d = processor.lyrics();

    // 只在前 24 行里找：制作信息不会更长，也避免把中段歌词误判成 credit
    const int limit = juce::jmin (d.size(), 24);

    for (int i = 0; i < limit; ++i)
        if (! looksLikeCredit (d.line (i).text))
            return i;

    return 0;
}

juce::Colour LyricSyncEditor::lineColourFor (int index, juce::Colour fallback) const
{
    const auto it = lineStyles.find (index);

    if (it == lineStyles.end() || it->second.colourIndex < 0)
        return fallback;

    return juce::Colour (kLineColours[juce::jlimit (0, kNumLineColours - 1,
                                                    it->second.colourIndex)]);
}

juce::String LyricSyncEditor::lineNoteFor (int index) const
{
    const auto it = lineStyles.find (index);

    if (it == lineStyles.end() || it->second.note.isEmpty())
        return {};

    // 以前的版本会把同一段备注写进选区里的每一句；这里做兼容处理：
    // 相邻且「备注 + 颜色」都相同的一组句子，只在第一句前面显示备注。
    if (index > 0)
    {
        const auto prev = lineStyles.find (index - 1);

        if (prev != lineStyles.end()
            && prev->second.note.isNotEmpty()
            && prev->second.note == it->second.note
            && prev->second.colourIndex == it->second.colourIndex)
            return {};
    }

    return it->second.note;
}

void LyricSyncEditor::saveLineStyles()
{
    juce::StringArray items;

    for (const auto& kv : lineStyles)
    {
        if (kv.second.colourIndex < 0 && kv.second.note.isEmpty())
            continue;

        juce::MemoryBlock mb;
        const auto utf8 = kv.second.note.toRawUTF8();
        mb.append (utf8, kv.second.note.getNumBytesAsUTF8());

        const auto b64 = juce::Base64::toBase64 (mb.getData(), mb.getSize());

        items.add (juce::String (kv.first) + "=" + juce::String (kv.second.colourIndex)
                   + "|" + b64);
    }

    processor.setSetting (Settings::lineStyles, items.joinIntoString (";"));
}

void LyricSyncEditor::loadLineStyles()
{
    lineStyles.clear();

    const auto text = processor.getSetting (Settings::lineStyles, "").toString();

    for (const auto& item : juce::StringArray::fromTokens (text, ";", ""))
    {
        if (item.isEmpty())
            continue;

        const int eq = item.indexOfChar ('=');

        if (eq <= 0)
            continue;

        const int index = item.substring (0, eq).getIntValue();
        auto rest = item.substring (eq + 1);
        const int bar = rest.indexOfChar ('|');

        LineStyle st;
        st.colourIndex = (bar >= 0 ? rest.substring (0, bar) : rest).getIntValue();

        if (bar >= 0)
        {
            const auto b64 = rest.substring (bar + 1);
            juce::MemoryOutputStream mo;

            if (juce::Base64::convertFromBase64 (mo, b64) && mo.getDataSize() > 0)
                st.note = juce::String::fromUTF8 (static_cast<const char*> (mo.getData()),
                                                  static_cast<int> (mo.getDataSize()));
        }

        lineStyles[index] = st;
    }
}

void LyricSyncEditor::armTapLine (int index)
{
    if (! tapMode || tapLines.empty())
        return;

    index = juce::jlimit (0, static_cast<int> (tapLines.size()) - 1, index);

    tapArmIndex = index;
    tapEditIndex = -1;

    // 让被预备的那一句出现在列表里
    {
        std::vector<int> ids;

        for (int i = 0; i < static_cast<int> (tapTimes.size()); ++i)
            if (tapTimes[(size_t) i] >= 0.0)
                ids.push_back (i);

        const bool v = (orientation == Orientation::vertical);
        const int  maxRows = v ? 4 : 7;
        const int  n = static_cast<int> (ids.size());

        tapListScroll = 0;

        for (int p = 0; p < n; ++p)
        {
            if (ids[(size_t) p] == index)
            {
                if (p < n - maxRows)
                    tapListScroll = n - maxRows - p;

                break;
            }
        }
    }

    // 手动预览：可以真的倒回 3 秒并停住，按播放就能顺着唱到这一句
    if (demoMode)
    {
        const auto range = audioRange();

        double at = tapTimes[(size_t) index];

        if (at < 0.0)
            at = (index > 0 && tapTimes[(size_t) (index - 1)] >= 0.0)
                   ? tapTimes[(size_t) (index - 1)] + 3.0
                   : range.getStart();

        demoTime = juce::jlimit (range.getStart(), range.getEnd(),
                                 juce::jmax (range.getStart(), at - 3.0));
        demoPlaying = false;
        lastWarning = tr (Lang::TextId::tapArmDemo);
    }
    else
    {
        lastWarning = {};
    }

    if (tapPauseButton.isVisible())
        tapPauseButton.setButtonText (tr (Lang::TextId::tapResume));

    repaint();
}

void LyricSyncEditor::cancelArm()
{
    tapArmIndex = -1;
    lastWarning = tr (Lang::TextId::tapArmCancel);
    repaint();
}

/** 正在编辑的那一行在屏幕上的位置；被滚出可视区时返回空。 */
juce::Rectangle<float> LyricSyncEditor::tapEditRowRect() const
{
    if (! tapMode || tapEditIndex < 0)
        return {};

    const auto rows = tapListRows (stageArea());
    const auto ids  = tapListIndices();

    for (int i = 0; i < static_cast<int> (rows.size())
                    && i < static_cast<int> (ids.size()); ++i)
        if (ids[(size_t) i] == tapEditIndex)
            return rows[(size_t) i];

    return {};
}

juce::Rectangle<float> LyricSyncEditor::tapRowArmButton (juce::Rectangle<float> row) const
{
    return row.removeFromRight (row.getHeight()).reduced (2.0f);
}


//==============================================================================
double LyricSyncEditor::parseTimecode (const juce::String& text) const
{
    auto s = text.trim().replaceCharacter (',', '.');

    if (s.isEmpty())
        return -1.0;

    for (auto c : s)
        if (! juce::CharacterFunctions::isDigit (c) && c != ':' && c != '.')
            return -1.0;

    const int colon = s.indexOfChar (':');

    if (colon >= 0)
    {
        const int    mins = s.substring (0, colon).getIntValue();
        const auto secs = s.substring (colon + 1);

        if (! secs.containsOnly ("0123456789."))
            return -1.0;

        const double v = secs.getDoubleValue();

        if (mins < 0 || v < 0.0)
            return -1.0;

        return static_cast<double> (mins) * 60.0 + v;
    }

    return juce::jmax (0.0, s.getDoubleValue());
}

void LyricSyncEditor::editTapRow (int index)
{
    if (! tapMode || tapLines.empty())
        return;

    index = juce::jlimit (0, static_cast<int> (tapLines.size()) - 1, index);

    tapEditIndex = index;
    tapArmIndex = -1;

    tapTextEditor.setText (tapLines[(size_t) index], false);
    tapTimeEditor.setText (tapTimes[(size_t) index] >= 0.0
                             ? formatTimecode (tapTimes[(size_t) index])
                             : juce::String(), false);

    // 手动预览下把播放头停在这一句，方便边听边改
    if (demoMode && tapTimes[(size_t) index] >= 0.0)
    {
        const auto range = audioRange();
        demoTime = juce::jlimit (range.getStart(), range.getEnd(), tapTimes[(size_t) index]);
        demoPlaying = false;
    }

    lastWarning = {};

    updateVisibility();
    resized();
    repaint();
}

void LyricSyncEditor::applyTapRow()
{
    if (! tapMode || tapEditIndex < 0 || tapEditIndex >= static_cast<int> (tapLines.size()))
    {
        closeTapRowEditing();
        return;
    }

    const int i = tapEditIndex;
    const double t = parseTimecode (tapTimeEditor.getText());

    if (t < 0.0)
    {
        lastWarning = tr (Lang::TextId::tapBadTime);
        repaint();
        return;
    }

    if (i > 0 && tapTimes[(size_t) (i - 1)] >= 0.0 && t <= tapTimes[(size_t) (i - 1)] + 0.02)
    {
        lastWarning = tr (Lang::TextId::tapTimeOrder);
        repaint();
        return;
    }

    if (i + 1 < static_cast<int> (tapLines.size())
        && tapTimes[(size_t) (i + 1)] >= 0.0
        && t >= tapTimes[(size_t) (i + 1)] - 0.02)
    {
        lastWarning = tr (Lang::TextId::tapTimeOrder);
        repaint();
        return;
    }

    const auto newText = tapTextEditor.getText().trim();

    if (newText.isNotEmpty())
        tapLines[(size_t) i] = newText;

    tapTimes[(size_t) i] = t;

    // 指针跳到下一个"还没打"的句子，已记录的不会被覆盖
    int next = i + 1;

    while (next < static_cast<int> (tapLines.size()) && tapTimes[(size_t) next] >= 0.0)
        ++next;

    tapIndex = next;
    lastWarning = {};

    closeTapRowEditing();
}

void LyricSyncEditor::restampTapRow()
{
    if (tapEditIndex < 0)
        return;

    double t = demoMode ? demoTime : displayTime;

    if (! std::isfinite (t) || t < 0.0)
        t = 0.0;

    tapTimeEditor.setText (formatTimecode (t), false);
    tapTimeEditor.grabKeyboardFocus();
}

void LyricSyncEditor::closeTapRowEditing()
{
    tapEditIndex = -1;
    tapTextEditor.setText ({}, false);
    tapTimeEditor.setText ({}, false);

    updateVisibility();
    resized();
    repaint();
}

void LyricSyncEditor::toggleHostTransport()
{
    // 独立版：直接控内置时钟
    if (standaloneMode)
    {
        processor.setStandalonePlaying (! processor.standalonePlaying.load());
        repaint();
        return;
    }

    // 打轴界面里的旧路径保留（此时 demoMode 恒为 false）
    if (tapMode && demoMode)
    {
        toggleTapPause();
        return;
    }

   #if JUCE_WINDOWS
    if (auto* peer = getPeer())
    {
        auto* hwnd = static_cast<HWND> (peer->getNativeHandle());

        if (hwnd != nullptr)
        {
            // 找到宿主主窗口（插件的顶层窗口），给它发一次空格
            HWND target = GetAncestor (hwnd, GA_ROOTOWNER);

            if (target == nullptr)
                target = GetAncestor (hwnd, GA_ROOT);

            if (target != nullptr)
            {
                PostMessage (target, WM_KEYDOWN, VK_SPACE, 0x00390001);
                PostMessage (target, WM_KEYUP,   VK_SPACE, 0xC0390001);
                lastWarning = tr (Lang::TextId::tapHostKeyHint);
            }
        }
    }
   #endif

    repaint();
}

//==============================================================================
void LyricSyncEditor::nudgeTap (int deltaMs)
{
    if (! tapMode || tapIndex <= 0)
        return;

    const int i = tapIndex - 1;

    if (tapTimes[(size_t) i] < 0.0)
        return;

    double v = tapTimes[(size_t) i] + static_cast<double> (deltaMs) / 1000.0;

    if (i > 0)
        v = juce::jmax (v, tapTimes[(size_t) (i - 1)] + 0.03);

    tapTimes[(size_t) i] = juce::jmax (0.0, v);
    lastWarning = tr (Lang::TextId::tapNudged) + "  "
                 + (deltaMs >= 0 ? "+" : "") + juce::String (deltaMs) + " ms";
    repaint();
}

void LyricSyncEditor::toggleTapPause()
{
    if (! tapMode)
        return;

    if (demoMode)
    {
        demoPlaying = ! demoPlaying;
        tapPauseButton.setButtonText (tr (demoPlaying ? Lang::TextId::tapPause
                                                     : Lang::TextId::tapResume));
        lastWarning = demoPlaying ? juce::String() : tr (Lang::TextId::tapPausedWarn);
    }
    else
    {
        // 宿主模式下播放由 DAW 控制，这里只给提示
        lastWarning = tr (Lang::TextId::tapPausedWarn);
    }

    repaint();
}


//==============================================================================
void LyricSyncEditor::showEditorMenu (juce::TextEditor& te, juce::Point<int> screenPos)
{
    auto sel = te.getHighlightedRegion();

    // 没选中任何文字时，把光标所在的整行选上：
    // 否则"剪切 / 复制 / 删除"全是灰的，看起来就像菜单不能用
    if (sel.getLength() <= 0)
    {
        const auto text = te.getText();
        const int caret = juce::jlimit (0, text.length(), te.getCaretPosition());

        int start = -1;

        for (int i = caret - 1; i >= 0; --i)
            if (text[i] == '\n') { start = i; break; }

        int end = text.length();

        for (int i = caret; i < text.length(); ++i)
            if (text[i] == '\n') { end = i; break; }

        sel = { start + 1, end };

        if (sel.getLength() > 0)
            te.setHighlightedRegion (sel);
    }

    const bool hasSel = sel.getLength() > 0;

    juce::PopupMenu m;
    m.setLookAndFeel (&lookAndFeel);

    m.addItem (1, tr (Lang::TextId::menuCut),    hasSel);
    m.addItem (2, tr (Lang::TextId::menuCopy),   hasSel);
    m.addItem (3, tr (Lang::TextId::menuPaste));
    m.addItem (4, tr (Lang::TextId::menuDelete), hasSel);
    m.addSeparator();
    m.addItem (5, tr (Lang::TextId::menuSelectAll));

    juce::Component::SafePointer<juce::TextEditor> target (&te);

    // 菜单位置跟着鼠标；原来用 withTargetComponent，菜单会被摆到整个编辑框的边上
    m.showMenuAsync (juce::PopupMenu::Options()
                         .withTargetScreenArea ({ screenPos.x, screenPos.y, 1, 1 }),
                     [target, sel] (int result)
    {
        if (target == nullptr || result == 0)
            return;

        // 菜单抢焦点后 TextEditor 会清掉选区，先恢复再执行操作
        target->grabKeyboardFocus();

        if (sel.getLength() > 0)
            target->setHighlightedRegion (sel);

        switch (result)
        {
            case 1: target->cut();     break;
            case 2: target->copy();    break;
            case 3: target->paste();   break;
            case 4: if (sel.getLength() > 0) target->insertTextAtCaret (juce::String()); break;
            case 5: target->selectAll(); break;
            default: break;
        }
    });
}

//==============================================================================
std::vector<int> LyricSyncEditor::tapListIndices() const
{
    std::vector<int> ids;

    if (! tapMode)
        return ids;

    const bool v = (orientation == Orientation::vertical);
    const int  maxRows = v ? 4 : 7;

    for (int i = 0; i < static_cast<int> (tapTimes.size()); ++i)
        if (tapTimes[(size_t) i] >= 0.0)
            ids.push_back (i);

    const int n = static_cast<int> (ids.size());

    if (n <= maxRows)
        return ids;

    // 从最新一行往回翻 tapListScroll 行
    const int scroll = juce::jlimit (0, n - maxRows, tapListScroll);
    const int end    = n - scroll;
    const int begin  = juce::jmax (0, end - maxRows);

    return std::vector<int> (ids.begin() + begin, ids.begin() + end);
}

juce::Rectangle<float> LyricSyncEditor::tapListTrack (juce::Rectangle<float> area) const
{
    const bool v    = (orientation == Orientation::vertical);
    const bool side = (! v && railWidth() > 60.0f);
    const int  maxRows = v ? 4 : 7;
    const auto ids = tapListIndices();

    if (! tapMode || static_cast<int> (ids.size()) < maxRows)
        return {};

    if (side)
        area = tapListArea();

    const float rowH = v ? 24.0f : (side ? 42.0f : 27.0f);
    const float top  = area.getY() + (side ? 0.0f : 52.0f);
    const float h    = static_cast<float> (ids.size()) * rowH;

    // 滑块加宽到 9 px，抓起来更容易（行右缘正好也停在 right-30）
    return { area.getRight() - (side ? 9.0f : 30.0f), top, 9.0f, h };
}

int LyricSyncEditor::tapListTotal() const
{
    int total = 0;

    for (auto t : tapTimes)
        if (t >= 0.0)
            ++total;

    return total;
}

int LyricSyncEditor::tapListMaxScroll() const
{
    const int maxRows = (orientation == Orientation::vertical) ? 4 : 7;
    return juce::jmax (0, tapListTotal() - maxRows);
}

juce::Rectangle<float> LyricSyncEditor::tapListThumb (juce::Rectangle<float> area) const
{
    const auto track = tapListTrack (area);

    if (track.isEmpty())
        return {};

    const int maxRows = (orientation == Orientation::vertical) ? 4 : 7;
    const int total = juce::jmax (1, tapListTotal());
    const int maxScroll = juce::jmax (1, tapListMaxScroll());

    const float frac = juce::jlimit (0.15f, 1.0f,
                                     static_cast<float> (maxRows) / static_cast<float> (total));
    const float thumbH = juce::jmax (20.0f, track.getHeight() * frac);
    const float t01 = 1.0f - static_cast<float> (tapListScroll) / static_cast<float> (maxScroll);
    const float thumbY = track.getY()
                       + (track.getHeight() - thumbH) * juce::jlimit (0.0f, 1.0f, t01);

    return track.withY (thumbY).withHeight (thumbH);
}

void LyricSyncEditor::syncTapToPlayhead (double t)
{
    if (! tapMode || tapLines.empty())
        return;

    // 找到播放头所在的句子：最后一个"已打点且时间不晚于 t"的行
    int target = 0;

    for (int i = 0; i < static_cast<int> (tapTimes.size()); ++i)
        if (tapTimes[(size_t) i] >= 0.0 && tapTimes[(size_t) i] <= t + 0.001)
            target = i;

    if (target != tapIndex)
    {
        tapIndex = target;
        tapListScroll = 0;
        lastWarning = {};
        repaint();
    }
}

void LyricSyncEditor::scrollTapList (int deltaRows)
{
    std::vector<int> all;

    for (int i = 0; i < static_cast<int> (tapTimes.size()); ++i)
        if (tapTimes[(size_t) i] >= 0.0)
            all.push_back (i);

    const bool v = (orientation == Orientation::vertical);
    const int  maxRows = v ? 4 : 7;
    const int  maxScroll = juce::jmax (0, static_cast<int> (all.size()) - maxRows);

    const int next = juce::jlimit (0, maxScroll, tapListScroll + deltaRows);

    if (next != tapListScroll)
    {
        tapListScroll = next;
        repaint();
    }
}

/** 打轴列表区域：横版侧栏展开时放进侧栏中部（用户指定的位置）；竖版/收起态仍在舞台。 */
juce::Rectangle<float> LyricSyncEditor::tapListArea() const
{
    if (orientation == Orientation::horizontal && railWidth() > 60.0f)
    {
        const float rail = railWidth();

        // 侧栏：标题行 y=10..30、分组标题 y=44、说明 y=74..106、分隔线 y=116
        // 列表从 y=126 起，到「记录本句」按钮上方（按钮在 baseH-66）为止。
        const float top    = 126.0f;
        const float bottom = baseH() - 116.0f;
        return { 10.0f, top, rail - 20.0f, juce::jmax (60.0f, bottom - top) };
    }

    return stageArea();
}

std::vector<juce::Rectangle<float>> LyricSyncEditor::tapListRows (juce::Rectangle<float> area) const
{
    std::vector<juce::Rectangle<float>> rows;

    const auto ids = tapListIndices();
    const int  n   = static_cast<int> (ids.size());

    if (n <= 0)
        return rows;

    const bool  v    = (orientation == Orientation::vertical);
    const bool  side = (! v && railWidth() > 60.0f);
    const float rowH = v ? 24.0f : (side ? 42.0f : 27.0f);

    // 侧栏展开时忽略传进来的舞台区域，直接用侧栏中部
    if (side)
        area = tapListArea();

    for (int i = 0; i < n; ++i)
    {
        if (v)
        {
            const float y = area.getY() + 56.0f + static_cast<float> (i) * rowH;
            rows.push_back ({ area.getX() + 30.0f, y, area.getWidth() - 60.0f, rowH - 2.0f });
        }
        else if (side)
        {
            const float y = area.getY() + static_cast<float> (i) * rowH;
            rows.push_back ({ area.getX(), y, area.getWidth(), rowH - 2.0f });
        }
        else
        {
            const float y = area.getY() + 52.0f + static_cast<float> (i) * rowH;
            rows.push_back ({ area.getRight() - 310.0f, y, 280.0f, rowH - 2.0f });
        }
    }

    return rows;
}

void LyricSyncEditor::finishTapMode()
{
    if (! tapMode)
        return;

    if (tapLines.empty())
    {
        exitTapMode();
        return;
    }

    for (auto& t : tapTimes)
        if (! std::isfinite (t) || t < 0.0)
            t = 0.0;

    for (size_t i = 1; i < tapTimes.size(); ++i)
        tapTimes[i] = juce::jmax (tapTimes[i], tapTimes[i - 1] + 0.05);

    juce::String lrc;
    for (size_t i = 0; i < tapLines.size(); ++i)
        lrc << "[" << formatStamp (tapTimes[i]) << "]" << tapLines[i] << "\n";

    tapMode = false;
    tapLines.clear();
    tapTimes.clear();
    tapIndex = 0;
    tapEditIndex = -1;
    tapArmIndex = -1;
    demoPlaying = false;

    // 不走 loadLyricsText()：它会把 offset 归零，而打轴时间已含当前 offset。
    const auto info = processor.setLyricsText (lrc, {}, language);

    fileDescription = tr (Lang::TextId::tapDoneLabel) + sep() + info.formatName
                    + sep() + juce::String (info.lineCount)
                    + tr (Lang::TextId::lineCountSuffix);
    lastWarning = info.warning;
    usingDemoLyrics = false;
    currentIndex = -2;
    displayIndex = -1.0f;

    archiveCurrentLyrics();

    updateVisibility();
    resized();
    repaint();
}

void LyricSyncEditor::exitTapMode()
{
    tapMode = false;
    tapLines.clear();
    tapTimes.clear();
    tapIndex = 0;
    tapEditIndex = -1;
    tapArmIndex = -1;
    demoPlaying = false;

    updateVisibility();
    resized();
    repaint();
}

//==============================================================================
void LyricSyncEditor::exportLyrics()
{
    const auto& doc = processor.lyrics();

    if (doc.isEmpty())
    {
        lastWarning = tr (Lang::TextId::exportEmpty);
        repaint();
        return;
    }

    // 不再把当初载入的原文照抄出去，而是按「当前状态」重新生成一份新 LRC：
    //   · 歌词文字：取当前文档（粘贴/编辑、打轴行编辑后的最新内容）
    //   · 时间码：取当前文档（打轴、重打、单行编辑后的最新时间）
    //   · 提前/延后：歌词时间 = 宿主时间 + offset，所以导出的音频时间 = 歌词时间 - offset
    // 这样导出的文件在任何播放器里都能直接对上音频，不依赖插件里的偏移设置。
    const double shift = offsetMs / 1000.0;
    const auto& info = doc.info();

    juce::String lrc;

    if (info.title.isNotEmpty())   lrc << "[ti:" << info.title  << "]\n";
    if (info.artist.isNotEmpty())  lrc << "[ar:" << info.artist << "]\n";
    if (info.album.isNotEmpty())   lrc << "[al:" << info.album  << "]\n";

    for (int i = 0; i < doc.size(); ++i)
    {
        const auto& line = doc.line (i);
        const double t = juce::jmax (0.0, line.time - shift);
        const auto stamp = juce::String ("[") + formatStamp (t) + "]";

        lrc << stamp << line.text << "\n";

        // 双语歌词：译文另起一行，共用同一时间码
        if (line.translation.isNotEmpty())
            lrc << stamp << line.translation << "\n";
    }

    auto defaultFile = juce::File::getSpecialLocation (juce::File::userDocumentsDirectory)
                          .getChildFile ("Lyrics.lrc");

    chooser = std::make_unique<juce::FileChooser> (tr (Lang::TextId::exportTitle),
                                                   defaultFile, "*.lrc");

    const int saveFlags = juce::FileBrowserComponent::saveMode
                        | juce::FileBrowserComponent::canSelectFiles
                        | juce::FileBrowserComponent::warnAboutOverwriting;

    chooser->launchAsync (saveFlags, [this, lrc] (const juce::FileChooser& fc)
    {
        auto file = fc.getResult();

        if (file == juce::File())
            return;

        if (file.getFileExtension().isEmpty())
            file = file.withFileExtension (".lrc");

        // 明确按 UTF-8 写出：replaceWithText 默认走系统编码，中文会变乱码
        juce::FileOutputStream out (file);
        bool ok = false;

        if (out.openedOk())
        {
            out.setPosition (0);
            out.truncate();
            ok = out.write (lrc.toRawUTF8(),
                            static_cast<size_t> (lrc.getNumBytesAsUTF8()));
            out.flush();
        }

        lastWarning = ok ? tr (Lang::TextId::exportDone) + "  " + file.getFileName()
                         : tr (Lang::TextId::exportFailed);
        repaint();
    });
}

//==============================================================================
void LyricSyncEditor::toggleZenMode()
{
    zenMode = ! zenMode;

    if (zenMode)
    {
        sizeBeforeZenW = getWidth();
        sizeBeforeZenH = getHeight();
        previewOffset = 0.0;
        previewDragging = false;
        settingsOpen = false;
        aboutOpen = false;

        // 铺满主显示器工作区（留一点边）
        if (auto* display = juce::Desktop::getInstance().getDisplays().getPrimaryDisplay())
        {
            const auto area = display->userArea;
            setSize (juce::jmax (360, area.getWidth() - 60),
                     juce::jmax (280, area.getHeight() - 60));
        }
    }
    else
    {
        setSize (juce::jmax (360, sizeBeforeZenW), juce::jmax (280, sizeBeforeZenH));
    }

    updateVisibility();
    resized();
    repaint();
}

//==============================================================================
void LyricSyncEditor::mouseDoubleClick (const juce::MouseEvent& e)
{
    // 双击步进器数值 = 回默认（偏移 0 / 字号 140 px）
    if (e.eventComponent == &offsetLabel)
    {
        offsetScrub = false;
        offsetHistory.push_back (offsetMs);

        if (offsetHistory.size() > 60)
            offsetHistory.erase (offsetHistory.begin());

        offsetMs = 0.0;
        processor.setSetting (Settings::offsetMs, offsetMs);
        updateLabels (displayTime);
        repaint();
        return;
    }

    if (e.eventComponent == &fontLabel)
    {
        fontScrub = false;
        fontScale = 1.0;
        fontSlider.setValue (fontScale, juce::dontSendNotification);
        processor.setSetting (Settings::fontScale, fontScale);
        saveUiPrefs();
        updateLabels (displayTime);
        repaint();
        return;
    }

    if (settingsOpen || aboutOpen || editing || tapMode)
        return;

    if (stageArea().transformedBy (uiTransform).contains (e.position))
    {
        previewDragging = false;
        toggleZenMode();
    }
}

//==============================================================================
bool LyricSyncEditor::isInterestedInFileDrag (const juce::StringArray& files)
{
    for (const auto& f : files)
        if (looksLikeLyricsFile (f))
            return true;

    return false;
}

void LyricSyncEditor::filesDropped (const juce::StringArray& files, int, int)
{
    if (! files.isEmpty())
        loadLyricsFile (juce::File (files[0]));
}
