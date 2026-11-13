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

    /** 国内很多 LRC 是 GBK 编码，这里做 UTF-8 / UTF-16 / GB18030 兼容读取。 */
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
LyricSyncEditor::LyricSyncEditor (LyricSyncProcessor& owner)
    : juce::AudioProcessorEditor (owner),
      processor (owner),
      palette (UiPalette::dark())
{
    setLookAndFeel (&lookAndFeel);
    setOpaque (true);

    syncControlsFromSettings();

    setResizable (true, true);
    setResizeLimits (720, 460, 2200, 1400);
    setSize (juce::jlimit (720, 2200, static_cast<int> (processor.getSetting (Settings::windowW, 960))),
             juce::jlimit (460, 1400, static_cast<int> (processor.getSetting (Settings::windowH, 600))));

    //--------------------------------------------------------------------------
    for (auto* b : { &loadButton, &pasteButton, &demoButton, &themeButton,
                     &minusButton, &plusButton, &resetButton,
                     &demoPlayButton, &demoStopButton,
                     &applyTextButton, &cancelTextButton })
    {
        b->addListener (this);
        addAndMakeVisible (*b);
    }

    demoClockToggle.addListener (this);
    addAndMakeVisible (demoClockToggle);

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
    hintLabel.setText ("提示：歌词跟随宿主播放头 · 任意跳转 / 循环 / 倒退都会立即对齐 · 支持拖入 .lrc / .srt / .txt",
                       juce::dontSendNotification);
    addAndMakeVisible (hintLabel);

    fontLabel.setFont (uiFont (11.0f));
    fontLabel.setJustificationType (juce::Justification::centredLeft);
    fontLabel.setText ("字号", juce::dontSendNotification);
    addAndMakeVisible (fontLabel);

    editorBox.setMultiLine (true, true);
    editorBox.setReturnKeyStartsNewLine (true);
    editorBox.setScrollbarsShown (true);
    editorBox.setFont (uiFont (14.0f));
    editorBox.setPopupMenuEnabled (true);
    addChildComponent (editorBox);      // 默认隐藏

    fileDescription = "示例歌词  ·  拖入文件即可替换";

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

    palette = UiPalette::fromIndex (themeIndex);
    fontSlider.setValue (fontScale, juce::dontSendNotification);
    demoClockToggle.setToggleState (demoMode, juce::dontSendNotification);
    themeButton.setButtonText (themeIndex == 1 ? "深色" : "浅色");
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

    l.setColour (juce::ToggleButton::textColourId,          palette.dim);
    l.setColour (juce::ToggleButton::tickColourId,          palette.accent);
    l.setColour (juce::ToggleButton::tickDisabledColourId,  palette.faint);

    clockLabel.setColour (juce::Label::textColourId, palette.text);
    statusLabel.setColour (juce::Label::textColourId, palette.dim);
    offsetLabel.setColour (juce::Label::textColourId, palette.accent);
    offsetLabel.setColour (juce::Label::backgroundColourId, palette.card);
    hintLabel.setColour (juce::Label::textColourId, palette.faint);
    fileLabel.setColour (juce::Label::textColourId, palette.dim);
    metaLabel.setColour (juce::Label::textColourId, palette.faint);
    fontLabel.setColour (juce::Label::textColourId, palette.faint);

    editorBox.applyFontToAllText (uiFont (14.0f));
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
void LyricSyncEditor::resized()
{
    const int w = getWidth();
    const int h = getHeight();
    constexpr int pad = 22;

    clockLabel .setBounds (w - 470, 8, 280, 30);
    statusLabel.setBounds (w - 470, 37, 280, 18);
    themeButton.setBounds (w - 104, 18, 80, 28);

    const juce::Rectangle<int> stage (pad, 72, w - pad * 2, h - 72 - 152);
    editorBox.setBounds (stage.reduced (2));

    int y = h - 146;
    seekSlider.setBounds (pad, y, w - pad * 2, 22);
    y += 28;

    int x = pad;
    auto place = [&] (juce::Component& c, int width, int height = 30)
    {
        c.setBounds (x, y, width, height);
        x += width + 8;
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
        x += 14;
        place (minusButton, 30);
        place (offsetLabel, 84);
        place (plusButton, 30);
        place (resetButton, 50);
        x += 14;
        place (fontLabel, 34);
        fontSlider.setBounds (x, y + 4, 110, 22);
        x += 118;
        demoClockToggle.setBounds (x, y + 2, 100, 26);
        x += 108;
        demoPlayButton.setBounds (x, y, 34, 30);
        demoStopButton.setBounds (x + 40, y, 34, 30);
    }

    y += 36;
    fileLabel.setBounds (pad, y, w / 2 - pad, 22);
    metaLabel.setBounds (w / 2, y, w / 2 - pad, 22);

    y += 24;
    hintLabel.setBounds (pad, y, w - pad * 2, 20);

    processor.setSetting (Settings::windowW, w);
    processor.setSetting (Settings::windowH, h);
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

    g.setColour (palette.accent.withAlpha (0.045f));
    g.fillEllipse (w - 230.0f, -190.0f, 460.0f, 460.0f);

    paintHeader (g, juce::Rectangle<float> (0.0f, 0.0f, w, 64.0f));
    paintStage  (g, juce::Rectangle<float> (22.0f, 72.0f, w - 44.0f, h - 224.0f));
    paintFooter (g, juce::Rectangle<float> (0.0f, h - 152.0f, w, 152.0f));
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
              "DAW 歌词同步  ·  跟随宿主播放头", 11.0f, palette.faint, false);

    // 状态灯
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
    g.drawText ("CURRENT LINE", static_cast<int> (area.getX() + 30.0f),
                static_cast<int> (area.getY() + 16.0f), 160, 16,
                juce::Justification::centredLeft);

    if (doc.isEmpty())
    {
        drawText (g, area.reduced (40.0f, 70.0f),
                  "把 .lrc / .srt / .txt 拖进窗口\n或点击左下角「载入歌词」",
                  22.0f, palette.dim, true);
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

    //---- 上一句 -------------------------------------------------------------
    juce::String prevText;
    if (idx > 0)
        prevText = doc.line (idx - 1).text;
    else if (idx == -1)
        prevText = "♪  前奏";

    drawText (g, prevArea, prevText, prevSize, palette.dim.withAlpha (0.55f), false);

    //---- 当前句 -------------------------------------------------------------
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

        // 句内进度
        const double start = line.time;
        const double end   = doc.lineEnd (idx);
        const double frac  = juce::jlimit (0.0, 1.0,
                                          (displayTime - start) / juce::jmax (0.01, end - start));

        const float barW = 180.0f * widthScale;
        auto bar = juce::Rectangle<float> (area.getCentreX() - barW * 0.5f,
                                           area.getBottom() - 20.0f, barW, 3.0f);
        g.setColour (palette.track);
        g.fillRoundedRectangle (bar, 2.0f);
        g.setColour (palette.accent.withAlpha (0.9f));
        g.fillRoundedRectangle (bar.withWidth (barW * static_cast<float> (frac)), 2.0f);
    }
    else
    {
        drawText (g, currArea.reduced (0.0f, 20.0f),
                  "·  ·  ·", mainSize * 0.8f, palette.faint, false);

        if (doc.size() > 0)
        {
            const double until = doc.line (0).time;
            drawText (g, currArea.withY (currArea.getBottom() - 26.0f).withHeight (24.0f),
                      juce::String ("下一句还有 ")
                      + juce::String (juce::jmax (0.0, until - displayTime), 1)
                      + juce::String (" 秒"),
                      12.0f, palette.faint, false);
        }
    }

    //---- 下一句 -------------------------------------------------------------
    const int nextIdx = idx < 0 ? 0 : idx + 1;
    if (nextIdx < doc.size())
        drawText (g, nextArea, doc.line (nextIdx).text, nextSize,
                  palette.dim.withAlpha (0.45f), false);

    // 解析警告显示在底部文件信息栏，避免遮挡歌词
}

void LyricSyncEditor::paintFooter (juce::Graphics& g, juce::Rectangle<float> area)
{
    g.setColour (palette.panelEdge.withAlpha (0.28f));
    g.drawLine (22.0f, area.getY() + 27.0f, area.getRight() - 22.0f,
                area.getY() + 27.0f, 1.0f);
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
        processor.reloadFromSettings();
        syncControlsFromSettings();
        refreshLookAndFeel();
        fileDescription = "宿主工程中保存的歌词";
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
        // 关键一行：直接用宿主的绝对项目时间，不做任何累加。
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
        label += juce::String ("    ⚠ ") + lastWarning;
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

    return "小节 " + juce::String (bar) + "."
         + juce::String (static_cast<int> (std::floor (beat))) + "."
         + juce::String::formatted ("%03d", tick)
         + "    ·    " + juce::String (processor.bpm.load(), 1)
         + " BPM    ·    " + juce::String (num) + "/" + juce::String (den);
}

juce::String LyricSyncEditor::statusText() const
{
    if (demoMode)
        return "手动预览  ·  拖动进度条定位";

    if (! processor.hasHostTime.load())
        return "等待宿主时间码…（可勾选手动预览）";

    juce::String s;
    if (processor.isRecording.load())    s = "录音中";
    else if (processor.isPlaying.load()) s = "播放中";
    else                                 s = "已停止";

    if (processor.isLooping.load())
        s += "  ·  循环";

    if (lastHostUpdateMs > 0.0
        && juce::Time::getMillisecondCounterHiRes() - lastHostUpdateMs > 1800.0)
        s += "  ·  宿主暂未推送时间码";

    return s;
}

//==============================================================================
void LyricSyncEditor::buttonClicked (juce::Button* button)
{
    if (button == &loadButton)          { openFileDialog(); return; }
    if (button == &pasteButton)         { enterEditMode(); return; }
    if (button == &demoButton)          { loadLyricsText (demoLyricsText(), "示例歌词"); return; }
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
        "选择歌词文件",
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
        fileDescription = "读取失败：" + file.getFileName();
        lastWarning = "文件为空或无法读取（支持 UTF-8 / UTF-16 / GBK）";
        repaint();
        return;
    }

    const auto info = processor.setLyricsText (text, file.getFullPathName());
    offsetMs = 0.0;
    processor.setSetting (Settings::offsetMs, 0.0);
    processor.setSetting (Settings::lyricsPath, file.getFullPathName());

    fileDescription = file.getFileName() + "  ·  " + info.formatName
                    + "  ·  " + juce::String (info.lineCount) + " 句";
    if (info.karaokeLines > 0)
        fileDescription += "  ·  逐字";

    lastWarning = info.warning;
    currentIndex = -2;
    demoTime = 0.0;
    refreshLookAndFeel();
    repaint();
}

void LyricSyncEditor::loadLyricsText (const juce::String& text, const juce::String& label)
{
    const auto info = processor.setLyricsText (text);
    offsetMs = 0.0;
    processor.setSetting (Settings::offsetMs, 0.0);
    processor.setSetting (Settings::lyricsPath, "");

    fileDescription = label + "  ·  " + info.formatName
                    + "  ·  " + juce::String (info.lineCount) + " 句";
    lastWarning = info.warning;
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
        loadLyricsText (editorBox.getText(), "粘贴的歌词文本");

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
    themeButton.setButtonText (index == 1 ? "深色" : "浅色");
    refreshLookAndFeel();
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
