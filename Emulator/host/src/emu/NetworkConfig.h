#pragma once

#include "config/Config.h"

namespace kickemu
{
    /**
        How the KICK's Ethernet port (the emulated ENET + AR8033) reaches the host.

        | mode          | host | QEMU                         | needs                                   |
        |---------------|------|------------------------------|-----------------------------------------|
        | none          | any  | no NIC backend               |                                         |
        | user          | any  | -nic user (NAT, slirp)       | nothing; port forwards for inbound      |
        | vmnet-shared  | mac  | -nic vmnet-shared            | root (Apple restricts vmnet)            |
        | vmnet-host    | mac  | -nic vmnet-host              | root                                    |
        | vmnet-bridged | mac  | -nic vmnet-bridged,ifname=   | root                                    |
        | tap           | lnx  | -nic tap,ifname=,script=no   | an existing tap the user may open       |
        | bridge        | lnx  | -nic bridge,br=              | qemu-bridge-helper setuid + bridge.conf |
        | macvtap       | lnx  | -nic tap,fd= (/dev/tapN)     | a macvtap link the user may open        |

        Stored under qemu.net.* in the configuration; the legacy qemu.network flag
        means "user" when no mode is set.
    */
    struct NetworkConfig
    {
        juce::String mode = "user";
        juce::String hostInterface;     // vmnet-bridged: en0; tap: tap0; macvtap: macvtap0
        juce::String bridge = "br0";    // bridge mode
        juce::String helper;            // bridge mode: qemu-bridge-helper path (empty = QEMU default)
        juce::String mac;               // guest MAC override (empty = the fused OCOTP MAC)
        int sshPort = 0;                // user mode: host port forwarded to guest 22 (0 = none)
        juce::StringArray forwards;     // user mode: extra "tcp:8080-:80" rules

        static NetworkConfig fromConfig (const Config& config);
        void toConfig (Config& config) const;
        juce::var toVar() const;
        static NetworkConfig fromVar (const juce::var& v, NetworkConfig base);

        /** Modes offered on this host OS, in menu order. */
        static juce::StringArray modesForHost (bool macOS);
        static juce::String describe (const juce::String& mode);
        /** What the mode needs from the host, shown next to the choice. */
        static juce::String requirement (const juce::String& mode);

        /** The -nic argument; tapFd is the open /dev/tapN for macvtap (-1 otherwise).
            Returns an empty string for "none" and sets error when the mode cannot work. */
        juce::String nicArgument (int tapFd, juce::String& error) const;

        /** Host network interfaces (name list for the bridged/tap choices). */
        static juce::StringArray hostInterfaces();

        /** macvtap: the character device of a macvtap link (/dev/tap<ifindex>). */
        static juce::File macvtapDevice (const juce::String& ifname);

        juce::String summary() const;
    };
}
