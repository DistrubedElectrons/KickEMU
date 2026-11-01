#include "LinuxSideParser.h"

// BUFFER_RX_SIZE from Kick/modules/input_events/ComConfig.hpp
static constexpr int linuxRxBufferSize = 20;

LinuxSideParser::LinuxSideParser()
    : mgr (CPktMgr::EN_COM_CHANNEL_USB, rx, linuxRxBufferSize, tx, linuxRxBufferSize, 60)
{
    mgr.Init();
}

std::vector<CPktMgr::HWMsg_t> LinuxSideParser::onSerialData (const std::vector<unsigned char>& chunk)
{
    std::vector<CPktMgr::HWMsg_t> out;

    // Same early exits as the original.
    int rxLength = (int) chunk.size();
    if (rxLength == 0 || rxLength < 6)
        return out;

    std::vector<char> serialData (chunk.begin(), chunk.end());
    char* byteArray = serialData.data();
    CPktMgr& refobjFrameHdl = mgr;

    while (rxLength)
    {
        int pktIndex = 0;
        int packetLength = byteArray[5 + pktIndex] + 7;

        if ((rxLength >= packetLength) && (byteArray[0] == 'M') && (byteArray[1] == 'T'))
        {
            while (packetLength)
            {
                if (packetLength > 1)
                    refobjFrameHdl.PushByteToRxFifo ((uint8_t) byteArray[0]);
                byteArray++;
                packetLength--;
                rxLength--;
            }
        }
        else
        {
            rxLength = 0;
        }

        if (refobjFrameHdl.IsFrameRcvComplete() == TRUE)
        {
            refobjFrameHdl.SetFrameRcvComplete (FALSE);
            CPktMgr::HWMsg_t rxMsg;
            rxMsg.pktType = refobjFrameHdl.GetRxPktType();
            rxMsg.pktCmd = refobjFrameHdl.GetRxCommand();
            rxMsg.pktDir = refobjFrameHdl.GetPktDir();

            int size = (int) refobjFrameHdl.GetRxFrameSize();
            while (size - 1)
            {
                rxMsg.data.push_back ((int) refobjFrameHdl.PopUint8FromRxFifo());
                size--;
            }
            refobjFrameHdl.ClearReceiver();

            if (rxMsg.pktDir == CPktMgr::enumPacketDir::EN_PKT_REQUEST)
                out.push_back (rxMsg);
        }
        else
        {
            refobjFrameHdl.ClearReceiver();
        }
    }
    return out;
}
