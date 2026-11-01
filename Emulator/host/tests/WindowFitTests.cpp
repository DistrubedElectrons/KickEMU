#include "config/WindowFit.h"

using namespace kickemu;

class WindowFitTests : public juce::UnitTest
{
public:
    WindowFitTests() : juce::UnitTest ("Window fit", "app") {}

    /** Aspect of the device area inside a window of the given bounds. */
    static double deviceAspect (const WindowFit& f, juce::Rectangle<int> b)
    {
        return (double) (b.getWidth() - f.chromeWidth) / (double) (b.getHeight() - f.chromeHeight);
    }

    void runTest() override
    {
        WindowFit fit;
        fit.aspect = 1.0;           // the top view is square
        fit.chromeWidth = 16;
        fit.chromeHeight = 92;

        beginTest ("Free when no aspect is set");
        {
            WindowFit free;
            juce::Rectangle<int> b (10, 20, 800, 500);
            expect (free.constrain (b, WindowFit::Edges::right) == b);
        }

        beginTest ("Dragging a side edge follows the width");
        {
            auto b = fit.constrain ({ 100, 100, 816, 300 }, WindowFit::Edges::right);
            expectEquals (b.getWidth(), 816);
            expectEquals (b.getHeight(), 800 + 92);
            expect (b.getPosition() == juce::Point<int> (100, 100));
        }

        beginTest ("Dragging the top edge follows the height and keeps the bottom");
        {
            juce::Rectangle<int> b (100, 50, 300, 692);
            auto r = fit.constrain (b, WindowFit::Edges::top);
            expectEquals (r.getHeight(), 692);
            expectEquals (r.getWidth(), 600 + 16);
            expectEquals (r.getBottom(), b.getBottom());
        }

        beginTest ("A corner drag keeps the larger device dimension, anchored at the opposite corner");
        {
            juce::Rectangle<int> b (100, 100, 1016, 600);
            auto r = fit.constrain (b, WindowFit::Edges::left | WindowFit::Edges::top);
            expectWithinAbsoluteError (deviceAspect (fit, r), 1.0, 0.002);
            expectEquals (r.getRight(), b.getRight());
            expectEquals (r.getBottom(), b.getBottom());
            expectEquals (r.getWidth(), 1016);
        }

        beginTest ("Snap keeps the window inside the screen area");
        {
            juce::Rectangle<int> screen (0, 25, 1512, 957);
            auto r = fit.snap ({ 200, 100, 1400, 900 }, screen);
            expectWithinAbsoluteError (deviceAspect (fit, r), 1.0, 0.002);
            expect (screen.contains (r));
            expectEquals (r.getHeight(), screen.getHeight());

            fit.aspect = 1.25;
            auto wide = fit.snap ({ 50, 50, 700, 700 }, screen);
            expectWithinAbsoluteError (deviceAspect (fit, wide), 1.25, 0.003);
            expectEquals (wide.getWidth(), 700);
            expect (wide.getPosition() == juce::Point<int> (50, 50));
        }
    }
};

static WindowFitTests windowFitTests;
