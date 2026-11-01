#include "app/SettingsView.h"
#include "app/CommandLine.h"

namespace kickemu
{
    namespace
    {
        void styleEditor (juce::TextEditor& e, const Theme& t, bool mono)
        {
            e.setFont (mono ? t.monoFont (11.5f) : t.uiFont (12.5f));
            e.setColour (juce::TextEditor::backgroundColourId, juce::Colour (0xff0e0e10));
            e.setColour (juce::TextEditor::textColourId, t.chromeText);
            e.setColour (juce::TextEditor::outlineColourId, t.chromeLine);
            e.setColour (juce::TextEditor::focusedOutlineColourId, t.accent);
        }

        void header (juce::Graphics& g, const Theme& t, juce::Rectangle<int> r, const juce::String& text)
        {
            g.setColour (t.chromeDim);
            g.setFont (t.uiFont (11.0f, true));
            g.drawText (text.toUpperCase(), r, juce::Justification::bottomLeft);
        }
    }

    // ------------------------------------------------------------------------------------

    IoView::IoView (Session& s, AppSettings& st, const Theme& t) : session (s), settings (st), theme (t)
    {
        addAndMakeVisible (midiIn);
        addAndMakeVisible (midiOut);
        midiIn.onChange = [this]
        {
            const int i = midiIn.getSelectedItemIndex() - 1;
            auto id = juce::isPositiveAndBelow (i, inputs.size()) ? inputs[i].identifier : juce::String();
            session.midi.setInputDevice (id);
            settings.set ("midiInputDevice", id);
        };
        midiOut.onChange = [this]
        {
            const int i = midiOut.getSelectedItemIndex() - 1;
            auto id = juce::isPositiveAndBelow (i, outputs.size()) ? outputs[i].identifier : juce::String();
            session.midi.setOutputDevice (id);
            settings.set ("midiOutputDevice", id);
        };

        const int channels = session.cv.getNumChannels();
        for (int ch = 0; ch < channels; ++ch)
        {
            auto* slider = cvInputs.add (new juce::Slider (juce::Slider::LinearHorizontal, juce::Slider::TextBoxRight));
            slider->setRange (0.0, session.cv.getFullScale(), 0.01);
            slider->setTextValueSuffix (" V");
            slider->setTextBoxStyle (juce::Slider::TextBoxRight, false, 64, 20);
            slider->setColour (juce::Slider::trackColourId, theme.accent);
            const double saved = settings.getDouble ("cvIn" + juce::String (ch), 0.0);
            slider->setValue (saved, juce::dontSendNotification);
            slider->onValueChange = [this, ch, slider]
            {
                session.cv.setInputVolts (ch, (float) slider->getValue());
                settings.set ("cvIn" + juce::String (ch), slider->getValue());
            };
            addAndMakeVisible (slider);

            auto* out = cvOutputs.add (new juce::Label());
            out->setFont (theme.monoFont (12.0f));
            out->setColour (juce::Label::textColourId, juce::Colour (0xff4fd2ff));
            out->setJustificationType (juce::Justification::centredRight);
            addAndMakeVisible (out);
        }

        refreshDevices();
        startTimerHz (8);
    }

    IoView::~IoView() { stopTimer(); }

    void IoView::refreshDevices()
    {
        inputs.clear();
        outputs.clear();
        // Our own virtual ports would loop back into the guest; hide them.
        for (auto& d : juce::MidiInput::getAvailableDevices())
            if (d.name != session.midi.getVirtualOutName())
                inputs.add (d);
        for (auto& d : juce::MidiOutput::getAvailableDevices())
            if (d.name != session.midi.getVirtualInName())
                outputs.add (d);

        midiIn.clear (juce::dontSendNotification);
        midiOut.clear (juce::dontSendNotification);
        midiIn.addItem ("Virtual port only", 1);
        midiOut.addItem ("Virtual port only", 1);
        for (int i = 0; i < inputs.size(); ++i)
        {
            midiIn.addItem (inputs[i].name, i + 2);
            if (inputs[i].identifier == session.midi.getInputDevice())
                midiIn.setSelectedId (i + 2, juce::dontSendNotification);
        }
        for (int i = 0; i < outputs.size(); ++i)
        {
            midiOut.addItem (outputs[i].name, i + 2);
            if (outputs[i].identifier == session.midi.getOutputDevice())
                midiOut.setSelectedId (i + 2, juce::dontSendNotification);
        }
        if (midiIn.getSelectedId() == 0) midiIn.setSelectedId (1, juce::dontSendNotification);
        if (midiOut.getSelectedId() == 0) midiOut.setSelectedId (1, juce::dontSendNotification);
    }

    void IoView::timerCallback()
    {
        for (int ch = 0; ch < cvOutputs.size(); ++ch)
            cvOutputs[ch]->setText (juce::String (session.cv.getOutputVolts (ch), 3) + " V", juce::dontSendNotification);

        auto& p = session.panel;
        juce::String t;
        t << "UART1: " << (p.uartConnected.load() ? "connected" : "waiting for QEMU")
          << "   link: " << (p.hostLinked.load() ? "established" : "not yet") << "\n"
          << "Packets in " << (juce::int64) p.packetsIn.load() << ", out " << (juce::int64) p.packetsOut.load()
          << ", framing errors " << (juce::int64) p.framingErrors.load() << "\n"
          << "BPM " << (p.bpm.load() > 0 ? juce::String (p.bpm.load()) : juce::String ("-"))
          << "   pad colour mode " << p.colourMode.load()
          << "   aftertouch " << (p.aftertouchEnabled.load() ? "on" : "off") << "\n"
          << "MIDI from guest " << (juce::int64) session.midi.messagesFromGuest.load()
          << ", to guest " << (juce::int64) session.midi.messagesToGuest.load()
          << "   CV link " << (session.cv.isConnected() ? "connected" : "waiting");
        if (t != mcuText)
        {
            mcuText = t;
            repaint (mcuArea);
        }
    }

    void IoView::paint (juce::Graphics& g)
    {
        g.fillAll (theme.chrome);
        header (g, theme, midiHeader, "MIDI (DIN on UART3)");
        header (g, theme, cvHeader, "CV / Gate: input voltage and DAC output");
        header (g, theme, mcuHeader, "Control surface");

        g.setFont (theme.uiFont (12.0f));
        g.setColour (theme.chromeDim);
        auto r = midiIn.getBounds().withX (16).withWidth (midiIn.getX() - 20);
        g.drawText ("Also receive from", r, juce::Justification::centredLeft);
        g.drawText ("Also send to", r.withY (midiOut.getY()), juce::Justification::centredLeft);
        g.drawText ("Virtual ports: \"" + session.midi.getVirtualOutName() + "\" (source), \"" + session.midi.getVirtualInName() + "\" (destination)",
                    midiOut.getBounds().withX (16).withWidth (getWidth() - 32).translated (0, 26), juce::Justification::centredLeft);

        for (int ch = 0; ch < cvInputs.size(); ++ch)
        {
            auto b = cvInputs[ch]->getBounds();
            g.setColour (theme.chromeText);
            g.drawText ("CH " + juce::String (ch + 1), juce::Rectangle<int> (16, b.getY(), 44, b.getHeight()), juce::Justification::centredLeft);
        }

        g.setColour (theme.chromeText);
        g.setFont (theme.monoFont (11.5f));
        auto lines = juce::StringArray::fromLines (mcuText);
        auto a = mcuArea;
        for (auto& l : lines)
            g.drawText (l, a.removeFromTop (18), juce::Justification::centredLeft);
    }

    void IoView::resized()
    {
        auto r = getLocalBounds().reduced (16, 8);
        midiHeader = r.removeFromTop (24);
        r.removeFromTop (4);
        midiIn.setBounds (r.removeFromTop (26).withTrimmedLeft (120));
        r.removeFromTop (6);
        midiOut.setBounds (r.removeFromTop (26).withTrimmedLeft (120));
        r.removeFromTop (34);

        cvHeader = r.removeFromTop (24);
        r.removeFromTop (4);
        for (int ch = 0; ch < cvInputs.size(); ++ch)
        {
            auto row = r.removeFromTop (26);
            cvOutputs[ch]->setBounds (row.removeFromRight (74));
            cvInputs[ch]->setBounds (row.withTrimmedLeft (48));
            r.removeFromTop (2);
        }
        r.removeFromTop (12);
        mcuHeader = r.removeFromTop (24);
        mcuArea = r.removeFromTop (80);
    }

    // ------------------------------------------------------------------------------------

    SettingsView::SettingsView (Session& s, AppSettings& st, const Theme& t, juce::StringArray keyboardMap)
        : session (s), settings (st), theme (t)
    {
        presentationLabel.setText ("Presentation background", juce::dontSendNotification);
        presentationLabel.setColour (juce::Label::textColourId, theme.chromeDim);
        presentationLabel.setFont (theme.uiFont (12.0f));
        addAndMakeVisible (presentationLabel);

        styleEditor (presentationColour, theme, true);
        presentationColour.setText (settings.getString ("presentationBackground",
                                                        session.config.getString ("host.ui.presentation_background", "#0c0c0e")), false);
        presentationColour.onReturnKey = presentationColour.onFocusLost = [this]
        {
            auto c = Theme::parseColour (presentationColour.getText(), theme.appBackground);
            settings.set ("presentationBackground", "#" + c.toDisplayString (false));
            if (onPresentationColour)
                onPresentationColour (c);
        };
        addAndMakeVisible (presentationColour);

        styleEditor (info, theme, true);
        info.setMultiLine (true);
        info.setReadOnly (true);
        auto& c = session.config;
        juce::String text;
        text << CommandLine::versionText() << "\n"
             << "License: AGPL-3.0-or-later; JUCE under its AGPLv3 option\n\n"
             << "Config   " << c.getFile().getFullPathName() << "\n"
             << "Profile  " << (c.getActiveProfile().isEmpty() ? juce::String ("(none)") : c.getActiveProfile()) << "\n"
             << "Machine  " << c.getString ("qemu.machine") << ", boot " << c.getString ("qemu.boot") << ", accel " << c.getString ("qemu.accel") << "\n"
             << "Run dir  " << c.getRunDir().getFullPathName() << "\n"
             << "Settings " << settings.file().getFile().getFullPathName() << "\n";
        info.setText (text, false);
        addAndMakeVisible (info);

        styleEditor (keys, theme, true);
        keys.setMultiLine (true);
        keys.setReadOnly (true);
        keys.setText ("Keyboard (click the device first)\n\n" + keyboardMap.joinIntoString ("\n")
                      + "\n\nApp shortcuts\n"
                        "Cmd+P      presentation mode\n"
                        "Cmd+Shift+S  snapshot PNG at 2x\n"
                        "Cmd+L      side panel\n"
                        "Cmd+1/2/3  top / rear / front view\n"
                        "Cmd+0      fit to window\n"
                        "Cmd+R      restart QEMU\n"
                        "Cmd+.      stop QEMU\n", false);
        addAndMakeVisible (keys);

        revealSnapshots.onClick = [this]
        {
            auto dir = session.config.resolvePath (session.config.getString ("host.ui.snapshot_dir", "~/Pictures/KickEmu"));
            dir.createDirectory();
            dir.revealToUser();
        };
        addAndMakeVisible (revealSnapshots);
    }

    void SettingsView::paint (juce::Graphics& g) { g.fillAll (theme.chrome); }

    void SettingsView::resized()
    {
        auto r = getLocalBounds().reduced (16, 12);
        auto row = r.removeFromTop (26);
        presentationLabel.setBounds (row.removeFromLeft (170));
        presentationColour.setBounds (row.removeFromLeft (100));
        r.removeFromTop (8);
        revealSnapshots.setBounds (r.removeFromTop (26).removeFromLeft (190));
        r.removeFromTop (12);
        info.setBounds (r.removeFromTop (130));
        r.removeFromTop (12);
        keys.setBounds (r);
    }
}
