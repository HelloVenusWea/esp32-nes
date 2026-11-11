#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace rom_store {

constexpr const char* kMountPoint = "/storage";
constexpr const char* kRomDir = "/storage/roms";
constexpr size_t kMaxRomSize = 4 * 1024 * 1024;

struct RomBuffer {
    uint8_t* data = nullptr;
    size_t size = 0;

    RomBuffer() = default;
    ~RomBuffer() { Reset(); }
    RomBuffer(const RomBuffer&) = delete;
    RomBuffer& operator=(const RomBuffer&) = delete;
    RomBuffer(RomBuffer&& o) noexcept : data(o.data), size(o.size) {
        o.data = nullptr;
        o.size = 0;
    }
    RomBuffer& operator=(RomBuffer&& o) noexcept {
        if (this != &o) {
            Reset();
            data = o.data;
            size = o.size;
            o.data = nullptr;
            o.size = 0;
        }
        return *this;
    }
    bool Alloc(size_t n);
    void Reset();
    bool empty() const { return data == nullptr || size == 0; }
};

struct RomEntry {
    std::string name;
    size_t size = 0;
    bool builtin = false;
};

/* Mounts the FAT partition (wear-levelled) and creates /storage/roms. */
bool Init();
bool Mounted();

/* Optional built-in ROM; null/empty when this build has no embedded ROM. */
const uint8_t* BuiltinRom(size_t* len);
RomEntry BuiltinEntry();

/* Sorted list, optional built-in entry first. */
std::vector<RomEntry> List();

bool Read(const std::string& name, RomBuffer& out);
bool Save(const std::string& name, const uint8_t* data, size_t len);
bool Delete(const std::string& name);

/* Returns empty string if the name is not acceptable. */
std::string SanitizeName(const std::string& name);

}  // namespace rom_store
