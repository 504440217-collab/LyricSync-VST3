#pragma once

#include <juce_core/juce_core.h>

//==============================================================================
/**
 *  酷狗 KRC 逐字歌词解码。
 *
 *  文件结构：
 *      "krc1" (4 bytes) | 加密正文（zlib 压缩 + 16 字节固定密钥循环异或）
 *
 *  解压后的明文：
 *      [id:...] [ar:...] [ti:...] [total:毫秒] [offset:毫秒] ...
 *      [行起始毫秒,行持续毫秒]<字偏移,字持续,0>字<...>字
 *
 *  本模块只做"翻译"：把 KRC 转成插件内部已经支持的增强 LRC
 *  （`[mm:ss.xx]字<mm:ss.xx>字...`），下游的解析、逐字高亮、进度条全都不用改。
 */
namespace Krc
{
    /** 数据是否以 "krc1" 开头。 */
    bool isKrcData (const void* data, int sizeInBytes);

    /** 解密 + 解压，返回 KRC 明文；失败返回空字符串。 */
    juce::String decrypt (const juce::MemoryBlock& raw);

    /** 把 KRC 明文转成增强 LRC（带 <mm:ss.xx> 逐字锚点），失败返回空字符串。 */
    juce::String toEnhancedLrc (const juce::String& plainText);

    /** 明文里 [total:毫秒] 对应的音频总长（秒）；没有该标签返回 -1。 */
    double totalSecondsFromPlain (const juce::String& plainText);

    /** 明文里 [offset:毫秒] 的值；没有返回 0。 */
    double offsetMsFromPlain (const juce::String& plainText);
}
