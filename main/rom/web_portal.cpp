#include "web_portal.h"

#include <cstdio>
#include <cstring>
#include <string>

#include <arpa/inet.h>
#include <lwip/sockets.h>
#include <netinet/in.h>
#include <esp_err.h>
#include <esp_event.h>
#include <esp_heap_caps.h>
#include <esp_http_server.h>
#include <esp_log.h>
#include <esp_netif.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <esp_wifi.h>
#include <esp_wifi_default.h>
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <nvs.h>
#include <sdkconfig.h>

#include "input/ble_gamepad.h"
#include "nes/nes_stream.h"
#include "nes/nes_system.h"
#include "input/input.h"
#include "display/panel_display.h"
#include "rom_store.h"

static const char* TAG = "Web";

extern const uint8_t g_index_html_start[] asm("_binary_index_html_start");
extern const uint8_t g_index_html_end[] asm("_binary_index_html_end");
extern const uint8_t g_tv_html_start[] asm("_binary_tv_html_start");
extern const uint8_t g_tv_html_end[] asm("_binary_tv_html_end");
extern const uint8_t g_stream_html_start[] asm("_binary_stream_html_start");
extern const uint8_t g_stream_html_end[] asm("_binary_stream_html_end");
extern const uint8_t g_jsnes_js_start[] asm("_binary_jsnes_min_js_start");
extern const uint8_t g_jsnes_js_end[] asm("_binary_jsnes_min_js_end");

namespace web {
namespace {

constexpr const char* kNvsNamespace = "nes_wifi";
constexpr const char* kNvsSsid = "ssid";
constexpr const char* kNvsPass = "pass";
constexpr EventBits_t kConnectedBit = BIT0;

enum class Mode { None, Sta, Ap };

httpd_handle_t s_server = nullptr;
esp_netif_t* s_sta_netif = nullptr;
esp_netif_t* s_ap_netif = nullptr;
EventGroupHandle_t s_events = nullptr;
esp_timer_handle_t s_reboot_timer = nullptr;
Mode s_mode = Mode::None;
bool s_started = false;
bool s_suspended = false;
std::string s_ssid;
std::string s_ip = "192.168.4.1";
volatile uint32_t s_generation = 0;
std::string s_play_request;
portMUX_TYPE s_play_mux = portMUX_INITIALIZER_UNLOCKED;
volatile int s_tv_clients = 0;
volatile int s_tv_fd = -1;
TaskHandle_t s_tv_task = nullptr;

/* UDP discovery for the Android TV APK. The app broadcasts
 * "ESP32NES_DISCOVER" to 255.255.255.255:8266 and gets "ESP32NES <ip>". */
void DiscoveryTask(void* arg) {
    (void)arg;
    const int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        ESP_LOGE(TAG, "discovery socket failed: %d", errno);
        vTaskDelete(nullptr);
        return;
    }
    int yes = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(8266);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(sock, (sockaddr*)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "discovery bind failed: %d", errno);
        close(sock);
        vTaskDelete(nullptr);
        return;
    }
    ESP_LOGI(TAG, "UDP discovery listening on 8266");
    while (true) {
        char buf[64] = {};
        sockaddr_in from = {};
        socklen_t from_len = sizeof(from);
        const int n = recvfrom(sock, buf, sizeof(buf) - 1, 0, (sockaddr*)&from, &from_len);
        if (n <= 0) {
            continue;
        }
        if (strstr(buf, "ESP32NES_DISCOVER") == nullptr) {
            continue;
        }
        char reply[64];
        snprintf(reply, sizeof(reply), "ESP32NES %s\n", Ip().c_str());
        sendto(sock, reply, strlen(reply), 0, (sockaddr*)&from, from_len);
    }
}

std::string UrlDecode(const char* src) {
    std::string out;
    for (const char* p = src; p != nullptr && *p != 0; p++) {
        if (*p == '+') {
            out.push_back(' ');
        } else if (*p == '%' && p[1] != 0 && p[2] != 0) {
            auto hex = [](char c) -> int {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                return -1;
            };
            int hi = hex(p[1]);
            int lo = hex(p[2]);
            if (hi >= 0 && lo >= 0) {
                out.push_back((char)((hi << 4) | lo));
                p += 2;
            } else {
                out.push_back(*p);
            }
        } else {
            out.push_back(*p);
        }
    }
    return out;
}

std::string JsonEscape(const std::string& in) {
    std::string out;
    for (unsigned char c : in) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[7];
                    snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out.push_back((char)c);
                }
        }
    }
    return out;
}

bool LoadCredentials(std::string* ssid, std::string* pass) {
    nvs_handle_t h = 0;
    if (nvs_open(kNvsNamespace, NVS_READONLY, &h) != ESP_OK) {
        return false;
    }
    char ssid_buf[33] = {};
    char pass_buf[65] = {};
    size_t ssid_len = sizeof(ssid_buf);
    size_t pass_len = sizeof(pass_buf);
    bool ok = nvs_get_str(h, kNvsSsid, ssid_buf, &ssid_len) == ESP_OK && ssid_buf[0] != 0;
    if (ok) {
        nvs_get_str(h, kNvsPass, pass_buf, &pass_len);
    }
    nvs_close(h);
    if (ok) {
        *ssid = ssid_buf;
        *pass = pass_buf;
    }
    return ok;
}

bool SaveCredentials(const std::string& ssid, const std::string& pass) {
    nvs_handle_t h = 0;
    if (nvs_open(kNvsNamespace, NVS_READWRITE, &h) != ESP_OK) {
        return false;
    }
    esp_err_t err = nvs_set_str(h, kNvsSsid, ssid.c_str());
    if (err == ESP_OK) {
        err = nvs_set_str(h, kNvsPass, pass.c_str());
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err == ESP_OK;
}

void ClearCredentials() {
    nvs_handle_t h = 0;
    if (nvs_open(kNvsNamespace, NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_key(h, kNvsSsid);
        nvs_erase_key(h, kNvsPass);
        nvs_commit(h);
        nvs_close(h);
    }
}

bool HasCredentials() {
    std::string ssid;
    std::string pass;
    return LoadCredentials(&ssid, &pass);
}

void SetReboot(const char* reason) {
    ESP_LOGW(TAG, "%s, rebooting in 1.2 s", reason);
    if (s_reboot_timer == nullptr) {
        esp_timer_create_args_t args = {};
        args.callback = [](void*) { esp_restart(); };
        args.name = "wifi_reboot";
        esp_timer_create(&args, &s_reboot_timer);
    }
    esp_timer_start_once(s_reboot_timer, 1200 * 1000);
}

void OnWifiEvent(void* arg, esp_event_base_t base, int32_t id, void* data) {
    (void)arg;
    if (base == WIFI_EVENT) {
        if (id == WIFI_EVENT_STA_START) {
            if (s_mode == Mode::Sta) {
                esp_wifi_connect();
            }
        } else if (id == WIFI_EVENT_STA_DISCONNECTED) {
            if (s_mode == Mode::Sta && !s_suspended) {
                xEventGroupClearBits(s_events, kConnectedBit);
                ESP_LOGW(TAG, "STA disconnected, reconnecting...");
                esp_wifi_connect();
            }
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        auto* event = (ip_event_got_ip_t*)data;
        char buf[16];
        snprintf(buf, sizeof(buf), IPSTR, IP2STR(&event->ip_info.ip));
        s_ip = buf;
        xEventGroupSetBits(s_events, kConnectedBit);
        ESP_LOGI(TAG, "STA got IP %s", s_ip.c_str());
    }
}

bool StartSta(const std::string& ssid, const std::string& pass) {
    s_sta_netif = esp_netif_create_default_wifi_sta();
    if (s_sta_netif == nullptr) {
        ESP_LOGE(TAG, "create STA netif failed");
        return false;
    }

    wifi_config_t wc = {};
    size_t ssid_len = ssid.size() > sizeof(wc.sta.ssid) ? sizeof(wc.sta.ssid) : ssid.size();
    memcpy(wc.sta.ssid, ssid.data(), ssid_len);
    size_t pass_len = pass.size() > sizeof(wc.sta.password) ? sizeof(wc.sta.password) : pass.size();
    memcpy(wc.sta.password, pass.data(), pass_len);
    wc.sta.threshold.authmode = pass.empty() ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK;
    wc.sta.pmf_cfg.capable = true;
    wc.sta.pmf_cfg.required = false;

    s_mode = Mode::Sta;
    xEventGroupClearBits(s_events, kConnectedBit);
    esp_err_t err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err == ESP_OK) {
        err = esp_wifi_set_config(WIFI_IF_STA, &wc);
    }
    if (err == ESP_OK) {
        esp_wifi_set_ps(WIFI_PS_NONE);
        err = esp_wifi_start();
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "STA start failed: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "connecting to \"%s\"...", ssid.c_str());
        EventBits_t bits = xEventGroupWaitBits(s_events, kConnectedBit, pdFALSE, pdFALSE,
                                               pdMS_TO_TICKS(15000));
        if (bits & kConnectedBit) {
            s_ssid = ssid;
            return true;
        }
        ESP_LOGW(TAG, "STA connect timeout");
    }

    s_mode = Mode::None;
    esp_wifi_stop();
    if (s_sta_netif != nullptr) {
        esp_netif_destroy_default_wifi(s_sta_netif);
        s_sta_netif = nullptr;
    }
    return false;
}

bool StartAp() {
    s_ap_netif = esp_netif_create_default_wifi_ap();
    if (s_ap_netif == nullptr) {
        ESP_LOGE(TAG, "create AP netif failed");
        return false;
    }

    uint8_t mac[6] = {};
    esp_wifi_get_mac(WIFI_IF_AP, mac);
    char ssid[33];
    snprintf(ssid, sizeof(ssid), "%s-%02X%02X", CONFIG_NES_AP_SSID_PREFIX, mac[4], mac[5]);

    wifi_config_t wc = {};
    size_t ssid_len = strlen(ssid);
    if (ssid_len > sizeof(wc.ap.ssid)) {
        ssid_len = sizeof(wc.ap.ssid);
    }
    memcpy(wc.ap.ssid, ssid, ssid_len);
    wc.ap.ssid_len = (uint8_t)ssid_len;
    wc.ap.channel = CONFIG_NES_AP_CHANNEL;
    wc.ap.max_connection = 2;
    const char* password = CONFIG_NES_AP_PASSWORD;
    if (strlen(password) >= 8) {
        size_t pw_len = strlen(password);
        if (pw_len > sizeof(wc.ap.password)) {
            pw_len = sizeof(wc.ap.password);
        }
        memcpy(wc.ap.password, password, pw_len);
        wc.ap.authmode = WIFI_AUTH_WPA2_PSK;
    } else {
        wc.ap.authmode = WIFI_AUTH_OPEN;
    }

    s_mode = Mode::Ap;
    esp_err_t err = esp_wifi_set_mode(WIFI_MODE_AP);
    if (err == ESP_OK) {
        err = esp_wifi_set_config(WIFI_IF_AP, &wc);
    }
    if (err == ESP_OK) {
        err = esp_wifi_start();
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "AP start failed: %s", esp_err_to_name(err));
        s_mode = Mode::None;
        return false;
    }

    esp_netif_ip_info_t ip_info = {};
    if (esp_netif_get_ip_info(s_ap_netif, &ip_info) == ESP_OK && ip_info.ip.addr != 0) {
        char buf[16];
        snprintf(buf, sizeof(buf), IPSTR, IP2STR(&ip_info.ip));
        s_ip = buf;
    } else {
        s_ip = "192.168.4.1";
    }
    s_ssid = ssid;
    ESP_LOGI(TAG, "provisioning AP \"%s\" at http://%s/", ssid, s_ip.c_str());
    return true;
}

esp_err_t SendJson(httpd_req_t* req, const char* status, const std::string& body) {
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, body.c_str(), (ssize_t)body.size());
}

esp_err_t SendError(httpd_req_t* req, const char* status, const char* message) {
    std::string body = std::string("{\"error\":\"") + JsonEscape(message) + "\"}";
    return SendJson(req, status, body);
}

bool QueryName(httpd_req_t* req, std::string* name, bool sanitize) {
    char query[256] = {};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
        return false;
    }
    char raw[192] = {};
    if (httpd_query_key_value(query, "name", raw, sizeof(raw)) != ESP_OK) {
        return false;
    }
    *name = UrlDecode(raw);
    if (sanitize) {
        *name = rom_store::SanitizeName(*name);
    }
    return !name->empty();
}

esp_err_t IndexHandler(httpd_req_t* req) {
    const size_t len = (size_t)(g_index_html_end - g_index_html_start);
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, (const char*)g_index_html_start, (ssize_t)len);
}

esp_err_t TvHandler(httpd_req_t* req) {
    const size_t len = (size_t)(g_tv_html_end - g_tv_html_start);
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, (const char*)g_tv_html_start, (ssize_t)len);
}

esp_err_t StreamPageHandler(httpd_req_t* req) {
    const size_t len = (size_t)(g_stream_html_end - g_stream_html_start);
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, (const char*)g_stream_html_start, (ssize_t)len);
}

esp_err_t StreamWsHandler(httpd_req_t* req) {
    const int fd = httpd_req_to_sockfd(req);
    if (fd >= 0) {
        int nodelay = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));
    }
    httpd_ws_frame_t frame = {};
    frame.type = HTTPD_WS_TYPE_TEXT;
    esp_err_t r = httpd_ws_recv_frame(req, &frame, 0);
    if (r != ESP_OK) {
        return r;
    }
    uint8_t buf[192] = {};
    frame.payload = buf;
    if (frame.len > sizeof(buf) - 1) {
        frame.len = sizeof(buf) - 1;
    }
    if (frame.len > 0) {
        httpd_ws_recv_frame(req, &frame, frame.len);
    }
    if (frame.type == HTTPD_WS_TYPE_CLOSE) {
        nesstream::Stop();
        return ESP_OK;
    }
    if (strncmp((const char*)buf, "start ", 6) == 0) {
        std::string name((const char*)buf + 6);
        while (!name.empty() && (name.back() == '\n' || name.back() == '\r')) {
            name.pop_back();
        }
        if (fd >= 0) {
            nesstream::Start(name, fd, req->handle);
        }
    } else if (strncmp((const char*)buf, "stop", 4) == 0) {
        nesstream::Stop();
    }
    return ESP_OK;
}

esp_err_t JsnesHandler(httpd_req_t* req) {
    const size_t len = (size_t)(g_jsnes_js_end - g_jsnes_js_start);
    httpd_resp_set_type(req, "application/javascript");
    httpd_resp_set_hdr(req, "Cache-Control", "public, max-age=86400");
    return httpd_resp_send(req, (const char*)g_jsnes_js_start, (ssize_t)len);
}

esp_err_t ScreenBmpHandler(httpd_req_t* req) {
    int w = 0;
    int h = 0;
    const uint16_t* canvas = display::CanvasData(&w, &h);
    if (canvas == nullptr || w <= 0 || h <= 0) {
        return SendError(req, "500 Internal Server Error", "no canvas");
    }
    const int row_bytes = (w * 3 + 3) & ~3;
    const size_t size = 54 + (size_t)row_bytes * h;
    uint8_t* bmp = (uint8_t*)heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (bmp == nullptr) {
        return SendError(req, "500 Internal Server Error", "out of memory");
    }
    memset(bmp, 0, 54);
    bmp[0] = 'B';
    bmp[1] = 'M';
    auto put16 = [](uint8_t* p, uint16_t v) {
        p[0] = (uint8_t)v;
        p[1] = (uint8_t)(v >> 8);
    };
    auto put32 = [](uint8_t* p, uint32_t v) {
        p[0] = (uint8_t)v;
        p[1] = (uint8_t)(v >> 8);
        p[2] = (uint8_t)(v >> 16);
        p[3] = (uint8_t)(v >> 24);
    };
    put32(bmp + 2, (uint32_t)size);
    put32(bmp + 10, 54);
    put32(bmp + 14, 40);
    put32(bmp + 18, (uint32_t)w);
    put32(bmp + 22, (uint32_t)h);
    put16(bmp + 26, 1);
    put16(bmp + 28, 24);
    put32(bmp + 34, (uint32_t)row_bytes * (uint32_t)h);
    for (int y = 0; y < h; y++) {
        uint8_t* dst = bmp + 54 + (size_t)row_bytes * (h - 1 - y);
        for (int x = 0; x < w; x++) {
            uint16_t v = canvas[y * w + x];
            v = (uint16_t)((v >> 8) | (v << 8));  // canvas is big-endian
            const uint8_t r = (uint8_t)(((v >> 11) & 0x1F) * 255 / 31);
            const uint8_t g = (uint8_t)(((v >> 5) & 0x3F) * 255 / 63);
            const uint8_t b = (uint8_t)((v & 0x1F) * 255 / 31);
            dst[x * 3 + 0] = b;
            dst[x * 3 + 1] = g;
            dst[x * 3 + 2] = r;
        }
    }
    httpd_resp_set_type(req, "image/bmp");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t err = httpd_resp_send(req, (const char*)bmp, (ssize_t)size);
    free(bmp);
    return err;
}

esp_err_t RomFileHandler(httpd_req_t* req) {
    std::string name;
    if (!QueryName(req, &name, false)) {
        return SendError(req, "400 Bad Request", "missing name");
    }
    const rom_store::RomEntry builtin = rom_store::BuiltinEntry();
    const uint8_t* data = nullptr;
    size_t len = 0;
    rom_store::RomBuffer buf;
    if (!builtin.name.empty() && name == builtin.name) {
        data = rom_store::BuiltinRom(&len);
    } else {
        const std::string safe = rom_store::SanitizeName(name);
        if (safe.empty() || !rom_store::Read(safe, buf)) {
            return SendError(req, "404 Not Found", "ROM not found");
        }
        data = buf.data;
        len = buf.size;
    }
    if (data == nullptr || len == 0) {
        return SendError(req, "404 Not Found", "ROM not found");
    }
    httpd_resp_set_type(req, "application/octet-stream");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, (const char*)data, (ssize_t)len);
}

esp_err_t RomsHandler(httpd_req_t* req) {
    std::string body = "{\"roms\":[";
    const auto roms = rom_store::List();
    for (size_t i = 0; i < roms.size(); i++) {
        if (i != 0) {
            body += ",";
        }
        char sizebuf[32];
        snprintf(sizebuf, sizeof(sizebuf), "%u", (unsigned)roms[i].size);
        body += "{\"name\":\"";
        body += JsonEscape(roms[i].name);
        body += "\",\"size\":";
        body += sizebuf;
        body += ",\"builtin\":";
        body += roms[i].builtin ? "true" : "false";
        body += "}";
    }
    body += "]}";
    return SendJson(req, "200 OK", body);
}

esp_err_t StatusHandler(httpd_req_t* req) {
    const char* mode = s_mode == Mode::Sta ? "sta" : (s_mode == Mode::Ap ? "ap" : "none");
    char buf[480];
    snprintf(buf, sizeof(buf),
             "{\"name\":\"ESP32-NES\",\"ip\":\"%s\",\"ssid\":\"%s\",\"mode\":\"%s\",\"pad\":\"%s\","
             "\"configured\":%s,\"free_internal\":%u,\"free_psram\":%u,"
             "\"stream\":%s,\"audio_overrun\":%u,\"audio_avail\":%u}",
             s_ip.c_str(), JsonEscape(s_ssid).c_str(), mode, JsonEscape(input::PadName()).c_str(),
             HasCredentials() ? "true" : "false",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
             nesstream::Active() ? "true" : "false",
             (unsigned)NesSys_AudioOverruns(),
             (unsigned)NesSys_AudioAvailable());
    return SendJson(req, "200 OK", buf);
}

esp_err_t UploadHandler(httpd_req_t* req) {
    std::string name;
    if (!QueryName(req, &name, true)) {
        return SendError(req, "400 Bad Request", "missing or invalid name");
    }
    if (req->content_len == 0 || req->content_len > rom_store::kMaxRomSize) {
        return SendError(req, "413 Payload Too Large", "bad content length");
    }
    const size_t len = req->content_len;
    uint8_t* buf = (uint8_t*)heap_caps_malloc(len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (buf == nullptr) {
        return SendError(req, "507 Insufficient Storage", "out of memory");
    }
    size_t got = 0;
    while (got < len) {
        int r = httpd_req_recv(req, (char*)buf + got, len - got);
        if (r == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }
        if (r <= 0) {
            free(buf);
            return SendError(req, "500 Internal Server Error", "receive failed");
        }
        got += (size_t)r;
    }
    if (got < 16 || memcmp(buf, "NES\x1a", 4) != 0) {
        free(buf);
        return SendError(req, "400 Bad Request", "not a .nes file");
    }
    bool ok = rom_store::Save(name, buf, got);
    free(buf);
    if (!ok) {
        return SendError(req, "500 Internal Server Error", "save failed");
    }
    s_generation = s_generation + 1;
    std::string body = "{\"ok\":true,\"name\":\"" + JsonEscape(name) + "\"}";
    return SendJson(req, "200 OK", body);
}

esp_err_t DeleteHandler(httpd_req_t* req) {
    std::string name;
    if (!QueryName(req, &name, true)) {
        return SendError(req, "400 Bad Request", "missing or invalid name");
    }
    if (!rom_store::Delete(name)) {
        return SendError(req, "404 Not Found", "delete failed");
    }
    s_generation = s_generation + 1;
    return SendJson(req, "200 OK", "{\"ok\":true}");
}

esp_err_t PlayHandler(httpd_req_t* req) {
    std::string name;
    if (!QueryName(req, &name, false)) {
        return SendError(req, "400 Bad Request", "missing name");
    }
    bool found = false;
    for (const auto& e : rom_store::List()) {
        if (e.name == name) {
            found = true;
            break;
        }
    }
    if (!found) {
        return SendError(req, "404 Not Found", "unknown ROM");
    }
    taskENTER_CRITICAL(&s_play_mux);
    s_play_request = name;
    taskEXIT_CRITICAL(&s_play_mux);
    return SendJson(req, "200 OK", "{\"ok\":true}");
}

esp_err_t WifiConfigHandler(httpd_req_t* req) {
    char query[512] = {};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
        return SendError(req, "400 Bad Request", "missing ssid");
    }
    char ssid_raw[160] = {};
    char pass_raw[192] = {};
    if (httpd_query_key_value(query, "ssid", ssid_raw, sizeof(ssid_raw)) != ESP_OK) {
        return SendError(req, "400 Bad Request", "missing ssid");
    }
    std::string ssid = UrlDecode(ssid_raw);
    std::string pass;
    if (httpd_query_key_value(query, "pass", pass_raw, sizeof(pass_raw)) == ESP_OK) {
        pass = UrlDecode(pass_raw);
    }
    if (ssid.empty() || ssid.size() > 32) {
        return SendError(req, "400 Bad Request", "invalid ssid");
    }
    if (!pass.empty() && pass.size() < 8) {
        return SendError(req, "400 Bad Request", "password must be 8+ characters");
    }
    if (!SaveCredentials(ssid, pass)) {
        return SendError(req, "500 Internal Server Error", "save failed");
    }
    ESP_LOGI(TAG, "saved Wi-Fi credentials for \"%s\"", ssid.c_str());
    esp_err_t err = SendJson(req, "200 OK", "{\"ok\":true,\"reboot\":true}");
    SetReboot("Wi-Fi credentials saved");
    return err;
}

esp_err_t WifiForgetHandler(httpd_req_t* req) {
    ClearCredentials();
    esp_err_t err = SendJson(req, "200 OK", "{\"ok\":true,\"reboot\":true}");
    SetReboot("Wi-Fi credentials cleared");
    return err;
}

esp_err_t BleHandler(httpd_req_t* req) {
    std::string body = "{\"connected\":";
    body += ble_gamepad::Connected() ? "true" : "false";
    body += ",\"scanning\":";
    body += ble_gamepad::Scanning() ? "true" : "false";
    body += ",\"name\":\"";
    body += JsonEscape(input::PadName());
    body += "\",\"devices\":[";
    const auto devices = ble_gamepad::Devices();
    for (size_t i = 0; i < devices.size(); i++) {
        if (i != 0) {
            body += ",";
        }
        char rssi[16];
        snprintf(rssi, sizeof(rssi), "%d", devices[i].rssi);
        body += "{\"name\":\"";
        body += JsonEscape(devices[i].name);
        body += "\",\"addr\":\"";
        body += JsonEscape(devices[i].addr);
        body += "\",\"usage\":\"";
        body += JsonEscape(devices[i].usage);
        body += "\",\"rssi\":";
        body += rssi;
        body += ",\"addr_type\":";
        char typebuf[8];
        snprintf(typebuf, sizeof(typebuf), "%u", (unsigned)devices[i].addr_type);
        body += typebuf;
        body += "}";
    }
    body += "]}";
    return SendJson(req, "200 OK", body);
}

esp_err_t BleScanHandler(httpd_req_t* req) {
    ble_gamepad::RequestScan();
    return SendJson(req, "200 OK", "{\"ok\":true}");
}

esp_err_t BleConnectHandler(httpd_req_t* req) {
    char query[256] = {};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
        return SendError(req, "400 Bad Request", "missing addr");
    }
    char raw[64] = {};
    if (httpd_query_key_value(query, "addr", raw, sizeof(raw)) != ESP_OK) {
        return SendError(req, "400 Bad Request", "missing addr");
    }
    const std::string addr = UrlDecode(raw);
    if (!ble_gamepad::ConnectTo(addr)) {
        return SendError(req, "400 Bad Request", "invalid addr");
    }
    return SendJson(req, "200 OK", "{\"ok\":true}");
}

esp_err_t BleDisconnectHandler(httpd_req_t* req) {
    ble_gamepad::Disconnect();
    return SendJson(req, "200 OK", "{\"ok\":true}");
}

/* Pushes the pad state at ~60 Hz using the async send API. This task is
 * independent of the httpd worker so ROM/file downloads are not blocked. */
void PadSenderTask(void* arg) {
    (void)arg;
    static uint8_t payload[2];
    while (s_tv_fd >= 0) {
        payload[0] = input::PadMask();
        payload[1] = (uint8_t)(input::PadName()[0] != 0 ? 1 : 0);
        httpd_ws_frame_t frame = {};
        frame.type = HTTPD_WS_TYPE_BINARY;
        frame.payload = payload;
        frame.len = sizeof(payload);
        esp_err_t err = httpd_ws_send_frame_async(s_server, s_tv_fd, &frame);
        if (err != ESP_OK) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(16));
    }
    ESP_LOGI(TAG, "pad WebSocket disconnected");
    s_tv_fd = -1;
    s_tv_clients = 0;
    s_tv_task = nullptr;
    vTaskDelete(nullptr);
}

/* The handler is first invoked when a client frame arrives; the page sends a
 * "start" text frame on open. We remember the socket and return immediately:
 * a separate task pushes 2-byte binary frames at ~60 Hz with the async send
 * API, so the httpd thread stays free for ROM/file requests. */
esp_err_t PadWsHandler(httpd_req_t* req) {
    httpd_ws_frame_t start = {};
    start.type = HTTPD_WS_TYPE_TEXT;
    esp_err_t r = httpd_ws_recv_frame(req, &start, 0);
    if (r != ESP_OK) {
        return r;
    }
    uint8_t scratch[16] = {};
    if (start.len > 0) {
        start.payload = scratch;
        if (start.len > sizeof(scratch)) {
            start.len = sizeof(scratch);
        }
        httpd_ws_recv_frame(req, &start, start.len);
    }
    if (start.type == HTTPD_WS_TYPE_CLOSE) {
        return ESP_OK;
    }
    const int fd = httpd_req_to_sockfd(req);
    if (fd < 0) {
        return ESP_FAIL;
    }
    int nodelay = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));
    s_tv_fd = fd;
    s_tv_clients = 1;
    ESP_LOGI(TAG, "pad WebSocket connected (fd=%d)", fd);
    if (s_tv_task == nullptr) {
        xTaskCreatePinnedToCore(PadSenderTask, "tv_pad", 4096, nullptr, 4, &s_tv_task, 0);
    }
    return ESP_OK;
}

esp_err_t Register(httpd_handle_t server, const char* uri, httpd_method_t method,
                   esp_err_t (*handler)(httpd_req_t*)) {
    httpd_uri_t u = {};
    u.uri = uri;
    u.method = method;
    u.handler = handler;
    u.user_ctx = nullptr;
    return httpd_register_uri_handler(server, &u);
}

bool StartHttpd() {
    httpd_config_t hc = HTTPD_DEFAULT_CONFIG();
    hc.max_uri_handlers = 22;
    hc.stack_size = 8192;
    hc.core_id = 0;  // keep HTTP work off the NES core
    hc.lru_purge_enable = true;
    esp_err_t err = httpd_start(&s_server, &hc);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd start: %s", esp_err_to_name(err));
        return false;
    }
    Register(s_server, "/", HTTP_GET, IndexHandler);
    Register(s_server, "/tv", HTTP_GET, TvHandler);
    Register(s_server, "/stream", HTTP_GET, StreamPageHandler);
    Register(s_server, "/jsnes.js", HTTP_GET, JsnesHandler);
    Register(s_server, "/api/screen.bmp", HTTP_GET, ScreenBmpHandler);
    Register(s_server, "/api/roms", HTTP_GET, RomsHandler);
    Register(s_server, "/api/rom/file", HTTP_GET, RomFileHandler);
    Register(s_server, "/api/status", HTTP_GET, StatusHandler);
    Register(s_server, "/api/rom", HTTP_PUT, UploadHandler);
    Register(s_server, "/api/rom", HTTP_DELETE, DeleteHandler);
    Register(s_server, "/api/play", HTTP_POST, PlayHandler);
    Register(s_server, "/api/wifi", HTTP_POST, WifiConfigHandler);
    Register(s_server, "/api/wifi", HTTP_DELETE, WifiForgetHandler);
    Register(s_server, "/api/ble", HTTP_GET, BleHandler);
    Register(s_server, "/api/ble/scan", HTTP_POST, BleScanHandler);
    Register(s_server, "/api/ble/connect", HTTP_POST, BleConnectHandler);
    Register(s_server, "/api/ble/disconnect", HTTP_POST, BleDisconnectHandler);
    httpd_uri_t ws = {};
    ws.uri = "/ws/pad";
    ws.method = HTTP_GET;
    ws.handler = PadWsHandler;
    ws.is_websocket = true;
    httpd_register_uri_handler(s_server, &ws);

    httpd_uri_t sws = {};
    sws.uri = "/ws/stream";
    sws.method = HTTP_GET;
    sws.handler = StreamWsHandler;
    sws.is_websocket = true;
    httpd_register_uri_handler(s_server, &sws);
    return true;
}

}  // namespace

bool Start() {
    if (s_started) {
        return true;
    }

    ESP_ERROR_CHECK(esp_netif_init());
    esp_err_t err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "event loop: %s", esp_err_to_name(err));
        return false;
    }
    s_events = xEventGroupCreate();

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&init_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "wifi init: %s", esp_err_to_name(err));
        return false;
    }
    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &OnWifiEvent, nullptr, nullptr);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &OnWifiEvent, nullptr, nullptr);

    std::string ssid;
    std::string pass;
    bool have_creds = LoadCredentials(&ssid, &pass);
    if (!have_creds && strlen(CONFIG_NES_WIFI_SSID) > 0) {
        ssid = CONFIG_NES_WIFI_SSID;
        pass = CONFIG_NES_WIFI_PASSWORD;
        have_creds = true;
    }

    bool online = false;
    if (have_creds) {
        online = StartSta(ssid, pass);
    }
    if (!online) {
        if (have_creds) {
            ESP_LOGW(TAG, "could not join \"%s\", starting provisioning AP", ssid.c_str());
        }
        StartAp();
    }

    if (!StartHttpd()) {
        return false;
    }
    s_started = true;
    xTaskCreatePinnedToCore(DiscoveryTask, "udp_discovery", 3072, nullptr, 2, nullptr, 0);
    if (s_mode == Mode::Sta) {
        ESP_LOGI(TAG, "ready on LAN: http://%s/ (SSID %s)", s_ip.c_str(), s_ssid.c_str());
    } else {
        ESP_LOGI(TAG, "ready for provisioning: http://%s/ (AP %s)", s_ip.c_str(), s_ssid.c_str());
    }
    return true;
}

void Suspend() {
    if (!s_started || s_suspended) {
        return;
    }
    ESP_LOGI(TAG, "stopping Wi-Fi for game mode");
    s_suspended = true;
    esp_wifi_stop();
}

void Resume() {
    if (!s_started || !s_suspended) {
        return;
    }
    esp_wifi_start();
    s_suspended = false;
    if (s_mode == Mode::Sta) {
        ESP_LOGI(TAG, "Wi-Fi resuming, reconnecting to \"%s\"", s_ssid.c_str());
    } else {
        ESP_LOGI(TAG, "provisioning AP resumed at http://%s/", s_ip.c_str());
    }
}

std::string Ip() {
    return s_ip;
}

std::string Ssid() {
    return s_ssid;
}

uint32_t Generation() {
    return s_generation;
}

std::string TakePlayRequest() {
    std::string out;
    taskENTER_CRITICAL(&s_play_mux);
    out.swap(s_play_request);
    taskEXIT_CRITICAL(&s_play_mux);
    return out;
}

bool TvActive() {
    return s_tv_clients > 0;
}

}  // namespace web
