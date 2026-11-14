#include "PluginEditor.h"
#include "DemoLyrics.h"

#include <cmath>
#include <string>

#if JUCE_WINDOWS
 #include <windows.h>
#endif

namespace
{
    juce::Font uiFont (float size, bool bold = false)
    {
       #if JUCE_WINDOWS
        // YaHei covers both CJK and Latin, so it works in either UI language.
        return juce::Font (juce::String ("Microsoft YaHei UI"), size,
                           bold ? juce::Font::bold : juce::Font::plain);
       #else
        return juce::Font (size, bold ? juce::Font::bold : juce::Font::plain);
       #endif
    }

    juce::String formatTimecode (double seconds)
    {
        const bool negative = seconds < 0.0;
        seconds = std::abs (seconds);

        const int mins = static_cast<int> (seconds / 60.0);
        const double secs = seconds - static_cast<double> (mins) * 60.0;

        return juce::String::formatted ("%s%02d:%06.3f", negative ? "-" : "", mins, secs);
    }

    /** Short m:ss, used for the "audio length" readout. */
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
    juce::String decodeLyricsFile (const juce::File& file)
    {
        juce::MemoryBlock data;
        if (! file.loadFileAsData (data) || data.getSize() == 0)
            return {};

        auto* bytes = static_cast<const char*> (data.getData());
        const int size = static_cast<int> (data.getSize());

        // UTF-16 LE BOM
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
        return p.endsWith (".lrc") || p.endsWith (".txt") || p.endsWith (".srt")
            || p.endsWith (".vtt") || p.endsWith (".ass") || p.endsWith (".ssa");
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
}

//==============================================================================
// Every widget must use the same Chinese-capable font, otherwise buttons and
// combo boxes fall back to a Latin-only typeface and show "tofu" squares.
juce::Font ScaledLookAndFeel::getTextButtonFont (juce::TextButton&, int)
{
    return uiFont (12.5f * scale);
}

juce::Font ScaledLookAndFeel::getComboBoxFont (juce::ComboBox&)
{
    return uiFont (12.0f * scale);
}

juce::Font ScaledLookAndFeel::getLabelFont (juce::Label&)
{
    return uiFont (12.0f * scale);
}

juce::Font ScaledLookAndFeel::getPopupMenuFont()
{
    return uiFont (13.0f * scale);
}

juce::Font ScaledLookAndFeel::getSliderPopupFont (juce::Slider&)
{
    return uiFont (12.0f * scale);
}

//==============================================================================
LyricSyncEditor::LyricSyncEditor (LyricSyncProcessor& owner)
    : juce::AudioProcessorEditor (owner),
      processor (owner),
      palette (UiPalette::dark())
{
    setLookAndFeel (&lookAndFeel);
    setOpaque (true);

    syncControlsFromSettings();

    // Keep the window itself at the design aspect ratio, so resizing is a true
    // uniform zoom rather than a stretch. The logical-canvas fallback in
    // paint()/resized() still protects us if a host ignores this constrainer.
    setResizable (true, true);
    setResizeLimits (480, 300, 2400, 1500);
    setWantsKeyboardFocus (true);

    if (auto* constrainer = getConstrainer())
        constrainer->setFixedAspectRatio (static_cast<double> (baseW / baseH));

    const int savedW = static_cast<int> (processor.getSetting (Settings::windowW, 960));
    const int w = juce::jlimit (480, 2400, savedW);
    setSize (w, juce::roundToInt (static_cast<float> (w) / (baseW / baseH)));

    //--------------------------------------------------------------------------
    for (auto* b : { &loadButton, &pasteButton, &demoButton, &themeButton,
                     &minusButton, &plusButton, &resetButton,
                     &demoPlayButton, &demoStopButton,
                     &applyTextButton, &cancelTextButton,
                     &tapStartButton, &tapUndoButton, &tapFinishButton, &tapExitButton,
                     &measureButton,
                     &demoClockToggle })
    {
        b->addListener (this);
        addAndMakeVisible (*b);
    }

    demoClockToggle.setClickingTogglesState (true);

    languageBox.setEditableText (false);
    languageBox.setJustificationType (juce::Justification::centred);

    // Item labels are language-independent, so they are added only once.
    updatingLanguageBox = true;
    languageBox.addItem (Lang::tr (Lang::TextId::languageChinese, 0), 1);
    languageBox.addItem (Lang::tr (Lang::TextId::languageEnglish, 0), 2);
    languageBox.setSelectedId (language + 1, juce::dontSendNotification);
    updatingLanguageBox = false;

    languageBox.addListener (this);
    addAndMakeVisible (languageBox);

    seekSlider.setRange (0.0, 60.0, 0.01);
    seekSlider.addListener (this);
    addAndMakeVisible (seekSlider);

    fontSlider.setRange (0.70, 1.60, 0.01);
    fontSlider.setValue (fontScale, juce::dontSendNotification);
    fontSlider.addListener (this);
    addAndMakeVisible (fontSlider);

    clockLabel.setFont (uiFont (21.0f, true));
    clockLabel.setJustificationType (juce::Justification::centredRight);
    addAndMakeVisible (clockLabel);

    statusLabel.setFont (uiFont (11.0f));
    statusLabel.setJustificationType (juce::Justification::centredRight);
    addAndMakeVisible (statusLabel);

    offsetLabel.setFont (uiFont (12.0f, true));
    offsetLabel.setJustificationType (juce::Justification::centred);
    addAndMakeVisible (offsetLabel);

    fileLabel.setFont (uiFont (11.5f));
    fileLabel.setJustificationType (juce::Justification::centredLeft);
    addAndMakeVisible (fileLabel);

    metaLabel.setFont (uiFont (11.5f));
    metaLabel.setJustificationType (juce::Justification::centredRight);
    addAndMakeVisible (metaLabel);

    hintLabel.setFont (uiFont (11.0f));
    hintLabel.setJustificationType (juce::Justification::centredLeft);
    addAndMakeVisible (hintLabel);

    fontLabel.setFont (uiFont (11.0f));
    fontLabel.setJustificationType (juce::Justification::centredLeft);
    addAndMakeVisible (fontLabel);

    audioLabel.setFont (uiFont (11.5f));
    audioLabel.setJustificationType (juce::Justification::centredRight);
    addAndMakeVisible (audioLabel);

    editorBox.setMultiLine (true, true);
    editorBox.setReturnKeyStartsNewLine (true);
    editorBox.setScrollbarsShown (true);
    editorBox.setFont (uiFont (14.0f));
    editorBox.setPopupMenuEnabled (true);
    addChildComponent (editorBox);      // hidden by default

    fileDescription = tr (Lang::TextId::demoFileDesc);

    applyUiText (true);
    refreshLookAndFeel();
    updateEditingVisibility();
    startTimerHz (30);

    // Created last: TooltipWindow adds itself to this component, so every
    // member it might touch is already constructed.
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
void LyricSyncEditor::syncControlsFromSettings()
{
    offsetMs   = static_cast<double> (processor.getSetting (Settings::offsetMs, 0.0));
    fontScale  = static_cast<double> (processor.getSetting (Settings::fontScale, 1.0));
    themeIndex = static_cast<int>    (processor.getSetting (Settings::theme, 0));
    demoMode   = static_cast<bool>   (processor.getSetting (Settings::demoClock, false));
    language   = juce::jlimit (0, 1, static_cast<int> (processor.getSetting (Settings::language, 0)));

    // Restore the last measured audio range so the progress bar is correct
    // immediately after a project reload, without waiting for playback.
    const double s = static_cast<double> (processor.getSetting (Settings::audioStart, -1.0));
    const double e = static_cast<double> (processor.getSetting (Settings::audioEnd,   -1.0));

    if (s >= 0.0 && e > s + 1.0)
    {
        processor.audioStartSeconds.store (s);
        processor.audioEndSeconds.store (e);
        savedAudioStart = s;
        savedAudioEnd   = e;
    }

    palette = UiPalette::fromIndex (themeIndex);
    fontSlider.setValue (fontScale, juce::dontSendNotification);
    demoClockToggle.setToggleState (demoMode, juce::dontSendNotification);
}

/** Re-apply every localised widget text. Called at startup and on language switch. */
void LyricSyncEditor::applyUiText (bool reloadDemoLyrics)
{
    loadButton      .setButtonText (tr (Lang::TextId::loadLyrics));
    pasteButton     .setButtonText (tr (Lang::TextId::pasteEdit));
    demoButton      .setButtonText (tr (Lang::TextId::demoLyrics));
    themeButton     .setButtonText (tr (themeIndex == 1 ? Lang::TextId::themeDark
                                                        : Lang::TextId::themeLight));

    updatingLanguageBox = true;
    languageBox.setSelectedId (language + 1, juce::dontSendNotification);
    updatingLanguageBox = false;

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

    fontLabel.setText (tr (Lang::TextId::fontSize), juce::dontSendNotification);
    hintLabel.setText (tr (Lang::TextId::hint), juce::dontSendNotification);

    //---- Hover explanations ------------------------------------------------
    loadButton      .setTooltip (tr (Lang::TextId::tipLoad));
    pasteButton     .setTooltip (tr (Lang::TextId::tipPaste));
    demoButton      .setTooltip (tr (Lang::TextId::tipDemo));
    themeButton     .setTooltip (tr (Lang::TextId::tipTheme));
    minusButton     .setTooltip (tr (Lang::TextId::tipMinus));
    plusButton      .setTooltip (tr (Lang::TextId::tipPlus));
    resetButton     .setTooltip (tr (Lang::TextId::tipReset));
    fontSlider      .setTooltip (tr (Lang::TextId::tipFont));
    fontLabel       .setTooltip (tr (Lang::TextId::tipFont));
    demoClockToggle .setTooltip (tr (Lang::TextId::tipPreview));
    demoPlayButton  .setTooltip (tr (Lang::TextId::tipPlay));
    demoStopButton  .setTooltip (tr (Lang::TextId::tipStop));
    languageBox     .setTooltip (tr (Lang::TextId::uiLanguage));
    seekSlider      .setTooltip (tr (Lang::TextId::tipSeek));
    applyTextButton .setTooltip (tr (Lang::TextId::tipApply));
    cancelTextButton.setTooltip (tr (Lang::TextId::tipCancel));
    tapStartButton  .setTooltip (tr (Lang::TextId::tipTap));
    tapUndoButton   .setTooltip (tr (Lang::TextId::tipUndo));
    tapFinishButton .setTooltip (tr (Lang::TextId::tipFinish));
    tapExitButton   .setTooltip (tr (Lang::TextId::tipExit));
    measureButton   .setTooltip (tr (Lang::TextId::tipMeasure));
    audioLabel      .setTooltip (tr (Lang::TextId::audioUnknown));
    editorBox       .setTooltip (tr (Lang::TextId::tipPaste));

    if (reloadDemoLyrics && usingDemoLyrics)
    {
        // Built-in demo follows the UI language; user-supplied lyrics never change.
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

    // The seek bar is drawn by paintFooter() on the fixed logical canvas, so
    // its progress is always visible regardless of slider look-and-feel.
    seekSlider.setColour (juce::Slider::trackColourId,      juce::Colours::transparentBlack);
    seekSlider.setColour (juce::Slider::backgroundColourId, juce::Colours::transparentBlack);
    seekSlider.setColour (juce::Slider::thumbColourId,      juce::Colours::transparentBlack);

    seekSlider.setColour (juce::Slider::textBoxTextColourId, palette.text);

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

    // Tooltip: readable in both themes, on a solid panel with an edge.
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

    editorBox.applyFontToAllText (uiFont (14.0f * uiScale));
    repaint();
}

void LyricSyncEditor::updateEditingVisibility()
{
    const bool normal = ! editing && ! tapMode;

    for (auto* c : { static_cast<juce::Component*> (&loadButton),
                     static_cast<juce::Component*> (&pasteButton),
                     static_cast<juce::Component*> (&demoButton),
                     static_cast<juce::Component*> (&minusButton),
                     static_cast<juce::Component*> (&plusButton),
                     static_cast<juce::Component*> (&resetButton),
                     static_cast<juce::Component*> (&offsetLabel),
                     static_cast<juce::Component*> (&fontLabel),
                     static_cast<juce::Component*> (&fontSlider),
                     static_cast<juce::Component*> (&demoClockToggle) })
        c->setVisible (normal);

    demoPlayButton .setVisible (normal && demoMode);
    demoStopButton .setVisible (normal && demoMode);

    applyTextButton .setVisible (editing);
    cancelTextButton.setVisible (editing);
    tapStartButton  .setVisible (editing);

    tapUndoButton   .setVisible (tapMode);
    tapFinishButton .setVisible (tapMode);
    tapExitButton   .setVisible (tapMode);

    seekSlider.setEnabled (demoMode);
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

void LyricSyncEditor::resized()
{
    const float w = static_cast<float> (getWidth());
    const float h = static_cast<float> (getHeight());

    // One single scale for painting, widgets and fonts. If the host forces a
    // non-8:5 window, the whole canvas is centred with letterbox margins
    // instead of being stretched or clipped.
    uiScale = juce::jmax (0.1f, juce::jmin (w / baseW, h / baseH));
    originX = (w - baseW * uiScale) * 0.5f;
    originY = (h - baseH * uiScale) * 0.5f;
    // Explicit matrix: p -> uiScale * p + origin. Chained scaled() would
    // scale the translation too, so build the matrix directly.
    uiTransform = juce::AffineTransform (uiScale, 0.0f, originX,
                                         0.0f, uiScale, originY);
    lookAndFeel.scale = uiScale;

    updateScaledFonts();

    clockLabel    .setBounds (scaledRect (490, 8, 280, 30));
    statusLabel   .setBounds (scaledRect (490, 37, 280, 18));
    languageBox   .setBounds (scaledRect (770, 18, 78, 28));
    themeButton   .setBounds (scaledRect (856, 18, 80, 28));

    editorBox .setBounds (scaledRect (24, 74, 912, 372));
    seekSlider.setBounds (scaledRect (22, 450, 916, 26));

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
        fontSlider.setBounds (scaledRect (x, 486, 110, 22));
        x += 118.0f;
        demoClockToggle.setBounds (scaledRect (x, 484, 112, 26));
        x += 120.0f;
        demoPlayButton.setBounds (scaledRect (x, 482, 34, 30));
        demoStopButton.setBounds (scaledRect (x + 40.0f, 482, 34, 30));
    }

    fileLabel .setBounds (scaledRect (22, 518, 420, 22));
    metaLabel .setBounds (scaledRect (452, 518, 296, 22));
    hintLabel .setBounds (scaledRect (22, 542, 648, 20));
    audioLabel.setBounds (scaledRect (676, 541, 150, 22));
    measureButton.setBounds (scaledRect (832, 538, 106, 26));

    processor.setSetting (Settings::windowW, getWidth());
    processor.setSetting (Settings::windowH, getHeight());
}

//==============================================================================
juce::Range<double> LyricSyncEditor::progressRange() const
{
    // The bar must describe this song's audio, not the project timeline.
    // The processor measures the first / last non-silent sample it sees on
    // the track; if that has not happened yet, fall back to the lyric length.
    const double s = processor.audioStartSeconds.load();
    const double e = processor.audioEndSeconds.load();

    if (s >= 0.0 && e > s + 1.0)
        return { s, e };

    return { 0.0, juce::jmax (5.0, processor.lyrics().totalDuration()) };
}

//==============================================================================
void LyricSyncEditor::paint (juce::Graphics& g)
{
    const float w = static_cast<float> (getWidth());
    const float h = static_cast<float> (getHeight());

    juce::ColourGradient gradient (palette.bgTop, 0.0f, 0.0f,
                                   palette.bgBottom, 0.0f, h, false);
    g.setGradientFill (gradient);
    g.fillAll();

    // Everything from here on is drawn in the fixed 960x600 logical canvas.
    const juce::Graphics::ScopedSaveState state (g);
    g.addTransform (uiTransform);

    g.setColour (palette.accent.withAlpha (0.045f));
    g.fillEllipse (baseW - 230.0f, -190.0f, 460.0f, 460.0f);

    paintHeader (g, juce::Rectangle<float> (0.0f, 0.0f, baseW, 64.0f));
    paintStage  (g, juce::Rectangle<float> (22.0f, 72.0f, baseW - 44.0f, baseH - 224.0f));
    paintFooter (g, juce::Rectangle<float> (0.0f, baseH - 152.0f, baseW, 152.0f));
}

void LyricSyncEditor::paintHeader (juce::Graphics& g, juce::Rectangle<float> area)
{
    g.setColour (palette.accent);
    g.fillRoundedRectangle (22.0f, 20.0f, 7.0f, 25.0f, 3.5f);
    g.setColour (palette.accent.withAlpha (0.35f));
    g.fillRoundedRectangle (33.0f, 27.0f, 5.0f, 11.0f, 2.5f);

    drawText (g, juce::Rectangle<float> (48.0f, 10.0f, 250.0f, 28.0f),
              "LyricSync", 20.0f, palette.text, true, -1, {}, 1.0f,
              juce::Justification::centredLeft);
    drawText (g, juce::Rectangle<float> (48.0f, 35.0f, 320.0f, 18.0f),
              tr (Lang::TextId::appSubtitle), 11.0f, palette.faint, false, -1, {}, 1.0f,
              juce::Justification::centredLeft);

    // Status LED
    const bool live = ! demoMode && processor.hasHostTime.load()
                   && (juce::Time::getMillisecondCounterHiRes() - lastHostUpdateMs < 1800.0);
    g.setColour (demoMode ? palette.warn
                          : (live ? (processor.isPlaying.load() ? palette.ok : palette.dim)
                                  : palette.faint));
    g.fillEllipse (area.getRight() - 478.0f, 27.0f, 8.0f, 8.0f);

    g.setColour (palette.panelEdge.withAlpha (0.4f));
    g.drawLine (22.0f, 64.0f, area.getRight() - 22.0f, 64.0f, 1.0f);
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

    //---- Top strip ----------------------------------------------------------
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
    // 当前句最大最亮，上下相邻句距离很近、并按远近递减透明度，
    // 像 KTV 字幕那样整段一起动，而不是只显示三行。
    const float widthScale = juce::jlimit (0.80f, 1.30f, area.getWidth() / 900.0f)
                           * static_cast<float> (fontScale);

    const int radius = 3;
    const auto inner = juce::Rectangle<float> (area.getX() + 26.0f, area.getY() + 36.0f,
                                               area.getWidth() - 52.0f,
                                               area.getHeight() - 78.0f);
    const float spacing = juce::jlimit (28.0f, 54.0f,
                                        inner.getHeight() / (2.0f * radius + 1.0f));
    const float centreY = inner.getCentreY();

    const bool hasCurrent = currentIndex >= 0 && currentIndex < doc.size();
    const bool hasTrans   = hasCurrent && doc.line (currentIndex).translation.isNotEmpty();
    const float lift      = hasTrans ? 9.0f * widthScale : 0.0f;

    // Two passes: everything else first, the current line last (on top).
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

            const auto lineRect = juce::Rectangle<float> (area.getX() + 42.0f, y,
                                                          area.getWidth() - 84.0f,
                                                          size * 1.42f);

            const auto& line = doc.line (i);

            if (isCurrent)
            {
                const bool karaoke = line.hasKaraoke();
                const int  sung    = karaoke ? doc.sungCharsAt (line, displayTime) : -1;
                const float fade   = 0.60f + 0.40f * transition;

                drawText (g, lineRect, line.text, size, palette.accent, true,
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

    //---- Intro / waiting for the first line ---------------------------------
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

    //---- Progress inside the current line -----------------------------------
    const float barW = 180.0f * widthScale;
    const auto bar = juce::Rectangle<float> (area.getCentreX() - barW * 0.5f,
                                             area.getBottom() - 20.0f, barW, 3.0f);
    g.setColour (palette.track);
    g.fillRoundedRectangle (bar, 2.0f);

    double frac = 0.0;
    if (currentIndex >= 0 && currentIndex < doc.size())
    {
        const double start = doc.line (currentIndex).time;
        const double end   = doc.lineEnd (currentIndex);
        frac = juce::jlimit (0.0, 1.0,
                             (displayTime - start) / juce::jmax (0.01, end - start));
    }

    if (frac > 0.0)
    {
        g.setColour (palette.accent.withAlpha (0.9f));
        g.fillRoundedRectangle (bar.withWidth (barW * static_cast<float> (frac)), 2.0f);
    }
}

//==============================================================================
void LyricSyncEditor::paintTapStage (juce::Graphics& g, juce::Rectangle<float> area)
{
    const int total = static_cast<int> (tapLines.size());
    const int idx   = juce::jlimit (0, juce::jmax (0, total - 1), tapIndex);

    //---- Header -------------------------------------------------------------
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

    const float widthScale = juce::jlimit (0.80f, 1.30f, area.getWidth() / 900.0f)
                           * static_cast<float> (fontScale);

    const float cx = area.getX() + 50.0f;
    const float cw = area.getWidth() - 100.0f;
    const float centreY = area.getCentreY() - 12.0f;

    //---- Just stamped -------------------------------------------------------
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

    //---- The line being timed ----------------------------------------------
    if (tapIndex < total)
    {
        drawText (g, juce::Rectangle<float> (cx, centreY - 26.0f, cw, 54.0f),
                  tapLines[(size_t) idx], 31.0f * widthScale, palette.accent, true);

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

    //---- Bottom instructions + the timestamp that will be written ----------
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
void LyricSyncEditor::paintFooter (juce::Graphics& g, juce::Rectangle<float> area)
{
    g.setColour (palette.panelEdge.withAlpha (0.28f));
    g.drawLine (22.0f, area.getY() + 27.0f, area.getRight() - 22.0f,
                area.getY() + 27.0f, 1.0f);

    // The seek bar is painted on the fixed logical canvas, so it stays visible
    // and correctly sized no matter how the host resizes the editor window.
    const auto track = juce::Rectangle<float> (22.0f, 461.0f, baseW - 44.0f, 6.0f);
    g.setColour (palette.track);
    g.fillRoundedRectangle (track, 3.0f);

    // Mapped against the measured audio length, not the project length.
    const auto range = progressRange();
    const double span = juce::jmax (0.01, range.getEnd() - range.getStart());
    const double frac = juce::jlimit (0.0, 1.0,
                                      (displayTime - range.getStart()) / span);

    if (frac > 0.0)
    {
        g.setColour (palette.accent.withAlpha (0.85f));
        g.fillRoundedRectangle (track.withWidth (track.getWidth() * static_cast<float> (frac)), 3.0f);
    }

    const float thumbX = juce::jlimit (track.getX(), track.getRight(),
                                       track.getX() + track.getWidth() * static_cast<float> (frac));
    g.setColour (palette.accent);
    g.fillEllipse (thumbX - 6.0f, track.getCentreY() - 6.0f, 12.0f, 12.0f);
}

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

    // No reduceClipRegion() here: under a non-identity AffineTransform the
    // clip rectangle's coordinate space is ambiguous, which could wrongly
    // clip text in a scaled window. AttributedString already lays out inside
    // `area` with by-char wrapping.
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
        refreshLookAndFeel();
        applyUiText (false);
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
    const auto range = progressRange();

    if (seekSlider.getMinimum() != range.getStart()
        || seekSlider.getMaximum() != range.getEnd())
        seekSlider.setRange (range.getStart(), range.getEnd(), 0.01);

    if (demoMode)
    {
        if (demoPlaying)
        {
            demoTime = juce::jlimit (range.getStart(), range.getEnd(), demoTime + dt);
            seekSlider.setValue (demoTime, juce::dontSendNotification);
        }
        displayTime = demoTime;
    }
    else
    {
        // The one line that matters: use the host's absolute project time.
        displayTime = processor.positionSeconds.load() + offsetMs / 1000.0;
        seekSlider.setValue (juce::jlimit (range.getStart(), range.getEnd(), displayTime),
                             juce::dontSendNotification);
    }

    const int idx = doc.findIndexAt (displayTime);
    if (idx != currentIndex)
    {
        currentIndex = idx;
        transition = 0.0f;
    }

    // Smooth karaoke scrolling: a timecode jump snaps instantly (that is the
    // whole point of reading the host clock), a normal line change eases.
    const float target = static_cast<float> (currentIndex);
    if (std::abs (target - displayIndex) > 2.5f)
        displayIndex = target;
    else
        displayIndex += (target - displayIndex) * 0.30f;

    transition = juce::jmin (1.0f, transition + 0.16f);

    // Persist the measured audio range (throttled, GUI thread only).
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
    const auto range = progressRange();
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
    if (button == &themeButton)         { setTheme (themeIndex == 0 ? 1 : 0); return; }
    if (button == &applyTextButton)     { leaveEditMode (true); return; }
    if (button == &cancelTextButton)    { leaveEditMode (false); return; }
    if (button == &demoPlayButton)      { demoPlaying = true; return; }
    if (button == &demoStopButton)      { demoPlaying = false; return; }

    if (button == &measureButton)
    {
        resetAudioMeasurement();
        return;
    }

    //---- Tap timing ---------------------------------------------------------
    if (button == &tapStartButton)      { enterTapMode(); return; }
    if (button == &tapUndoButton)       { undoStamp(); return; }
    if (button == &tapFinishButton)     { finishTapMode(); return; }
    if (button == &tapExitButton)       { exitTapMode(); return; }

    if (button == &demoClockToggle)
    {
        demoMode = demoClockToggle.getToggleState();
        processor.setSetting (Settings::demoClock, demoMode);
        demoPlaying = false;

        const auto range = progressRange();
        demoTime = demoMode ? juce::jlimit (range.getStart(), range.getEnd(),
                                            processor.positionSeconds.load())
                            : range.getStart();

        updateEditingVisibility();
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

    // Buttons steal keyboard focus; in tap mode Space must go to the editor.
    if (tapMode)
        grabKeyboardFocus();
}

void LyricSyncEditor::comboBoxChanged (juce::ComboBox* box)
{
    if (box == &languageBox && ! updatingLanguageBox)
        setLanguage (languageBox.getSelectedId() - 1);
}

void LyricSyncEditor::sliderValueChanged (juce::Slider* slider)
{
    if (slider == &seekSlider && demoMode)
    {
        demoTime = seekSlider.getValue();
        displayTime = demoTime;
        repaint();
    }
    else if (slider == &fontSlider)
    {
        fontScale = fontSlider.getValue();
        processor.setSetting (Settings::fontScale, fontScale);
        repaint();
    }
}

//==============================================================================
void LyricSyncEditor::mouseDown (const juce::MouseEvent&)
{
    if (! tapMode)
        return;

    grabKeyboardFocus();
    stampTap();
}

bool LyricSyncEditor::keyPressed (const juce::KeyPress& key)
{
    if (tapMode)
    {
        if (key.getKeyCode() == juce::KeyPress::spaceKey
            || key.getKeyCode() == juce::KeyPress::returnKey)
        {
            grabKeyboardFocus();
            stampTap();
            return true;
        }

        if (key.getKeyCode() == juce::KeyPress::escapeKey)
        {
            exitTapMode();
            return true;
        }
    }

    return false;
}

//==============================================================================
void LyricSyncEditor::openFileDialog()
{
    chooser = std::make_unique<juce::FileChooser> (
        tr (Lang::TextId::fileChooserTitle),
        juce::File::getSpecialLocation (juce::File::userHomeDirectory),
        "*.lrc;*.txt;*.srt;*.vtt");

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
    const auto text = decodeLyricsFile (file);

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
    processor.setSetting (Settings::lyricsPath, file.getFullPathName());

    fileDescription = file.getFileName() + sep() + info.formatName
                    + sep() + juce::String (info.lineCount)
                    + tr (Lang::TextId::lineCountSuffix);
    if (info.karaokeLines > 0)
        fileDescription += tr (Lang::TextId::karaokeSuffix);

    lastWarning = info.warning;
    usingDemoLyrics = false;
    currentIndex = -2;
    displayIndex = -1.0f;
    demoTime = progressRange().getStart();

    // 新歌 -> 音频长度重新测量
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
    processor.setSetting (Settings::lyricsPath, "");

    fileDescription = label + sep() + info.formatName
                    + sep() + juce::String (info.lineCount)
                    + tr (Lang::TextId::lineCountSuffix);
    lastWarning = info.warning;
    usingDemoLyrics = isDemo;
    currentIndex = -2;
    displayIndex = -1.0f;
    demoTime = progressRange().getStart();
    repaint();
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
    updateEditingVisibility();
    resized();
    repaint();
}

void LyricSyncEditor::leaveEditMode (bool apply)
{
    if (apply)
        loadLyricsText (editorBox.getText(), tr (Lang::TextId::pastedLabel));

    editing = false;
    editorBox.setVisible (false);
    updateEditingVisibility();
    resized();
    repaint();
}

//==============================================================================
void LyricSyncEditor::enterTapMode()
{
    // 文本来源：正在编辑的文本优先，其次是已保存的歌词。
    const auto source = editing ? editorBox.getText()
                                : processor.getSetting (Settings::lyricsText, "").toString();

    tapOriginalText = processor.getSetting (Settings::lyricsText, "").toString();

    std::vector<juce::String> plain;

    for (auto& raw : juce::StringArray::fromLines (source))
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

    const auto range = progressRange();

    if (demoMode)
    {
        demoTime = range.getStart();
        demoPlaying = true;
    }

    updateEditingVisibility();
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

    // 时间必须递增，否则后面二分查找会错乱。
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

    // 注意：这里不能走 loadLyricsText()——它会把 offset 归零，
    // 而打轴记录的时间已经包含了当前 offset，归零会让整首歌偏移。
    const auto info = processor.setLyricsText (lrc, {}, language);
    processor.setSetting (Settings::lyricsPath, "");

    fileDescription = tr (Lang::TextId::tapDoneLabel) + sep() + info.formatName
                    + sep() + juce::String (info.lineCount)
                    + tr (Lang::TextId::lineCountSuffix);
    lastWarning = info.warning;
    usingDemoLyrics = false;
    currentIndex = -2;
    displayIndex = -1.0f;

    updateEditingVisibility();
    resized();
    repaint();
}

void LyricSyncEditor::exitTapMode()
{
    tapMode = false;
    tapLines.clear();
    tapTimes.clear();
    tapIndex = 0;

    // 打轴过程没有改动歌词文档，退出即等于放弃。
    updateEditingVisibility();
    resized();
    repaint();
}

//==============================================================================
void LyricSyncEditor::setTheme (int index)
{
    themeIndex = index;
    palette = UiPalette::fromIndex (index);
    processor.setSetting (Settings::theme, index);
    themeButton.setButtonText (tr (index == 1 ? Lang::TextId::themeDark
                                              : Lang::TextId::themeLight));
    refreshLookAndFeel();
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
