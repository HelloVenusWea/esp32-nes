#pragma once

#include <cstddef>
#include <cstdint>

/* InfoNES platform hooks used by the player/display/audio modules. */

void NesSys_SetRom(const uint8_t* data, size_t len);
void NesSys_SetPad(uint8_t pad_mask);
void NesSys_SetFrameLimit(uint32_t limit);
void NesSys_SetFrameSink(void (*sink)(const uint16_t* frame));
void NesSys_ResetState();
void NesSys_RequestStop();
void NesSys_StartPacing();
void NesSys_StopPacing();
uint32_t NesSys_FrameCount();

/* Bring-up aid: dump a brightness thumbnail at frame 120 and log progress. */
void NesSys_SetDebug(bool enabled);

uint32_t NesSys_AudioRead(int16_t* out, uint32_t max_samples);
uint32_t NesSys_AudioAvailable();
uint32_t NesSys_AudioOverruns();
