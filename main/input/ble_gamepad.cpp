#include "ble_gamepad.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include "ble_raw_hid.h"
#include "esp_hid_common.h"
#include "esp_hid_gap.h"
#include "esp_hidh.h"
#include "hid_report.h"
#include "input.h"

#if CONFIG_BT_NIMBLE_ENABLED
#include "host/ble_hs.h"
#include "host/ble_store.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
extern "C" void ble_store_config_init(void);
#endif

static const char* TAG = "BlePad";

namespace ble_gamepad {
namespace {

volatile bool s_connected = false;
volatile bool s_opening = false;
volatile bool s_scanning = false;
volatile bool s_pending_close = false;
bool s_blacklist_on_close = false;
uint8_t s_bad_bda[6] = {};
int64_t s_bad_until_us = 0;
bool s_started = false;
int64_t s_open_started_us = 0;
esp_hidh_dev_t* s_dev = nullptr;
hidreport::Map s_map;
bool s_map_valid = false;
bool s_xbox = false;
std::string s_name;
uint32_t s_reports = 0;

struct Candidate {
    std::string name;
    uint8_t bda[6] = {};
    uint8_t addr_type = 0;
    int rssi = 0;
    esp_hid_usage_t usage = ESP_HID_USAGE_GENERIC;
    bool raw = false;
};

SemaphoreHandle_t s_cand_mutex = nullptr;
Candidate s_candidates[8];
int s_candidate_count = 0;

uint8_t s_req_bda[6] = {};
uint8_t s_req_addr_type = 0;
bool s_req_raw = false;
volatile bool s_req_connect = false;
volatile bool s_req_disconnect = false;
volatile bool s_scan_now = false;

bool NameLooksLikeXbox(const char* name) {
    return name != nullptr && strstr(name, "Xbox") != nullptr;
}

bool NameLooksLikeGamepad(const char* name) {
    if (name == nullptr) {
        return false;
    }
    return strstr(name, "Xbox") != nullptr || strstr(name, "ontroller") != nullptr ||
           strstr(name, "amepad") != nullptr || strstr(name, "oystick") != nullptr;
}

void UpdateCandidates(esp_hid_scan_result_t* results) {
    if (s_cand_mutex == nullptr) {
        return;
    }
    xSemaphoreTake(s_cand_mutex, portMAX_DELAY);
    s_candidate_count = 0;
    for (esp_hid_scan_result_t* r = results; r != nullptr && s_candidate_count < 8; r = r->next) {
        const bool is_pad = r->usage == ESP_HID_USAGE_GAMEPAD ||
                            r->usage == ESP_HID_USAGE_JOYSTICK || NameLooksLikeGamepad(r->name);
        if (!is_pad) {
            continue;
        }
        Candidate& c = s_candidates[s_candidate_count++];
        c.name = r->name != nullptr ? r->name : "(unnamed)";
        memcpy(c.bda, r->bda, sizeof(c.bda));
        c.addr_type = r->ble.addr_type;
        c.rssi = r->rssi;
        c.usage = r->usage;
        c.raw = (r->flags & 1) != 0;
    }
    xSemaphoreGive(s_cand_mutex);
}

void LogFields() {
    int logged = 0;
    for (int i = 0; i < s_map.field_count && logged < 40; i++) {
        const hidreport::Field& f = s_map.fields[i];
        ESP_LOGI(TAG, "field id=%u off=%u bits=%u page=0x%02x usage=0x%02x min=%ld max=%ld",
                 (unsigned)f.report_id, (unsigned)f.bit_offset, (unsigned)f.bit_size,
                 (unsigned)f.usage_page, (unsigned)f.usage, (long)f.logical_min,
                 (long)f.logical_max);
        logged++;
    }
    if (logged == 0) {
        ESP_LOGW(TAG, "no input fields parsed (%d total)", s_map.field_count);
    }
}

void DumpReportMap(const uint8_t* data, size_t len) {
    ESP_LOGI(TAG, "report map: %u bytes", (unsigned)len);
    for (size_t off = 0; off < len && off < 192; off += 32) {
        char line[32 * 3 + 1];
        int pos = 0;
        for (size_t i = off; i < len && i < off + 32; i++) {
            pos += snprintf(line + pos, sizeof(line) - pos, "%02x ", data[i]);
        }
        ESP_LOGI(TAG, "  %03u: %s", (unsigned)off, line);
    }
}

void BuildProfile(esp_hidh_dev_t* dev) {
    s_map_valid = false;
    s_map.field_count = 0;
    memset(s_map.input_bits, 0, sizeof(s_map.input_bits));

    const char* name = esp_hidh_dev_name_get(dev);
    s_name = name != nullptr ? name : "Gamepad";
    s_xbox = NameLooksLikeXbox(s_name.c_str());
    input::SetPadName(s_name.c_str());

    size_t num_maps = 0;
    esp_hid_raw_report_map_t* maps = nullptr;
    esp_err_t err = esp_hidh_dev_report_maps_get(dev, &num_maps, &maps);
    ESP_LOGI(TAG, "open \"%s\", %u report map(s), err=%s", s_name.c_str(), (unsigned)num_maps,
             esp_err_to_name(err));
    esp_hidh_dev_dump(dev, stdout);
    if (err != ESP_OK || maps == nullptr) {
        return;
    }

    for (size_t i = 0; i < num_maps; i++) {
        ESP_LOGI(TAG, "map[%u] data=%p len=%u", (unsigned)i, (const void*)maps[i].data,
                 (unsigned)maps[i].len);
        if (maps[i].data == nullptr || maps[i].len == 0) {
            continue;
        }
        DumpReportMap(maps[i].data, maps[i].len);
    }

    /* Parse into the single global map (it is too large for the 4 KB event
     * callback stack). Prefer the first map that contains gamepad fields. */
    for (size_t i = 0; i < num_maps; i++) {
        if (maps[i].data == nullptr || maps[i].len == 0) {
            continue;
        }
        if (!s_map.Parse(maps[i].data, maps[i].len)) {
            continue;
        }
        int interesting = 0;
        for (int j = 0; j < s_map.field_count; j++) {
            const hidreport::Field& f = s_map.fields[j];
            if (f.usage_page == 0x09 ||
                (f.usage_page == 0x01 && (f.usage == 0x30 || f.usage == 0x31 || f.usage == 0x39))) {
                interesting++;
            }
        }
        if (interesting > 0) {
            s_map_valid = true;
            break;
        }
    }

    if (s_map_valid) {
        ESP_LOGI(TAG, "profile: %d fields%s", s_map.field_count, s_xbox ? " (Xbox layout)" : "");
    } else {
        ESP_LOGW(TAG, "no standard gamepad fields; keeping device for raw report capture");
    }
    LogFields();
}

uint8_t HatToPad(int hat) {
    switch (hat & 0x0F) {
        case 0: return input::kUp;
        case 1: return input::kUp | input::kRight;
        case 2: return input::kRight;
        case 3: return input::kDown | input::kRight;
        case 4: return input::kDown;
        case 5: return input::kDown | input::kLeft;
        case 6: return input::kLeft;
        case 7: return input::kUp | input::kLeft;
        default: return 0;
    }
}

void HandleInput(const esp_hidh_event_data_t* ev) {
    const uint8_t* data = ev->input.data;
    size_t len = ev->input.length;
    const uint8_t report_id = (uint8_t)ev->input.report_id;
    if (len == 0 || data == nullptr) {
        return;
    }

    s_reports++;
    if (s_reports <= 5) {
        char hex[3 * 32 + 1];
        int pos = 0;
        const size_t shown = len < 32 ? len : 32;
        for (size_t i = 0; i < shown && pos < (int)sizeof(hex); i++) {
            pos += snprintf(hex + pos, sizeof(hex) - pos, "%02x ", data[i]);
        }
        ESP_LOGI(TAG, "input id=%u len=%u hex=%s", (unsigned)report_id, (unsigned)len, hex);
    }
    if (s_reports % 300 == 0) {
        ESP_LOGI(TAG, "input id=%u len=%u first=%02x", (unsigned)report_id, (unsigned)len, data[0]);
    }

    /* BLE HID report characteristic values include the Report ID byte when
     * report IDs are used; the descriptor offsets exclude it. */
    if (report_id != 0 && data[0] == report_id) {
        data++;
        len--;
    }

    uint8_t buttons[17] = {};
    int hat = -1;
    bool has_hat = false;
    int32_t axis_x = 0;
    int32_t axis_y = 0;
    const hidreport::Field* fx = nullptr;
    const hidreport::Field* fy = nullptr;

    if (s_map_valid) {
        for (int i = 0; i < s_map.field_count; i++) {
            const hidreport::Field& f = s_map.fields[i];
            if (f.report_id != report_id) {
                continue;
            }
            const uint32_t raw = s_map.Read(data, len, f);
            if (f.usage_page == 0x09) {
                if (!f.variable) {
                    if (raw >= 1 && raw <= 16) {
                        buttons[raw] = 1;
                    }
                } else if (f.usage >= 1 && f.usage <= 16) {
                    buttons[f.usage] = raw != 0 ? 1 : 0;
                }
            } else if (f.usage_page == 0x01) {
                if (f.usage == 0x39) {
                    hat = (int)raw;
                    has_hat = true;
                } else if (f.usage == 0x30) {
                    axis_x = s_map.ReadSigned(data, len, f);
                    fx = &f;
                } else if (f.usage == 0x31) {
                    axis_y = s_map.ReadSigned(data, len, f);
                    fy = &f;
                }
            }
        }
    }

    uint8_t pad = 0;
    if (s_xbox) {
        if (buttons[1]) pad |= input::kA;
        if (buttons[2]) pad |= input::kB;
        if (buttons[7]) pad |= input::kSelect;
        if (buttons[8]) pad |= input::kStart;
    } else {
        if (buttons[1]) pad |= input::kA;
        if (buttons[2]) pad |= input::kB;
        if (buttons[9] || buttons[7]) pad |= input::kSelect;
        if (buttons[10] || buttons[8]) pad |= input::kStart;
        if (!buttons[1] && !buttons[2]) {
            if (buttons[3]) pad |= input::kA;
            if (buttons[4]) pad |= input::kB;
        }
    }

    bool dpad_from_buttons = false;
    if (buttons[11] || buttons[12] || buttons[13] || buttons[14]) {
        dpad_from_buttons = true;
        if (buttons[11]) pad |= input::kUp;
        if (buttons[12]) pad |= input::kDown;
        if (buttons[13]) pad |= input::kLeft;
        if (buttons[14]) pad |= input::kRight;
    }

    if (has_hat) {
        pad |= HatToPad(hat);
    } else if (!dpad_from_buttons) {
        auto axis_dir = [](int32_t value, const hidreport::Field& f) {
            const int32_t center = (f.logical_min + f.logical_max) / 2;
            const int32_t dead = (f.logical_max - f.logical_min) / 6;
            if (value < center - dead) return -1;
            if (value > center + dead) return 1;
            return 0;
        };
        if (fx != nullptr) {
            const int dir = axis_dir(axis_x, *fx);
            if (dir < 0) pad |= input::kLeft;
            if (dir > 0) pad |= input::kRight;
        }
        if (fy != nullptr) {
            const int dir = axis_dir(axis_y, *fy);
            if (dir < 0) pad |= input::kUp;
            if (dir > 0) pad |= input::kDown;
        }
    }

    input::SetPad(pad);
}

void OnHidEvent(void* arg, esp_event_base_t base, int32_t id, void* event_data) {
    (void)arg;
    (void)base;
    auto* param = (esp_hidh_event_data_t*)event_data;
    switch ((esp_hidh_event_t)id) {
        case ESP_HIDH_OPEN_EVENT:
            s_opening = false;
            if (param->open.status == ESP_OK) {
                esp_hidh_dev_t* dev = param->open.dev;
                const char* name = esp_hidh_dev_name_get(dev);
                const esp_hid_usage_t usage = esp_hidh_dev_usage_get(dev);
                s_dev = dev;
                s_connected = true;
                BuildProfile(dev);
                const bool is_pad = usage == ESP_HID_USAGE_GAMEPAD ||
                                    usage == ESP_HID_USAGE_JOYSTICK || NameLooksLikeXbox(name) ||
                                    NameLooksLikeGamepad(name) || s_map_valid;
                ESP_LOGI(TAG, "connected: usage=%s, map=%s", esp_hid_usage_str(usage),
                         s_map_valid ? "yes" : "no");
                if (!is_pad) {
                    /* HID device, but not a gamepad (keyboard/mouse/remote). */
                    ESP_LOGW(TAG, "not a gamepad, closing");
                    s_pending_close = true;
                }
            } else {
                s_connected = false;
                input::SetPadName("");
                ESP_LOGE(TAG, "open failed: %s", esp_err_to_name(param->open.status));
            }
            break;

        case ESP_HIDH_CLOSE_EVENT:
            ESP_LOGI(TAG, "disconnected");
            if (s_blacklist_on_close) {
                const uint8_t* bda = esp_hidh_dev_bda_get(param->close.dev);
                if (bda != nullptr) {
                    memcpy(s_bad_bda, bda, sizeof(s_bad_bda));
                    s_bad_until_us = esp_timer_get_time() + 5 * 60 * 1000 * 1000;
                    ESP_LOGI(TAG, "blacklisting non-gamepad for 5 minutes");
                }
                s_blacklist_on_close = false;
            }
            s_connected = false;
            s_opening = false;
            s_pending_close = false;
            s_dev = nullptr;
            s_map_valid = false;
            input::SetPad(0);
            input::SetPadName("");
            esp_hidh_dev_free(param->close.dev);
            break;

        case ESP_HIDH_INPUT_EVENT:
            HandleInput(param);
            break;

        case ESP_HIDH_BATTERY_EVENT:
            ESP_LOGI(TAG, "battery %u%%", (unsigned)param->battery.level);
            break;

        default:
            break;
    }
}

void HostTask(void* arg) {
    (void)arg;
    ESP_LOGI(TAG, "NimBLE host task started");
    nimble_port_run();
    nimble_port_freertos_deinit();
    vTaskDelete(nullptr);
}

void ScanTask(void* arg) {
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(3000));
    while (true) {
        bool do_connect = false;
        bool do_disconnect = false;
        bool req_raw = false;
        uint8_t req_bda[6] = {};
        uint8_t req_addr_type = 0;
        if (s_cand_mutex != nullptr) {
            xSemaphoreTake(s_cand_mutex, portMAX_DELAY);
            if (s_req_disconnect) {
                s_req_disconnect = false;
                do_disconnect = true;
            }
            if (s_req_connect) {
                s_req_connect = false;
                do_connect = true;
                req_raw = s_req_raw;
                memcpy(req_bda, s_req_bda, sizeof(req_bda));
                req_addr_type = s_req_addr_type;
            }
            xSemaphoreGive(s_cand_mutex);
        }
        if (do_disconnect) {
            ESP_LOGI(TAG, "manual disconnect");
            ble_raw_hid::Disconnect();
            if (s_dev != nullptr) {
                esp_hidh_dev_close(s_dev);
                vTaskDelay(pdMS_TO_TICKS(500));
            }
            input::SetPad(0);
            input::SetPadName("");
        }
        if (do_connect) {
            ESP_LOGI(TAG, "manual connect %02x:%02x:%02x:%02x:%02x:%02x (type %u, raw %d)", req_bda[0],
                     req_bda[1], req_bda[2], req_bda[3], req_bda[4], req_bda[5],
                     (unsigned)req_addr_type, req_raw ? 1 : 0);
            if (s_dev != nullptr) {
                esp_hidh_dev_close(s_dev);
                vTaskDelay(pdMS_TO_TICKS(500));
            }
            if (req_raw) {
                ble_raw_hid::Connect(req_bda, req_addr_type);
            } else {
                s_opening = true;
                s_open_started_us = esp_timer_get_time();
                esp_hidh_dev_t* opened =
                    esp_hidh_dev_open(req_bda, ESP_HID_TRANSPORT_BLE, req_addr_type);
                if (opened == nullptr) {
                    ESP_LOGW(TAG, "manual open returned null");
                    s_opening = false;
                }
            }
        }
        if (s_pending_close && s_dev != nullptr) {
            s_pending_close = false;
            s_blacklist_on_close = true;
            esp_hidh_dev_close(s_dev);
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }
        if (s_opening && esp_timer_get_time() - s_open_started_us > 12 * 1000 * 1000) {
            ESP_LOGW(TAG, "open timeout, retrying next scan");
            s_opening = false;
        }
        if (!s_connected && !s_opening && !ble_raw_hid::Connected() &&
            !ble_raw_hid::Connecting()) {
            size_t count = 0;
            esp_hid_scan_result_t* results = nullptr;
            s_scanning = true;
            esp_err_t err = esp_hid_scan(15, &count, &results);
            s_scanning = false;
            if (err == ESP_OK) {
                UpdateCandidates(results);
            }
            if (err == ESP_OK && count > 0) {
                const esp_hid_scan_result_t* best = nullptr;
                int best_score = 0;
                for (const esp_hid_scan_result_t* r = results; r != nullptr; r = r->next) {
                    if (r->transport != ESP_HID_TRANSPORT_BLE) {
                        continue;
                    }
                    ESP_LOGD(TAG, "scan: \"%s\" usage=%s rssi=%d bda=%02x:%02x:%02x:%02x:%02x:%02x",
                             r->name ? r->name : "?", esp_hid_usage_str(r->usage), r->rssi,
                             r->bda[0], r->bda[1], r->bda[2], r->bda[3], r->bda[4], r->bda[5]);
                    if (s_bad_until_us != 0 && esp_timer_get_time() < s_bad_until_us &&
                        memcmp(r->bda, s_bad_bda, sizeof(s_bad_bda)) == 0) {
                        continue;
                    }
                    int score = 0;
                    if (r->usage == ESP_HID_USAGE_GAMEPAD || r->usage == ESP_HID_USAGE_JOYSTICK) {
                        score = 3;
                    }
                    if (NameLooksLikeXbox(r->name)) {
                        score = 4;
                    } else if (NameLooksLikeGamepad(r->name)) {
                        score = 3;
                    }
                    if (score == 0) {
                        continue;
                    }
                    if (best == nullptr || score > best_score ||
                        (score == best_score && r->rssi > best->rssi)) {
                        best = r;
                        best_score = score;
                    }
                }
                if (best != nullptr) {
                    ESP_LOGI(TAG, "opening \"%s\" (score %d, %02x:%02x:%02x:%02x:%02x:%02x, rssi %d, raw %d)",
                             best->name ? best->name : "?", best_score, best->bda[0], best->bda[1],
                             best->bda[2], best->bda[3], best->bda[4], best->bda[5], best->rssi,
                             (best->flags & 1) ? 1 : 0);
                    if (best->flags & 1) {
                        ble_raw_hid::Connect(best->bda, best->ble.addr_type);
                    } else {
                        s_opening = true;
                        s_open_started_us = esp_timer_get_time();
                        esp_hidh_dev_t* opened = esp_hidh_dev_open(
                            const_cast<uint8_t*>(best->bda), best->transport, best->ble.addr_type);
                        if (opened == nullptr) {
                            ESP_LOGW(TAG, "dev_open returned null, will retry");
                            s_opening = false;
                        }
                    }
                } else {
                    ESP_LOGD(TAG, "scan: %u HID device(s), no unblacklisted candidate", (unsigned)count);
                }
            }
            esp_hid_scan_results_free(results);
        }
        if (s_scan_now) {
            s_scan_now = false;
            vTaskDelay(pdMS_TO_TICKS(200));
        } else {
            vTaskDelay(pdMS_TO_TICKS(1500));
        }
    }
}

}  // namespace

bool Init() {
    if (s_started) {
        return true;
    }
    if (s_cand_mutex == nullptr) {
        s_cand_mutex = xSemaphoreCreateMutex();
    }
    ESP_LOGI(TAG, "starting BLE HID host");
    esp_err_t err = esp_hid_gap_init(HID_HOST_MODE);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "gap init failed: %s", esp_err_to_name(err));
        return false;
    }

    esp_hidh_config_t cfg = {};
    cfg.callback = &OnHidEvent;
    cfg.event_stack_size = 4096;
    cfg.callback_arg = nullptr;
    err = esp_hidh_init(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "hidh init failed: %s", esp_err_to_name(err));
        return false;
    }

#if CONFIG_BT_NIMBLE_ENABLED
    /* esp_hidh defaults to KEYBOARD_ONLY; gamepads normally pair with
     * Just Works (NoInputNoOutput). */
    ble_hs_cfg.sm_io_cap = BLE_HS_IO_NO_INPUT_OUTPUT;
    ble_store_config_init();
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;
    nimble_port_freertos_init(HostTask);
#endif

    xTaskCreatePinnedToCore(ScanTask, "ble_scan", 6144, nullptr, 2, nullptr, 0);
    s_started = true;
    return true;
}

bool Connected() {
    return s_connected || ble_raw_hid::Connected();
}

bool Scanning() {
    return s_scanning;
}

std::vector<DeviceInfo> Devices() {
    std::vector<DeviceInfo> out;
    if (s_cand_mutex == nullptr) {
        return out;
    }
    xSemaphoreTake(s_cand_mutex, portMAX_DELAY);
    for (int i = 0; i < s_candidate_count; i++) {
        const Candidate& c = s_candidates[i];
        DeviceInfo d;
        d.name = c.name;
        char addr[24];
        snprintf(addr, sizeof(addr), "%02x:%02x:%02x:%02x:%02x:%02x", c.bda[0], c.bda[1], c.bda[2],
                 c.bda[3], c.bda[4], c.bda[5]);
        d.addr = addr;
        d.usage = esp_hid_usage_str(c.usage);
        d.rssi = c.rssi;
        d.addr_type = c.addr_type;
        out.push_back(std::move(d));
    }
    xSemaphoreGive(s_cand_mutex);
    return out;
}

void RequestScan() {
    s_scan_now = true;
}

bool ConnectTo(const std::string& addr) {
    unsigned int b[6] = {};
    if (sscanf(addr.c_str(), "%x:%x:%x:%x:%x:%x", &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]) != 6) {
        return false;
    }
    if (s_cand_mutex == nullptr) {
        return false;
    }
    uint8_t bda[6];
    for (int i = 0; i < 6; i++) {
        bda[i] = (uint8_t)b[i];
    }
    xSemaphoreTake(s_cand_mutex, portMAX_DELAY);
    uint8_t addr_type = 0;
    for (int i = 0; i < s_candidate_count; i++) {
        if (memcmp(s_candidates[i].bda, bda, sizeof(bda)) == 0) {
            addr_type = s_candidates[i].addr_type;
            break;
        }
    }
    memcpy(s_req_bda, bda, sizeof(s_req_bda));
    s_req_addr_type = addr_type;
    bool raw = false;
    for (int i = 0; i < s_candidate_count; i++) {
        if (memcmp(s_candidates[i].bda, bda, sizeof(bda)) == 0) {
            raw = s_candidates[i].raw;
            break;
        }
    }
    s_req_raw = raw;
    s_req_connect = true;
    s_scan_now = true;
    if (memcmp(s_bad_bda, bda, sizeof(bda)) == 0) {
        s_bad_until_us = 0;
    }
    xSemaphoreGive(s_cand_mutex);
    return true;
}

void Disconnect() {
    ble_raw_hid::Disconnect();
    if (s_cand_mutex != nullptr) {
        xSemaphoreTake(s_cand_mutex, portMAX_DELAY);
        s_req_disconnect = true;
        xSemaphoreGive(s_cand_mutex);
    }
}

}  // namespace ble_gamepad
