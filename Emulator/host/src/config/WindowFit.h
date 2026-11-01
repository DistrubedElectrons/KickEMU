#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace kickemu
{
    /**
        Window geometry that shows the device without empty bars: the area left
        after the fixed chrome (title bar, status line, side panel, margins) keeps
        the aspect ratio of the device view. Pure arithmetic, so the window code
        and the tests share it.
    */
    struct WindowFit
    {
        double aspect = 0.0;        // device width / height; 0 = window is free
        int chromeWidth = 0;        // pixels around the device, horizontally
        int chromeHeight = 0;       // and vertically

        /** Which window edges the user is dragging (combine with |). */
        enum Edges { none = 0, left = 1, right = 2, top = 4, bottom = 8 };

        bool isActive() const { return aspect > 0.0; }

        /**
            Adjusts bounds proposed by a resize so the device area keeps the aspect.
            A side drag follows the width, a top/bottom drag the height, a corner
            drag the larger of the two; the edges opposite the drag stay put.
        */
        juce::Rectangle<int> constrain (juce::Rectangle<int> proposed, int edges) const;

        /** Reshapes a window (keeping its width where possible) and fits it into screenArea. */
        juce::Rectangle<int> snap (juce::Rectangle<int> current, juce::Rectangle<int> screenArea) const;
    };
}
