#pragma once

#include <juce_core/juce_core.h>

#include <vector>

//==============================================================================
/**
 *  在线歌词：酷狗直连为主，LRCLIB 兜底，可选自定义服务地址。
 *
 *  这里只放"纯函数"：同步执行、不碰任何 UI 状态，全部在后台线程调用。
 *  返回的 LRC 文本可直接交给 LyricSyncProcessor::setLyricsText()。
 *
 *  酷狗返回的是 base64 编码的 KRC；解密/转增强 LRC 复用项目里已有的 Krc 模块。
 *  LRCLIB 返回的 syncedLyrics 本身就是标准 LRC。
 */
namespace OnlineLyrics
{
    struct Result
    {
        juce::String title;
        juce::String artist;
        juce::String album;
        juce::String source;          // "Kugou" / "LRCLIB" / "Custom"
        int  durationSec = 0;
        juce::String id;              // 酷狗下载所需
        juce::String accessKey;       // 酷狗下载所需
        juce::String lrc;             // 已经拿到的歌词（LRCLIB / 自定义）
        juce::String preview;         // 结果列表里的一行预览
        bool wordSync = false;        // 是否逐字
    };

    struct Outcome
    {
        std::vector<Result> results;
        juce::String via;             // 实际命中的来源
        juce::String error;           // "empty" / "notfound" / 空 = 成功
    };

    /** 搜索。customUrl 支持 {q} {artist} {duration} 占位符，为空则不启用。 */
    Outcome search (juce::String title, juce::String artist,
                    int durationSec, juce::String customUrl);

    /** 取某一条结果的完整 LRC；酷狗结果会再发一次下载请求。失败返回空。 */
    juce::String download (const Result& result);
}
