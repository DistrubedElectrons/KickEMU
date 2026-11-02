#pragma once

#include "app/Session.h"
#include "automation/ToolRegistry.h"
#include "config/Layout.h"

namespace kickemu
{
    /**
        Registers the emulator's automation tools (machine power, console, screen,
        front panel, touch, MIDI, CV, SD slot, audio files, USB, raw QMP) against a
        session. The registry is served over MCP on stdio (--mcp) and on the
        automation socket; see Emulator/docs/automation.md.
    */
    class AutomationTools
    {
    public:
        AutomationTools (Session& session, const Layout& layout);

        const ToolRegistry& registry() const { return tools; }

        /** Text sent to MCP clients in the initialize response. */
        static juce::String instructions();

    private:
        void addMachineTools();
        void addConsoleTools();
        void addScreenTools();
        void addPanelTools();
        void addIoTools();
        void addMediaTools();

        /** Runs a QMP command and waits for the reply. */
        ToolResult qmp (const juce::String& command, const juce::var& arguments, int timeoutMs = 10000);

        /** Runs fn on the message thread and waits (Session state lives there). */
        bool onMessageThread (std::function<void()> fn, int timeoutMs = 10000);

        /** Sends one multitouch phase for a contact, as the screen component does. */
        ToolResult touchPhase (int slot, int phase, int x, int y);

        const LayoutControl* control (const juce::String& id) const;

        Session& session;
        const Layout& layout;
        ToolRegistry tools;

        std::mutex touchLock;
        std::array<int, 10> trackingIds {};
        std::array<bool, 10> touching {};
        std::array<juce::Point<int>, 10> touchAxes {};
        int nextTrackingId = 1;
        std::atomic<bool> shiftHeld { false };
    };
}
