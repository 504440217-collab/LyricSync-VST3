#pragma once

#include "PluginProcessor.h"
#include "CastServer.h"
#include "KrcLyrics.h"
#include "OnlineLyrics.h"
#include "WordLyrics.h"
#include "UiPalette.h"
#include "I18n.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <vector>
#include <map>

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

    /** 提示框（tooltip）用侧栏同款中文字体：默认无衬线字体没有汉字字形，会显示成方框。 */
    void drawTooltip (juce::Graphics& g, const juce::String& text,
                      int width, int height) override
    {
        g.setColour (findColour (juce::TooltipWindow::backgroundColourId));
        g.fillRoundedRectangle (0.0f, 0.0f, (float) width, (float) height, 6.0f);
        g.setColour (findColour (juce::TooltipWindow::outlineColourId));
        g.drawRoundedRectangle (0.5f, 0.5f, (float) width - 1.0f, (float) height - 1.0f,
                                6.0f, 1.0f);

        g.setColour (findColour (juce::TooltipWindow::textColourId));
        g.setFont (juce::Font (typeface, 12.5f, juce::Font::plain));
        g.drawFittedText (text, 8, 4, width - 16, height - 8,
                          juce::Justification::centred, 4);
    }

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
/** 无文字的图标按钮（齿轮 / 关闭 / 横竖切换），用 Path 画，不依赖字体里有没有该字形。 */
class IconButton : public juce::Button
{
public:
    enum class Icon { gear, close, none, orient, undo };

    explicit IconButton (Icon iconToDraw) : juce::Button (juce::String()), icon (iconToDraw) {}

    void paintButton (juce::Graphics& g, bool over, bool down) override;

    juce::Colour normalColour { juce::Colours::white };
    juce::Colour hoverColour  { juce::Colours::white };

    /** true = 自己带 1px 细边框（和侧栏其它按钮同款），false = 纯图标。 */
    bool boxed = false;
    juce::Colour boxFill { juce::Colours::transparentBlack };   // 遮挡用底色（侧栏色）

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
    void paintOverChildren (juce::Graphics& g) override;   // 刷子图标画在按钮文字之上
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
    juce::Point<float> toLogicalPoint (juce::Point<float> physical) const;
    void seekStandaloneAt (juce::Point<float> logical);
    void comboBoxChanged (juce::ComboBox* box) override;

    //---- Painting -----------------------------------------------------------
    void paintHeader (juce::Graphics& g);
    void paintSidebar (juce::Graphics& g);
    float railWidth() const;
    bool  railOpenNow() const;                            // 侧栏目标状态（不含动画中间值）
    juce::Rectangle<float> railToggleRect() const;
    juce::Rectangle<float> railIconRect (int index) const;
    int  railIconAt (juce::Point<float> logical) const;
    void paintRailIcons (juce::Graphics& g);
    void toggleSidebar();
    void paintSidebarSearch (juce::Graphics& g);
    void paintResultsOverlay (juce::Graphics& g);           // 搜索结果（竖版=歌词上方浮层）
    void paintStepperBox (juce::Graphics& g, juce::Rectangle<float> box,
                          const juce::String& value, bool active = false,
                          bool chevrons = false);

    //---- 侧栏 / 底部面板几何（内联搜索 + 步进器 + 分段开关） -----------------
    juce::Rectangle<float> sideTopRow() const;
    juce::Rectangle<float> sideSearchBox() const;
    juce::Rectangle<float> sideSearchRefresh() const;
    juce::Rectangle<float> sideResultsArea() const;
    std::vector<juce::Rectangle<float>> sideResultRows() const;
    juce::Rectangle<float> sideStartRow() const;
    juce::Rectangle<float> sideOffsetStepper() const;
    juce::Rectangle<float> sideFontStepper() const;
    juce::Rectangle<float> sideSegArea() const;
    juce::Rectangle<float> sideToolsRow() const;

    int  sideResultAt (juce::Point<float> logical) const;
    void fontStep (int px);
    void setAdvanceMode (int mode);
    void refreshStepperColours();          // 步进器数值：平时白字，调整时转绿

    void paintStage  (juce::Graphics& g, juce::Rectangle<float> area);
    void paintTapStage (juce::Graphics& g, juce::Rectangle<float> area);
    void paintTapList (juce::Graphics& g, const juce::Rectangle<float>& area, bool listInSide);
    void keepTapRowVisible (int lineId);                 // 让某行保持在列表可见窗口里
    int  tapListMaxRows() const;                         // 列表可视行数（按空间动态算）
    std::vector<int> tapListSortedIds() const;           // 全部行按有效时间码排序
    void syncTapLyricsToDoc();                           // 打轴修改实时写回歌词文档
    void paintSettings (juce::Graphics& g, juce::Rectangle<float> panel);
    void paintAbout    (juce::Graphics& g, juce::Rectangle<float> panel);
    void paintSearch   (juce::Graphics& g, juce::Rectangle<float> panel);
    void paintStandaloneTransport (juce::Graphics& g);

    //---- 右下角读数条（走带） -------------------------------------------------
    void paintHud (juce::Graphics& g);
    void hudSeekTo (juce::Point<float> logical);
    juce::Rectangle<float> hudBounds() const;
    juce::Rectangle<float> hudPlayRect() const;
    juce::Rectangle<float> hudBarRect() const;
    juce::Rectangle<float> hudTempoRect() const;
    juce::Rectangle<float> hudUndoRect() const;
    void paintInfoLine (juce::Graphics& g);
    void drawText    (juce::Graphics& g, juce::Rectangle<float> area,
                      const juce::String& text, float size, juce::Colour colour,
                      bool bold, float sungProgress = -1.0f,
                      juce::Colour sungColour = {}, float alpha = 1.0f,
                      juce::Justification just = juce::Justification::centred,
                      const juce::String& prefix = {});

    //---- Layout -------------------------------------------------------------
    juce::Rectangle<int> scaledRect (float x, float y, float w, float h) const;
    juce::Rectangle<float> stageArea() const;
    juce::Rectangle<float> settingsPanelArea() const;
    juce::Rectangle<float> aboutPanelArea() const;
    juce::Rectangle<float> searchPanelArea() const;
    std::vector<juce::Rectangle<float>> searchResultRows (juce::Rectangle<float> panel) const;
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
    void setLyricsStart();                // 把当前播放位置设为歌词起点
    void undoSetStart();                  // 撤销上一次起点调整

    //---- 在线搜歌词 ---------------------------------------------------------
    void showSearch (bool show);
    void startSearch();
    void searchFinished (OnlineLyrics::Outcome outcome, int token);
    void loadSelectedResult();
    void downloadFinished (juce::String lrc, juce::String label, int token);
    void watchSearchTimeout (double nowMs);
    void startLxFetch();
    void lxFinished (OnlineLyrics::LxResult result);
    void commitCustomUrl();

    void enterAnnotateMode();             // 进入"着色与备注"模式（就地选择，不切界面）
    void exitAnnotateMode();
    void openLineStyle (int index);       // 右键后在鼠标旁打开小选择框
    void applyColourToSelection (int colourIndex);   // 点色块：颜色立即生效
    void applyLineStyle();                // 点应用：备注才写入
    void clearLineStyle();
    void closeLineStyle();
    void paintAnnotateOverlay (juce::Graphics& g, juce::Rectangle<float> area);
    juce::Rectangle<float> stageLineRect (int index, juce::Rectangle<float> area) const;
    int  stageLineAt (juce::Point<float> logicalPos) const;
    std::vector<juce::Rectangle<float>> styleSwatches (juce::Rectangle<float> card) const;
    juce::Rectangle<float> styleCard() const;
    void loadLineStyles();
    void saveLineStyles();
    int firstVocalLineIndex() const;      // 跳过作词/作曲等制作信息后的首句唱词
    juce::Colour lineColourFor (int index, juce::Colour fallback) const;
    juce::String lineNoteFor (int index) const;

    void armTapLine (int index);          // 预备重打某一句（不清空任何时间）
    void cancelArm();                     // 取消预备
    void nudgeTap (int deltaMs);          // 微调刚打好的那一句
    void toggleTapPause();                // 手动预览下暂停/继续
    void toggleHostTransport();            // 给宿主发空格，开始/暂停播放
    void editTapRow (int index);           // 打开某一行的编辑
    void applyTapRow();                    // 保存这一行的文字与时间
    void restampTapRow();                  // 把这一行的时间改成当前播放位置
    void closeTapRowEditing();             // 取消编辑
    double parseTimecode (const juce::String& text) const;
    juce::Rectangle<float> tapListArea() const;            // 列表所在区域：横版展开=侧栏，其余=舞台
    std::vector<juce::Rectangle<float>> tapListRows (juce::Rectangle<float> area) const;
    std::vector<int> tapListIndices() const;      // 已记录行的行号（与 tapListRows 一一对应）
    juce::Rectangle<float> tapListTrack (juce::Rectangle<float> area) const;  // 列表右侧滚动条
    juce::Rectangle<float> tapListThumb (juce::Rectangle<float> area) const;  // 可拖动的滑块
    int  tapListTotal() const;            // 已打点总行数
    int  tapListMaxScroll() const;        // 最多能往回翻几行
    juce::Rectangle<float> tapRowArmButton (juce::Rectangle<float> row) const;
    juce::Rectangle<float> tapEditRowRect() const;   // 正在编辑那一行的可视矩形 // 列表行右侧「重打」热区
    void scrollTapList (int deltaRows);
    void syncTapToPlayhead (double t);   // 宿主跳转时间后，把打轴指针挪到对应那一句
    void showEditorMenu (juce::TextEditor& te, juce::Point<int> screenPos);   // 右键菜单（跟随中英文）
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
    void styleSidebarControls();   // 侧栏控件统一成扁平深色（忽略皮肤的高亮底）
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
    //---- 逐句样式（颜色 + 备注） --------------------------------------------
    struct LineStyle
    {
        int colourIndex = -1;                // -1 = 默认（跟随主题/歌词颜色）
        juce::String note;
    };

    std::map<int, LineStyle> lineStyles;

    bool   annotateMode    = false;
    int    styleEditIndex  = -1;             // >=0 表示小选择框已打开
    int    styleEditColour = -1;
    int    annotateSelLo   = -1;             // 拖选范围（含），-1 = 未选
    int    annotateSelHi   = -1;
    int    annotateAnchor  = -1;             // 拖选起点
    bool   annotateDragging = false;
    float  annotateViewIndex = 0.0f;         // 着色模式下固定的歌词中心行
    juce::Point<float> styleCardAnchor { 220.0f, 180.0f };   // 右键位置（逻辑坐标）
    std::vector<double> offsetHistory;       // 「设起点 / 偏移」的撤销栈

    //---- 在线搜歌词 ---------------------------------------------------------
    //---- Standalone 独立版走带 ----------------------------------------------
    bool   sidebarCollapsed = false;   // 左侧功能栏是否收起（写进全局偏好）
    float  railAnimWidth   = -1.0f;    // 侧栏收起/展开动画的当前宽度（<0 表示未在动画）
    float  railAnimFrom    = 0.0f;     // 动画起点宽度
    double railAnimStartMs = 0.0;      // 动画开始时间戳
    bool   standaloneMode = false;          // 运行时检测：只有独立版显示走带控件
    bool   standaloneSeekDragging = false;

    // 走带读数条状态
    bool   hudDrag        = false;    // 正在拖进度条
    double hudHoldUntilMs = 0.0;      // 预览定位保持到什么时候
    bool   offsetScrub    = false;    // 正在拖「偏移」数值
    double offsetScrubMs  = 0.0;
    bool   fontScrub      = false;    // 正在拖「字号」数值
    double fontScrubPx    = 140.0;
    bool   offsetHot      = false;    // 数值正在被调整（文字转强调色）
    bool   fontHot        = false;
    double offsetFlashMs  = 0.0;      // 调整后高亮保持到什么时候
    double fontFlashMs    = 0.0;
    juce::Rectangle<float> standaloneSeekBar;  // 逻辑坐标，自绘进度条热区

    // 在线搜歌词的后台线程池。用池而不是 std::thread::detach()：
    // 宿主卸载插件时可以把它们停掉并等待退出，否则 DLL 卸载会被在途线程拖住。
    juce::ThreadPool networkPool { 2 };

    bool   searchOpen  = false;
    bool   searchBusy  = false;
    juce::String searchStatus;
    juce::String customSearchUrl;            // 全局偏好里保存的自定义服务地址
    juce::String lxMusicUrl { "http://127.0.0.1:23330" };
    bool   simplifyChinese = true;           // 在线歌词繁体转简体
    bool   useSystemProxy  = false;          // true = 走系统代理（v2rayN 等）
    std::vector<OnlineLyrics::Result> searchResults;
    int    searchSel    = -1;
    int    searchScroll = 0;
    bool   searchListOpen = false;   // 竖版结果浮层是否展开（点选/点空白收回）
    juce::String searchDiag;                 // 失败时显示给用户的诊断串
    double searchStartedMs = 0.0;            // 看门狗：请求卡住时自动解锁
    int    searchToken = 0;                  // 过期回调不再生效

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
    int  tapListSelected = -1;            // 单击选中的已记录行（双击才进编辑）
    bool tapStagePress = false;           // 舞台按下，抬起时判定「点击打轴 / 拖动翻看」
    double tapListHoldUntilMs = 0.0;      // 手动滚动列表后暂停跟随的截止时间
    std::vector<double> tapOriginalTimes; // 进入打轴时备份的原始时间轴（完成时回填未重打句）
    
    //---- 局域网网页投屏 -----------------------------------------------------
    CastServer         castServer;          // 内嵌 HTTP 服务（歌词页 / state / lyrics）
    juce::CriticalSection castLock;         // 保护 castLyricsJson
    juce::String       castLyricsJson;      // 服务线程读的歌词 JSON
    juce::String       castUrl;             // http://<LAN-IP>:<端口>
    bool               castOpen   = false;  // 投屏面板是否打开（服务是否运行）
    int                castPort   = 0;
    int                castQrSize = 0;      // QR 模块边长（0 = 未生成）
    std::vector<bool>  castQrModules;
    double             lastCastJsonMs = 0.0;
    std::atomic<int>   castVersion { 0 };
    
    void startCast();
    void stopCast();
    void buildQr();
    void rebuildCastLyrics();
    juce::Rectangle<float> castPanelArea() const;
    void paintCast (juce::Graphics& g);
    bool tapFresh = false;                // 全新歌词打轴（无原始时间码）：舞台静止逐句打
    double tapListLastClickMs = 0.0;      // 上一击列表行的时间（自判双击用）
    juce::Point<float> tapStagePressPos;
    int  tapStagePressLine = -1;          // 按下时命中的歌词行
    int  tapArmIndex = -1;                // >=0 表示已预备重打第 N 句，下一次打点只写这一句
    int  tapListScroll = 0;               // 列表往回翻了几行（0 = 停在最新）
    bool tapListDragging = false;         // 正抓住滚动条滑块上下拖
    float tapDragStartY = 0.0f;           // 拖动起点（逻辑坐标）
    int  tapDragStartScroll = 0;
    double tapFollowTime = -1.0;          // 上一帧的播放位置，用来识别"跳转"

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
    IconButton orientButton { IconButton::Icon::orient };
    IconButton logoButton  { IconButton::Icon::none };
    IconButton closeSettingsButton { IconButton::Icon::close };
    IconButton closeAboutButton    { IconButton::Icon::close };

    juce::TextButton  gearButton;
    juce::TextButton  loadButton;
    juce::TextButton  pasteButton;
    juce::TextButton  castButton;             // 投屏（局域网）
    juce::TextButton  fontMinusButton, fontPlusButton;
    juce::TextButton  segLineButton, segWordButton;
    juce::TextButton  demoButton;
    juce::TextButton  minusButton;
    juce::TextButton  plusButton       { "+" };
    juce::TextButton  resetButton;
    juce::TextButton  demoPlayButton;
    juce::TextButton  demoStopButton;
    juce::TextButton  applyTextButton;
    juce::TextButton  cancelTextButton;
    juce::TextButton  tapStartButton;
    juce::TextButton  padStampButton;         // 记录本句（空格）
    juce::TextButton  tapUndoButton;
    juce::TextButton  tapRestartButton;
    juce::TextButton  tapNudgeBackButton;
    juce::TextButton  tapNudgeFwdButton;
    juce::TextButton  tapPauseButton;
    juce::TextButton  tapHostTransportButton;   // 同步宿主播放/暂停
    juce::TextButton  tapRestampAllButton;            // 重打全部：整首时间轴归零从头打
    IconButton        castCloseButton  { IconButton::Icon::close };   // 投屏面板关闭
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
    juce::TextButton  setStartButton;
    IconButton        undoSetStartButton { IconButton::Icon::undo };
    juce::TextButton  annotateButton;
    juce::TextButton  styleApplyButton, styleClearButton, styleCancelButton;
    juce::TextEditor  styleNoteEditor;

    juce::TextButton  onlineLyricsButton;
    juce::TextButton  standalonePlayButton;   // 仅独立版：播放 / 暂停
    juce::TextButton  searchGoButton, searchLoadButton, searchLxButton;
    juce::TextEditor  lxUrlEditor;
    juce::TextButton  simplifyToggle;
    juce::ComboBox    networkBox;        // 直连 / 跟随系统代理
    IconButton        closeSearchButton { IconButton::Icon::close };
    juce::TextEditor  searchTitleEditor, searchArtistEditor, customUrlEditor;
    juce::ShapeButton lxRefreshButton { "lx", palette.dim, palette.text, palette.accent };   // 洛雪取词（音符图标）

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
