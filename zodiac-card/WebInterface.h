/*
 * WebInterfaceComputerCard — base class adding USB MIDI SysEx to ComputerCard.
 * Uses the Arduino RP2040 core's built-in MIDIUSB library.
 * Runs MIDI I/O on core1 for bidirectional communication with a browser via WebMIDI.
 *
 * Subclasses override:
 *   MIDICore()              — called continuously on core1; send data here
 *   ProcessIncomingSysEx()  — handle messages from the web interface
 *   ProcessSample()         — 48kHz audio processing (inherited from ComputerCard)
 */

#ifndef WEB_INTERFACE_H
#define WEB_INTERFACE_H

#include "ComputerCard.h"
#include "MIDIUSB.h"
#include "USB.h"
#include "pico/multicore.h"
#include "pico/time.h"

class WebInterfaceComputerCard : public ComputerCard
{
public:
    // Call from setup() after EnableNormalisationProbe(), before Run()
    void beginMIDI(const char *name = "Zodiac Card")
    {
        USB.setProduct(name);   // OS-level USB device name
        MidiUSB.setName(name);  // MIDI jack name
        MidiUSB.begin();
        multicore_launch_core1(core1);
    }

    static void core1()
    {
        ((WebInterfaceComputerCard *)ThisPtr())->USBCore();
    }

    void SendSysEx(uint8_t *data, uint32_t size)
    {
        uint8_t header[] = {0xF0, MIDI_MANUFACTURER_ID};
        uint8_t footer[] = {0xF7};
        MidiUSB.write(header, 2);
        MidiUSB.write(data, size);
        MidiUSB.write(footer, 1);
        MidiUSB.flush();
    }

    void SendCC(uint8_t cc, uint8_t val)
    {
        uint8_t msg[] = {0xB0, (uint8_t)(cc & 0x7F), (uint8_t)(val & 0x7F)};
        MidiUSB.write(msg, 3);
        MidiUSB.flush();
    }

    void USBCore()
    {
        while (1)
        {
            // Read and parse incoming MIDI packets
            midiEventPacket_t pkt = MidiUSB.read();
            while (pkt.header != 0)
            {
                ParseMIDIPacket(pkt);
                pkt = MidiUSB.read();
            }

            MIDICore();
            sleep_us(500);
        }
    }

    void ParseMIDIPacket(midiEventPacket_t pkt)
    {
        uint8_t cin = pkt.header & 0x0F;
        switch (cin)
        {
        case 0x04: // SysEx start or continue (3 bytes)
            if (sysexLen < sysexBufSize) sysexBuf[sysexLen++] = pkt.byte1;
            if (sysexLen < sysexBufSize) sysexBuf[sysexLen++] = pkt.byte2;
            if (sysexLen < sysexBufSize) sysexBuf[sysexLen++] = pkt.byte3;
            break;
        case 0x05: // SysEx end with 1 byte
            if (sysexLen < sysexBufSize) sysexBuf[sysexLen++] = pkt.byte1;
            DispatchSysEx();
            break;
        case 0x06: // SysEx end with 2 bytes
            if (sysexLen < sysexBufSize) sysexBuf[sysexLen++] = pkt.byte1;
            if (sysexLen < sysexBufSize) sysexBuf[sysexLen++] = pkt.byte2;
            DispatchSysEx();
            break;
        case 0x07: // SysEx end with 3 bytes
            if (sysexLen < sysexBufSize) sysexBuf[sysexLen++] = pkt.byte1;
            if (sysexLen < sysexBufSize) sysexBuf[sysexLen++] = pkt.byte2;
            if (sysexLen < sysexBufSize) sysexBuf[sysexLen++] = pkt.byte3;
            DispatchSysEx();
            break;
        case 0x09: // Note On
        {
            uint8_t note = pkt.byte2 & 0x7F;
            uint8_t vel  = pkt.byte3 & 0x7F;
            if (vel > 0) ProcessIncomingNoteOn(note, vel);
            break;
        }
        }
    }

    void DispatchSysEx()
    {
        // Expected buffer: F0 7D [data...] F7
        if (sysexLen >= 3 && sysexBuf[0] == 0xF0 &&
            sysexBuf[1] == MIDI_MANUFACTURER_ID &&
            sysexBuf[sysexLen - 1] == 0xF7)
        {
            ProcessIncomingSysEx(sysexBuf + 2, sysexLen - 3);
        }
        sysexLen = 0;
    }

    virtual void MIDICore() {}
    virtual void ProcessIncomingSysEx(uint8_t * /*data*/, uint32_t /*size*/) {}
    virtual void ProcessIncomingNoteOn(uint8_t /*note*/, uint8_t /*velocity*/) {}

private:
    static constexpr uint8_t MIDI_MANUFACTURER_ID = 0x7D; // prototyping/private use
    static constexpr unsigned sysexBufSize = 1024;
    uint8_t sysexBuf[1024] = {};
    unsigned sysexLen = 0;
};

#endif // WEB_INTERFACE_H
