#pragma once

#include "PluginProcessor.h"
#include "UiPalette.h"
#include "I18n.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <vector>

//==============================================================================
/**
 *  LookAndFeel that keeps EVERY widget on the same Chinese-capable typeface and
 *  scales its font together with the UI. Without this, TextButton / ComboBox
 *  fall back to JUCE's default sans-serif and Chinese labels render as tofu.
 */
class ScaledLookAndFeel : public juce::LookAndFeel_V4
{
public:
    float scale = 1.0f;

    juce::Font getTextButtonFont (juce::TextButton&, int buttonHeight) override;
    juce::Font getComboBoxFont   (juce::ComboBox&) override;
    juce::Font getLabelFont      (juce::Label&) override;
    juce::Font getPopupMenuFont  () override;
    juce::Font getSliderPopupFont (juce::Slider&) override;
};

//==============================================================================
/**
 *  Plugin UI. All localised text is supplied by Lang::tr() in applyUiText(),
 *  so the constructor never embeds non-ASCII literals in a juce::String.
 *
 *  Layout uses a fixed 960x600 logical canvas. Everything (painting via an
 *  AffineTransform, widgets via scaledRect(), fonts via ScaledLookAndFeel) is
 *  scaled by one single factor, so resizing can never clip the progress bar or
 *  push controls out of view. The editor also asks the host to keep the 8:5
 *  aspect ratio; if a host forces another ratio, the canvas is centred instead.
 *
 *  The editor never advances time itself: every frame reads the host's
 *  absolute project time and binary-searches the lyrics document.
 *
 *  Three display modes:
 *    · normal      – karaoke stack (current line enlarged, neighbours faded)
 *    · editing     – text editor over the stage
 *    · tap timing  – play the song, click / press Space to stamp each line
 */
class LyricSyncEditor : public juce::AudioProcessorEditor,
                        private juce::Timer,
                        public  juce::FileDragAndDropTarget,
                        private juce::Button::Listener,
                        private juce::Slider::Listener,
                        private juce::ComboBox::Listener
{
public:
    explicit LyricSyncEditor (LyricSyncProcessor& owner);
    ~LyricSyncEditor() override;

    void paint (juce::Graphics& g) override;
    void resized() override;

    void mouseDown (const juce::MouseEvent& e) override;
    bool keyPressed (const juce::KeyPress& key) override;

    bool isInterestedInFileDrag (const juce::StringArray& files) override;
    void filesDropped (const juce::StringArray& files, int x, int y) override;

private:
    static constexpr float baseW = 960.0f;
    static constexpr float baseH = 600.0f;

    void timerCallback() override;
    void buttonClicked (juce::Button* button) override;
    void sliderValueChanged (juce::Slider* slider) override;
    void comboBoxChanged (juce::ComboBox* box) override;

    //---- Painting -----------------------------------------------------------
    void paintHeader (juce::Graphics& g, juce::Rectangle<float> area);
    void paintStage  (juce::Graphics& g, juce::Rectangle<float> area);
    void paintTapStage (juce::Graphics& g, juce::Rectangle<float> area);
    void paintFooter (juce::Graphics& g, juce::Rectangle<float> area);
    void drawText    (juce::Graphics& g, juce::Rectangle<float> area,
                      const juce::String& text, float size, juce::Colour colour,
                      bool bold, int sungChars = -1,
                      juce::Colour sungColour = {}, float alpha = 1.0f,
                      juce::Justification just = juce::Justification::centred);

    //---- Scaled layout ------------------------------------------------------
    juce::Rectangle<int> scaledRect (float x, float y, float w, float h) const;
    void updateScaledFonts();

    /** 进度条使用的时间范围：优先用实测音频长度，测不到时退回歌词时长。 */
    juce::Range<double> progressRange() const;

    //---- Interaction --------------------------------------------------------
    void openFileDialog();
    void loadLyricsFile (const juce::File& file);
    void loadLyricsText (const juce::String& text, const juce::String& label,
                         bool isDemo = false);
    void enterEditMode();
    void leaveEditMode (bool apply);

    void enterTapMode();
    void stampTap();
    void undoStamp();
    void finishTapMode();
    void exitTapMode();

    void resetAudioMeasurement();
    void setTheme (int index);
    void setLanguage (int next);
    void applyUiText (bool reloadDemoLyrics);
    void refreshLookAndFeel();
    void syncControlsFromSettings();
    void updateEditingVisibility();
    void updateLabels (double t);

    juce::String statusText() const;
    juce::String timecodeText (double t) const;
    juce::String barBeatText() const;
    juce::String audioRangeText() const;

    juce::String tr (Lang::TextId id) const { return Lang::tr (id, language); }
    juce::String sep() const                { return tr (Lang::TextId::separator); }

    //---- Data ---------------------------------------------------------------
    LyricSyncProcessor& processor;

    UiPalette palette;
    ScaledLookAndFeel lookAndFeel;

    float uiScale  = 1.0f;
    float originX  = 0.0f;
    float originY  = 0.0f;
    juce::AffineTransform uiTransform;

    double displayTime   = 0.0;
    double offsetMs      = 0.0;
    double fontScale     = 1.0;
    int    themeIndex    = 0;
    int    language      = 0;              // 0 = Chinese, 1 = English
    int    currentIndex  = -2;
    float  transition    = 1.0f;
    float  displayIndex  = -1.0f;          // 平滑滚动用的连续行号

    bool   editing         = false;
    bool   demoMode        = false;
    bool   demoPlaying     = false;
    bool   usingDemoLyrics = true;
    bool   updatingLanguageBox = false;
    double demoTime        = 0.0;
    double lastTickMs      = 0.0;

    //---- Tap timing ---------------------------------------------------------
    bool tapMode = false;
    std::vector<juce::String> tapLines;
    std::vector<double>       tapTimes;
    int  tapIndex = 0;
    juce::String tapOriginalText;

    //---- Audio length persistence ------------------------------------------
    double lastAudioSaveMs = 0.0;
    double savedAudioStart = -1.0;
    double savedAudioEnd   = -1.0;

    juce::uint32 lastTimeUpdates = 0;
    double lastHostUpdateMs      = 0.0;
    juce::String lastWarning;
    juce::String fileDescription;

    //---- Widgets. Texts are assigned in applyUiText(). ----------------------
    juce::TextButton  loadButton;
    juce::TextButton  pasteButton;
    juce::TextButton  demoButton;
    juce::TextButton  themeButton;
    juce::ComboBox    languageBox;        // Chinese / English
    juce::TextButton  minusButton;
    juce::TextButton  plusButton       { "+" };
    juce::TextButton  resetButton;
    juce::TextButton  demoPlayButton;
    juce::TextButton  demoStopButton;
    juce::TextButton  applyTextButton;
    juce::TextButton  cancelTextButton;

    // Tap timing buttons
    juce::TextButton  tapStartButton;
    juce::TextButton  tapUndoButton;
    juce::TextButton  tapFinishButton;
    juce::TextButton  tapExitButton;

    juce::TextButton  measureButton;      // 重测音频

    // A toggled TextButton (rather than ToggleButton) so the Chinese label
    // also goes through getTextButtonFont() and never renders as tofu.
    juce::TextButton demoClockToggle;

    juce::Slider seekSlider  { juce::Slider::LinearHorizontal, juce::Slider::NoTextBox };
    juce::Slider fontSlider  { juce::Slider::LinearHorizontal, juce::Slider::NoTextBox };

    juce::Label clockLabel, statusLabel, offsetLabel, fileLabel, metaLabel,
                hintLabel, fontLabel, audioLabel;
    juce::TextEditor editorBox;

    // Hover explanations ("按键旁边出现的中文说明框"). Created at the very end
    // of the constructor so it never touches a half-built editor.
    std::unique_ptr<juce::TooltipWindow> tooltipWindow;

    std::unique_ptr<juce::FileChooser> chooser;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (LyricSyncEditor)
};
