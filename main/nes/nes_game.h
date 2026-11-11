#pragma once

#include <cstddef>
#include <cstdint>

namespace nesgame {

/* Blocking: runs until the ROM quits (BOOT long press / Select+Start).
 * frame_limit > 0 is a bring-up self-test: stop after N frames.
 * debug enables the frame thumbnail and periodic progress logs. */
bool Run(const uint8_t* rom, size_t len, const char* display_name,
         uint32_t frame_limit = 0, bool debug = false);

}  // namespace nesgame
