#include "emu/PanelState.h"
#include "config/Layout.h"

using namespace kickemu;

class PanelStateTests : public juce::UnitTest
{
public:
    PanelStateTests() : juce::UnitTest ("Panel state", "panel") {}

    void runTest() override
    {
        beginTest ("Transparent pads are unlit");
        {
            PanelState s;
            s.pads[0].mode = PanelState::padOn;
            s.pads[0].colourId = 15;
            expectEquals (s.padAppearance (0, 0.0, 0.5).colourId, -1);
        }

        beginTest ("Colour modes: dim / off / velocity / full");
        {
            PanelState s;
            auto& p = s.pads[1];
            p.mode = PanelState::padSampleAssigned;
            p.colourId = 6;

            s.colourMode = PanelState::modeDimVelocity;
            expectWithinAbsoluteError (s.padAppearance (1, 0, 0.5).brightness, PanelState::dimLevel, 0.001f);
            p.pressed = true;
            p.pressVelocity = 127;
            expectWithinAbsoluteError (s.padAppearance (1, 0, 0.5).brightness, 1.0f, 0.001f);
            p.pressVelocity = 64;
            expectWithinAbsoluteError (s.padAppearance (1, 0, 0.5).brightness, 64.0f / 127.0f, 0.001f);

            s.colourMode = PanelState::modeOffFull;
            p.pressed = false;
            expectEquals (s.padAppearance (1, 0, 0.5).brightness, 0.0f);
            p.pressed = true;
            expectEquals (s.padAppearance (1, 0, 0.5).brightness, 1.0f);

            // Empty pads: OFF_DIM_* keeps them dark, DIM_* dims them.
            p.pressed = false;
            p.mode = PanelState::padSampleNotAssigned;
            s.colourMode = PanelState::modeOffDimVelocity;
            expectEquals (s.padAppearance (1, 0, 0.5).brightness, 0.0f);
            s.colourMode = PanelState::modeDimFull;
            expectWithinAbsoluteError (s.padAppearance (1, 0, 0.5).brightness, PanelState::dimLevel, 0.001f);
        }

        beginTest ("Blink alternates and can use a second colour");
        {
            PanelState s;
            auto& p = s.pads[2];
            p.mode = PanelState::padOnBlink;
            p.colourId = 12;
            expectEquals (s.padAppearance (2, 0.1, 1.0).brightness, 1.0f);
            expectEquals (s.padAppearance (2, 0.6, 1.0).brightness, 0.0f);
            p.blinkTwoColours = true;
            p.blinkOffColourId = 8;
            expectEquals (s.padAppearance (2, 0.6, 1.0).colourId, 8);
        }

        beginTest ("Slider bar and centred level");
        {
            PanelState s;
            s.sliderMode = PanelState::sliderBar;
            s.sliderPercent = 50;
            expectEquals (s.sliderLed (0, 16), 1.0f);
            expectEquals (s.sliderLed (7, 16), 1.0f);
            expectEquals (s.sliderLed (8, 16), 0.0f);
            s.sliderMode = PanelState::sliderLevel;
            s.sliderPercent = 100;
            expectEquals (s.sliderLed (15, 16), 1.0f);
            expectEquals (s.sliderLed (0, 16), 0.0f);
        }

        beginTest ("Layout file parses and has every control");
        {
            Layout layout;
            auto r = layout.parse (juce::File (KICKEMU_TEST_LAYOUT).loadFileAsString());
            expect (r.wasOk(), r.getErrorMessage());
            expectEquals ((int) layout.ofType ("pad").size(), 16);
            expectEquals ((int) layout.ofType ("encoder").size(), 4);
            expectEquals ((int) layout.ofType ("round_button").size(), 5);
            expectEquals ((int) layout.ofType ("jog_key").size(), 4);
            expect (layout.find ("jog") != nullptr);
            expect (layout.find ("screen") != nullptr);
            expect (layout.find ("slider") != nullptr);
            expectEquals (layout.rear.connectors.size(), 15);

            // Pads are numbered from the bottom left.
            auto* pad1 = layout.find ("pad1");
            auto* pad13 = layout.find ("pad13");
            auto* pad4 = layout.find ("pad4");
            expect (pad1->rect.getY() > pad13->rect.getY());
            expect (pad4->rect.getX() > pad1->rect.getX());

            // Every button id is unique.
            std::set<int> ids;
            for (auto& c : layout.controls)
                if (c.hw >= 0 && c.type != "pad")
                    expect (ids.insert (c.hw).second, "duplicate hw id " + juce::String (c.hw));
        }
    }
};

static PanelStateTests panelStateTests;
