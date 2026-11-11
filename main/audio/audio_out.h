#pragma once

#include <cstddef>
#include <cstdint>

#include <driver/i2c_master.h>
#include <esp_err.h>

namespace audio {

/* ES8388 output only, 24 kHz / 16-bit / mono (mono is duplicated to both slots). */
bool Init(i2c_master_bus_handle_t i2c_bus);
bool Start();
void Stop();
void SetVolume(int volume_0_100);

/* Blocking write. Returns the number of samples accepted (0 on error). */
size_t Write(const int16_t* data, size_t samples);

/* Bring-up/diagnostic: 1 kHz sine for `ms` milliseconds, then mute. */
void PlayTestTone(int ms);

}  // namespace audio
