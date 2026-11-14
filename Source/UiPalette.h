#pragma once

#include <juce_graphics/juce_graphics.h>

/** 一套皮肤配色。自定义图片皮肤只是在这个基础上叠一层图片。 */
struct UiPalette
{
    juce::Colour bgTop, bgBottom, panel, panelEdge, card;
    juce::Colour text, dim, faint;
    juce::Colour accent, accentSoft, warn, ok, track;
    juce::Colour stageTint;              // 舞台区额外叠加的透明色（自定义皮肤用）

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
        p.stageTint  = juce::Colour (0x00000000);
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
        p.stageTint  = juce::Colour (0x00000000);
        return p;
    }

    /** 午夜蓝：深蓝底 + 天蓝强调。 */
    static UiPalette midnight()
    {
        UiPalette p = dark();
        p.bgTop      = juce::Colour (0xff0d1730);
        p.bgBottom   = juce::Colour (0xff060b18);
        p.panel      = juce::Colour (0xff16213d);
        p.panelEdge  = juce::Colour (0xff2b3b63);
        p.card       = juce::Colour (0xff101a30);
        p.text       = juce::Colour (0xffe8f1ff);
        p.dim        = juce::Colour (0xff8fa4c9);
        p.faint      = juce::Colour (0xff566a92);
        p.accent     = juce::Colour (0xff6fb4ff);
        p.accentSoft = juce::Colour (0xff3d7ac4);
        p.track      = juce::Colour (0xff22314f);
        return p;
    }

    /** 暖阳：深棕底 + 琥珀强调。 */
    static UiPalette warm()
    {
        UiPalette p = dark();
        p.bgTop      = juce::Colour (0xff22190f);
        p.bgBottom   = juce::Colour (0xff140e08);
        p.panel      = juce::Colour (0xff2e2317);
        p.panelEdge  = juce::Colour (0xff4d3b25);
        p.card       = juce::Colour (0xff261c12);
        p.text       = juce::Colour (0xfffff3e2);
        p.dim        = juce::Colour (0xffc4ab8a);
        p.faint      = juce::Colour (0xff856d50);
        p.accent     = juce::Colour (0xffffb45c);
        p.accentSoft = juce::Colour (0xffc9853a);
        p.track      = juce::Colour (0xff3d2f1f);
        return p;
    }

    /** 森林：墨绿底 + 草绿强调。 */
    static UiPalette forest()
    {
        UiPalette p = dark();
        p.bgTop      = juce::Colour (0xff0d1a13);
        p.bgBottom   = juce::Colour (0xff050d09);
        p.panel      = juce::Colour (0xff15261c);
        p.panelEdge  = juce::Colour (0xff2a4433);
        p.card       = juce::Colour (0xff101e16);
        p.text      = juce::Colour (0xffe9fff1);
        p.dim        = juce::Colour (0xff9dc0a9);
        p.faint      = juce::Colour (0xff5c7d68);
        p.accent     = juce::Colour (0xff7ee081);
        p.accentSoft = juce::Colour (0xff48a558);
        p.track      = juce::Colour (0xff24382a);
        return p;
    }

    /** 自定义图片皮肤：文字用主题色，背景交给图片。 */
    static UiPalette customImage()
    {
        UiPalette p = dark();
        p.bgTop      = juce::Colour (0xff101216);
        p.bgBottom   = juce::Colour (0xff07080a);
        p.panel      = juce::Colour (0xcc151922);
        p.card       = juce::Colour (0xaa12161d);
        p.stageTint  = juce::Colour (0x66000000);
        return p;
    }

    static int numSkins() { return 6; }

    static UiPalette forSkin (int index)
    {
        switch (index)
        {
            case 1:  return light();
            case 2:  return midnight();
            case 3:  return warm();
            case 4:  return forest();
            case 5:  return customImage();
            default: return dark();
        }
    }
};
