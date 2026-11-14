#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "DemoLyrics.h"

#include <cmath>

//==============================================================================
LyricSyncProcessor::LyricSyncProcessor()
    : AudioProcessor (BusesProperties()
                        .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
                        .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      settings (juce::Identifier ("LyricSync"))
{
    setSetting (Settings::lyricsText, demoLyricsText());
    setSetting (Settings::lyricsPath, "");
    setSetting (Settings::offsetMs,   0.0);
    setSetting (Settings::fontScale,  1.0);
    setSetting (Settings::theme,      0);
    setSetting (Settings::windowW,    960);
    setSetting (Settings::windowH,    600);
    setSetting (Settings::demoClock,  false);
    setSetting (Settings::language,   0);
    setSetting (Settings::stateVersion, 2);
    setSetting (Settings::audioStart, -1.0);
    setSetting (Settings::audioEnd,   -1.0);

    document.loadText (demoLyricsText (0));
}

//==============================================================================
void LyricSyncProcessor::prepareToPlay (double sampleRate, int)
{
    if (sampleRate > 0.0)
        hostSampleRate.store (sampleRate);
}

void LyricSyncProcessor::releaseResources()
{
    // 没有任何音频资源需要释放
}

bool LyricSyncProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto& in  = layouts.getMainInputChannelSet();
    const auto& out = layouts.getMainOutputChannelSet();

    // 容许宿主临时禁用某一侧总线
    if (in.isDisabled() || out.isDisabled())
        return true;

    if (in != out)
        return false;

    return in == juce::AudioChannelSet::mono()
        || in == juce::AudioChannelSet::stereo();
}

void LyricSyncProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    // 音频直通：一个采样都不动，宿主给什么就还什么。
    juce::ignoreUnused (midi);
    readHostTime();
    updateAudioRange (buffer);
}

void LyricSyncProcessor::processBlockBypassed (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    // 即使宿主把插件旁路，也要继续读取播放头，歌词才不会停住。
    juce::ignoreUnused (midi);
    readHostTime();
    updateAudioRange (buffer);
}

//------------------------------------------------------------------------------
/**
 *  音频长度测量。
 *
 *  VST3 效果器拿不到"片段长度"这种信息，但它能拿到经过本轨的真实音频。
 *  所以这里用最朴素也最可靠的办法：找出非静音区间的第一个和最后一个块，
 *  把它们换算成工程时间，作为这首歌的起点/终点。
 *
 *  音频线程纪律：只做峰值扫描 + 原子 CAS，不加锁、不分配内存。
 */
void LyricSyncProcessor::updateAudioRange (const juce::AudioBuffer<float>& buffer)
{
    const int numSamples = buffer.getNumSamples();
    if (numSamples <= 0)
        return;

    float peak = 0.0f;
    for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
        peak = juce::jmax (peak, buffer.getMagnitude (ch, 0, numSamples));

    if (peak < 0.0005f)          // -66 dBFS 以下当作静音（空白小节 / 未播放）
        return;

    const double sr = hostSampleRate.load();
    if (sr <= 0.0)
        return;

    const double blockStart = positionSeconds.load();
    if (! std::isfinite (blockStart) || blockStart < 0.0)
        return;

    const double blockEnd = blockStart + static_cast<double> (numSamples) / sr;

    // 只在真正播放时测量，避免某些宿主在停止状态下仍输出残留 buffer
    if (! isPlaying.load() && blockStart <= 0.0)
        return;

    double curStart = audioStartSeconds.load (std::memory_order_relaxed);
    while ((curStart < 0.0 || blockStart < curStart)
           && ! audioStartSeconds.compare_exchange_weak (curStart, blockStart,
                                                         std::memory_order_relaxed))
    {
    }

    double curEnd = audioEndSeconds.load (std::memory_order_relaxed);
    while (blockEnd > curEnd
           && ! audioEndSeconds.compare_exchange_weak (curEnd, blockEnd,
                                                       std::memory_order_relaxed))
    {
    }
}

void LyricSyncProcessor::resetAudioRange()
{
    audioStartSeconds.store (-1.0);
    audioEndSeconds.store (-1.0);
}

void LyricSyncProcessor::storeAudioRange (double startSeconds, double endSeconds)
{
    {
        const juce::ScopedLock sl (settingsLock);
        settings.setProperty (Settings::audioStart, startSeconds, nullptr);
        settings.setProperty (Settings::audioEnd,   endSeconds,   nullptr);
    }

    if (startSeconds >= 0.0)
        audioStartSeconds.store (startSeconds);

    if (endSeconds > 0.0)
        audioEndSeconds.store (endSeconds);
}

//------------------------------------------------------------------------------
/**
 *  同步的核心：每个音频回调都向宿主问一次"项目绝对时间"。
 *  这里绝不做 position += blockDuration 之类的累加，所以播放头跳到
 *  任意位置（包括循环回绕、往回拖、停止后重新定位）都能立即对齐。
 *
 *  JUCE 的 getPlayHead() 在 VST3 下读取的正是 Steinberg::Vst::ProcessContext
 *  的 projectTimeSamples / projectTimeMusic / state 等字段。
 */
void LyricSyncProcessor::readHostTime()
{
    auto* head = getPlayHead();
    if (head == nullptr)
        return;

    if (auto pos = head->getPosition())
    {
        bool gotSeconds = false;

        if (auto samples = pos->getTimeInSamples())
        {
            timeInSamples.store (*samples);

            const double sr = hostSampleRate.load();
            if (sr > 0.0)
            {
                positionSeconds.store (static_cast<double> (*samples) / sr);
                gotSeconds = true;
            }
        }

        if (auto seconds = pos->getTimeInSeconds())
        {
            positionSeconds.store (*seconds);
            gotSeconds = true;
        }

        // 极少数宿主只给音乐时间；用 BPM 换算兜底，避免歌词完全不动
        if (! gotSeconds)
        {
            if (auto ppq = pos->getPpqPosition())
            {
                const double tempo = bpm.load();
                if (tempo > 0.0)
                    positionSeconds.store (*ppq * 60.0 / tempo);
            }
        }

        if (auto ppq = pos->getPpqPosition())
            ppqPosition.store (*ppq);

        if (auto value = pos->getBpm())
        {
            if (*value > 0.0)
                bpm.store (*value);
        }

        if (auto sig = pos->getTimeSignature())
        {
            if (sig->numerator > 0 && sig->denominator > 0)
            {
                timeSigNum.store (sig->numerator);
                timeSigDen.store (sig->denominator);
            }
        }

        // JUCE 7 里这三个状态直接返回 bool，不是 Optional<bool>
        isPlaying.store   (pos->getIsPlaying());
        isRecording.store (pos->getIsRecording());
        isLooping.store   (pos->getIsLooping());

        hasHostTime.store (true);
        timeUpdates.fetch_add (1, std::memory_order_relaxed);
    }
}

//==============================================================================
juce::AudioProcessorEditor* LyricSyncProcessor::createEditor()
{
    return new LyricSyncEditor (*this);
}

//==============================================================================
void LyricSyncProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    juce::String xml;
    {
        const juce::ScopedLock sl (settingsLock);
        // 把当前测到的音频范围一并写进工程，重新打开时进度条不会跳回 0。
        settings.setProperty (Settings::audioStart, audioStartSeconds.load(), nullptr);
        settings.setProperty (Settings::audioEnd,   audioEndSeconds.load(),   nullptr);
        xml = settings.toXmlString();
    }

    if (xml.isNotEmpty())
        destData.append (xml.toRawUTF8(), xml.getNumBytesAsUTF8());
}

void LyricSyncProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    if (data == nullptr || sizeInBytes <= 0)
        return;

    const auto text = juce::String::fromUTF8 (static_cast<const char*> (data), sizeInBytes);

    if (auto xml = juce::parseXML (text))
    {
        auto loaded = juce::ValueTree::fromXml (*xml);

        if (loaded.isValid() && loaded.hasType ("LyricSync"))
        {
            // v1 built the demo lyrics with the ASCII-only String constructor,
            // so saved demo text was mojibake. Replace ONLY the built-in demo;
            // user-supplied lyrics are left untouched.
            if (static_cast<int> (loaded.getProperty (Settings::stateVersion, 1)) < 2)
            {
                const auto saved = loaded.getProperty (Settings::lyricsText, "").toString();

                if (saved.startsWith ("[ti:LyricSync"))
                {
                    const int lang = juce::jlimit (0, 1,
                        static_cast<int> (loaded.getProperty (Settings::language, 0)));
                    loaded.setProperty (Settings::lyricsText, demoLyricsText (lang), nullptr);
                }
            }

            loaded.setProperty (Settings::stateVersion, 2, nullptr);

            {
                const juce::ScopedLock sl (settingsLock);
                settings = loaded;
            }

            // 歌词文档只在 GUI 线程解析，编辑器检测到这个标记后自行重载。
            stateReloadPending.store (true);

            // 恢复上次测量的音频范围（进度条需要它）
            const double savedStart = static_cast<double> (loaded.getProperty (Settings::audioStart, -1.0));
            const double savedEnd   = static_cast<double> (loaded.getProperty (Settings::audioEnd,   -1.0));
            audioStartSeconds.store (savedStart);
            audioEndSeconds.store (savedEnd);
        }
    }
}

LyricsInfo LyricSyncProcessor::setLyricsText (const juce::String& text,
                                               const juce::String& path,
                                               int language)
{
    const auto info = document.loadText (text, 4.0, language);

    {
        const juce::ScopedLock sl (settingsLock);
        settings.setProperty (Settings::lyricsText, text, nullptr);

        if (path.isNotEmpty())
            settings.setProperty (Settings::lyricsPath, path, nullptr);
    }

    return info;
}

void LyricSyncProcessor::reloadFromSettings (int language)
{
    juce::String text;

    {
        const juce::ScopedLock sl (settingsLock);
        text = settings.getProperty (Settings::lyricsText,
                                     demoLyricsText (language)).toString();
    }

    document.loadText (text, 4.0, language);
    stateReloadPending.store (false);
}

void LyricSyncProcessor::setSetting (const juce::Identifier& key, const juce::var& value)
{
    const juce::ScopedLock sl (settingsLock);
    settings.setProperty (key, value, nullptr);
}

juce::var LyricSyncProcessor::getSetting (const juce::Identifier& key, const juce::var& fallback) const
{
    const juce::ScopedLock sl (settingsLock);
    return settings.getProperty (key, fallback);
}

//==============================================================================
//  JUCE 的插件工厂宏：VST3 / Standalone 等不同格式由 CMake 分别生成
//==============================================================================
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new LyricSyncProcessor();
}
