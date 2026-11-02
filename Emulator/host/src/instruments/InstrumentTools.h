#pragma once

#include "app/Session.h"
#include "automation/ToolRegistry.h"

namespace kickemu
{
    /** Registers the instrument tools (measure, spectrum, midi_monitor, instrument_view);
        see Emulator/docs/automation.md. */
    void addInstrumentTools (ToolRegistry& tools, Session& session);
}
