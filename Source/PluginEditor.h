#pragma once

#include "PluginProcessor.h"
#include "KrcLyrics.h"
#include "WordLyrics.h"
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
    juce::String typeface { "Microsoft YaHei UI" };

    juce::Font getTextButtonFont (juce::TextButton&, int buttonHeight) override;
    juce::Font getComboBoxFont   (juce::ComboBox&) override;
    juce::Font getLabelFont      (juce::Label&) override;
    juce::Font getPopupMenuFont  () override;
    juce::Font getSliderPopupFont (juce::Slider&) override;

    /** 极夜玻璃：圆角半透明按钮 + 1px 高光描边。 */
    void drawButtonBackground (juce::Graphics&, juce::Button&,
                               const juce::Colour& backgroundColour,
                               bool shouldDrawButtonAsHighlighted,
                               bool shouldDrawButtonAsDown) override;

    void drawComboBox (juce::Graphics&, int width, int height, bool isButtonDown,
                       int buttonX, int buttonY, int buttonW, int buttonH,
                       juce::ComboBox&) override;
};

//==============================================================================
/** 无文字的图标按钮（齿轮 / 关闭），用 Path 画，不依赖字体里有没有该字形。 */
class IconButton : public juce::Button
{
public:
    enum class Icon { gear, close, none };

    explicit IconButton (Icon iconToDraw) : juce::Button (juce::String()), icon (iconToDraw) {}

    void paintButton (juce::Graphics& g, bool over, bool down) override;

    juce::Colour normalColour { juce::Colours::white };
    juce::Colour hoverColour  { juce::Colours::white };

private:
    Icon icon;
};

//==============================================================================
/**
 *  Plugin UI.
 *
 *  · 两套逻辑画布（横版 960×600 / 竖版 600×900），所有控件按其中的一套排布，
 *    再统一套一个 AffineTransform，所以缩放、换版型都不会裁切。
 *  · 窗口比例 50/75/100/125/150/200% 由齿轮设置面板控制，随工程保存。
 *  · 设置面板与关于面板都画在逻辑画布内，跟着一起缩放。
 */
class LyricSyncEditor : public juce::AudioProcessorEditor,
                        private juce::Timer,
                        public  juce::MouseListener,          // 接住提示词的点击
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
    void mouseDrag (const juce::MouseEvent& e) override;
    void mouseUp   (const juce::MouseEvent& e) override;
    void mouseMove (const juce::MouseEvent& e) override;
    void mouseDoubleClick (const juce::MouseEvent& e) override;
    void mouseWheelMove (const juce::MouseEvent& e,
                         const juce::MouseWheelDetails& wheel) override;
    bool keyPressed (const juce::KeyPress& key) override;

    bool isInterestedInFileDrag (const juce::StringArray& files) override;
    void filesDropped (const juce::StringArray& files, int x, int y) override;

private:
    enum class Orientation { horizontal, vertical };

    static constexpr float hBaseW = 960.0f, hBaseH = 600.0f;
    static constexpr float vBaseW = 600.0f, vBaseH = 900.0f;

    float baseW() const
    {
        if (zenMode)
            return 960.0f;

        if (orientation != Orientation::vertical)
            return hBaseW;

        // 竖版：窗口被拉宽时画布跟着变宽（歌词卡片变宽），高度基准不变。
        // 以前固定 600 宽，窗口一宽就缩成中间一条窄卡，字也变小。
        const float w = juce::jmax (1.0f, static_cast<float> (getWidth()));
        const float h = juce::jmax (1.0f, static_cast<float> (getHeight()));
        const float fromAspect = vBaseH * (w / h);

        return juce::jlimit (vBaseW, vBaseW * 2.6f, fromAspect);
    }

    /** 纯享模式下画布比例跟窗口一致，缩放才会均匀、铺满不留黑边。 */
    float baseH() const
    {
        if (zenMode)
        {
            const float w = juce::jmax (1.0f, static_cast<float> (getWidth()));
            const float h = juce::jmax (1.0f, static_cast<float> (getHeight()));
            return 960.0f * h / w;
        }

        return orientation == Orientation::vertical ? vBaseH : hBaseH;
    }

    /** 竖版画布加宽后，原本按 600 宽排的控件需要整体右移居中。 */
    float vShift() const
    {
        return orientation == Orientation::vertical ? (baseW() - vBaseW) * 0.5f : 0.0f;
    }

    void timerCallback() override;
    void buttonClicked (juce::Button* button) override;
    void sliderValueChanged (juce::Slider* slider) override;
    void comboBoxChanged (juce::ComboBox* box) override;

    //---- Painting -----------------------------------------------------------
    void paintHeader (juce::Graphics& g);
    void paintStage  (juce::Graphics& g, juce::Rectangle<float> area);
    void paintTapStage (juce::Graphics& g, juce::Rectangle<float> area);
    void paintSettings (juce::Graphics& g, juce::Rectangle<float> panel);
    void paintAbout    (juce::Graphics& g, juce::Rectangle<float> panel);
    void paintInfoLine (juce::Graphics& g);
    void drawText    (juce::Graphics& g, juce::Rectangle<float> area,
                      const juce::String& text, float size, juce::Colour colour,
                      bool bold, float sungProgress = -1.0f,
                      juce::Colour sungColour = {}, float alpha = 1.0f,
                      juce::Justification just = juce::Justification::centred);

    //---- Layout -------------------------------------------------------------
    juce::Rectangle<int> scaledRect (float x, float y, float w, float h) const;
    juce::Rectangle<float> stageArea() const;
    juce::Rectangle<float> settingsPanelArea() const;
    juce::Rectangle<float> aboutPanelArea() const;
    void updateScaledFonts();
    void applyWindowSize();

    /** 进度条已去掉，这里只保留"音频长度"读数所需的范围。 */
    juce::Range<double> audioRange() const;

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
    void restartTapLine (int index);      // 从这里重打：清掉该句及之后的时间
    void nudgeTap (int deltaMs);          // 微调刚打好的那一句
    void toggleTapPause();                // 手动预览下暂停/继续
    void toggleHostTransport();            // 给宿主发空格，开始/暂停播放
    void editTapRow (int index);           // 打开某一行的编辑
    void applyTapRow();                    // 保存这一行的文字与时间
    void restampTapRow();                  // 把这一行的时间改成当前播放位置
    void closeTapRowEditing();             // 取消编辑
    double parseTimecode (const juce::String& text) const;
    std::vector<juce::Rectangle<float>> tapListRows (juce::Rectangle<float> area) const;
    std::vector<int> tapListIndices() const;      // 已记录行的行号（与 tapListRows 一一对应）
    void showEditorMenu (juce::TextEditor& te);   // 右键菜单（跟随中英文）
    void finishTapMode();
    void exitTapMode();

    void resetAudioMeasurement();
    void exportLyrics();
    void toggleZenMode();
    void setSkin (int index);
    void setLanguage (int next);
    void setOrientation (Orientation next);
    void setZoomPercent (int percent);
    void setFontIndex (int index);
    void setLyricColour (int index);
    void pickBackgroundImage();
    void clearBackgroundImage();

    void showSettings (bool shouldShow);
    void showAbout (bool shouldShow);
    void applyUiText (bool reloadDemoLyrics);
    void refreshLookAndFeel();
    void syncControlsFromSettings();
    void updateVisibility();
    void updateLabels (double t);

    void saveUiPrefs();
    void loadUiPrefs();
    void archiveCurrentLyrics();

    juce::String statusText() const;
    juce::String timecodeText (double t) const;
    juce::String barBeatText() const;
    juce::String audioRangeText() const;
    juce::String hintText() const;
    void updateHint (bool force);

    juce::Font uiFont (float size, bool bold = false) const;

    /** 界面控件（LookAndFeel）用的字体名：永远挑一个含汉字的字体。 */
    juce::String uiTypefaceName() const;

    /** 跳到下一条左下角提示。 */
    void nextHint();
    juce::Colour lyricColour() const;

    juce::String tr (Lang::TextId id) const { return Lang::tr (id, language); }
    juce::String sep() const                { return tr (Lang::TextId::separator); }

    //---- Data ---------------------------------------------------------------
    LyricSyncProcessor& processor;

    UiPalette palette;
    ScaledLookAndFeel lookAndFeel;

    Orientation orientation = Orientation::horizontal;
    int zoomPercent = 100;           // 50 / 75 / 100 / 125 / 150 / 200

    float smoothedSung = -1.0f;         // 逐字扫光的平滑值（避免时间码分块造成的微跳）
    float uiScale  = 1.0f;
    float uiScaleX = 1.0f;          // 允许横向/纵向独立伸缩
    float uiScaleY = 1.0f;
    float originX  = 0.0f;
    float originY  = 0.0f;
    juce::AffineTransform uiTransform;

    double displayTime   = 0.0;
    double offsetMs      = 0.0;
    double fontScale     = 1.0;
    int    skinIndex     = 0;
    int    language      = 0;              // 0 = Chinese, 1 = English
    int    fontIndex     = 0;
    int    colourIndex   = 0;
    int    advanceMode   = 0;              // 0 = 逐句跳动, 1 = 逐字跳动
    int    currentIndex  = -2;
    int    previousIndex = -2;             // 换句时做颜色渐变
    float  transition    = 1.0f;
    float  displayIndex  = -1.0f;

    bool   editing         = false;
    bool   demoMode        = false;
    bool   demoPlaying     = false;
    bool   usingDemoLyrics = true;
    bool   settingsOpen    = false;
    bool   aboutOpen       = false;
    bool   updatingCombos  = false;
    bool   showHints       = true;         // 左下角轮播提示开关
    int    hintIndex       = 0;
    double hintRotateMs    = 0.0;
    bool   zenMode         = false;        // 纯享全屏（只显示歌词）
    int    sizeBeforeZenW  = 960;
    int    sizeBeforeZenH  = 600;
    double tempoAnalyseMs  = 0.0;
    double wheelSaveMs     = 0.0;      // Ctrl+滚轮改字号时限制偏好写入频率
    double detectedBpm     = 0.0;
    bool   previewDragging = false;        // 在歌词区上下拖动预览
    double previewOffset   = 0.0;
    double previewDragStartOffset = 0.0;
    int    previewDragStartY = 0;
    double demoTime        = 0.0;
    double lastTickMs      = 0.0;

    //---- Tap timing ---------------------------------------------------------
    bool tapMode = false;
    std::vector<juce::String> tapLines;
    std::vector<double>       tapTimes;
    int  tapIndex = 0;
    int  tapEditIndex = -1;               // >=0 表示正在编辑某一行

    //---- Audio length -------------------------------------------------------
    double lastAudioSaveMs = 0.0;
    double savedAudioStart = -1.0;
    double savedAudioEnd   = -1.0;
    double audioTotalSeconds = -1.0;

    juce::uint32 lastTimeUpdates = 0;
    double lastHostUpdateMs      = 0.0;
    juce::String lastWarning;
    juce::String fileDescription;
    juce::String archivedPath;
    juce::String editShownText;            // 编辑框里显示的（去掉 <...> 的干净文本）
    juce::String editOriginalText;         // 原始歌词（未改动时保留逐字锚点）

    juce::Image customBg;
    juce::String customBgPath;          // 存档副本路径（写进全局偏好，工程丢了也不怕）
    bool prefsLoaded = false;           // 全局偏好存在时，外观以它为准

    //---- Widgets ------------------------------------------------------------
    IconButton gearButton  { IconButton::Icon::gear };
    IconButton logoButton  { IconButton::Icon::none };
    IconButton closeSettingsButton { IconButton::Icon::close };
    IconButton closeAboutButton    { IconButton::Icon::close };

    juce::TextButton  loadButton;
    juce::TextButton  pasteButton;
    juce::TextButton  demoButton;
    juce::TextButton  minusButton;
    juce::TextButton  plusButton       { "+" };
    juce::TextButton  resetButton;
    juce::TextButton  demoPlayButton;
    juce::TextButton  demoStopButton;
    juce::TextButton  applyTextButton;
    juce::TextButton  cancelTextButton;
    juce::TextButton  tapStartButton;
    juce::TextButton  tapUndoButton;
    juce::TextButton  tapRestartButton;
    juce::TextButton  tapNudgeBackButton;
    juce::TextButton  tapNudgeFwdButton;
    juce::TextButton  tapPauseButton;
    juce::TextButton  tapHostTransportButton;   // 同步宿主播放/暂停
    juce::TextButton  tapRowRestampButton;
    juce::TextButton  tapRowApplyButton;
    juce::TextButton  tapRowCancelButton;
    juce::TextEditor  tapTextEditor, tapTimeEditor;
    juce::TextButton  tapFinishButton;
    juce::TextButton  tapExitButton;
    juce::TextButton  measureButton;
    juce::TextButton  exportButton;
    juce::TextButton  retempoButton;
    juce::TextButton  hintToggle;

    juce::TextButton  demoClockToggle;

    // Settings panel widgets
    juce::ComboBox    languageBox, zoomBox, skinBox, fontBox, colourBox, advanceBox;
    juce::TextButton  orientHButton, orientVButton;
    juce::TextButton  pickImageButton, clearImageButton;

    juce::Slider fontSlider  { juce::Slider::LinearHorizontal, juce::Slider::NoTextBox };

    juce::Label offsetLabel, fileLabel, hintLabel, fontLabel, audioLabel;
    juce::TextEditor editorBox;

    std::unique_ptr<juce::TooltipWindow> tooltipWindow;
    std::unique_ptr<juce::FileChooser> chooser;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (LyricSyncEditor)
};
