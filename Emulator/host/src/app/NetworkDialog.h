#pragma once

#include "emu/NetworkConfig.h"
#include "panel/Theme.h"

#include <juce_gui_basics/juce_gui_basics.h>

namespace kickemu
{
    /**
        The network settings of the LINK port: mode (NAT, vmnet, TAP, bridge,
        macvtap, unplugged), the host side it attaches to, port forwards and the
        guest MAC. Shows what each mode needs from the host before it is chosen.
    */
    class NetworkDialog : public juce::Component
    {
    public:
        NetworkDialog (const NetworkConfig& current, const Theme& theme, bool machineRunning);

        /** Called with the new settings; restartNow when the user asked to power-cycle. */
        std::function<void (const NetworkConfig&, bool restartNow)> onApply;

        void paint (juce::Graphics& g) override;
        void resized() override;

        /** Opens the dialog as a window on top of `parent`. */
        static void show (juce::Component* parent, const NetworkConfig& current, const Theme& theme, bool running,
                          std::function<void (const NetworkConfig&, bool)> onApply);

    private:
        void modeChanged();
        NetworkConfig collect() const;
        void apply (bool restart);

        const Theme& theme;
        NetworkConfig initial;
        juce::StringArray modes;

        juce::Label modeLabel { {}, "Mode" }, ifLabel { {}, "Host interface" }, bridgeLabel { {}, "Bridge" },
                    helperLabel { {}, "Bridge helper" }, sshLabel { {}, "SSH to localhost port" },
                    fwdLabel { {}, "Port forwards" }, macLabel { {}, "MAC address" };
        juce::ComboBox mode, iface;
        juce::TextEditor bridge, helper, ssh, forwards, mac;
        juce::Label requirement;
        juce::TextButton applyRestart { "Apply and restart" }, applyLater { "Apply at next power-on" }, cancel { "Cancel" };
        juce::String error;
    };
}
