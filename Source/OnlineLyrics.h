#pragma once

#include <juce_core/juce_core.h>

#include <vector>

//==============================================================================
/**
 *  在线歌词：酷狗歌曲库精确匹配为主，LRCLIB 兜底，可选自定义服务地址与 LX Music。
 *
 *  这里只放"纯函数"：同步执行、不碰任何 UI 状态，全部在后台线程调用。
 *  返回的 LRC 文本可直接交给 LyricSyncProcessor::setLyricsText()。
 *
 *  酷狗的歌词搜索接口 lyrics.kugou.com/search 不带歌手信息，同名歌会串；
 *  因此先用歌曲库接口拿到准确的 singername / hash，再用 hash 精确查歌词。
 *  KRC 解密/转增强 LRC 复用项目里已有的 Krc 模块。
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
        juce::String hash;            // 酷狗歌曲 hash（精确查歌词）
        juce::String id;              // 已解析出的歌词 id
        juce::String accessKey;       // 已解析出的歌词 accesskey
        juce::String lrc;             // 已经拿到的歌词（LRCLIB / 自定义）
        juce::String preview;         // 结果列表里的一行预览
        bool wordSync = false;        // 是否逐字
        int  lineCount = 0;           // 下载后统计到的时间码行数
    };

    struct Outcome
    {
        std::vector<Result> results;
        juce::String via;             // 实际命中的来源
        juce::String error;           // "empty" / "notfound" / 空 = 成功
        juce::String detail;          // 诊断信息：每一步的 HTTP 状态与结果数
    };

    struct LxResult
    {
        juce::String title;
        juce::String artist;
        juce::String lrc;
        int  lineCount = 0;
        juce::String error;           // "connect" / "noLyric" / 空 = 成功
    };

    /**
     *  卸载前收尾：关掉正在使用的 WinHTTP 句柄，让在途请求立刻返回，
     *  并把取消标记置位，后续请求直接放弃（不再发起新的连接）。
     *  必须在销毁编辑器之前调用，否则宿主卸载插件 DLL 时会被卡住。
     */
    void cancelAll();

    /** 新建编辑器时清掉取消标记，恢复正常联网。 */
    void resetCancel();

    /** 网络连接方式：false = WinHTTP 直连（默认）；true = 跟随系统代理（v2rayN 等）。 */
    void setUseSystemProxy (bool shouldUse);
    bool isUsingSystemProxy();

    /** 搜索。customUrl 支持 {q} {artist} {duration} 占位符，为空则不启用。 */
    Outcome search (juce::String title, juce::String artist,
                    int durationSec, juce::String customUrl);

    /** 取某一条结果的完整 LRC；simplify=true 时把繁体转成简体。失败返回空。 */
    juce::String download (const Result& result, bool simplify);

    /** 从 LX Music 桌面版开放 API 取「当前播放歌曲」的 LRC。 */
    LxResult fetchFromLx (juce::String baseUrl, bool simplify);

    int  countTimedLines (const juce::String& text);
    juce::String toSimplified (const juce::String& text);
}
