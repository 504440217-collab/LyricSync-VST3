#pragma once

#include <juce_core/juce_core.h>

//==============================================================================
/**
 *  Word 文档歌词读取。
 *
 *  · .docx  —— 本质是 zip，取 word/document.xml，按 w:p / w:t 还原段落文字。
 *  · .doc   —— 先看是不是 RTF（很多"另存为 .doc"其实是 RTF），是就剥控制符；
 *              否则按 Word 97 二进制做一次"可打印 run"抽取（够用，不保证 100%）。
 */
namespace WordDoc
{
    bool looksLikeWordFile (const juce::String& path);

    /** 返回纯文本（一行一句）；失败返回空字符串。 */
    juce::String extractText (const juce::File& file, const juce::MemoryBlock& data);
}
