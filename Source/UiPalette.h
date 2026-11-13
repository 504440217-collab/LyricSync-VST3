#pragma once

#include <juce_graphics/juce_graphics.h>

/** 集中管理配色，深色 / 浅色两套主题。 */
struct UiPalette
{
    juce::Colour bgTop, bgBottom, panel, panelEdge, card;
    juce::Colour text, dim, faint;
    juce::Colour accent, accentSoft, warn, ok, track;

    static UiPalette dark()
    {
        UiPalette p;
        p.bgTop      = juce::Colour (0xff11141c);
        p.bgBottom   = juce::Colour (0xff0b0d13);
        p.panel      = juce::Colour (0xff1b2030);
        p.panelEdge  = juce::Colour (0xff333c55);
        p.card       = juce::Colour (0xff161b27);
        p.text       = juce::Colour (0xfff2f5ff);
        p.dim        = juce::Colour (0xff9aa5bf);
        p.faint      = juce::Colour (0xff5f6a85);
        p.accent     = juce::Colour (0xff5ce1c1);
        p.accentSoft = juce::Colour (0xff3aa88f);
        p.warn       = juce::Colour (0xffffb86b);
        p.ok         = juce::Colour (0xff74e08a);
        p.track      = juce::Colour (0xff2b3247);
        return p;
    }

    static UiPalette light()
    {
        UiPalette p;
        p.bgTop      = juce::Colour (0xfff7f9ff);
        p.bgBottom   = juce::Colour (0xffe9edf7);
        p.panel      = juce::Colour (0xffffffff);
        p.panelEdge  = juce::Colour (0xffd4dbea);
        p.card       = juce::Colour (0xfff4f7fd);
        p.text       = juce::Colour (0xff1a2233);
        p.dim        = juce::Colour (0xff5d6883);
        p.faint      = juce::Colour (0xff98a2b8);
        p.accent     = juce::Colour (0xff0e9e86);
        p.accentSoft = juce::Colour (0xff63c6b2);
        p.warn       = juce::Colour (0xffc46b12);
        p.ok         = juce::Colour (0xff1c8f52);
        p.track      = juce::Colour (0xffdfe5f2);
        return p;
    }

    static UiPalette fromIndex (int index)
    {
        return index == 1 ? light() : dark();
    }
};
