/** The Daisy's own USB socket as a composite device:
 *   - serial port (CDC-ACM): the monitor link, logs, ESC BOOT for flashing
 *   - USB audio input (UAC1): the master output, stereo, 48 kHz, 16 bit
 *  Both are standard classes, so macOS, iPadOS, Windows and Linux need
 *  no driver. The Daisy's audio clock is the master: each 1 ms packet
 *  carries 47, 48 or 49 frames to follow it ("asynchronous" endpoint).
 */
#pragma once
#include <cstdint>
#include <cstddef>

namespace usbdev
{
/** Starts the device (replaces libDaisy's UsbHandle on the built-in port) */
void Init();

/* ---- serial ---- */
using RxFn = void (*)(const uint8_t* data, uint32_t len);
void SetRx(RxFn fn);
bool SerialReady(); // a host has configured us
/** Queues data; false while the last transfer is still going */
bool SerialSend(const uint8_t* data, uint16_t len);

/* ---- audio ---- */
/** Audio callback: the block that just went to the codec */
void AudioPush(const float* l, const float* r, size_t n);
bool AudioStreaming(); // the host is recording / monitoring us
struct AudioStats
{
    uint32_t packets, underruns, overruns, incomplete;
    int      fill; // frames waiting
};
AudioStats Stats();
} // namespace usbdev
