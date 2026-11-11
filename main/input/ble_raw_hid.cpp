#include "ble_raw_hid.h"

#include <cstdio>
#include <cstring>

#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <host/ble_gap.h>
#include <host/ble_gatt.h>
#include <host/ble_hs.h>
#include <host/ble_uuid.h>
#include <os/os_mbuf.h>

#include "input.h"

static const char* TAG = "RawHid";

namespace ble_raw_hid {
namespace {

constexpr uint16_t kHidService = 0x1812;
constexpr uint16_t kProtocolMode = 0x2A4B;
constexpr uint16_t kReport = 0x2A4D;
constexpr uint16_t kCccd = 0x2902;
constexpr uint16_t kNoConn = 0xFFFF;

uint16_t s_conn = kNoConn;
volatile bool s_connected = false;
volatile bool s_connecting = false;
volatile bool s_discovering = false;
volatile bool s_ready = false;

uint16_t s_hid_start = 0;
uint16_t s_hid_end = 0;
uint16_t s_protocol_handle = 0;
uint16_t s_reports[8] = {};
uint16_t s_cccd[8] = {};
uint8_t s_report_count = 0;
uint8_t s_dsc_index = 0;
uint8_t s_cccd_index = 0;
bool s_proto_done = false;
uint32_t s_notify_count = 0;
uint8_t s_last_report[64] = {};
int s_last_len = 0;

void StartDiscovery();

int OnWrite(uint16_t conn, const struct ble_gatt_error* error, struct ble_gatt_attr* attr,
            void* arg) {
    (void)conn;
    (void)attr;
    (void)arg;
    ESP_LOGI(TAG, "write status=%d", error ? error->status : -1);

    if (!s_proto_done && s_protocol_handle != 0) {
        s_proto_done = true;
    }
    while (s_cccd_index < s_report_count) {
        const uint16_t handle = s_cccd[s_cccd_index++];
        if (handle == 0) {
            continue;
        }
        uint8_t on[2] = {0x01, 0x00};
        int rc = ble_gattc_write_flat(s_conn, handle, on, sizeof(on), OnWrite, nullptr);
        if (rc == 0) {
            return 0;
        }
        ESP_LOGW(TAG, "cccd write handle=%u rc=%d", handle, rc);
    }
    s_ready = true;
    ESP_LOGI(TAG, "raw HID ready: %u report(s)", (unsigned)s_report_count);
    return 0;
}

void StartWrites() {
    s_cccd_index = 0;
    s_proto_done = false;
    if (s_protocol_handle != 0) {
        uint8_t mode = 0x01;  // Report protocol
        int rc = ble_gattc_write_flat(s_conn, s_protocol_handle, &mode, 1, OnWrite, nullptr);
        if (rc == 0) {
            return;
        }
        ESP_LOGW(TAG, "protocol mode write rc=%d", rc);
    }
    OnWrite(s_conn, nullptr, nullptr, nullptr);
}

int OnDsc(uint16_t conn, const struct ble_gatt_error* error, uint16_t chr_val_handle,
          const struct ble_gatt_dsc* dsc, void* arg) {
    (void)conn;
    (void)chr_val_handle;
    (void)arg;
    if (error->status == 0 && dsc != nullptr) {
        if (ble_uuid_u16(&dsc->uuid.u) == kCccd && s_dsc_index < s_report_count &&
            s_cccd[s_dsc_index] == 0) {
            s_cccd[s_dsc_index] = dsc->handle;
            ESP_LOGI(TAG, "cccd handle=%u for report handle=%u", dsc->handle,
                     s_reports[s_dsc_index]);
        }
        return 0;
    }
    if (error->status == BLE_HS_EDONE) {
        s_dsc_index++;
        if (s_dsc_index < s_report_count) {
            int rc = ble_gattc_disc_all_dscs(s_conn, s_reports[s_dsc_index],
                                             s_reports[s_dsc_index] + 8, OnDsc, nullptr);
            if (rc != 0) {
                ESP_LOGW(TAG, "dsc discovery rc=%d", rc);
                s_dsc_index++;
            }
        } else {
            StartWrites();
        }
    }
    return 0;
}

int OnChr(uint16_t conn, const struct ble_gatt_error* error, const struct ble_gatt_chr* chr,
          void* arg) {
    (void)conn;
    (void)arg;
    if (error->status == 0 && chr != nullptr) {
        const uint16_t uuid = ble_uuid_u16(&chr->uuid.u);
        if (uuid == kProtocolMode) {
            s_protocol_handle = chr->val_handle;
        } else if (uuid == kReport && (chr->properties & BLE_GATT_CHR_PROP_NOTIFY)) {
            if (s_report_count < 8) {
                s_reports[s_report_count] = chr->val_handle;
                s_cccd[s_report_count] = 0;
                s_report_count++;
                ESP_LOGI(TAG, "report char handle=%u props=0x%02x", chr->val_handle,
                         chr->properties);
            }
        }
        return 0;
    }
    if (error->status == BLE_HS_EDONE) {
        ESP_LOGI(TAG, "characteristics done: %u reports, protocol=%u", (unsigned)s_report_count,
                 s_protocol_handle);
        s_dsc_index = 0;
        if (s_report_count > 0) {
            int rc = ble_gattc_disc_all_dscs(s_conn, s_reports[0], s_reports[0] + 8, OnDsc, nullptr);
            if (rc != 0) {
                ESP_LOGW(TAG, "dsc discovery rc=%d", rc);
                StartWrites();
            }
        } else {
            StartWrites();
        }
    }
    return 0;
}

int OnSvc(uint16_t conn, const struct ble_gatt_error* error, const struct ble_gatt_svc* svc,
          void* arg) {
    (void)arg;
    if (error->status == 0 && svc != nullptr) {
        if (ble_uuid_u16(&svc->uuid.u) == kHidService) {
            s_hid_start = svc->start_handle;
            s_hid_end = svc->end_handle;
            ESP_LOGI(TAG, "HID service range %u..%u", s_hid_start, s_hid_end);
        }
        return 0;
    }
    if (error->status == BLE_HS_EDONE) {
        if (s_hid_start != 0) {
            int rc = ble_gattc_disc_all_chrs(conn, s_hid_start, s_hid_end, OnChr, nullptr);
            if (rc != 0) {
                ESP_LOGE(TAG, "characteristic discovery rc=%d", rc);
            }
        } else {
            ESP_LOGE(TAG, "HID service not found");
        }
    }
    return 0;
}

void StartDiscovery() {
    if (s_discovering || s_conn == kNoConn) {
        return;
    }
    s_discovering = true;
    ESP_LOGI(TAG, "discovering services");
    int rc = ble_gattc_disc_all_svcs(s_conn, OnSvc, nullptr);
    if (rc != 0) {
        ESP_LOGE(TAG, "service discovery rc=%d", rc);
        s_discovering = false;
    }
}

void HandleNotify(const struct ble_gap_event* event) {
    const uint16_t handle = event->notify_rx.attr_handle;
    const struct os_mbuf* om = event->notify_rx.om;
    if (om == nullptr) {
        return;
    }
    uint8_t buf[64] = {};
    int len = OS_MBUF_PKTLEN(om);
    if (len > (int)sizeof(buf)) {
        len = sizeof(buf);
    }
    os_mbuf_copydata(om, 0, len, buf);
    s_notify_count++;
    const bool changed = len != s_last_len || memcmp(buf, s_last_report, len) != 0;
    if (s_notify_count <= 3 || changed) {
        char hex[3 * 32 + 1] = {};
        int pos = 0;
        const int shown = len < 32 ? len : 32;
        for (int i = 0; i < shown && pos < (int)sizeof(hex); i++) {
            pos += snprintf(hex + pos, sizeof(hex) - pos, "%02x ", buf[i]);
        }
        if (s_notify_count <= 3) {
            ESP_LOGI(TAG, "notify handle=%u len=%d data=%s", handle, len, hex);
        } else {
            ESP_LOGD(TAG, "notify handle=%u len=%d data=%s", handle, len, hex);
        }
        memcpy(s_last_report, buf, len);
        s_last_len = len;
    }

    /* ShanWan BM-835 10-byte report:
     *   [0]=LX [1]=LY [2]=RX [3]=RY (0x80 center, up/left decrease)
     *   [4]=hat (0=up 2=right 4=down 6=left, 0xFF released, odd=diagonals)
     *   [5]=A 0x01, B 0x02, X 0x08, Y 0x10, T 0x40, LB 0x80
     *   [6]=Start 0x08
     */
    if (len == 10) {
        uint8_t pad = 0;
        const uint8_t b5 = buf[5];
        const uint8_t b6 = buf[6];
        if (b5 & 0x01) pad |= input::kA;
        if (b5 & 0x02) pad |= input::kB;
        /* T (0x40) is not always reported by this pad; LB (0x80) is a
         * reliable Select alternative. */
        if (b5 & 0x40) pad |= input::kSelect;
        if (b5 & 0x80) pad |= input::kSelect;
        if (b6 & 0x08) pad |= input::kStart;

        const uint8_t hat = buf[4];
        if (hat <= 7) {
            switch (hat) {
                case 0: pad |= input::kUp; break;
                case 1: pad |= input::kUp | input::kRight; break;
                case 2: pad |= input::kRight; break;
                case 3: pad |= input::kDown | input::kRight; break;
                case 4: pad |= input::kDown; break;
                case 5: pad |= input::kDown | input::kLeft; break;
                case 6: pad |= input::kLeft; break;
                case 7: pad |= input::kUp | input::kLeft; break;
                default: break;
            }
        } else {
            const int dx = (int)buf[0] - 0x80;
            const int dy = (int)buf[1] - 0x80;
            constexpr int kDead = 40;
            if (dx < -kDead) pad |= input::kLeft;
            if (dx > kDead) pad |= input::kRight;
            if (dy < -kDead) pad |= input::kUp;
            if (dy > kDead) pad |= input::kDown;
        }
        input::SetPad(pad);
    }
    (void)handle;
}

int RawGapEvent(struct ble_gap_event* event, void* arg) {
    (void)arg;
    switch (event->type) {
        case BLE_GAP_EVENT_CONNECT:
            if (event->connect.status == 0) {
                s_conn = event->connect.conn_handle;
                s_connected = true;
                s_connecting = false;
                s_discovering = false;
                s_ready = false;
                input::SetPad(0);
                input::SetPadName("ShanWan BM-835");
                ESP_LOGI(TAG, "connected handle=%u", s_conn);
                int rc = ble_gap_security_initiate(s_conn);
                ESP_LOGI(TAG, "security initiate rc=%d", rc);
                if (rc != 0) {
                    StartDiscovery();
                }
            } else {
                s_connecting = false;
                s_connected = false;
                ESP_LOGE(TAG, "connect failed status=%d", event->connect.status);
            }
            break;

        case BLE_GAP_EVENT_ENC_CHANGE:
            ESP_LOGI(TAG, "enc change status=%d", event->enc_change.status);
            if (event->enc_change.status == 0) {
                StartDiscovery();
            }
            break;

        case BLE_GAP_EVENT_DISCONNECT:
            ESP_LOGI(TAG, "disconnected reason=%d", event->disconnect.reason);
            s_connected = false;
            s_connecting = false;
            s_discovering = false;
            s_ready = false;
            s_conn = kNoConn;
            s_report_count = 0;
            s_protocol_handle = 0;
            s_hid_start = 0;
            input::SetPad(0);
            input::SetPadName("");
            break;

        case BLE_GAP_EVENT_NOTIFY_RX:
            HandleNotify(event);
            break;

        case BLE_GAP_EVENT_MTU:
            ESP_LOGI(TAG, "mtu=%d", event->mtu.value);
            break;

        default:
            break;
    }
    return 0;
}

}  // namespace

bool Connect(const uint8_t* bda, uint8_t addr_type) {
    if (s_connected || s_connecting) {
        return true;
    }
    /* Match esp_hidh's proven connect call: public own address, default
     * connection parameters, 30 s timeout. */
    ble_gap_disc_cancel();
    ble_addr_t addr = {};
    addr.type = addr_type;
    memcpy(addr.val, bda, sizeof(addr.val));
    int rc = ble_gap_connect(0, &addr, 30000, nullptr, RawGapEvent, nullptr);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gap_connect rc=%d", rc);
        return false;
    }
    s_connecting = true;
    ESP_LOGI(TAG, "connecting %02x:%02x:%02x:%02x:%02x:%02x type=%u", bda[0], bda[1], bda[2],
             bda[3], bda[4], bda[5], (unsigned)addr_type);
    return true;
}

bool Connected() {
    return s_connected;
}

bool Connecting() {
    return s_connecting;
}

void Disconnect() {
    if (s_conn != kNoConn) {
        ble_gap_terminate(s_conn, BLE_ERR_REM_USER_CONN_TERM);
    }
    s_connected = false;
    s_connecting = false;
    s_conn = kNoConn;
    input::SetPad(0);
    input::SetPadName("");
}

}  // namespace ble_raw_hid
