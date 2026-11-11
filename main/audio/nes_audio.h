#pragma once

#include <cstdint>

namespace nesaudio {

/* Pulls 24 kHz mono PCM from the InfoNES APU ring and writes it to ES8388. */
bool Start();
void Stop();
uint32_t PushedSamples();

}  // namespace nesaudio
