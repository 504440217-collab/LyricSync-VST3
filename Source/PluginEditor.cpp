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
}

LyricSyncEditor::~LyricSyncEditor()
{
    stopTimer();
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
    languageBox.setTooltip (tr (Lang::TextId::uiLanguage));

    minusButton     .setButtonText (tr (Lang::TextId::minusSymbol));
    resetButton     .setButtonText (tr (Lang::TextId::reset));
    demoPlayButton  .setButtonText (tr (Lang::TextId::playSymbol));
    demoStopButton  .setButtonText (tr (Lang::TextId::stopSymbol));
    applyTextButton .setButtonText (tr (Lang::TextId::applyLyrics));
    cancelTextButton.setButtonText (tr (Lang::TextId::cancel));
    demoClockToggle .setButtonText (tr (Lang::TextId::manualPreview));

    fontLabel.setText (tr (Lang::TextId::fontSize), juce::dontSendNotification);
    hintLabel.setText (tr (Lang::TextId::hint), juce::dontSendNotification);

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

    clockLabel.setColour (juce::Label::textColourId, palette.text);
    statusLabel.setColour (juce::Label::textColourId, palette.dim);
    offsetLabel.setColour (juce::Label::textColourId, palette.accent);
    offsetLabel.setColour (juce::Label::backgroundColourId, palette.card);
    hintLabel.setColour (juce::Label::textColourId, palette.faint);
    fileLabel.setColour (juce::Label::textColourId, palette.dim);
    metaLabel.setColour (juce::Label::textColourId, palette.faint);
    fontLabel.setColour (juce::Label::textColourId, palette.faint);

    editorBox.applyFontToAllText (uiFont (14.0f * uiScale));
    repaint();
}

void LyricSyncEditor::updateEditingVisibility()
{
    const bool normal = ! editing;

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
    applyTextButton.setVisible (editing);
    cancelTextButton.setVisible (editing);

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
        place (applyTextButton, 104);
        place (cancelTextButton, 84);
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

    fileLabel.setBounds (scaledRect (22, 518, 458, 22));
    metaLabel.setBounds (scaledRect (480, 518, 458, 22));
    hintLabel.setBounds (scaledRect (22, 542, 916, 20));

    processor.setSetting (Settings::windowW, getWidth());
    processor.setSetting (Settings::windowH, getHeight());
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
              "LyricSync", 20.0f, palette.text, true, -1, {}, 1.0f);
    drawText (g, juce::Rectangle<float> (48.0f, 35.0f, 300.0f, 18.0f),
              tr (Lang::TextId::appSubtitle), 11.0f, palette.faint, false);

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

    if (editing)
        return;

    auto& doc = processor.lyrics();

    g.setColour (palette.faint);
    g.setFont (uiFont (10.5f, true));
    g.drawText (tr (Lang::TextId::currentLine), static_cast<int> (area.getX() + 30.0f),
                static_cast<int> (area.getY() + 16.0f), 160, 16,
                juce::Justification::centredLeft);

    if (doc.isEmpty())
    {
        drawText (g, area.reduced (40.0f, 70.0f),
                  tr (Lang::TextId::emptyStage), 22.0f, palette.dim, true);
        return;
    }

    const int idx = currentIndex;
    g.setFont (uiFont (10.5f));
    g.drawText (juce::String (juce::jmax (0, idx + 1)) + " / " + juce::String (doc.size()),
                static_cast<int> (area.getRight() - 130.0f),
                static_cast<int> (area.getY() + 16.0f), 100, 16,
                juce::Justification::centredRight);

    const float widthScale = juce::jlimit (0.78f, 1.30f,
                                           static_cast<float> (area.getWidth() / 900.0))
                           * static_cast<float> (fontScale);

    const float prevSize = 17.0f * widthScale;
    const float mainSize = 38.0f * widthScale;
    const float nextSize = 17.0f * widthScale;
    const float transSize = 15.0f * widthScale;

    auto inner = area.reduced (36.0f, 34.0f);
    const float ih = inner.getHeight();

    auto prevArea = inner.withHeight (ih * 0.25f);
    auto currArea = inner.withY (inner.getY() + ih * 0.25f).withHeight (ih * 0.50f);
    auto nextArea = inner.withY (inner.getY() + ih * 0.75f).withHeight (ih * 0.25f);

    //---- Previous line ------------------------------------------------------
    juce::String prevText;
    if (idx > 0)
        prevText = doc.line (idx - 1).text;
    else if (idx == -1)
        prevText = tr (Lang::TextId::intro);

    drawText (g, prevArea, prevText, prevSize, palette.dim.withAlpha (0.55f), false);

    //---- Current line -------------------------------------------------------
    if (idx >= 0)
    {
        const auto& line = doc.line (idx);
        const int sung = doc.sungCharsAt (line, displayTime);
        const bool karaoke = line.hasKaraoke();

        auto mainArea  = currArea.withTrimmedBottom (transSize + 14.0f);
        auto transArea = currArea.withY (currArea.getBottom() - (transSize + 12.0f))
                                 .withHeight (transSize + 12.0f);

        const juce::Colour baseColour = karaoke ? palette.text : palette.accent;
        const float fade = 0.55f + 0.45f * transition;

        drawText (g, mainArea, line.text, mainSize, baseColour, true,
                  karaoke ? sung : -1, palette.accent, fade);

        if (line.translation.isNotEmpty())
            drawText (g, transArea, line.translation, transSize,
                      palette.dim.withAlpha (0.8f), false, -1, {}, fade);

    }
    else
    {
        drawText (g, currArea.reduced (0.0f, 20.0f),
                  juce::String (".  .  ."), mainSize * 0.8f, palette.faint, false);

        if (doc.size() > 0)
        {
            const double until = doc.line (0).time;
            drawText (g, currArea.withY (currArea.getBottom() - 26.0f).withHeight (24.0f),
                      tr (Lang::TextId::nextLineIn)
                      + juce::String (juce::jmax (0.0, until - displayTime), 1)
                      + tr (Lang::TextId::secondsSuffix),
                      12.0f, palette.faint, false);
        }
    }

    //---- Line progress: always drawn, even before the first lyric -----------
    const float barW = 180.0f * widthScale;
    auto bar = juce::Rectangle<float> (area.getCentreX() - barW * 0.5f,
                                       area.getBottom() - 20.0f, barW, 3.0f);
    g.setColour (palette.track);
    g.fillRoundedRectangle (bar, 2.0f);

    double frac = 0.0;
    if (idx >= 0)
    {
        const double start = doc.line (idx).time;
        const double end   = doc.lineEnd (idx);
        frac = juce::jlimit (0.0, 1.0,
                             (displayTime - start) / juce::jmax (0.01, end - start));
    }

    if (frac > 0.0)
    {
        g.setColour (palette.accent.withAlpha (0.9f));
        g.fillRoundedRectangle (bar.withWidth (barW * static_cast<float> (frac)), 2.0f);
    }

    //---- Next line ----------------------------------------------------------
    const int nextIdx = idx < 0 ? 0 : idx + 1;
    if (nextIdx < doc.size())
        drawText (g, nextArea, doc.line (nextIdx).text, nextSize,
                  palette.dim.withAlpha (0.45f), false);
}

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

    const double duration = processor.lyrics().totalDuration();
    const double frac = duration > 0.0
        ? juce::jlimit (0.0, 1.0, displayTime / duration) : 0.0;

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
                                int sungChars, juce::Colour sungColour, float alpha)
{
    if (text.isEmpty() || area.isEmpty())
        return;

    juce::AttributedString s;
    s.setText (text);
    s.setFont (uiFont (size, bold));
    s.setJustification (juce::Justification::centred);
    s.setWordWrap (juce::AttributedString::byChar);
    s.setColour (colour.withAlpha (alpha));

    if (sungChars > 0)
        s.setColour (juce::Range<int> (0, juce::jmin (sungChars, text.length())),
                     sungColour.withAlpha (alpha));

    const juce::Graphics::ScopedSaveState state (g);
    g.reduceClipRegion (area.toNearestInt());
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
    const double duration = doc.totalDuration();

    seekSlider.setRange (0.0, duration, 0.01);

    if (demoMode)
    {
        if (demoPlaying)
        {
            demoTime = juce::jlimit (0.0, duration, demoTime + dt);
            seekSlider.setValue (demoTime, juce::dontSendNotification);
        }
        displayTime = demoTime;
    }
    else
    {
        // The one line that matters: use the host's absolute project time.
        displayTime = processor.positionSeconds.load() + offsetMs / 1000.0;
        seekSlider.setValue (juce::jlimit (0.0, duration, displayTime),
                             juce::dontSendNotification);
    }

    const int idx = doc.findIndexAt (displayTime);
    if (idx != currentIndex)
    {
        currentIndex = idx;
        transition = 0.0f;
    }
    transition = juce::jmin (1.0f, transition + 0.14f);

    updateLabels (displayTime);
    repaint();
}

void LyricSyncEditor::updateLabels (double t)
{
    clockLabel .setText (timecodeText (t), juce::dontSendNotification);
    statusLabel.setText (statusText(), juce::dontSendNotification);
    offsetLabel.setText (juce::String (offsetMs, 0) + " ms", juce::dontSendNotification);
    metaLabel  .setText (barBeatText(), juce::dontSendNotification);

    auto label = fileDescription;
    if (lastWarning.isNotEmpty())
        label += tr (Lang::TextId::warningPrefix) + lastWarning;
    fileLabel.setText (label, juce::dontSendNotification);
}

juce::String LyricSyncEditor::timecodeText (double t) const
{
    return formatTimecode (t);
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

    if (button == &demoClockToggle)
    {
        demoMode = demoClockToggle.getToggleState();
        processor.setSetting (Settings::demoClock, demoMode);
        demoPlaying = false;
        demoTime = demoMode ? juce::jlimit (0.0, processor.lyrics().totalDuration(),
                                            processor.positionSeconds.load())
                            : 0.0;
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
    demoTime = 0.0;
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
    demoTime = 0.0;
    repaint();
}

void LyricSyncEditor::enterEditMode()
{
    editing = true;
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
