#include "hid_report.h"

#include <cstring>

namespace hidreport {
namespace {

int32_t SignExtend(uint32_t value, int size) {
    switch (size) {
        case 1:
            return (int32_t)(int8_t)(value & 0xFF);
        case 2:
            return (int32_t)(int16_t)(value & 0xFFFF);
        case 4:
            return (int32_t)value;
        default:
            return 0;
    }
}

}  // namespace

bool Map::Parse(const uint8_t* descriptor, size_t length) {
    field_count = 0;
    memset(input_bits, 0, sizeof(input_bits));
    if (descriptor == nullptr || length == 0) {
        return false;
    }

    uint32_t usage_page = 0;
    uint32_t report_size = 0;
    uint32_t report_count = 0;
    uint8_t report_id = 0;
    int32_t logical_min = 0;
    int32_t logical_max = 0;
    uint32_t usages[32] = {};
    uint32_t usage_pages[32] = {};
    int usage_count = 0;
    uint32_t usage_min = 0;
    uint32_t usage_max = 0;
    bool has_usage_min = false;
    bool has_usage_max = false;
    uint32_t bit_offset = 0;

    for (size_t i = 0; i < length;) {
        const uint8_t prefix = descriptor[i++];
        if (prefix == 0xFE) {  // long item
            if (i + 2 > length) {
                break;
            }
            const uint8_t data_size = descriptor[i];
            i += 2 + data_size;
            continue;
        }
        int size = prefix & 0x03;
        if (size == 3) {
            size = 4;
        }
        const int type = (prefix >> 2) & 0x03;
        const int tag = prefix >> 4;
        if (i + (size_t)size > length) {
            break;
        }
        uint32_t value = 0;
        for (int b = 0; b < size; b++) {
            value |= (uint32_t)descriptor[i + b] << (8 * b);
        }
        i += size;

        if (type == 1) {  // Global
            switch (tag) {
                case 0x0: usage_page = value; break;
                case 0x1: logical_min = SignExtend(value, size); break;
                case 0x2: logical_max = SignExtend(value, size); break;
                case 0x7: report_size = value; break;
                case 0x8:
                    report_id = (uint8_t)(value & 0xFF);
                    bit_offset = input_bits[report_id];
                    break;
                case 0x9: report_count = value; break;
                default: break;  // push/pop/physical/unit: ignored
            }
        } else if (type == 2) {  // Local
            switch (tag) {
                case 0x0:
                    if (usage_count < 32) {
                        uint32_t usage = value;
                        uint32_t page = usage_page;
                        if (size == 4) {  // extended usage carries its own page
                            page = value >> 16;
                            usage = value & 0xFFFF;
                        }
                        usages[usage_count] = usage;
                        usage_pages[usage_count] = page;
                        usage_count++;
                    }
                    break;
                case 0x1: usage_min = value; has_usage_min = true; break;
                case 0x2: usage_max = value; has_usage_max = true; break;
                default: break;
            }
        } else if (type == 0) {  // Main
            if (tag == 0x8) {    // Input
                const bool constant = (value & 0x01) != 0;
                const bool variable = (value & 0x02) != 0;
                for (uint32_t n = 0; n < report_count; n++) {
                    uint32_t usage = 0;
                    uint32_t page = usage_page;
                    if (n < (uint32_t)usage_count) {
                        usage = usages[n];
                        page = usage_pages[n];
                    } else if (has_usage_min) {
                        usage = usage_min + (n - (uint32_t)usage_count);
                    } else if (usage_count > 0) {
                        usage = usages[usage_count - 1];
                        page = usage_pages[usage_count - 1];
                    }
                    if (!constant && report_size > 0 && field_count < kMaxFields) {
                        Field& f = fields[field_count++];
                        f.report_id = report_id;
                        f.bit_offset = (uint16_t)bit_offset;
                        f.bit_size = (uint8_t)(report_size > 32 ? 32 : report_size);
                        f.usage_page = page;
                        f.usage = usage;
                        f.logical_min = logical_min;
                        f.logical_max = logical_max;
                        f.constant = constant;
                        f.variable = variable;
                    }
                    bit_offset += report_size;
                }
                input_bits[report_id] = (uint16_t)(bit_offset > 0xFFFF ? 0xFFFF : bit_offset);
            }
            /* Output/Feature items do not share the input bit stream. */
            usage_count = 0;
            has_usage_min = false;
            has_usage_max = false;
            (void)usage_max;
        }
    }
    return field_count > 0;
}

uint32_t Map::Read(const uint8_t* payload, size_t length, const Field& f) const {
    uint32_t value = 0;
    if (payload == nullptr || f.bit_size == 0 || f.bit_size > 32) {
        return 0;
    }
    for (int i = 0; i < f.bit_size; i++) {
        const uint32_t bit = (uint32_t)f.bit_offset + (uint32_t)i;
        if (bit / 8 >= length) {
            return 0;
        }
        if (payload[bit / 8] & (1u << (bit % 8))) {
            value |= (1u << i);
        }
    }
    return value;
}

int32_t Map::ReadSigned(const uint8_t* payload, size_t length, const Field& f) const {
    const uint32_t value = Read(payload, length, f);
    if (f.bit_size > 0 && f.bit_size < 32 && (value & (1u << (f.bit_size - 1)))) {
        return (int32_t)(value | (~0u << f.bit_size));
    }
    return (int32_t)value;
}

}  // namespace hidreport
