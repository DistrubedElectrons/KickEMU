#include "app/NetworkDialog.h"

namespace kickemu
{
    NetworkDialog::NetworkDialog (const NetworkConfig& current, const Theme& t, bool running)
        : theme (t), initial (current)
    {
       #if JUCE_MAC
        modes = NetworkConfig::modesForHost (true);
       #else
        modes = NetworkConfig::modesForHost (false);
       #endif
        modes.addIfNotAlreadyThere (current.mode);   // keep a mode set by hand in the config

        for (int i = 0; i < modes.size(); ++i)
            mode.addItem (NetworkConfig::describe (modes[i]), i + 1);
        mode.setSelectedId (modes.indexOf (current.mode) + 1, juce::dontSendNotification);
        mode.onChange = [this] { modeChanged(); };

        iface.setEditableText (true);
        iface.addItemList (NetworkConfig::hostInterfaces(), 1);
        iface.setText (current.hostInterface, juce::dontSendNotification);

        bridge.setText (current.bridge);
        helper.setText (current.helper);
        helper.setTextToShowWhenEmpty ("QEMU default (libexec/qemu-bridge-helper)", juce::Colours::grey);
        ssh.setText (current.sshPort > 0 ? juce::String (current.sshPort) : juce::String());
        ssh.setTextToShowWhenEmpty ("off", juce::Colours::grey);
        ssh.setInputRestrictions (5, "0123456789");
        forwards.setText (current.forwards.joinIntoString (" "));
        forwards.setTextToShowWhenEmpty ("tcp:8080-:80 udp:5000-:5000", juce::Colours::grey);
        mac.setText (current.mac);
        mac.setTextToShowWhenEmpty ("fused OCOTP address", juce::Colours::grey);

        requirement.setJustificationType (juce::Justification::topLeft);
        requirement.setFont (theme.uiFont (12.0f));
        requirement.setColour (juce::Label::textColourId, juce::Colours::white.withAlpha (0.7f));

        for (auto* l : { &modeLabel, &ifLabel, &bridgeLabel, &helperLabel, &sshLabel, &fwdLabel, &macLabel })
        {
            l->setFont (theme.uiFont (12.5f));
            l->setJustificationType (juce::Justification::centredRight);
            addAndMakeVisible (l);
        }
        for (auto* c : std::initializer_list<juce::Component*> { &mode, &iface, &bridge, &helper, &ssh, &forwards, &mac,
                                                               &requirement, &applyRestart, &applyLater, &cancel })
            addAndMakeVisible (c);

        applyRestart.setEnabled (running);
        applyRestart.onClick = [this] { apply (true); };
        applyLater.setButtonText (running ? "Apply at next power-on" : "Apply");
        applyLater.onClick = [this] { apply (false); };
        cancel.onClick = [this] { if (auto* w = findParentComponentOfClass<juce::DialogWindow>()) w->exitModalState (0); };

        modeChanged();
        setSize (560, 430);
    }

    void NetworkDialog::modeChanged()
    {
        const auto m = modes[mode.getSelectedId() - 1];
        const bool user = m == "user", needsIf = m == "vmnet-bridged" || m == "tap" || m == "macvtap", br = m == "bridge";
        for (auto* c : std::initializer_list<juce::Component*> { &sshLabel, &ssh, &fwdLabel, &forwards })
            c->setVisible (user);
        ifLabel.setVisible (needsIf);
        iface.setVisible (needsIf);
        for (auto* c : std::initializer_list<juce::Component*> { &bridgeLabel, &bridge, &helperLabel, &helper })
            c->setVisible (br);
        macLabel.setVisible (m != "none");
        mac.setVisible (m != "none");
        requirement.setText (NetworkConfig::requirement (m), juce::dontSendNotification);
        error.clear();
        resized();
        repaint();
    }

    NetworkConfig NetworkDialog::collect() const
    {
        NetworkConfig n = initial;
        n.mode = modes[mode.getSelectedId() - 1];
        n.hostInterface = iface.getText().trim();
        n.bridge = bridge.getText().trim();
        n.helper = helper.getText().trim();
        n.sshPort = ssh.getText().getIntValue();
        n.forwards = juce::StringArray::fromTokens (forwards.getText(), " ,\n", {});
        n.forwards.removeEmptyStrings();
        n.mac = mac.getText().trim();
        return n;
    }

    void NetworkDialog::apply (bool restart)
    {
        auto n = collect();
        // Validate with the same code that builds the command line (macvtap's fd is opened at start).
        juce::String problem;
        n.nicArgument (n.mode == "macvtap" ? 0 : -1, problem);
        if (n.mac.isNotEmpty() && ! n.mac.matchesWildcard ("??:??:??:??:??:??", true))
            problem = "MAC address must look like 02:00:00:12:34:56";
        if (problem.isNotEmpty())
        {
            error = problem;
            repaint();
            return;
        }
        if (onApply)
            onApply (n, restart);
        if (auto* w = findParentComponentOfClass<juce::DialogWindow>())
            w->exitModalState (1);
    }

    void NetworkDialog::paint (juce::Graphics& g)
    {
        g.fillAll (theme.appBackground);
        if (error.isNotEmpty())
        {
            g.setColour (juce::Colour (0xffff6b5b));
            g.setFont (theme.uiFont (12.5f, true));
            g.drawFittedText (error, getLocalBounds().reduced (20).removeFromBottom (74).removeFromTop (30), juce::Justification::centredLeft, 2);
        }
    }

    void NetworkDialog::resized()
    {
        auto r = getLocalBounds().reduced (20);
        auto buttons = r.removeFromBottom (32);
        cancel.setBounds (buttons.removeFromRight (90));
        buttons.removeFromRight (8);
        applyLater.setBounds (buttons.removeFromRight (180));
        buttons.removeFromRight (8);
        applyRestart.setBounds (buttons.removeFromRight (150));
        r.removeFromBottom (40);   // error line

        auto row = [&r] (juce::Label& label, juce::Component& field, int h = 28)
        {
            if (! field.isVisible())
                return;
            auto line = r.removeFromTop (h);
            label.setBounds (line.removeFromLeft (150));
            line.removeFromLeft (10);
            field.setBounds (line);
            r.removeFromTop (8);
        };
        row (modeLabel, mode);
        requirement.setBounds (r.removeFromTop (58).withTrimmedLeft (160));
        r.removeFromTop (6);
        row (ifLabel, iface);
        row (bridgeLabel, bridge);
        row (helperLabel, helper);
        row (sshLabel, ssh);
        row (fwdLabel, forwards);
        row (macLabel, mac);
    }

    void NetworkDialog::show (juce::Component* parent, const NetworkConfig& current, const Theme& theme, bool running,
                              std::function<void (const NetworkConfig&, bool)> onApply)
    {
        auto* content = new NetworkDialog (current, theme, running);
        content->onApply = std::move (onApply);
        juce::DialogWindow::LaunchOptions o;
        o.content.setOwned (content);
        o.dialogTitle = "Network (LINK port)";
        o.dialogBackgroundColour = theme.appBackground;
        o.componentToCentreAround = parent;
        o.useNativeTitleBar = true;
        o.resizable = false;
        o.launchAsync();
    }
}
