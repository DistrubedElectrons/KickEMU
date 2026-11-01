#include "config/WindowFit.h"

namespace kickemu
{
    juce::Rectangle<int> WindowFit::constrain (juce::Rectangle<int> proposed, int edges) const
    {
        if (! isActive())
            return proposed;

        const int dw = juce::jmax (1, proposed.getWidth() - chromeWidth);
        const int dh = juce::jmax (1, proposed.getHeight() - chromeHeight);
        const bool horizontal = (edges & (left | right)) != 0;
        const bool vertical = (edges & (top | bottom)) != 0;

        // Which dimension leads: the dragged one, or for a corner the one that grew more
        bool byWidth = horizontal;
        if (horizontal == vertical)
            byWidth = (double) dw / (double) dh >= aspect;

        int w = proposed.getWidth(), h = proposed.getHeight();
        if (byWidth)
            h = juce::roundToInt ((double) dw / aspect) + chromeHeight;
        else
            w = juce::roundToInt ((double) dh * aspect) + chromeWidth;

        // Keep the edges opposite the drag where they were
        const int x = (edges & left) ? proposed.getRight() - w : proposed.getX();
        const int y = (edges & top) ? proposed.getBottom() - h : proposed.getY();
        return { x, y, w, h };
    }

    juce::Rectangle<int> WindowFit::snap (juce::Rectangle<int> current, juce::Rectangle<int> screenArea) const
    {
        if (! isActive())
            return current.constrainedWithin (screenArea);

        auto r = constrain (current, right);
        if (r.getHeight() > screenArea.getHeight())
        {
            // Too tall for the screen: let the height decide
            r = constrain (r.withHeight (screenArea.getHeight()), bottom);
        }
        if (r.getWidth() > screenArea.getWidth())
            r = constrain (r.withWidth (screenArea.getWidth()), right);
        return r.constrainedWithin (screenArea);
    }
}
