#include "rom_store.h"

#include <algorithm>
#include <cctype>
#include <cstring>

#include <dirent.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_vfs_fat.h>
#include <sys/stat.h>
#include <unistd.h>
#include <wear_levelling.h>

static const char* TAG = "RomStore";

#if NES_HAS_BUILTIN
extern const uint8_t g_builtin_rom_start[] asm("_binary_contra_nes_start");
extern const uint8_t g_builtin_rom_end[] asm("_binary_contra_nes_end");
#endif

namespace rom_store {
namespace {

wl_handle_t s_wl = WL_INVALID_HANDLE;
bool s_mounted = false;

std::string PathFor(const std::string& name) {
    return std::string(kRomDir) + "/" + name;
}

bool HasNesExtension(const std::string& name) {
    if (name.size() < 4) {
        return false;
    }
    std::string ext = name.substr(name.size() - 4);
    for (char& c : ext) {
        c = (char)tolower((unsigned char)c);
    }
    return ext == ".nes";
}

bool LessNoCase(const std::string& a, const std::string& b) {
    size_t n = a.size() < b.size() ? a.size() : b.size();
    for (size_t i = 0; i < n; i++) {
        int ca = tolower((unsigned char)a[i]);
        int cb = tolower((unsigned char)b[i]);
        if (ca != cb) {
            return ca < cb;
        }
    }
    return a.size() < b.size();
}

}  // namespace

bool RomBuffer::Alloc(size_t n) {
    Reset();
    data = (uint8_t*)heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (data == nullptr) {
        data = (uint8_t*)malloc(n);
    }
    size = data != nullptr ? n : 0;
    return data != nullptr;
}

void RomBuffer::Reset() {
    free(data);
    data = nullptr;
    size = 0;
}

bool Init() {
    if (s_mounted) {
        return true;
    }
    esp_vfs_fat_mount_config_t mount_cfg = {};
    mount_cfg.format_if_mount_failed = true;
    mount_cfg.max_files = 8;
    mount_cfg.allocation_unit_size = 4 * 1024;
    mount_cfg.disk_status_check_enable = false;
    mount_cfg.use_one_fat = false;

    esp_err_t err = esp_vfs_fat_spiflash_mount_rw_wl(kMountPoint, "storage", &mount_cfg, &s_wl);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mount failed: %s", esp_err_to_name(err));
        return false;
    }
    s_mounted = true;

    struct stat st = {};
    if (stat(kRomDir, &st) != 0) {
        if (mkdir(kRomDir, 0775) != 0) {
            ESP_LOGE(TAG, "mkdir %s failed", kRomDir);
        }
    }
    ESP_LOGI(TAG, "FAT mounted at %s", kMountPoint);
    return true;
}

bool Mounted() {
    return s_mounted;
}

const uint8_t* BuiltinRom(size_t* len) {
#if NES_HAS_BUILTIN
    if (len != nullptr) {
        *len = (size_t)(g_builtin_rom_end - g_builtin_rom_start);
    }
    return g_builtin_rom_start;
#else
    if (len != nullptr) {
        *len = 0;
    }
    return nullptr;
#endif
}

RomEntry BuiltinEntry() {
    RomEntry e;
#if NES_HAS_BUILTIN
    e.name = "魂斗罗 (简).nes";
    e.size = (size_t)(g_builtin_rom_end - g_builtin_rom_start);
    e.builtin = true;
#endif
    return e;
}

std::vector<RomEntry> List() {
    std::vector<RomEntry> out;
    const RomEntry builtin = BuiltinEntry();
    if (!builtin.name.empty()) {
        out.push_back(builtin);
    }
    const size_t first_fat = out.size();
    if (!s_mounted) {
        return out;
    }
    const std::string builtin_name = builtin.name;
    DIR* dir = opendir(kRomDir);
    if (dir == nullptr) {
        ESP_LOGW(TAG, "opendir %s failed", kRomDir);
        return out;
    }
    struct dirent* ent = nullptr;
    while ((ent = readdir(dir)) != nullptr) {
        const std::string name = ent->d_name;
        if (name == "." || name == ".." || !HasNesExtension(name)) {
            continue;
        }
        /* The same ROM may still exist on FAT from before it was embedded;
         * show the built-in copy only. */
        if (name == builtin_name) {
            continue;
        }
        struct stat st = {};
        if (stat(PathFor(name).c_str(), &st) != 0 || !S_ISREG(st.st_mode)) {
            continue;
        }
        RomEntry e;
        e.name = name;
        e.size = (size_t)st.st_size;
        out.push_back(std::move(e));
        if (out.size() >= 200) {
            break;
        }
    }
    closedir(dir);

    std::sort(out.begin() + first_fat, out.end(), [](const RomEntry& a, const RomEntry& b) {
        return LessNoCase(a.name, b.name);
    });
    return out;
}

bool Read(const std::string& name, RomBuffer& out) {
    const std::string path = PathFor(name);
    FILE* f = fopen(path.c_str(), "rb");
    if (f == nullptr) {
        ESP_LOGE(TAG, "open %s failed", path.c_str());
        return false;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return false;
    }
    long size = ftell(f);
    if (size <= 0 || (size_t)size > kMaxRomSize) {
        ESP_LOGE(TAG, "bad size %ld for %s", size, name.c_str());
        fclose(f);
        return false;
    }
    rewind(f);
    if (!out.Alloc((size_t)size)) {
        ESP_LOGE(TAG, "out of memory for %ld bytes", size);
        fclose(f);
        return false;
    }
    size_t got = fread(out.data, 1, out.size, f);
    fclose(f);
    if (got != out.size) {
        ESP_LOGE(TAG, "short read %u/%u", (unsigned)got, (unsigned)out.size);
        out.Reset();
        return false;
    }
    return true;
}

bool Save(const std::string& name, const uint8_t* data, size_t len) {
    if (!s_mounted || data == nullptr || len < 16 || len > kMaxRomSize) {
        return false;
    }
    const std::string path = PathFor(name);
    FILE* f = fopen(path.c_str(), "wb");
    if (f == nullptr) {
        ESP_LOGE(TAG, "create %s failed", path.c_str());
        return false;
    }
    size_t written = fwrite(data, 1, len, f);
    fclose(f);
    if (written != len) {
        ESP_LOGE(TAG, "short write %u/%u", (unsigned)written, (unsigned)len);
        unlink(path.c_str());
        return false;
    }
    ESP_LOGI(TAG, "saved %s (%u bytes)", name.c_str(), (unsigned)len);
    return true;
}

bool Delete(const std::string& name) {
    if (!s_mounted) {
        return false;
    }
    const std::string path = PathFor(name);
    if (unlink(path.c_str()) != 0) {
        ESP_LOGW(TAG, "unlink %s failed", path.c_str());
        return false;
    }
    ESP_LOGI(TAG, "deleted %s", name.c_str());
    return true;
}

std::string SanitizeName(const std::string& name) {
    std::string out;
    for (unsigned char c : name) {
        if (c < 0x20 || c == 0x7F) {
            continue;
        }
        if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' ||
            c == '<' || c == '>' || c == '|') {
            continue;
        }
        out.push_back((char)c);
        if (out.size() >= 96) {
            break;
        }
    }
    /* Trim leading dots/spaces (hidden files, FAT quirks). */
    size_t start = 0;
    while (start < out.size() && (out[start] == '.' || out[start] == ' ')) {
        start++;
    }
    out = out.substr(start);
    if (out.empty() || out == "." || out == "..") {
        return {};
    }
    if (!HasNesExtension(out)) {
        out += ".nes";
    }
    return out;
}

}  // namespace rom_store
