#include "instruments/InstrumentsView.h"

namespace kickemu
{
    namespace
    {
        constexpr int barHeight = 34;

        class InstrumentWindow : public juce::DocumentWindow
        {
        public:
            InstrumentWindow (InstrumentHub& hub, const Theme& theme, InstrumentsView::Page page)
                : juce::DocumentWindow ("KickEmu Instruments", theme.chrome, juce::DocumentWindow::allButtons)
            {
                auto* view = new InstrumentsView (hub, theme, false);
                view->showPage (page);
                view->setSize (760, 620);
                setContentOwned (view, true);
                setUsingNativeTitleBar (true);
                setResizable (true, true);
                setResizeLimits (420, 420, 4000, 4000);
                centreWithSize (getWidth(), getHeight());
            }

            void closeButtonPressed() override { setVisible (false); }
        };
    }

    InstrumentsView::InstrumentsView (InstrumentHub& h, const Theme& t, bool allowPopOut)
        : hub (h), theme (t), scopeView (h, t), spectrumView (h, t), meterView (h, t), midiView (h, t)
    {
        const char* names[] = { "Scope", "Spectrum", "Meter", "MIDI" };
        for (int i = 0; i < 4; ++i)
        {
            auto* b = pageButtons.add (new juce::TextButton (names[i]));
            b->setRadioGroupId (0x5c0e);
            b->setClickingTogglesState (true);
            b->setColour (juce::TextButton::buttonColourId, juce::Colour (0xff1f1f23));
            b->setColour (juce::TextButton::buttonOnColourId, theme.accent.withAlpha (0.3f));
            b->setColour (juce::TextButton::textColourOnId, juce::Colours::white);
            b->setColour (juce::TextButton::textColourOffId, theme.chromeDim);
            b->setConnectedEdges ((i > 0 ? juce::Button::ConnectedOnLeft : 0) | (i < 3 ? juce::Button::ConnectedOnRight : 0));
            b->onClick = [this, i] { showPage ((Page) i); };
            addAndMakeVisible (b);
        }
        if (allowPopOut)
        {
            windowButton.setTooltip ("Open the instruments in a separate, resizable window");
            windowButton.onClick = [this] { openWindow(); };
            addAndMakeVisible (windowButton);
        }
        addChildComponent (scopeView);
        addChildComponent (spectrumView);
        addChildComponent (meterView);
        addChildComponent (midiView);
        showPage (scope);
    }

    InstrumentsView::~InstrumentsView() = default;

    void InstrumentsView::showPage (Page p)
    {
        page = p;
        juce::Component* views[] = { &scopeView, &spectrumView, &meterView, &midiView };
        for (int i = 0; i < 4; ++i)
        {
            views[i]->setVisible (i == (int) p);
            pageButtons[i]->setToggleState (i == (int) p, juce::dontSendNotification);
        }
    }

    void InstrumentsView::openWindow()
    {
        if (window == nullptr)
            window = std::make_unique<InstrumentWindow> (hub, theme, page);
        window->setVisible (true);
        window->toFront (true);
    }

    void InstrumentsView::paint (juce::Graphics& g)
    {
        g.fillAll (theme.chrome);
        g.setColour (theme.chromeLine);
        g.drawHorizontalLine (barHeight + 6, 0.0f, (float) getWidth());
    }

    void InstrumentsView::resized()
    {
        auto r = getLocalBounds();
        auto bar = r.removeFromTop (barHeight + 6).reduced (10, 6);
        if (windowButton.isVisible())
        {
            windowButton.setBounds (bar.removeFromRight (72));
            bar.removeFromRight (10);
        }
        const int w = bar.getWidth() / 4;
        for (int i = 0; i < 4; ++i)
            pageButtons[i]->setBounds (i == 3 ? bar : bar.removeFromLeft (w));
        for (auto* v : std::initializer_list<juce::Component*> { &scopeView, &spectrumView, &meterView, &midiView })
            v->setBounds (r);
    }
}
