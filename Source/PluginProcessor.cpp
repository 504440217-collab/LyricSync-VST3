#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "DemoLyrics.h"

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

    document.loadText (demoLyricsText());
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
    juce::ignoreUnused (buffer, midi);
    readHostTime();
}

void LyricSyncProcessor::processBlockBypassed (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    // 即使宿主把插件旁路，也要继续读取播放头，歌词才不会停住。
    juce::ignoreUnused (buffer, midi);
    readHostTime();
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
            {
                const juce::ScopedLock sl (settingsLock);
                settings = loaded;
            }

            // 歌词文档只在 GUI 线程解析，编辑器检测到这个标记后自行重载。
            stateReloadPending.store (true);
        }
    }
}

LyricsInfo LyricSyncProcessor::setLyricsText (const juce::String& text, const juce::String& path)
{
    const auto info = document.loadText (text);

    {
        const juce::ScopedLock sl (settingsLock);
        settings.setProperty (Settings::lyricsText, text, nullptr);

        if (path.isNotEmpty())
            settings.setProperty (Settings::lyricsPath, path, nullptr);
    }

    return info;
}

void LyricSyncProcessor::reloadFromSettings()
{
    juce::String text;

    {
        const juce::ScopedLock sl (settingsLock);
        text = settings.getProperty (Settings::lyricsText, demoLyricsText()).toString();
    }

    document.loadText (text);
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
