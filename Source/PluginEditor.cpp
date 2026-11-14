#include "PluginEditor.h"
#include "DemoLyrics.h"

#include <cmath>
#include <string>

#if JUCE_WINDOWS
 #include <windows.h>
#endif

namespace
{
    const char* const kVersion = "1.7.3";

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

    const int kZoomSteps[] = { 50, 75, 100, 125, 150, 200 };
    const int kNumZoomSteps = static_cast<int> (sizeof (kZoomSteps) / sizeof (kZoomSteps[0]));

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
    return juce::Font (typeface, 12.5f * scale, juce::Font::plain);
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

    const auto b = getLocalBounds().toFloat().reduced (1.0f);
    const auto colour = (over || down) ? hoverColour : normalColour;

    if (over || down)
    {
        g.setColour (normalColour.withAlpha (0.10f));
        g.fillRoundedRectangle (b, 8.0f);
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

    // 全局默认值先铺一遍，工程状态恢复后会覆盖它（见 syncControlsFromSettings）。
    loadUiPrefs();
    syncControlsFromSettings();

    // 右下角缩放柄回来了：自由拖拽即"无极缩放"，画布始终按比例整体缩放。
    setResizable (true, true);
    setResizeLimits (280, 280, 2600, 2600);

    applyWindowSize();

    //--------------------------------------------------------------------------
    for (auto* b : { &loadButton, &pasteButton, &demoButton,
                     &minusButton, &plusButton, &resetButton,
                     &demoPlayButton, &demoStopButton,
                     &applyTextButton, &cancelTextButton,
                     &tapStartButton, &tapUndoButton, &tapFinishButton, &tapExitButton,
                     &tapRestartButton, &tapNudgeBackButton, &tapNudgeFwdButton, &tapPauseButton,
                     &tapHostTransportButton,
                     &tapRowRestampButton, &tapRowApplyButton, &tapRowCancelButton,
                     &measureButton, &exportButton, &retempoButton, &demoClockToggle,
                     &orientHButton, &orientVButton, &pickImageButton, &clearImageButton,
                     &hintToggle })
    {
        b->addListener (this);
        addAndMakeVisible (*b);
    }

    for (auto* b : { &gearButton, &logoButton, &closeSettingsButton, &closeAboutButton })
    {
        b->addListener (this);
        addAndMakeVisible (*b);
    }

    demoClockToggle.setClickingTogglesState (true);
    hintToggle.setClickingTogglesState (true);

    for (auto* box : { &languageBox, &zoomBox, &skinBox, &fontBox, &colourBox, &advanceBox })
    {
        box->setEditableText (false);
        box->setJustificationType (juce::Justification::centred);
        box->addListener (this);
        addAndMakeVisible (*box);
    }

    fontSlider.setRange (0.70, 1.60, 0.01);
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

    addLabel (offsetLabel, 12.0f, true,  juce::Justification::centred);
    addLabel (fileLabel,   11.5f, false, juce::Justification::centredLeft);
    addLabel (hintLabel,   11.0f, false, juce::Justification::centredLeft);
    hintLabel.setMouseCursor (juce::MouseCursor::PointingHandCursor);
    hintLabel.addMouseListener (this, false);      // 点提示词 -> 换下一条
    addLabel (fontLabel,   11.0f, false, juce::Justification::centredLeft);
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
    startTimerHz (30);
    hintRotateMs = juce::Time::getMillisecondCounterHiRes();

    tooltipWindow = std::make_unique<juce::TooltipWindow> (this, 550);
}

LyricSyncEditor::~LyricSyncEditor()
{
    stopTimer();
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
        else if (key == "fontScale")   fontScale   = juce::jlimit (0.70, 1.60, value.getDoubleValue());
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
    fontScale = static_cast<double> (processor.getSetting (Settings::fontScale, 1.0));
    language  = juce::jlimit (0, 1, static_cast<int> (processor.getSetting (Settings::language, 0)));
    demoMode  = static_cast<bool>  (processor.getSetting (Settings::demoClock, false));

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
    loadButton  .setButtonText (tr (Lang::TextId::loadLyrics));
    pasteButton .setButtonText (tr (Lang::TextId::pasteEdit));
    demoButton  .setButtonText (tr (Lang::TextId::demoLyrics));

    minusButton     .setButtonText (tr (Lang::TextId::minusSymbol));
    resetButton     .setButtonText (tr (Lang::TextId::reset));
    demoPlayButton  .setButtonText (tr (Lang::TextId::playSymbol));
    demoStopButton  .setButtonText (tr (Lang::TextId::stopSymbol));
    applyTextButton .setButtonText (tr (Lang::TextId::applyLyrics));
    cancelTextButton.setButtonText (tr (Lang::TextId::cancel));
    demoClockToggle .setButtonText (tr (Lang::TextId::manualPreview));

    tapStartButton  .setButtonText (tr (Lang::TextId::tapStart));
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
    exportButton    .setButtonText (tr (Lang::TextId::exportLrc));
    retempoButton   .setButtonText (tr (Lang::TextId::retempo));

    orientHButton   .setButtonText (tr (Lang::TextId::orientH));
    orientVButton   .setButtonText (tr (Lang::TextId::orientV));
    pickImageButton .setButtonText (tr (Lang::TextId::pickImage));
    clearImageButton.setButtonText (tr (Lang::TextId::clearImage));

    fontLabel.setText (tr (Lang::TextId::fontSize), juce::dontSendNotification);
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

    updatingCombos = false;

    //---- Hover explanations -------------------------------------------------
    loadButton      .setTooltip (tr (Lang::TextId::tipLoad));
    pasteButton     .setTooltip (tr (Lang::TextId::tipPaste));
    demoButton      .setTooltip (tr (Lang::TextId::tipDemo));
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
    hintLabel       .setTooltip (tr (Lang::TextId::hintClick));

    if (reloadDemoLyrics && usingDemoLyrics)
    {
        loadLyricsText (demoLyricsText (language), tr (Lang::TextId::demoLyrics), true);
        fileDescription = tr (Lang::TextId::demoFileDesc);
    }

    repaint();
}

void LyricSyncEditor::refreshLookAndFeel()
{
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

    offsetLabel.setColour (juce::Label::textColourId, palette.accent);
    offsetLabel.setColour (juce::Label::backgroundColourId, palette.card);
    hintLabel.setColour (juce::Label::textColourId, palette.faint);
    fileLabel.setColour (juce::Label::textColourId, palette.dim);
    fontLabel.setColour (juce::Label::textColourId, palette.faint);
    audioLabel.setColour (juce::Label::textColourId, palette.dim);

    gearButton.normalColour = palette.dim;
    gearButton.hoverColour  = palette.accent;
    closeSettingsButton.normalColour = palette.dim;
    closeSettingsButton.hoverColour  = palette.text;
    closeAboutButton.normalColour = palette.dim;
    closeAboutButton.hoverColour  = palette.text;

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

    const bool overlay = settingsOpen || aboutOpen;
    const bool normal  = ! editing && ! tapMode && ! overlay;

    for (auto* c : { static_cast<juce::Component*> (&loadButton),
                     static_cast<juce::Component*> (&pasteButton),
                     static_cast<juce::Component*> (&demoButton),
                     static_cast<juce::Component*> (&minusButton),
                     static_cast<juce::Component*> (&plusButton),
                     static_cast<juce::Component*> (&resetButton),
                     static_cast<juce::Component*> (&offsetLabel),
                     static_cast<juce::Component*> (&fontLabel),
                     static_cast<juce::Component*> (&fontSlider),
                     static_cast<juce::Component*> (&demoClockToggle),
                     static_cast<juce::Component*> (&measureButton),
                     static_cast<juce::Component*> (&exportButton),
                     static_cast<juce::Component*> (&retempoButton),
                     static_cast<juce::Component*> (&gearButton) })
        c->setVisible (normal);

    demoPlayButton .setVisible (normal && demoMode);
    demoStopButton .setVisible (normal && demoMode);

    // 纯享模式把上面这些都隐藏过，回来必须逐个恢复；
    // 这三个 Label 不在上面那批里，之前漏掉，导致退出全屏后提示词永久消失。
    fileLabel .setVisible (normal);
    audioLabel.setVisible (normal);
    hintLabel .setVisible (normal && showHints);

    applyTextButton .setVisible (editing && ! overlay);
    cancelTextButton.setVisible (editing && ! overlay);
    tapStartButton  .setVisible (editing && ! overlay);

    const bool tapMain = tapMode && ! overlay && tapEditIndex < 0;

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
    logoButton.setVisible (! overlay);

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

juce::Rectangle<float> LyricSyncEditor::stageArea() const
{
    if (zenMode)
        return { 10.0f, 10.0f, baseW() - 20.0f, baseH() - 20.0f };

    if (orientation == Orientation::vertical)
        return { 22.0f, 72.0f, baseW() - 44.0f, 556.0f };

    return { 22.0f, 72.0f, hBaseW - 44.0f, 394.0f };
}

juce::Rectangle<float> LyricSyncEditor::settingsPanelArea() const
{
    if (orientation == Orientation::vertical)
        return { juce::jmax (20.0f, (baseW() - 520.0f) * 0.5f), 150.0f, 520.0f, 540.0f };

    return { 200.0f, 66.0f, 560.0f, 470.0f };
}

juce::Rectangle<float> LyricSyncEditor::aboutPanelArea() const
{
    if (orientation == Orientation::vertical)
        return { 44.0f, 210.0f, 512.0f, 460.0f };

    return { 230.0f, 120.0f, 500.0f, 370.0f };
}

void LyricSyncEditor::updateScaledFonts()
{
    offsetLabel.setFont (uiFont (12.0f  * uiScale, true));
    fileLabel  .setFont (uiFont (11.5f * uiScale));
    hintLabel  .setFont (uiFont (11.0f  * uiScale));
    fontLabel  .setFont (uiFont (11.0f  * uiScale));
    audioLabel .setFont (uiFont (11.5f * uiScale));
    editorBox  .setFont (uiFont (14.0f  * uiScale));
    editorBox.applyFontToAllText (uiFont (14.0f * uiScale));

    tapTextEditor.applyFontToAllText (uiFont (13.0f * uiScale));
    tapTimeEditor.applyFontToAllText (uiFont (13.0f * uiScale));
}

void LyricSyncEditor::applyWindowSize()
{
    const int w = juce::roundToInt (baseW() * static_cast<float> (zoomPercent) / 100.0f);
    const int h = juce::roundToInt (baseH() * static_cast<float> (zoomPercent) / 100.0f);
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
        gearButton   .setBounds (scaledRect (882, 12, 46, 46));

        editorBox.setBounds (scaledRect (stage.getX() + 2.0f, stage.getY() + 2.0f,
                                         stage.getWidth() - 4.0f, stage.getHeight() - 4.0f));

        float x = 22.0f;
        auto place = [&] (juce::Component& c, float bw, float bh = 30.0f)
        {
            c.setBounds (scaledRect (x, 482.0f, bw, bh));
            x += bw + 8.0f;
        };

        if (editing)
        {
            place (applyTextButton, 96);
            place (cancelTextButton, 76);
            x += 10.0f;
            place (tapStartButton, 104);
        }
        else if (tapMode)
        {
            place (tapUndoButton, 74);
            place (tapRestartButton, 96);
            x += 6.0f;
            place (tapNudgeBackButton, 62);
            place (tapNudgeFwdButton, 62);
            x += 6.0f;
            place (tapHostTransportButton, 92);
            x += 6.0f;
            place (tapPauseButton, 84);
            x += 6.0f;
            place (tapFinishButton, 84);
            place (tapExitButton, 74);
        }
        else
        {
            place (loadButton, 92);
            place (pasteButton, 104);
            place (demoButton, 88);
            x += 14.0f;
            place (minusButton, 30);
            place (offsetLabel, 84);
            place (plusButton, 30);
            place (resetButton, 50);
            x += 14.0f;
            place (fontLabel, 34);
            fontSlider.setBounds (scaledRect (x, 486, 100, 22));
            x += 108.0f;
            demoClockToggle.setBounds (scaledRect (x, 484, 108, 26));
            x += 116.0f;
            demoPlayButton.setBounds (scaledRect (x, 482, 34, 30));
            demoStopButton.setBounds (scaledRect (x + 40.0f, 482, 34, 30));
            measureButton.setBounds (scaledRect (682, 538, 110, 26));
            exportButton .setBounds (scaledRect (800, 538, 138, 26));
        }

        fileLabel .setBounds (scaledRect (22, 518, 420, 22));
        hintLabel .setBounds (scaledRect (22, 542, 540, 20));
        audioLabel.setBounds (scaledRect (566, 541, 110, 22));
        retempoButton.setBounds (scaledRect (836, 512, 102, 24));
    }
    else
    {
        const float ox = vShift();

        gearButton   .setBounds (scaledRect (baseW() - 68.0f, 12, 46, 46));

        editorBox.setBounds (scaledRect (stage.getX() + 2.0f, stage.getY() + 2.0f,
                                         stage.getWidth() - 4.0f, stage.getHeight() - 4.0f));

        if (editing)
        {
            applyTextButton .setBounds (scaledRect (ox + 22, 646, 176, 30));
            cancelTextButton.setBounds (scaledRect (ox + 206, 646, 176, 30));
            tapStartButton  .setBounds (scaledRect (ox + 390, 646, 176, 30));
        }
        else if (tapMode)
        {
            tapUndoButton      .setBounds (scaledRect (ox + 22, 646, 168, 30));
            tapRestartButton   .setBounds (scaledRect (ox + 198, 646, 168, 30));
            tapFinishButton    .setBounds (scaledRect (ox + 374, 646, 168, 30));

            tapNudgeBackButton .setBounds (scaledRect (ox + 22, 682, 96, 30));
            tapNudgeFwdButton  .setBounds (scaledRect (ox + 126, 682, 96, 30));
            tapPauseButton     .setBounds (scaledRect (ox + 230, 682, 104, 30));
            tapHostTransportButton.setBounds (scaledRect (ox + 342, 682, 124, 30));
            tapExitButton      .setBounds (scaledRect (ox + 474, 682, 84, 30));
        }
        else
        {
            loadButton .setBounds (scaledRect (ox + 22, 644, 176, 30));
            pasteButton.setBounds (scaledRect (ox + 206, 644, 176, 30));
            demoButton .setBounds (scaledRect (ox + 390, 644, 176, 30));

            minusButton .setBounds (scaledRect (ox + 22, 682, 56, 30));
            offsetLabel .setBounds (scaledRect (ox + 86, 682, 120, 30));
            plusButton  .setBounds (scaledRect (ox + 214, 682, 56, 30));
            resetButton .setBounds (scaledRect (ox + 278, 682, 96, 30));
            fontLabel   .setBounds (scaledRect (ox + 382, 686, 40, 22));
            fontSlider  .setBounds (scaledRect (ox + 426, 686, 140, 22));

            demoClockToggle.setBounds (scaledRect (ox + 22, 720, 176, 30));
            demoPlayButton .setBounds (scaledRect (ox + 206, 720, 60, 30));
            demoStopButton .setBounds (scaledRect (ox + 274, 720, 60, 30));
            measureButton  .setBounds (scaledRect (ox + 342, 720, 150, 30));
        }

        fileLabel .setBounds (scaledRect (22, 762, baseW() - 44.0f, 22));
        hintLabel .setBounds (scaledRect (22, 812, baseW() - 44.0f, 20));
        audioLabel.setBounds (scaledRect (ox + 22, 836, 240, 22));
        exportButton.setBounds (scaledRect (ox + 270, 836, 200, 26));
        retempoButton.setBounds (scaledRect (ox + 478, 836, 100, 26));
    }

    //---- Settings panel -----------------------------------------------------
    {
        const auto panel = settingsPanelArea();
        const bool v = (orientation == Orientation::vertical);
        const float ctrlX = panel.getX() + (v ? 168.0f : 190.0f);
        const float ctrlW = panel.getRight() - 24.0f - ctrlX;
        const float rowH = 44.0f;

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
    }

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
            const float cx = 60.0f, cy = 250.0f, cw = 540.0f;

            tapTextEditor.setBounds (scaledRect (cx + 24, cy + 52, cw - 48, 30));
            tapTimeEditor.setBounds (scaledRect (cx + 88, cy + 92, 160, 26));
            tapRowRestampButton.setBounds (scaledRect (cx + 312, cy + 90, 110, 30));
            tapRowApplyButton  .setBounds (scaledRect (cx + 24, cy + 140, 120, 30));
            tapRowCancelButton .setBounds (scaledRect (cx + 156, cy + 140, 120, 30));
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
        juce::ColourGradient gradient (palette.bgTop, 0.0f, 0.0f,
                                       palette.bgBottom, 0.0f, bounds.getHeight(), false);
        g.setGradientFill (gradient);
        g.fillAll();

    }

    // 两层柔光：右上青绿、左下靛蓝。自定义图片皮肤也叠，否则照片会把氛围全挡住。
    {
        const float w = bounds.getWidth();
        const float h = bounds.getHeight();

        const auto glow = [&g] (float cx, float cy, float radius, juce::Colour c)
        {
            for (int i = 0; i < 5; ++i)
            {
                const float rr = radius * (1.0f - 0.18f * static_cast<float> (i));
                g.setColour (c.withAlpha (0.040f));
                g.fillEllipse (cx - rr, cy - rr, rr * 2.0f, rr * 2.0f);
            }
        };

        glow (w * 0.84f, h * 0.02f, w * 0.40f, palette.accent);
        glow (w * 0.06f, h * 1.02f, w * 0.34f, juce::Colour (0xff5b7bff));
    }

    // 逻辑画布
    const juce::Graphics::ScopedSaveState state (g);
    g.addTransform (uiTransform);

    g.setColour (palette.accent.withAlpha (0.045f));
    g.fillEllipse (baseW() - 230.0f, -190.0f, 460.0f, 460.0f);

    if (! zenMode)
        paintHeader (g);

    paintStage  (g, stageArea());

    if (zenMode)
        drawText (g, juce::Rectangle<float> (0.0f, baseH() - 34.0f, baseW(), 20.0f),
                  tr (Lang::TextId::zenExit), 11.0f,
                  palette.faint.withAlpha (0.45f), false);
    else
        paintInfoLine (g);

    if (settingsOpen)
        paintSettings (g, settingsPanelArea());

    if (aboutOpen)
        paintAbout (g, aboutPanelArea());
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

void LyricSyncEditor::paintStage (juce::Graphics& g, juce::Rectangle<float> area)
{
    // 用「文字色 + 低透明度」做玻璃：深色主题是薄薄一层白，浅色主题是薄薄一层黑
    g.setColour (palette.text.withAlpha (0.055f));
    g.fillRoundedRectangle (area, 22.0f);

    // 上亮下暗的渐变，做出玻璃的厚度感
    {
        juce::ColourGradient thick (juce::Colours::transparentBlack,
                                    area.getX(), area.getCentreY(),
                                    juce::Colours::black.withAlpha (0.16f),
                                    area.getX(), area.getBottom(), false);
        g.setGradientFill (thick);
        g.fillRoundedRectangle (area, 22.0f);
    }

    // 外描边：比填充亮一档，就是"玻璃边缘"
    g.setColour (palette.text.withAlpha (0.16f));
    g.drawRoundedRectangle (area.reduced (0.5f), 22.0f, 1.2f);

    // 顶部内侧一道高光
    {
        juce::Path topEdge;
        topEdge.addRoundedRectangle (area.getX() + 18.0f, area.getY() + 1.2f,
                                     area.getWidth() - 36.0f, 10.0f, 5.0f);
        g.setColour (palette.text.withAlpha (0.10f));
        g.fillPath (topEdge);
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

    if (! zenMode)
        g.drawText (tr (Lang::TextId::currentLine),
                    static_cast<int> (area.getX() + 30.0f),
                    static_cast<int> (area.getY() + 15.0f), 200, 16,
                    juce::Justification::centredLeft);

    if (! doc.isEmpty() && ! zenMode)
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
                          - (isCurrent ? lift : 0.0f);

            const auto lineRect = juce::Rectangle<float> (area.getX() + 40.0f, y,
                                                          area.getWidth() - 80.0f,
                                                          size * 1.42f);

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

                // 换句时的整行颜色过渡（由白渐入主题色）
                const auto tint = palette.text.interpolatedWith (currentTint,
                                                                 juce::jlimit (0.0f, 1.0f, transition));

                if (advanceMode == 1)
                {
                    // 逐字：未唱的字是白色，唱到的字由白渐变为主题色（字内渐变）
                    drawText (g, lineRect, line.text, size, palette.text, true,
                              sung, currentTint, fade);
                }
                else
                {
                    // 逐句：整行在换句时由白渐变为主题色
                    drawText (g, lineRect, line.text, size, tint, true,
                              -1.0f, palette.text, fade);
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
                auto colour = palette.text;

                if (i == previousIndex && transition < 0.999f)
                    colour = currentTint.interpolatedWith (palette.text,
                                                           juce::jlimit (0.0f, 1.0f, transition));

                drawText (g, lineRect, line.text, size, colour,
                          ad < 1.5f, -1.0f, {}, alpha);
            }
        }
    }

    if (std::abs (previewOffset) > 0.02)
    {
        const juce::String badge = tr (Lang::TextId::previewBadge) + "  "
                                 + (previewOffset >= 0.0 ? "+" : "-")
                                 + juce::String (std::abs (previewOffset), 1) + " s";

        drawText (g, juce::Rectangle<float> (area.getRight() - 220.0f, area.getBottom() - 46.0f,
                                             190.0f, 18.0f),
                  badge, 11.0f, palette.warn, false, -1, {}, 1.0f,
                  juce::Justification::centredRight);
    }

    if (! hasCurrent)
    {
        const double firstTime = doc.isEmpty() ? 0.0 : doc.line (0).time;
        const double bpmNow = detectedBpm > 0.0 ? detectedBpm : processor.bpm.load();
        const double secPerBeat = bpmNow > 20.0 ? 60.0 / bpmNow : 0.0;

        const double beatsLeft = secPerBeat > 0.0
                               ? (firstTime - displayTime) / secPerBeat : 99.0;

        // 第一句前 4 拍：打 4 个圆点倒数
        const bool counting = secPerBeat > 0.0
                           && beatsLeft > -0.30 && beatsLeft <= 4.35;

        const float y = centreY - (counting ? 58.0f : 13.0f);

        drawText (g, juce::Rectangle<float> (area.getX() + 60.0f, y,
                                             area.getWidth() - 120.0f, 26.0f),
                  tr (Lang::TextId::intro), 17.0f * widthScale,
                  palette.faint, false);

        if (! counting)
        {
            drawText (g, juce::Rectangle<float> (area.getX() + 60.0f, y + 30.0f,
                                                 area.getWidth() - 120.0f, 20.0f),
                      tr (Lang::TextId::nextLineIn)
                        + juce::String (juce::jmax (0.0, firstTime - displayTime), 1)
                        + tr (Lang::TextId::secondsSuffix),
                      11.5f * widthScale, palette.faint, false);
        }
        else
        {
            const int lit = juce::jlimit (0, 4,
                                          4 - static_cast<int> (std::ceil (juce::jmax (0.0, beatsLeft))) + 1);

            const float r = 7.0f * widthScale;
            const float gap = 34.0f * widthScale;
            const float dotY = centreY + 6.0f;

            for (int i = 0; i < 4; ++i)
            {
                const float cx = area.getCentreX() + (static_cast<float> (i) - 1.5f) * gap;
                const bool on = i < lit;

                g.setColour (on ? currentTint : palette.track);
                g.fillEllipse (cx - r, dotY - r, r * 2.0f, r * 2.0f);
            }

            drawText (g, juce::Rectangle<float> (area.getX() + 60.0f, dotY + 22.0f,
                                                 area.getWidth() - 120.0f, 20.0f),
                      tr (Lang::TextId::bpmPrefix) + juce::String (bpmNow, 1),
                      11.0f * widthScale, palette.faint, false);
        }
    }
}

//==============================================================================
void LyricSyncEditor::paintTapStage (juce::Graphics& g, juce::Rectangle<float> area)
{
    //---- 行编辑：盖住打轴主界面，只留这张小卡片 --------------------------
    if (tapEditIndex >= 0)
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

    g.setColour (palette.accent);
    g.setFont (uiFont (10.5f, true));
    g.drawText (tr (Lang::TextId::tapTitle),
                static_cast<int> (area.getX() + 30.0f),
                static_cast<int> (area.getY() + 15.0f), 240, 16,
                juce::Justification::centredLeft);

    g.setColour (palette.faint);
    g.setFont (uiFont (10.5f));
    g.drawText (juce::String (juce::jmin (tapIndex + 1, total)) + " / " + juce::String (total),
                static_cast<int> (area.getRight() - 160.0f),
                static_cast<int> (area.getY() + 15.0f), 130, 16,
                juce::Justification::centredRight);

    const float refW = (v ? 520.0f : 900.0f);
    const float widthScale = juce::jlimit (0.80f, 1.70f, area.getWidth() / refW)
                           * static_cast<float> (fontScale);

    const auto rows = tapListRows (area);
    const bool hasList = ! rows.empty();

    float mainX = area.getX() + 40.0f;
    float mainW = area.getWidth() - 80.0f;

    if (! v && hasList)
        mainW = rows[0].getX() - mainX - 24.0f;

    const float cx = mainX;
    const float cw = mainW;

    // 竖版把列表放在上方，主内容顺着列表往下排；横版仍然左右分栏
    const float listBottom = rows.empty() ? area.getY() + 48.0f : rows.back().getBottom();
    const float centreY = v ? listBottom + 74.0f : area.getCentreY() - 8.0f;

    if (! v && tapIndex > 0 && tapIndex - 1 < total)
    {
        drawText (g, juce::Rectangle<float> (cx, centreY - 96.0f, cw, 20.0f),
                  tr (Lang::TextId::tapPrev) + "  "
                    + formatTimecode (tapTimes[(size_t) (tapIndex - 1)]),
                  10.5f * widthScale, palette.faint, false);

        drawText (g, juce::Rectangle<float> (cx, centreY - 74.0f, cw, 30.0f),
                  tapLines[(size_t) (tapIndex - 1)], 16.0f * widthScale,
                  palette.dim, false);
    }

    if (tapIndex < total)
    {
        drawText (g, juce::Rectangle<float> (cx, centreY - 24.0f, cw, 52.0f),
                  tapLines[(size_t) idx], 30.0f * widthScale, lyricColour(), true);

        if (idx + 1 < total)
        {
            drawText (g, juce::Rectangle<float> (cx, centreY + 36.0f, cw, 18.0f),
                      tr (Lang::TextId::tapNext), 10.5f * widthScale, palette.faint, false);

            drawText (g, juce::Rectangle<float> (cx, centreY + 54.0f, cw, 26.0f),
                      tapLines[(size_t) (idx + 1)], 15.0f * widthScale,
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

        drawText (g, juce::Rectangle<float> (rows[0].getX(), rows[0].getY() - 20.0f,
                                             rows[0].getWidth(), 16.0f),
                  tr (Lang::TextId::tapListTitle), 10.0f * widthScale,
                  palette.faint, false, -1.0f, {}, 1.0f,
                  juce::Justification::centredLeft);

        for (int i = 0; i < static_cast<int> (rows.size()); ++i)
        {
            const auto r  = rows[(size_t) i];
            const int  li = ids[(size_t) i];

            g.setColour (palette.text.withAlpha (0.05f));
            g.fillRoundedRectangle (r, 5.0f);
            g.setColour (palette.text.withAlpha (0.10f));
            g.drawRoundedRectangle (r, 5.0f, 1.0f);

            auto ri = r.toNearestInt();

            g.setColour (palette.faint);
            g.setFont (uiFont (9.5f));
            g.drawText (juce::String (li + 1), ri.removeFromLeft (24),
                        juce::Justification::centredRight, false);

            g.setColour (palette.accent);
            g.setFont (uiFont (10.5f, true));
            g.drawText (formatTimecode (tapTimes[(size_t) li]),
                        ri.removeFromLeft (92),
                        juce::Justification::centredLeft, false);

            g.setColour (palette.dim);
            g.setFont (uiFont (10.5f));
            g.drawText (tapLines[(size_t) li], ri.withTrimmedLeft (6),
                        juce::Justification::centredLeft, true);
        }
    }

    //---- 底部提示 -------------------------------------------------------
    drawText (g, juce::Rectangle<float> (cx, area.getBottom() - (v ? 66.0f : 92.0f), cw, 20.0f),
              tr (Lang::TextId::tapHint), 11.0f * widthScale, palette.dim, false);

    drawText (g, juce::Rectangle<float> (cx, area.getBottom() - (v ? 44.0f : 68.0f), cw, 24.0f),
              tr (Lang::TextId::tapWillStamp) + "  " + formatTimecode (displayTime),
              (v ? 14.0f : 16.0f) * widthScale, palette.accent, true);

    const bool paused = tapMode && demoMode && ! demoPlaying;

    drawText (g, juce::Rectangle<float> (cx, area.getBottom() - (v ? 22.0f : 38.0f), cw, 18.0f),
              paused ? tr (Lang::TextId::tapPausedWarn) : tr (Lang::TextId::tapSpaceWarn),
              10.0f * widthScale, paused ? palette.warn : palette.faint, false);
}

//==============================================================================
void LyricSyncEditor::paintInfoLine (juce::Graphics& g)
{
    // 右下角只显示加粗的测速结果；时间码在右上角已经有了，
    // 这里右侧的位置留给「重新测速」按钮。
    const bool v = (orientation == Orientation::vertical);
    const auto area = v ? juce::Rectangle<float> (22.0f, 786.0f,
                                                  baseW() - 44.0f, 22.0f)
                        : juce::Rectangle<float> (452.0f, 518.0f, 380.0f, 22.0f);

    const juce::String bpmText = tr (Lang::TextId::bpmPrefix)
                               + (detectedBpm > 0.0 ? juce::String (detectedBpm, 1)
                                                    : juce::String ("--"));

    juce::AttributedString s;
    s.setJustification (juce::Justification::centredRight);
    s.append (bpmText, uiFont (14.0f, true));
    s.setColour (juce::Range<int> (0, bpmText.length()),
                 detectedBpm > 0.0 ? palette.accent : palette.faint);

    const juce::Graphics::ScopedSaveState state (g);
    s.draw (g, area);
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
    const float rowH = 44.0f;

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
        Lang::TextId::showHints
    };

    for (auto id : labels)
    {
        drawText (g, juce::Rectangle<float> (labelX, y, labelW, 28.0f),
                  tr (id), 13.0f, palette.dim, false, -1, {}, 1.0f,
                  juce::Justification::centredLeft);
        y += rowH;
    }
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
                                juce::Justification just)
{
    if (text.isEmpty() || area.isEmpty())
        return;

    juce::AttributedString s;
    s.setJustification (just);
    s.setWordWrap (juce::AttributedString::byChar);

    const int idx = juce::jlimit (0, kNumFonts - 1, fontIndex);
    const int style = bold ? juce::Font::bold : juce::Font::plain;

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

    if (sungProgress > 0.0f)
    {
        const int total = text.length();
        const int full  = juce::jlimit (0, total, static_cast<int> (std::floor (sungProgress)));
        const float frac = juce::jlimit (0.0f, 1.0f, sungProgress - static_cast<float> (full));

        if (full > 0)
            s.setColour (juce::Range<int> (0, full), sungColour.withAlpha (alpha));

        // 正在唱的这个字：底色 → 高亮色按字内进度混合，扫光就是连续的
        if (full < total && frac > 0.001f)
        {
            const auto blend = colour.interpolatedWith (sungColour, frac);
            s.setColour (juce::Range<int> (full, full + 1), blend.withAlpha (alpha));
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

    auto updates = processor.timeUpdates.load();
    if (updates != lastTimeUpdates)
    {
        lastTimeUpdates = updates;
        lastHostUpdateMs = now;
    }

    auto& doc = processor.lyrics();
    const auto range = audioRange();

    // 一开始播放就把手动预览偏移清掉，歌词立刻回到播放头位置
    if (! previewDragging && previewOffset != 0.0 && processor.isPlaying.load())
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
        const auto want = tr (processor.isPlaying.load() ? Lang::TextId::tapHostPause
                                                         : Lang::TextId::tapHostPlay);

        if (tapHostTransportButton.getButtonText() != want)
            tapHostTransportButton.setButtonText (want);
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
    repaint();
}

void LyricSyncEditor::updateLabels (double t)
{
    offsetLabel.setText (juce::String (offsetMs, 0) + " ms", juce::dontSendNotification);
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
    hintLabel.setText (showHints ? hintText() : juce::String(), juce::dontSendNotification);

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
    if (button == &loadButton)          { openFileDialog(); return; }
    if (button == &pasteButton)         { enterEditMode(); return; }
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
    if (button == &tapUndoButton)       { undoStamp(); return; }
    if (button == &tapRestartButton)    { restartTapLine (tapIndex); return; }
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
        if (button == &resetButton)  offsetMs = 0.0;
        if (button == &minusButton)  offsetMs -= 100.0;
        if (button == &plusButton)   offsetMs += 100.0;

        offsetMs = juce::jlimit (-60000.0, 60000.0, offsetMs);
        processor.setSetting (Settings::offsetMs, offsetMs);
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
            showEditorMenu (*te);
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

    if (tapMode)
    {
        if (tapEditIndex >= 0)
            return;                       // 正在编辑某一行，画布点击不记录

        grabKeyboardFocus();

        // 点「已记录」里的某一行：打开这一行的编辑器
        const auto rows = tapListRows (stageArea());
        const auto ids  = tapListIndices();
        const auto p = toLogical (e.position);

        for (int i = 0; i < static_cast<int> (rows.size()); ++i)
        {
            if (rows[(size_t) i].contains (p))
            {
                editTapRow (ids[(size_t) i]);
                return;
            }
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

    const double next = juce::jlimit (0.70, 1.60, fontScale + step);

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
        const auto rows = tapListRows (stageArea());
        const auto p = juce::Point<float> ((e.position.x - originX) / juce::jmax (0.01f, uiScaleX),
                                           (e.position.y - originY) / juce::jmax (0.01f, uiScaleY));

        for (const auto& r : rows)
            if (r.contains (p))
            {
                setMouseCursor (juce::MouseCursor::PointingHandCursor);
                return;
            }

        setMouseCursor (juce::MouseCursor::NormalCursor);
        return;
    }

    const bool hand = ! editing && ! settingsOpen && ! aboutOpen
                   && stageArea().transformedBy (uiTransform).contains (e.position);

    setMouseCursor (hand ? juce::MouseCursor::DraggingHandCursor
                         : juce::MouseCursor::NormalCursor);
}

bool LyricSyncEditor::keyPressed (const juce::KeyPress& key)
{
    if (key.getKeyCode() == juce::KeyPress::escapeKey)
    {
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
    }

    return false;
}

//==============================================================================
void LyricSyncEditor::showSettings (bool shouldShow)
{
    settingsOpen = shouldShow;

    if (settingsOpen)
        aboutOpen = false;

    updateVisibility();
    resized();
    repaint();
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

    const int flags = juce::FileBrowserComponent::openMode
                    | juce::FileBrowserComponent::canSelectFiles;

    chooser->launchAsync (flags, [this] (const juce::FileChooser& fc)
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

    const int flags = juce::FileBrowserComponent::openMode
                    | juce::FileBrowserComponent::canSelectFiles;

    chooser->launchAsync (flags, [this] (const juce::FileChooser& fc)
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
        lastWarning = tr (Lang::TextId::tapNoLines);
        repaint();
        return;
    }

    tapLines = std::move (plain);
    tapTimes = std::move (times);
    tapEditIndex = -1;

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
    if (! tapMode || tapLines.empty() || tapIndex >= static_cast<int> (tapLines.size()))
        return;

    double t = demoMode ? demoTime : displayTime;
    if (! std::isfinite (t) || t < 0.0)
        t = 0.0;

    if (tapIndex > 0)
    {
        const double prev = tapTimes[(size_t) (tapIndex - 1)];

        // 暂停时时间码不再走，连点两次会得到同一个值 —— 直接忽略，避免打出假时间
        if (t < prev + 0.10)
        {
            lastWarning = tr (Lang::TextId::tapTooClose);
            repaint();
            return;
        }
    }

    tapTimes[(size_t) tapIndex] = t;
    ++tapIndex;

    if (tapIndex >= static_cast<int> (tapLines.size()))
        finishTapMode();
    else
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

void LyricSyncEditor::restartTapLine (int index)
{
    if (! tapMode || tapLines.empty())
        return;

    index = juce::jlimit (0, static_cast<int> (tapLines.size()) - 1, index);

    for (int i = index; i < static_cast<int> (tapTimes.size()); ++i)
        tapTimes[(size_t) i] = -1.0;

    tapIndex = index;
    lastWarning = demoMode ? tr (Lang::TextId::tapPausedWarn)
                           : tr (Lang::TextId::tapHostRewind);

    // 手动预览下直接把播放头拨回上一句结束处，并停住，等用户点「继续」
    if (demoMode)
    {
        const double from = (index > 0 && tapTimes[(size_t) (index - 1)] >= 0.0)
                              ? tapTimes[(size_t) (index - 1)]
                              : audioRange().getStart();

        demoTime = juce::jlimit (audioRange().getStart(), audioRange().getEnd(), from);
        demoPlaying = false;
    }

    if (tapPauseButton.isVisible())
        tapPauseButton.setButtonText (tr (Lang::TextId::tapResume));

    repaint();
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
    if (! tapMode)
        return;

    // 手动预览模式下，插件自己就能走带
    if (demoMode)
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
void LyricSyncEditor::showEditorMenu (juce::TextEditor& te)
{
    const bool hasSel = te.getHighlightedRegion().getLength() > 0;

    juce::PopupMenu m;
    m.setLookAndFeel (&lookAndFeel);

    m.addItem (1, tr (Lang::TextId::menuCut),    hasSel);
    m.addItem (2, tr (Lang::TextId::menuCopy),   hasSel);
    m.addItem (3, tr (Lang::TextId::menuPaste));
    m.addItem (4, tr (Lang::TextId::menuDelete), hasSel);
    m.addSeparator();
    m.addItem (5, tr (Lang::TextId::menuSelectAll));

    juce::Component::SafePointer<juce::TextEditor> target (&te);

    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&te),
                     [target] (int result)
    {
        if (target == nullptr || result == 0)
            return;

        switch (result)
        {
            case 1: target->cut();     break;
            case 2: target->copy();    break;
            case 3: target->paste();   break;
            case 4: target->insertTextAtCaret (juce::String()); break;   // 删除选区
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

    if (static_cast<int> (ids.size()) > maxRows)
        ids.erase (ids.begin(), ids.begin() + (static_cast<int> (ids.size()) - maxRows));

    return ids;
}

std::vector<juce::Rectangle<float>> LyricSyncEditor::tapListRows (juce::Rectangle<float> area) const
{
    std::vector<juce::Rectangle<float>> rows;

    const auto ids = tapListIndices();
    const int  n   = static_cast<int> (ids.size());

    if (n <= 0)
        return rows;

    const bool  v    = (orientation == Orientation::vertical);
    const float rowH = v ? 24.0f : 27.0f;

    for (int i = 0; i < n; ++i)
    {
        if (v)
        {
            const float y = area.getY() + 56.0f + static_cast<float> (i) * rowH;
            rows.push_back ({ area.getX() + 30.0f, y, area.getWidth() - 60.0f, rowH - 2.0f });
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
    demoPlaying = false;

    updateVisibility();
    resized();
    repaint();
}

//==============================================================================
void LyricSyncEditor::exportLyrics()
{
    const auto body = processor.getSetting (Settings::lyricsText, "").toString();

    if (body.trim().isEmpty())
    {
        lastWarning = tr (Lang::TextId::emptyFile);
        repaint();
        return;
    }

    auto defaultFile = juce::File::getSpecialLocation (juce::File::userDocumentsDirectory)
                          .getChildFile ("Lyrics.lrc");

    chooser = std::make_unique<juce::FileChooser> (tr (Lang::TextId::exportTitle),
                                                   defaultFile, "*.lrc");

    const int flags = juce::FileBrowserComponent::saveMode
                    | juce::FileBrowserComponent::canSelectFiles
                    | juce::FileBrowserComponent::warnAboutOverwriting;

    chooser->launchAsync (flags, [this] (const juce::FileChooser& fc)
    {
        auto file = fc.getResult();

        if (file == juce::File())
            return;

        if (file.getFileExtension().isEmpty())
            file = file.withFileExtension (".lrc");

        const auto text = processor.getSetting (Settings::lyricsText, "").toString();
        file.replaceWithText (text, false, false, "\r\n");
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
