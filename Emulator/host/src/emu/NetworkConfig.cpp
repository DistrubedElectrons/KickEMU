#include "emu/NetworkConfig.h"

#include <ifaddrs.h>
#include <net/if.h>

namespace kickemu
{
    NetworkConfig NetworkConfig::fromConfig (const Config& c)
    {
        NetworkConfig n;
        n.mode = c.getString ("qemu.net.mode");
        if (n.mode.isEmpty())
            n.mode = c.getBool ("qemu.network", false) ? "user" : "none";
        n.hostInterface = c.getString ("qemu.net.interface");
        n.bridge = c.getString ("qemu.net.bridge", "br0");
        n.helper = c.getString ("qemu.net.bridge_helper");
        n.mac = c.getString ("qemu.net.mac");
        n.sshPort = c.getInt ("qemu.ssh_host_port", 0);
        n.forwards = c.getStringArray ("qemu.net.forwards");
        return n;
    }

    void NetworkConfig::toConfig (Config& c) const
    {
        c.set ("qemu.net.mode", mode);
        c.set ("qemu.network", mode != "none");
        c.set ("qemu.net.interface", hostInterface);
        c.set ("qemu.net.bridge", bridge);
        c.set ("qemu.net.bridge_helper", helper);
        c.set ("qemu.net.mac", mac);
        c.set ("qemu.ssh_host_port", sshPort);
        juce::Array<juce::var> f;
        for (auto& r : forwards)
            f.add (r);
        c.set ("qemu.net.forwards", f);
    }

    juce::var NetworkConfig::toVar() const
    {
        auto* o = new juce::DynamicObject();
        o->setProperty ("mode", mode);
        o->setProperty ("interface", hostInterface);
        o->setProperty ("bridge", bridge);
        o->setProperty ("bridge_helper", helper);
        o->setProperty ("mac", mac);
        o->setProperty ("ssh_port", sshPort);
        o->setProperty ("forwards", forwards.joinIntoString (" "));
        return juce::var (o);
    }

    NetworkConfig NetworkConfig::fromVar (const juce::var& v, NetworkConfig n)
    {
        auto str = [&v] (const char* k, juce::String& target) { if (v.hasProperty (k)) target = v[k].toString(); };
        str ("mode", n.mode);
        str ("interface", n.hostInterface);
        str ("bridge", n.bridge);
        str ("bridge_helper", n.helper);
        str ("mac", n.mac);
        if (v.hasProperty ("ssh_port"))
            n.sshPort = (int) v["ssh_port"];
        if (v.hasProperty ("forwards"))
            n.forwards = juce::StringArray::fromTokens (v["forwards"].toString(), " ,", {});
        n.forwards.removeEmptyStrings();
        return n;
    }

    juce::StringArray NetworkConfig::modesForHost (bool macOS)
    {
        if (macOS)
            return { "user", "vmnet-shared", "vmnet-host", "vmnet-bridged", "none" };
        return { "user", "bridge", "tap", "macvtap", "none" };
    }

    juce::String NetworkConfig::describe (const juce::String& m)
    {
        if (m == "none") return "Unplugged";
        if (m == "user") return "NAT (user mode)";
        if (m == "vmnet-shared") return "Shared NAT (vmnet)";
        if (m == "vmnet-host") return "Host only (vmnet)";
        if (m == "vmnet-bridged") return "Bridged to a host interface (vmnet)";
        if (m == "tap") return "TAP interface";
        if (m == "bridge") return "Linux bridge";
        if (m == "macvtap") return "macvtap on a host interface";
        return m;
    }

    juce::String NetworkConfig::requirement (const juce::String& m)
    {
        if (m == "user") return "Works everywhere. The KICK sits behind NAT; forward ports to reach it (ssh: localhost:<port>).";
        if (m.startsWith ("vmnet")) return "Gets an address from macOS (DHCP). Apple only allows vmnet for root: start KickEmu with sudo.";
        if (m == "tap") return "Needs a tap interface you may open, e.g. ip tuntap add tap0 mode tap user $USER && ip link set tap0 up.";
        if (m == "bridge") return "Joins a Linux bridge through qemu-bridge-helper (setuid root, bridge listed in /etc/qemu/bridge.conf).";
        if (m == "macvtap") return "The KICK appears on the LAN. Needs a macvtap link with a device you may open: "
                                  "ip link add link eth0 name macvtap0 type macvtap mode bridge && chown $USER /dev/tap$(cat /sys/class/net/macvtap0/ifindex).";
        if (m == "none") return "No cable in the LINK port.";
        return {};
    }

    juce::String NetworkConfig::nicArgument (int tapFd, juce::String& error) const
    {
        juce::String nic;
        if (mode == "none" || mode.isEmpty())
            return {};
        if (mode == "user")
        {
            nic = "user";
            if (sshPort > 0)
                nic << ",hostfwd=tcp:127.0.0.1:" << sshPort << "-:22";
            for (auto& f : forwards)
            {
                // tcp:8080-:80 or udp:5000-:5000; bind host side to loopback unless given.
                auto rule = f.trim();
                auto proto = rule.upToFirstOccurrenceOf (":", false, false);
                auto rest = rule.fromFirstOccurrenceOf (":", false, false);
                if ((proto != "tcp" && proto != "udp") || ! rest.containsChar ('-'))
                {
                    error = "Port forward must look like tcp:8080-:80, got: " + rule;
                    return {};
                }
                nic << ",hostfwd=" << proto << ":" << (rest.startsWithChar (':') || rest.containsChar ('.') ? rest : "127.0.0.1:" + rest);
            }
        }
        else if (mode == "vmnet-shared" || mode == "vmnet-host")
        {
            nic = mode;
        }
        else if (mode == "vmnet-bridged")
        {
            if (hostInterface.isEmpty()) { error = "Bridged vmnet needs a host interface (e.g. en0)"; return {}; }
            nic = "vmnet-bridged,ifname=" + hostInterface;
        }
        else if (mode == "tap")
        {
            if (hostInterface.isEmpty()) { error = "TAP mode needs an interface name (e.g. tap0)"; return {}; }
            nic = "tap,ifname=" + hostInterface + ",script=no,downscript=no";
        }
        else if (mode == "bridge")
        {
            nic = "bridge,br=" + (bridge.isNotEmpty() ? bridge : juce::String ("br0"));
            if (helper.isNotEmpty())
                nic << ",helper=" << helper;
        }
        else if (mode == "macvtap")
        {
            if (tapFd < 0) { error = "macvtap: cannot open " + macvtapDevice (hostInterface).getFullPathName(); return {}; }
            nic = "tap,fd=" + juce::String (tapFd);
        }
        else
        {
            error = "Unknown network mode: " + mode;
            return {};
        }
        if (mac.isNotEmpty())
            nic << ",mac=" << mac;
        return nic;
    }

    juce::StringArray NetworkConfig::hostInterfaces()
    {
        juce::StringArray names;
        ifaddrs* list = nullptr;
        if (getifaddrs (&list) == 0)
        {
            for (auto* a = list; a != nullptr; a = a->ifa_next)
                if ((a->ifa_flags & IFF_LOOPBACK) == 0)
                    names.addIfNotAlreadyThere (a->ifa_name);
            freeifaddrs (list);
        }
        names.sortNatural();
        return names;
    }

    juce::File NetworkConfig::macvtapDevice (const juce::String& ifname)
    {
        auto index = juce::File ("/sys/class/net/" + ifname + "/ifindex").loadFileAsString().trim();
        return index.isEmpty() ? juce::File() : juce::File ("/dev/tap" + index);
    }

    juce::String NetworkConfig::summary() const
    {
        auto s = describe (mode);
        if ((mode == "vmnet-bridged" || mode == "tap" || mode == "macvtap") && hostInterface.isNotEmpty())
            s << " (" << hostInterface << ")";
        if (mode == "bridge")
            s << " (" << bridge << ")";
        if (mode == "user" && sshPort > 0)
            s << ", ssh on localhost:" << sshPort;
        return s;
    }
}
