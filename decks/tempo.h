/** The tempo of a track and where its beats fall, for loops by the beat and,
 *  later, SYNC. One steady tempo is assumed (produced dance music); a track
 *  without a steady beat gets bpm 0. Not real time: a loader thread runs it.
 *  No Linux or file code here; tested in engine_test.cpp with made tracks. */
#pragma once
#include <cstdint>

namespace dk
{
constexpr float kBpmLo = 80.f, kBpmHi = 160.f; // the answer lies in here: 174 comes out as 87, a page can double it

struct Tempo
{
    float  bpm   = 0; // 0: no steady beat found
    double beat0 = 0; // frame of the first beat line at or after the start
    float  sure  = 0; // the best tempo's fit over the middling one's (about 1: nothing stands out)
};

/** pcm: 44.1 kHz stereo, interleaved */
Tempo FindTempo(const int16_t* pcm, uint32_t frames);
} // namespace dk
