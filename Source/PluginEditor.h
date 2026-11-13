#pragma once

#include "PluginProcessor.h"
#include "UiPalette.h"
#include "I18n.h"

#include <juce_gui_basics/juce_gui_basics.h>

//==============================================================================
/**
 *  Plugin UI. All localised text is supplied by Lang::tr() in applyUiText(),
 *  so the constructor never embeds non-ASCII literals in a juce::String.
 *
 *  The editor never advances time itself: every frame reads the host's
 *  absolute project time and binary-searches the lyrics document.
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

    bool isInterestedInFileDrag (const juce::StringArray& files) override;
    void filesDropped (const juce::StringArray& files, int x, int y) override;

private:
    void timerCallback() override;
    void buttonClicked (juce::Button* button) override;
    void sliderValueChanged (juce::Slider* slider) override;
    void comboBoxChanged (juce::ComboBox* box) override;

    //---- Painting -----------------------------------------------------------
    void paintHeader (juce::Graphics& g, juce::Rectangle<float> area);
    void paintStage  (juce::Graphics& g, juce::Rectangle<float> area);
    void paintFooter (juce::Graphics& g, juce::Rectangle<float> area);
    void drawText    (juce::Graphics& g, juce::Rectangle<float> area,
                      const juce::String& text, float size, juce::Colour colour,
                      bool bold, int sungChars = -1,
                      juce::Colour sungColour = {}, float alpha = 1.0f);

    //---- Interaction --------------------------------------------------------
    void openFileDialog();
    void loadLyricsFile (const juce::File& file);
    void loadLyricsText (const juce::String& text, const juce::String& label,
                         bool isDemo = false);
    void enterEditMode();
    void leaveEditMode (bool apply);
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

    juce::String tr (Lang::TextId id) const { return Lang::tr (id, language); }
    juce::String sep() const                { return tr (Lang::TextId::separator); }

    //---- Data ---------------------------------------------------------------
    LyricSyncProcessor& processor;

    UiPalette palette;
    juce::LookAndFeel_V4 lookAndFeel;

    double displayTime   = 0.0;
    double offsetMs      = 0.0;
    double fontScale     = 1.0;
    int    themeIndex    = 0;
    int    language      = 0;              // 0 = Chinese, 1 = English
    int    currentIndex  = -2;
    float  transition    = 1.0f;

    bool   editing         = false;
    bool   demoMode        = false;
    bool   demoPlaying     = false;
    bool   usingDemoLyrics = true;
    bool   updatingLanguageBox = false;
    double demoTime        = 0.0;
    double lastTickMs      = 0.0;

    juce::uint32 lastTimeUpdates = 0;
    double lastHostUpdateMs      = 0.0;
    juce::String lastWarning;
    juce::String fileDescription;

    //---- Widgets. Texts are assigned in applyUiText(). ----------------------
    juce::TextButton  loadButton;
    juce::TextButton  pasteButton;
    juce::TextButton  demoButton;
    juce::TextButton  themeButton;
    juce::ComboBox    languageBox;        // 中文 / English
    juce::TextButton  minusButton;
    juce::TextButton  plusButton       { "+" };
    juce::TextButton  resetButton;
    juce::TextButton  demoPlayButton;
    juce::TextButton  demoStopButton;
    juce::TextButton  applyTextButton;
    juce::TextButton  cancelTextButton;

    juce::ToggleButton demoClockToggle;

    juce::Slider seekSlider  { juce::Slider::LinearHorizontal, juce::Slider::NoTextBox };
    juce::Slider fontSlider  { juce::Slider::LinearHorizontal, juce::Slider::NoTextBox };

    juce::Label clockLabel, statusLabel, offsetLabel, fileLabel, metaLabel, hintLabel, fontLabel;
    juce::TextEditor editorBox;

    std::unique_ptr<juce::FileChooser> chooser;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (LyricSyncEditor)
};
