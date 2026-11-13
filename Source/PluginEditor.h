#pragma once

#include "PluginProcessor.h"
#include "UiPalette.h"

#include <juce_gui_basics/juce_gui_basics.h>

//==============================================================================
/**
 *  插件界面。
 *
 *  界面自己不推进时间：每帧只读取处理器从宿主播放头拿到的绝对时间，
 *  再对歌词文档做一次二分查找，所以随机拖动播放头与连续播放走的是
 *  完全相同的代码路径。
 */
class LyricSyncEditor : public juce::AudioProcessorEditor,
                        private juce::Timer,
                        public  juce::FileDragAndDropTarget,
                        private juce::Button::Listener,
                        private juce::Slider::Listener
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

    //---- 绘制 ---------------------------------------------------------------
    void paintHeader (juce::Graphics& g, juce::Rectangle<float> area);
    void paintStage  (juce::Graphics& g, juce::Rectangle<float> area);
    void paintFooter (juce::Graphics& g, juce::Rectangle<float> area);
    void drawText    (juce::Graphics& g, juce::Rectangle<float> area,
                      const juce::String& text, float size, juce::Colour colour,
                      bool bold, int sungChars = -1,
                      juce::Colour sungColour = {}, float alpha = 1.0f);

    //---- 交互 ---------------------------------------------------------------
    void openFileDialog();
    void loadLyricsFile (const juce::File& file);
    void loadLyricsText (const juce::String& text, const juce::String& label);
    void enterEditMode();
    void leaveEditMode (bool apply);
    void setTheme (int index);
    void refreshLookAndFeel();
    void syncControlsFromSettings();
    void updateEditingVisibility();
    void updateLabels (double t);

    juce::String statusText() const;
    juce::String timecodeText (double t) const;
    juce::String barBeatText() const;

    //---- 数据 ---------------------------------------------------------------
    LyricSyncProcessor& processor;

    UiPalette palette;
    juce::LookAndFeel_V4 lookAndFeel;

    double displayTime   = 0.0;
    double offsetMs      = 0.0;
    double fontScale     = 1.0;
    int    themeIndex    = 0;
    int    currentIndex  = -2;
    float  transition    = 1.0f;

    bool   editing       = false;
    bool   demoMode      = false;
    bool   demoPlaying   = false;
    double demoTime      = 0.0;
    double lastTickMs    = 0.0;

    juce::uint32 lastTimeUpdates = 0;
    double lastHostUpdateMs      = 0.0;
    juce::String lastWarning;
    juce::String fileDescription;

    //---- 控件 ---------------------------------------------------------------
    juce::TextButton  loadButton       { "载入歌词" };
    juce::TextButton  pasteButton      { "粘贴 / 编辑" };
    juce::TextButton  demoButton       { "示例歌词" };
    juce::TextButton  themeButton      { "浅色" };
    juce::TextButton  minusButton      { "−" };
    juce::TextButton  plusButton       { "+" };
    juce::TextButton  resetButton      { "归零" };
    juce::TextButton  demoPlayButton   { "▶" };
    juce::TextButton  demoStopButton   { "■" };
    juce::TextButton  applyTextButton  { "应用歌词" };
    juce::TextButton  cancelTextButton { "取消" };

    juce::ToggleButton demoClockToggle { "手动预览" };

    juce::Slider seekSlider  { juce::Slider::LinearHorizontal, juce::Slider::NoTextBox };
    juce::Slider fontSlider  { juce::Slider::LinearHorizontal, juce::Slider::NoTextBox };

    juce::Label clockLabel, statusLabel, offsetLabel, fileLabel, metaLabel, hintLabel, fontLabel;
    juce::TextEditor editorBox;

    std::unique_ptr<juce::FileChooser> chooser;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (LyricSyncEditor)
};
