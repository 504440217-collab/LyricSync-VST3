#include "PluginEditor.h"
#include "DemoLyrics.h"

#include <cmath>
#include <string>

#if JUCE_WINDOWS
 #include <windows.h>
#endif

namespace
{
    const char* const kVersion = "1.5.0";

    // 常见字体（Windows 自带或系统常见），显示名直接用字体名，跨语言一致。
    const char* const kFontNames[] =
    {
        "Microsoft YaHei UI", "DengXian", "SimHei", "SimSun",
        "KaiTi", "FangSong", "Segoe UI", "Arial"
    };
    const int kNumFonts = static_cast<int> (sizeof (kFontNames) / sizeof (kFontNames[0]));

    // 0 = 跟随皮肤（用 accent）
    const juce::uint32 kLyricColours[] =
    {
        0x00000000, 0xfff2f5ff, 0xffffe9a3, 0xff5ce1c1, 0xff7cc6ff, 0xffff9ec4
    };
    const int kNumLyricColours = static_cast<int> (sizeof (kLyricColours) / sizeof (kLyricColours[0]));

    const int kZoomSteps[] = { 50, 75, 100, 125, 150, 200 };
    const int kNumZoomSteps = static_cast<int> (sizeof (kZoomSteps) / sizeof (kZoomSteps[0]));

    juce::String formatTimecode (double seconds)
    {
        const bool negative = seconds < 0.0;
        seconds = std::abs (seconds);

        const int mins = static_cast<int> (seconds / 60.0);
        const double secs = seconds - static_cast<double> (mins) * 60.0;

        return juce::String::formatted ("%s%02d:%06.3f", negative ? "-" : "", mins, secs);
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
            || p.endsWith (".ssa");
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

    setResizable (false, false);
    applyWindowSize();

    //--------------------------------------------------------------------------
    for (auto* b : { &loadButton, &pasteButton, &demoButton,
                     &minusButton, &plusButton, &resetButton,
                     &demoPlayButton, &demoStopButton,
                     &applyTextButton, &cancelTextButton,
                     &tapStartButton, &tapUndoButton, &tapFinishButton, &tapExitButton,
                     &measureButton, &demoClockToggle,
                     &orientHButton, &orientVButton, &pickImageButton, &clearImageButton })
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

    for (auto* box : { &languageBox, &zoomBox, &skinBox, &fontBox, &colourBox })
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

    addLabel (clockLabel,  21.0f, true,  juce::Justification::centredRight);
    addLabel (statusLabel, 11.0f, false, juce::Justification::centredRight);
    addLabel (offsetLabel, 12.0f, true,  juce::Justification::centred);
    addLabel (fileLabel,   11.5f, false, juce::Justification::centredLeft);
    addLabel (metaLabel,   11.5f, false, juce::Justification::centredRight);
    addLabel (hintLabel,   11.0f, false, juce::Justification::centredLeft);
    addLabel (fontLabel,   11.0f, false, juce::Justification::centredLeft);
    addLabel (audioLabel,  11.5f, false, juce::Justification::centredRight);

    editorBox.setMultiLine (true, true);
    editorBox.setReturnKeyStartsNewLine (true);
    editorBox.setScrollbarsShown (true);
    editorBox.setFont (uiFont (14.0f));
    editorBox.setPopupMenuEnabled (true);
    addChildComponent (editorBox);

    fileDescription = tr (Lang::TextId::demoFileDesc);

    applyUiText (true);
    refreshLookAndFeel();
    updateVisibility();
    startTimerHz (30);

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
    }

    skinIndex   = juce::jlimit (0, UiPalette::numSkins() - 1, skinIndex);
    fontIndex   = juce::jlimit (0, kNumFonts - 1, fontIndex);
    colourIndex = juce::jlimit (0, kNumLyricColours - 1, colourIndex);
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

    f.replaceWithText (lines.joinIntoString ("\n"));
}

//==============================================================================
juce::Font LyricSyncEditor::uiFont (float size, bool bold) const
{
    return juce::Font (juce::String (kFontNames[juce::jlimit (0, kNumFonts - 1, fontIndex)]),
                       size, bold ? juce::Font::bold : juce::Font::plain);
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

    const auto bgPath = processor.getSetting (Settings::customBg, "").toString();
    if (bgPath.isNotEmpty() && juce::File (bgPath).existsAsFile())
        customBg = juce::ImageFileFormat::loadFrom (juce::File (bgPath));

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

    palette = UiPalette::forSkin (skinIndex);
    lookAndFeel.typeface = juce::String (kFontNames[fontIndex]);
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
    measureButton   .setButtonText (tr (Lang::TextId::measureAudio));

    orientHButton   .setButtonText (tr (Lang::TextId::orientH));
    orientVButton   .setButtonText (tr (Lang::TextId::orientV));
    pickImageButton .setButtonText (tr (Lang::TextId::pickImage));
    clearImageButton.setButtonText (tr (Lang::TextId::clearImage));

    fontLabel.setText (tr (Lang::TextId::fontSize), juce::dontSendNotification);
    hintLabel.setText (tr (Lang::TextId::hint), juce::dontSendNotification);

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
        fontBox.addItem (kFontNames[i], i + 1);
    fontBox.setSelectedId (fontIndex + 1, juce::dontSendNotification);

    colourBox.clear (juce::dontSendNotification);
    for (int i = 0; i < kNumLyricColours; ++i)
        colourBox.addItem (tr (static_cast<Lang::TextId> (static_cast<int> (Lang::TextId::colorAuto) + i)),
                           i + 1);
    colourBox.setSelectedId (colourIndex + 1, juce::dontSendNotification);

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
    measureButton   .setTooltip (tr (Lang::TextId::tipMeasure));
    audioLabel      .setTooltip (tr (Lang::TextId::audioUnknown));
    editorBox       .setTooltip (tr (Lang::TextId::tipPaste));

    gearButton      .setTooltip (tr (Lang::TextId::tipGear));
    logoButton      .setTooltip (tr (Lang::TextId::tipLogo));
    zoomBox         .setTooltip (tr (Lang::TextId::tipZoom));
    skinBox         .setTooltip (tr (Lang::TextId::tipSkin));
    fontBox         .setTooltip (tr (Lang::TextId::tipTypeface));
    colourBox       .setTooltip (tr (Lang::TextId::tipColor));
    languageBox     .setTooltip (tr (Lang::TextId::uiLanguage));
    orientHButton   .setTooltip (tr (Lang::TextId::tipLayout));
    orientVButton   .setTooltip (tr (Lang::TextId::tipLayout));
    pickImageButton .setTooltip (tr (Lang::TextId::tipPickImage));
    clearImageButton.setTooltip (tr (Lang::TextId::tipClearImage));
    closeSettingsButton.setTooltip (tr (Lang::TextId::tipClose));

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

    l.setColour (juce::TextButton::buttonColourId,   palette.panel);
    l.setColour (juce::TextButton::buttonOnColourId, palette.accentSoft);
    l.setColour (juce::TextButton::textColourOffId,  palette.text);
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

    l.setColour (juce::ComboBox::backgroundColourId,  palette.panel);
    l.setColour (juce::ComboBox::textColourId,        palette.text);
    l.setColour (juce::ComboBox::outlineColourId,     palette.panelEdge);
    l.setColour (juce::ComboBox::arrowColourId,       palette.dim);
    l.setColour (juce::ComboBox::buttonColourId,      palette.panelEdge);
    l.setColour (juce::PopupMenu::backgroundColourId, palette.panel);
    l.setColour (juce::PopupMenu::textColourId,       palette.text);
    l.setColour (juce::PopupMenu::highlightedBackgroundColourId, palette.accentSoft);
    l.setColour (juce::PopupMenu::highlightedTextColourId,       palette.text);

    l.setColour (juce::TooltipWindow::backgroundColourId, palette.panel);
    l.setColour (juce::TooltipWindow::textColourId,       palette.text);
    l.setColour (juce::TooltipWindow::outlineColourId,    palette.panelEdge);

    clockLabel.setColour (juce::Label::textColourId, palette.text);
    statusLabel.setColour (juce::Label::textColourId, palette.dim);
    offsetLabel.setColour (juce::Label::textColourId, palette.accent);
    offsetLabel.setColour (juce::Label::backgroundColourId, palette.card);
    hintLabel.setColour (juce::Label::textColourId, palette.faint);
    fileLabel.setColour (juce::Label::textColourId, palette.dim);
    metaLabel.setColour (juce::Label::textColourId, palette.faint);
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
    repaint();
}

void LyricSyncEditor::updateVisibility()
{
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
                     static_cast<juce::Component*> (&gearButton) })
        c->setVisible (normal);

    demoPlayButton .setVisible (normal && demoMode);
    demoStopButton .setVisible (normal && demoMode);

    applyTextButton .setVisible (editing && ! overlay);
    cancelTextButton.setVisible (editing && ! overlay);
    tapStartButton  .setVisible (editing && ! overlay);

    tapUndoButton   .setVisible (tapMode && ! overlay);
    tapFinishButton .setVisible (tapMode && ! overlay);
    tapExitButton   .setVisible (tapMode && ! overlay);

    for (auto* c : { static_cast<juce::Component*> (&languageBox),
                     static_cast<juce::Component*> (&zoomBox),
                     static_cast<juce::Component*> (&skinBox),
                     static_cast<juce::Component*> (&fontBox),
                     static_cast<juce::Component*> (&colourBox),
                     static_cast<juce::Component*> (&orientHButton),
                     static_cast<juce::Component*> (&orientVButton),
                     static_cast<juce::Component*> (&pickImageButton),
                     static_cast<juce::Component*> (&clearImageButton),
                     static_cast<juce::Component*> (&closeSettingsButton) })
        c->setVisible (settingsOpen);

    closeAboutButton.setVisible (aboutOpen);
    logoButton.setVisible (! overlay);

    clearImageButton.setEnabled (customBg.isValid());
}

//==============================================================================
juce::Rectangle<int> LyricSyncEditor::scaledRect (float x, float y, float w, float h) const
{
    return juce::Rectangle<int> (
        juce::roundToInt (originX + x * uiScale),
        juce::roundToInt (originY + y * uiScale),
        juce::jmax (1, juce::roundToInt (w * uiScale)),
        juce::jmax (1, juce::roundToInt (h * uiScale)));
}

juce::Rectangle<float> LyricSyncEditor::stageArea() const
{
    if (orientation == Orientation::vertical)
        return { 22.0f, 72.0f, vBaseW - 44.0f, 556.0f };

    return { 22.0f, 72.0f, hBaseW - 44.0f, 394.0f };
}

juce::Rectangle<float> LyricSyncEditor::settingsPanelArea() const
{
    if (orientation == Orientation::vertical)
        return { 40.0f, 180.0f, 520.0f, 482.0f };

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
    clockLabel .setFont (uiFont (21.0f  * uiScale, true));
    statusLabel.setFont (uiFont (11.0f  * uiScale));
    offsetLabel.setFont (uiFont (12.0f  * uiScale, true));
    fileLabel  .setFont (uiFont (11.5f * uiScale));
    metaLabel  .setFont (uiFont (11.5f * uiScale));
    hintLabel  .setFont (uiFont (11.0f  * uiScale));
    fontLabel  .setFont (uiFont (11.0f  * uiScale));
    audioLabel .setFont (uiFont (11.5f * uiScale));
    editorBox  .setFont (uiFont (14.0f  * uiScale));
    editorBox.applyFontToAllText (uiFont (14.0f * uiScale));
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

    uiScale = juce::jmax (0.1f, juce::jmin (w / baseW(), h / baseH()));
    originX = (w - baseW() * uiScale) * 0.5f;
    originY = (h - baseH() * uiScale) * 0.5f;
    uiTransform = juce::AffineTransform (uiScale, 0.0f, originX,
                                         0.0f, uiScale, originY);
    lookAndFeel.scale = uiScale;
    lookAndFeel.typeface = juce::String (kFontNames[fontIndex]);

    updateScaledFonts();

    const auto stage = stageArea();

    logoButton.setBounds (scaledRect (14, 4,
                                      orientation == Orientation::vertical ? 250.0f : 310.0f,
                                      54));

    if (orientation == Orientation::horizontal)
    {
        gearButton   .setBounds (scaledRect (882, 12, 46, 46));
        clockLabel   .setBounds (scaledRect (520, 8, 340, 30));
        statusLabel  .setBounds (scaledRect (520, 37, 340, 18));

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
            place (tapUndoButton, 82);
            place (tapFinishButton, 88);
            place (tapExitButton, 82);
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
            measureButton.setBounds (scaledRect (832, 538, 106, 26));
        }

        fileLabel .setBounds (scaledRect (22, 518, 420, 22));
        metaLabel .setBounds (scaledRect (452, 518, 296, 22));
        hintLabel .setBounds (scaledRect (22, 542, 648, 20));
        audioLabel.setBounds (scaledRect (676, 541, 150, 22));
    }
    else
    {
        gearButton   .setBounds (scaledRect (532, 12, 46, 46));
        clockLabel   .setBounds (scaledRect (300, 6, 226, 30));
        statusLabel  .setBounds (scaledRect (300, 36, 226, 18));

        editorBox.setBounds (scaledRect (stage.getX() + 2.0f, stage.getY() + 2.0f,
                                         stage.getWidth() - 4.0f, stage.getHeight() - 4.0f));

        if (editing)
        {
            applyTextButton .setBounds (scaledRect (22, 646, 176, 30));
            cancelTextButton.setBounds (scaledRect (206, 646, 176, 30));
            tapStartButton  .setBounds (scaledRect (390, 646, 176, 30));
        }
        else if (tapMode)
        {
            tapUndoButton  .setBounds (scaledRect (22, 646, 176, 30));
            tapFinishButton.setBounds (scaledRect (206, 646, 176, 30));
            tapExitButton  .setBounds (scaledRect (390, 646, 176, 30));
        }
        else
        {
            loadButton .setBounds (scaledRect (22, 644, 176, 30));
            pasteButton.setBounds (scaledRect (206, 644, 176, 30));
            demoButton .setBounds (scaledRect (390, 644, 176, 30));

            minusButton .setBounds (scaledRect (22, 682, 56, 30));
            offsetLabel .setBounds (scaledRect (86, 682, 120, 30));
            plusButton  .setBounds (scaledRect (214, 682, 56, 30));
            resetButton .setBounds (scaledRect (278, 682, 96, 30));
            fontLabel   .setBounds (scaledRect (382, 686, 40, 22));
            fontSlider  .setBounds (scaledRect (426, 686, 140, 22));

            demoClockToggle.setBounds (scaledRect (22, 720, 176, 30));
            demoPlayButton .setBounds (scaledRect (206, 720, 60, 30));
            demoStopButton .setBounds (scaledRect (274, 720, 60, 30));
            measureButton  .setBounds (scaledRect (342, 720, 150, 30));
        }

        fileLabel .setBounds (scaledRect (22, 762, 556, 22));
        metaLabel .setBounds (scaledRect (22, 786, 556, 22));
        hintLabel .setBounds (scaledRect (22, 812, 556, 20));
        audioLabel.setBounds (scaledRect (22, 836, 300, 22));
    }

    //---- Settings panel -----------------------------------------------------
    {
        const auto panel = settingsPanelArea();
        const bool v = (orientation == Orientation::vertical);
        const float ctrlX = panel.getX() + (v ? 168.0f : 190.0f);
        const float ctrlW = panel.getRight() - 24.0f - ctrlX;
        const float rowH = 48.0f;

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
        colourBox.setBounds (scaledRect (ctrlX, y, ctrlW, 28));
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

        g.setColour (palette.bgTop.withAlpha (0.62f));
        g.fillAll();
    }
    else
    {
        juce::ColourGradient gradient (palette.bgTop, 0.0f, 0.0f,
                                       palette.bgBottom, 0.0f, bounds.getHeight(), false);
        g.setGradientFill (gradient);
        g.fillAll();
    }

    // 逻辑画布
    const juce::Graphics::ScopedSaveState state (g);
    g.addTransform (uiTransform);

    g.setColour (palette.accent.withAlpha (0.045f));
    g.fillEllipse (baseW() - 230.0f, -190.0f, 460.0f, 460.0f);

    paintHeader (g);
    paintStage  (g, stageArea());

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

    if (orientation == Orientation::horizontal)
        drawText (g, juce::Rectangle<float> (48.0f, 35.0f, 330.0f, 18.0f),
                  tr (Lang::TextId::appSubtitle), 11.0f, palette.faint, false, -1, {}, 1.0f,
                  juce::Justification::centredLeft);

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
    g.setColour (palette.card);
    g.fillRoundedRectangle (area, 18.0f);
    g.setColour (palette.panelEdge.withAlpha (0.55f));
    g.drawRoundedRectangle (area, 18.0f, 1.0f);

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
    g.drawText (tr (Lang::TextId::currentLine),
                static_cast<int> (area.getX() + 30.0f),
                static_cast<int> (area.getY() + 15.0f), 200, 16,
                juce::Justification::centredLeft);

    if (! doc.isEmpty())
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
    const float widthScale = juce::jlimit (0.80f, 1.30f, area.getWidth() / refW)
                           * static_cast<float> (fontScale);

    const float innerH = area.getHeight() - 78.0f;
    const int radius = juce::jlimit (2, 4, static_cast<int> (innerH / 92.0f));

    const auto inner = juce::Rectangle<float> (area.getX() + 26.0f, area.getY() + 36.0f,
                                               area.getWidth() - 52.0f, innerH);
    const float spacing = juce::jlimit (26.0f, 56.0f,
                                        inner.getHeight() / (2.0f * radius + 1.0f));
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
                const bool karaoke = line.hasKaraoke();
                const int  sung    = karaoke ? doc.sungCharsAt (line, displayTime) : -1;
                const float fade   = 0.60f + 0.40f * transition;

                drawText (g, lineRect, line.text, size, currentTint, true,
                          sung, palette.text, fade);

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
                drawText (g, lineRect, line.text, size, palette.text,
                          ad < 1.5f, -1, {}, alpha);
            }
        }
    }

    if (! hasCurrent)
    {
        const float y = centreY - 13.0f;

        drawText (g, juce::Rectangle<float> (area.getX() + 60.0f, y,
                                             area.getWidth() - 120.0f, 26.0f),
                  tr (Lang::TextId::intro), 17.0f * widthScale,
                  palette.faint, false);

        const double until = doc.line (0).time;
        drawText (g, juce::Rectangle<float> (area.getX() + 60.0f, y + 30.0f,
                                             area.getWidth() - 120.0f, 20.0f),
                  tr (Lang::TextId::nextLineIn)
                    + juce::String (juce::jmax (0.0, until - displayTime), 1)
                    + tr (Lang::TextId::secondsSuffix),
                  11.5f * widthScale, palette.faint, false);
    }
}

//==============================================================================
void LyricSyncEditor::paintTapStage (juce::Graphics& g, juce::Rectangle<float> area)
{
    const int total = static_cast<int> (tapLines.size());
    const int idx   = juce::jlimit (0, juce::jmax (0, total - 1), tapIndex);

    g.setColour (palette.accent);
    g.setFont (uiFont (10.5f, true));
    g.drawText (tr (Lang::TextId::tapTitle),
                static_cast<int> (area.getX() + 30.0f),
                static_cast<int> (area.getY() + 15.0f), 240, 16,
                juce::Justification::centredLeft);

    g.setColour (palette.faint);
    g.setFont (uiFont (10.5f));
    g.drawText (juce::String (juce::jmin (tapIndex + 1, total)) + " / "
                  + juce::String (total),
                static_cast<int> (area.getRight() - 160.0f),
                static_cast<int> (area.getY() + 15.0f), 130, 16,
                juce::Justification::centredRight);

    const float refW = (orientation == Orientation::vertical ? 520.0f : 900.0f);
    const float widthScale = juce::jlimit (0.80f, 1.30f, area.getWidth() / refW)
                           * static_cast<float> (fontScale);

    const float cx = area.getX() + 50.0f;
    const float cw = area.getWidth() - 100.0f;
    const float centreY = area.getCentreY() - 12.0f;

    if (tapIndex > 0 && tapIndex - 1 < total)
    {
        drawText (g, juce::Rectangle<float> (cx, centreY - 104.0f, cw, 20.0f),
                  tr (Lang::TextId::tapPrev) + "  "
                    + formatTimecode (tapTimes[(size_t) (tapIndex - 1)]),
                  10.5f * widthScale, palette.faint, false);

        drawText (g, juce::Rectangle<float> (cx, centreY - 80.0f, cw, 32.0f),
                  tapLines[(size_t) (tapIndex - 1)], 17.0f * widthScale,
                  palette.dim, false);
    }

    if (tapIndex < total)
    {
        drawText (g, juce::Rectangle<float> (cx, centreY - 26.0f, cw, 54.0f),
                  tapLines[(size_t) idx], 31.0f * widthScale, lyricColour(), true);

        if (idx + 1 < total)
        {
            drawText (g, juce::Rectangle<float> (cx, centreY + 40.0f, cw, 18.0f),
                      tr (Lang::TextId::tapNext), 10.5f * widthScale, palette.faint, false);

            drawText (g, juce::Rectangle<float> (cx, centreY + 58.0f, cw, 28.0f),
                      tapLines[(size_t) (idx + 1)], 16.0f * widthScale,
                      palette.dim.withAlpha (0.55f), false);
        }
    }
    else
    {
        drawText (g, juce::Rectangle<float> (cx, centreY - 26.0f, cw, 54.0f),
                  tr (Lang::TextId::tapDoneLabel), 24.0f * widthScale, palette.ok, true);
    }

    drawText (g, juce::Rectangle<float> (area.getX() + 30.0f, area.getBottom() - 96.0f,
                                         area.getWidth() - 60.0f, 22.0f),
              tr (Lang::TextId::tapHint), 12.0f * widthScale, palette.dim, false);

    drawText (g, juce::Rectangle<float> (area.getX() + 30.0f, area.getBottom() - 70.0f,
                                         area.getWidth() - 60.0f, 28.0f),
              tr (Lang::TextId::tapWillStamp) + "  " + formatTimecode (displayTime),
              17.0f * widthScale, palette.accent, true);

    drawText (g, juce::Rectangle<float> (area.getX() + 30.0f, area.getBottom() - 38.0f,
                                         area.getWidth() - 60.0f, 18.0f),
              tr (Lang::TextId::tapSpaceWarn), 10.5f * widthScale, palette.faint, false);
}

//==============================================================================
void LyricSyncEditor::paintSettings (juce::Graphics& g, juce::Rectangle<float> panel)
{
    g.setColour (juce::Colours::black.withAlpha (0.55f));
    g.fillRect (juce::Rectangle<float> (0.0f, 0.0f, baseW(), baseH()));

    g.setColour (palette.panel);
    g.fillRoundedRectangle (panel, 16.0f);
    g.setColour (palette.panelEdge);
    g.drawRoundedRectangle (panel, 16.0f, 1.0f);

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
    const float rowH = 48.0f;

    const Lang::TextId labels[] =
    {
        Lang::TextId::setLanguage,
        Lang::TextId::setZoom,
        Lang::TextId::setLayout,
        Lang::TextId::setSkin,
        Lang::TextId::setBackground,
        Lang::TextId::setFont,
        Lang::TextId::setColor
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

    g.setColour (palette.panel);
    g.fillRoundedRectangle (panel, 16.0f);
    g.setColour (palette.panelEdge);
    g.drawRoundedRectangle (panel, 16.0f, 1.0f);

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
                                int sungChars, juce::Colour sungColour, float alpha,
                                juce::Justification just)
{
    if (text.isEmpty() || area.isEmpty())
        return;

    juce::AttributedString s;
    s.setText (text);
    s.setFont (uiFont (size, bold));
    s.setJustification (just);
    s.setWordWrap (juce::AttributedString::byChar);
    s.setColour (colour.withAlpha (alpha));

    if (sungChars > 0)
        s.setColour (juce::Range<int> (0, juce::jmin (sungChars, text.length())),
                     sungColour.withAlpha (alpha));

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

    if (demoMode)
    {
        if (demoPlaying)
            demoTime = juce::jlimit (range.getStart(), range.getEnd(), demoTime + dt);

        displayTime = demoTime;
    }
    else
    {
        displayTime = processor.positionSeconds.load() + offsetMs / 1000.0;
    }

    const int idx = doc.findIndexAt (displayTime);
    if (idx != currentIndex)
    {
        currentIndex = idx;
        transition = 0.0f;
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
    clockLabel .setText (timecodeText (t), juce::dontSendNotification);
    statusLabel.setText (statusText(), juce::dontSendNotification);
    offsetLabel.setText (juce::String (offsetMs, 0) + " ms", juce::dontSendNotification);
    metaLabel  .setText (barBeatText(), juce::dontSendNotification);
    audioLabel .setText (audioRangeText(), juce::dontSendNotification);

    auto label = fileDescription;
    if (lastWarning.isNotEmpty())
        label += tr (Lang::TextId::warningPrefix) + lastWarning;
    fileLabel.setText (label, juce::dontSendNotification);
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
         + " BPM" + sep() + juce::String (num) + "/" + juce::String (den);
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

    if (button == &tapStartButton)      { enterTapMode(); return; }
    if (button == &tapUndoButton)       { undoStamp(); return; }
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
}

void LyricSyncEditor::sliderValueChanged (juce::Slider* slider)
{
    if (slider == &fontSlider)
    {
        fontScale = fontSlider.getValue();
        processor.setSetting (Settings::fontScale, fontScale);
        repaint();
    }
}

//==============================================================================
void LyricSyncEditor::mouseDown (const juce::MouseEvent& e)
{
    const auto toLogical = [this] (juce::Point<float> p)
    {
        const float s = juce::jmax (0.01f, uiScale);
        return juce::Point<float> ((p.x - originX) / s, (p.y - originY) / s);
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
        grabKeyboardFocus();
        stampTap();
    }
}

bool LyricSyncEditor::keyPressed (const juce::KeyPress& key)
{
    if (key.getKeyCode() == juce::KeyPress::escapeKey)
    {
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
    lookAndFeel.typeface = juce::String (kFontNames[fontIndex]);

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
        processor.setSetting (Settings::customBg, file.getFullPathName());
        setSkin (5);
        updateVisibility();
        repaint();
    });
}

void LyricSyncEditor::clearBackgroundImage()
{
    customBg = juce::Image {};
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
        "*.lrc;*.krc;*.txt;*.srt;*.vtt");

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
    double krcTotal = -1.0;

    if (loaded && Krc::isKrcData (data.getData(), static_cast<int> (data.getSize())))
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
                    + (isKrc ? tr (Lang::TextId::krcFormat) : info.formatName)
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
    editorBox.setText (processor.getSetting (Settings::lyricsText, "").toString());
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
        loadLyricsText (editorBox.getText(), tr (Lang::TextId::pastedLabel));

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

    std::vector<juce::String> plain;

    for (const auto& raw : juce::StringArray::fromLines (source))
    {
        auto t = stripTimeTags (raw).trim();

        if (t.isNotEmpty())
            plain.push_back (t);
    }

    if (plain.empty())
    {
        lastWarning = tr (Lang::TextId::tapNoLines);
        repaint();
        return;
    }

    tapLines = std::move (plain);
    tapTimes.assign (tapLines.size(), -1.0);
    tapIndex = 0;
    tapMode = true;
    editing = false;
    editorBox.setVisible (false);
    lastWarning = {};

    if (demoMode)
    {
        demoTime = audioRange().getStart();
        demoPlaying = true;
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
        t = juce::jmax (t, tapTimes[(size_t) (tapIndex - 1)] + 0.06);

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

    updateVisibility();
    resized();
    repaint();
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
