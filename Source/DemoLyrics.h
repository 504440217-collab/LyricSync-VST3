#pragma once

#include "I18n.h"

/** Built-in demonstration lyrics. Text lives in I18n.cpp as UTF-8 escapes. */
inline juce::String demoLyricsText (int language = 0)
{
    return Lang::demoLyrics (language);
}
