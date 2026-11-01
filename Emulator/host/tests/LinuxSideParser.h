#pragma once

#include <PktMgr/PktMgr.hpp>
#include <vector>

/**
    A copy of the receive path of HwSerialData::changeListenerCallback() in
    Kick/modules/input_events/input_events.cpp, kept as close to the original as
    possible (including its packet length rule data[5] + 7 and the dropped last
    byte), so the tests check that MCU frames are accepted by the real parser.
*/
class LinuxSideParser
{
public:
    LinuxSideParser();

    /** Feeds one read() worth of bytes, as the serial port listener gets them.
        Returns the dispatched messages (requests only, like the original). */
    std::vector<CPktMgr::HWMsg_t> onSerialData (const std::vector<unsigned char>& chunk);

private:
    unsigned char rx[20] {}, tx[20] {};
    CPktMgr mgr;
};
