#pragma once

#include <cstddef>
#include <cstdint>

namespace hidreport {

struct Field {
    uint8_t report_id = 0;
    uint16_t bit_offset = 0;  // bits from the start of the report payload (report ID excluded)
    uint8_t bit_size = 0;
    uint32_t usage_page = 0;
    uint32_t usage = 0;
    int32_t logical_min = 0;
    int32_t logical_max = 0;
    bool constant = false;
    bool variable = true;
};

/* Minimal HID report descriptor parser: enough to locate gamepad buttons,
 * hat switch and X/Y axes in the input reports. */
struct Map {
    static constexpr int kMaxFields = 128;
    Field fields[kMaxFields];
    int field_count = 0;
    uint16_t input_bits[256] = {};  // total input bits per report ID

    bool Parse(const uint8_t* descriptor, size_t length);
    uint32_t Read(const uint8_t* payload, size_t length, const Field& f) const;
    int32_t ReadSigned(const uint8_t* payload, size_t length, const Field& f) const;
};

}  // namespace hidreport
