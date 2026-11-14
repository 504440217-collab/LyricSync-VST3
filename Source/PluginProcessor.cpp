#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "DemoLyrics.h"

#include <cmath>
#include <vector>

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
    setSetting (Settings::audioTotal, -1.0);

    // 界面外观：-1 表示"没设置过"，编辑器会用全局偏好兜底
    setSetting (Settings::skin,        -1);
    setSetting (Settings::zoom,        -1);
    setSetting (Settings::orientation, -1);
    setSetting (Settings::font,        -1);
    setSetting (Settings::colour,      -1);
    setSetting (Settings::customBg,    "");

    document.loadText (demoLyricsText (0));
}

//==============================================================================
void LyricSyncProcessor::prepareToPlay (double sampleRate, int)
{
    if (sampleRate > 0.0)
    {
        hostSampleRate.store (sampleRate);
        tempo.prepare (sampleRate);
    }
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

    if (isPlaying.load())
        tempo.pushBlock (buffer);
}

void LyricSyncProcessor::processBlockBypassed (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    // 即使宿主把插件旁路，也要继续读取播放头，歌词才不会停住。
    juce::ignoreUnused (midi);
    readHostTime();
    updateAudioRange (buffer);

    if (isPlaying.load())
        tempo.pushBlock (buffer);
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
    juce::String path;

    {
        const juce::ScopedLock sl (settingsLock);
        text = settings.getProperty (Settings::lyricsText, "").toString();
        path = settings.getProperty (Settings::lyricsPath, "").toString();
    }

    // 工程里没有歌词文本（老工程 / 被清空），但记着本地歌词库的路径：
    // 直接从歌词库读回来，实现"重开工程自动还原这首歌的歌词"。
    if (text.trim().isEmpty() && path.isNotEmpty())
    {
        const juce::File archived (path);

        if (archived.existsAsFile())
            text = archived.loadFileAsString();
    }

    if (text.trim().isEmpty())
        text = demoLyricsText (language);

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
//  节拍检测
//==============================================================================
void TempoTracker::prepare (double sampleRate) noexcept
{
    hopSamples = juce::jmax (64, static_cast<int> (std::lround (sampleRate / 100.0)));  // 10 ms
    hopSeconds = static_cast<double> (hopSamples) / juce::jmax (1.0, sampleRate);
    hopSum = 0.0f;
    hopFill = 0;
    writeIndex.store (0);
    available.store (0);
    bpm = 0.0;
    confidence = 0.0;
}

void TempoTracker::pushBlock (const juce::AudioBuffer<float>& buffer) noexcept
{
    const int numCh = buffer.getNumChannels();
    const int numSamples = buffer.getNumSamples();

    if (numCh <= 0 || numSamples <= 0 || hopSamples <= 0)
        return;

    // 用户按了「重新测速」：在这一帧里把环形缓冲清干净（音频线程做，无锁竞争）
    if (resetRequested.exchange (false, std::memory_order_relaxed))
    {
        writeIndex.store (0, std::memory_order_relaxed);
        available.store (0, std::memory_order_relaxed);
        hopSum = 0.0f;
        hopFill = 0;
    }

    int pos = 0;

    while (pos < numSamples)
    {
        const int take = juce::jmin (numSamples - pos, hopSamples - hopFill);

        float sum = 0.0f;

        for (int ch = 0; ch < numCh; ++ch)
        {
            const float* data = buffer.getReadPointer (ch, pos);

            for (int i = 0; i < take; ++i)
                sum += data[i] * data[i];
        }

        hopSum += sum;
        hopFill += take;
        pos += take;

        if (hopFill >= hopSamples)
        {
            const float rms = std::sqrt (hopSum / static_cast<float> (hopSamples * numCh));
            const int idx = writeIndex.load (std::memory_order_relaxed);

            energy[idx] = rms;
            writeIndex.store ((idx + 1) % maxHops, std::memory_order_release);

            if (available.load (std::memory_order_relaxed) < maxHops)
                available.fetch_add (1, std::memory_order_relaxed);

            hopSum = 0.0f;
            hopFill = 0;
        }
    }
}

void TempoTracker::analyse() noexcept
{
    const int n = juce::jmin (available.load(), maxHops);

    if (n < 400 || hopSeconds <= 0.0)
    {
        confidence = 0.0;
        return;
    }

    std::vector<float> e (static_cast<size_t> (n));
    const int wp = writeIndex.load (std::memory_order_acquire);

    for (int i = 0; i < n; ++i)
        e[static_cast<size_t> (i)] = energy[(wp - n + i + maxHops * 2) % maxHops];

    float maxE = 1.0e-5f;

    for (auto v : e)
        maxE = juce::jmax (maxE, v);

    // 起点包络：能量上升沿
    std::vector<float> onset (static_cast<size_t> (n), 0.0f);

    for (int i = 1; i < n; ++i)
    {
        const float d = (e[static_cast<size_t> (i)] - e[static_cast<size_t> (i - 1)]) / maxE;
        onset[static_cast<size_t> (i)] = d > 0.0f ? d : 0.0f;
    }

    const int lagMin = juce::jmax (6, static_cast<int> (std::lround (60.0 / 200.0 / hopSeconds)));
    const int lagMax = juce::jmin (n / 2, static_cast<int> (std::lround (60.0 / 45.0 / hopSeconds)));

    if (lagMax <= lagMin + 2)
    {
        confidence = 0.0;
        return;
    }

    std::vector<double> scores (static_cast<size_t> (lagMax + 2), 0.0);
    double best = 0.0, total = 0.0;
    int bestLag = 0, counted = 0;

    for (int lag = lagMin; lag <= lagMax; ++lag)
    {
        double s = 0.0;

        for (int i = 0; i + lag < n; ++i)
            s += static_cast<double> (onset[static_cast<size_t> (i)])
               * static_cast<double> (onset[static_cast<size_t> (i + lag)]);

        s /= static_cast<double> (n - lag);
        scores[static_cast<size_t> (lag)] = s;

        total += s;
        ++counted;

        if (s > best)
        {
            best = s;
            bestLag = lag;
        }
    }

    if (bestLag <= 0 || counted == 0 || best <= 1.0e-9)
    {
        confidence = 0.0;
        return;
    }

    double refined = static_cast<double> (bestLag);

    if (bestLag > lagMin && bestLag < lagMax)
    {
        const double a = scores[static_cast<size_t> (bestLag - 1)];
        const double b = scores[static_cast<size_t> (bestLag)];
        const double c = scores[static_cast<size_t> (bestLag + 1)];
        const double denom = a - 2.0 * b + c;

        if (std::abs (denom) > 1.0e-12)
            refined += 0.5 * (a - c) / denom;
    }

    double estimate = 60.0 / juce::jmax (1.0e-6, refined * hopSeconds);

    while (estimate < 70.0)   estimate *= 2.0;
    while (estimate > 175.0)  estimate *= 0.5;

    const double mean = total / static_cast<double> (counted);
    const double ratio = mean > 1.0e-9 ? best / mean : 0.0;

    bpm = estimate;
    confidence = juce::jlimit (0.0, 1.0, (ratio - 1.0) / 2.5);
}

//==============================================================================
//  JUCE 的插件工厂宏：VST3 / Standalone 等不同格式由 CMake 分别生成
//==============================================================================
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new LyricSyncProcessor();
}
