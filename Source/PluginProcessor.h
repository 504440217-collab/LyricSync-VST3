#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_data_structures/juce_data_structures.h>

#include "LyricsDocument.h"

#include <atomic>

//==============================================================================
/** 设置项的 key，编辑器与处理器共用。 */
namespace Settings
{
    inline const juce::Identifier lyricsText { "lyricsText" };
    inline const juce::Identifier lyricsPath { "lyricsPath" };
    inline const juce::Identifier offsetMs   { "offsetMs" };
    inline const juce::Identifier fontScale  { "fontScale" };
    inline const juce::Identifier theme      { "theme" };
    inline const juce::Identifier windowW    { "windowW" };
    inline const juce::Identifier windowH    { "windowH" };
    inline const juce::Identifier demoClock  { "demoClock" };
    inline const juce::Identifier language  { "language" };   // 0 = 中文, 1 = English
    inline const juce::Identifier stateVersion { "stateVersion" };
    // 从轨道实际音频测得的范围（秒，工程时间）。-1 表示尚未测到。
    inline const juce::Identifier audioStart { "audioStart" };
    inline const juce::Identifier audioEnd   { "audioEnd" };
}

//==============================================================================
/**
 *  LyricSync 处理器。
 *
 *  音频完全直通（不改一个采样），唯一的音频线程工作是从宿主的
 *  ProcessContext / AudioPlayHead 读取"项目绝对时间"并写入原子变量。
 *  编辑器只按时间戳查表，不做累加计时，因此：
 *      · 任意跳转时间码  -> 下一帧立即跟随
 *      · 循环 / 倒放 / 变速 / 停止  -> 都以宿主播放头为准
 *      · 工程重新打开    -> 歌词文本随宿主工程一起保存
 */
class LyricSyncProcessor : public juce::AudioProcessor
{
public:
    LyricSyncProcessor();
    ~LyricSyncProcessor() override = default;

    //---- AudioProcessor -----------------------------------------------------
    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;

    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;

    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    void processBlockBypassed (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override                 { return true; }

    const juce::String getName() const override    { return "LyricSync"; }
    bool acceptsMidi() const override              { return false; }
    bool producesMidi() const override             { return false; }
    bool isMidiEffect() const override             { return false; }
    double getTailLengthSeconds() const override   { return 0.0; }

    int getNumPrograms() override                  { return 1; }
    int getCurrentProgram() override               { return 0; }
    void setCurrentProgram (int) override          {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    //---- 宿主播放头（音频线程 -> GUI 线程，单向原子传递） --------------------
    std::atomic<double>      positionSeconds { 0.0 };
    std::atomic<double>      ppqPosition    { 0.0 };
    std::atomic<double>      bpm            { 120.0 };
    std::atomic<double>      hostSampleRate { 48000.0 };
    std::atomic<juce::int64> timeInSamples  { 0 };
    std::atomic<int>         timeSigNum     { 4 };
    std::atomic<int>         timeSigDen     { 4 };
    std::atomic<bool>        isPlaying      { false };
    std::atomic<bool>        isRecording    { false };
    std::atomic<bool>        isLooping      { false };
    std::atomic<bool>        hasHostTime    { false };
    std::atomic<juce::uint32> timeUpdates  { 0 };

    //---- 音频长度测量（音频线程写，GUI 线程读） ------------------------------
    //  插件作为插入效果收到的是这条轨道（或总线）的真实音频，因此可以在
    //  音频线程里用"非静音区间"的起点/终点还原歌曲的实际长度：
    //  进度条按它显示，而不是按工程总长度或歌词文件的最后一句。
    std::atomic<double> audioStartSeconds { -1.0 };
    std::atomic<double> audioEndSeconds   { -1.0 };

    /** 清空测量结果（换歌、或用户点击"重测音频"时调用）。 */
    void resetAudioRange();

    /** 由编辑器把测量结果写回工程状态 / 或在载入工程后恢复。 */
    void storeAudioRange (double startSeconds, double endSeconds);

    //---- 歌词与设置（GUI 线程访问） -----------------------------------------
    LyricsDocument&       lyrics()       noexcept { return document; }
    const LyricsDocument& lyrics() const noexcept { return document; }

    /** 解析并替换歌词，同时写入工程状态。返回解析摘要。 */
    LyricsInfo setLyricsText (const juce::String& text, const juce::String& path = {},
                              int language = 0);

    /** 宿主载入工程后，由编辑器在 GUI 线程调用，避免跨线程改歌词。 */
    void reloadFromSettings (int language = 0);

    bool consumeStateReloadFlag() noexcept
    {
        return stateReloadPending.exchange (false);
    }

    void   setSetting (const juce::Identifier& key, const juce::var& value);
    juce::var getSetting (const juce::Identifier& key, const juce::var& fallback) const;

private:
    LyricsDocument document;
    juce::ValueTree settings;
    mutable juce::CriticalSection settingsLock;
    std::atomic<bool> stateReloadPending { false };

    void readHostTime();
    void updateAudioRange (const juce::AudioBuffer<float>& buffer);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (LyricSyncProcessor)
};
