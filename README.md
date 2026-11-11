# ESP32-NES

独立运行在 **正点原子 ATK-DNESP32S3**（ESP32-S3 / 16 MB Flash / 8 MB Octal PSRAM）上的 NES 模拟器固件。
不依赖小智（xiaozhi）框架，开机进入 ROM 列表，屏幕直绘 + ES8388 出声 + BLE 手柄操作；ROM 通过局域网网页上传。

> 本公开仓库不包含任何 NES ROM。请通过网页上传你合法持有的 `.nes` 文件。

## 已验证结果

| 项目 | 结果 |
|---|---|
| `mrio.nes`（mapper 0，40 KB，内部 RAM） | **60.1 fps**（WiFi + BLE 同时在线） |
| 音频 | APU 22050 → 24000 Hz，ES8388 OUT1/OUT2 均 0 dB；稳定后 0 欠载 |
| 中文 ROM 名 | UTF-8 + GB2312 子集（7542 字形），实测 `超级马里奥.nes` 显示/启动正常 |
| 显示 | ST7789 320×240，游戏区 256×240 居中，80 MHz SPI |
| ROM 存储 | 16 MB Flash 内 13 MB FAT 分区，网页上传/删除，断电保留 |
| 网络 | STA 入网（DHCP），失败时回退 `NES-XXXX` 配网热点 |
| 输入 | BLE HID Host 已验证；GamepadSpace-Q34B（ShanWan BM-835）走专用原始 GATT 解析 |
| 大屏串流 `/stream` | ESP32 无头 58–60 fps；图块 RLE/2bpp + 24 kHz PCM；Chrome 实测 58–60 fps、音频 ~23k/s |

## 硬件

| 模块 | 引脚 |
|---|---|
| ST7789 2.4" 320×240 | SCLK=12, MOSI=11, DC=40, CS=21, 复位/背光经 XL9555 |
| ES8388 | MCLK=3, BCLK=46, WS=9, DOUT=10, DIN=14；I2C SDA=41, SCL=42 |
| XL9555 | I2C 0x20；P1_0 背光使能，P0_2 屏复位 |
| BOOT 按键 | GPIO0（低有效） |

## 目录结构

```
main/
  app_main.cpp          菜单 / 游戏状态机
  board/                板级初始化、引脚、8×8 + CJK 字库
  display/              ST7789 直绘（游戏帧 + 菜单）
  audio/                ES8388 输出 + APU PCM 泵
  input/                BOOT、BLE HID Host、HID 描述符解析
  rom/                  FAT 存储、网页上传、STA/AP、管理页与 TV 接口
  nes/                  InfoNES 核心 + 平台层 + 本地播放器 + 电视串流
  assets/               管理页、/tv、/stream、jsnes
components/esp_codec_dev/  ES8388 编解码（vendored）
tv-app/                 Android TV APK（原生启动器 + WebView）
licenses/               InfoNES / esp_codec_dev / Unifont 许可证
```

## 构建

需要 ESP-IDF **6.1**（本项目在 `C:\Espressif\frameworks\esp-idf-v6.1` 验证）：

```bat
call C:\Espressif\export_idf.cmd
cd esp32-nes
idf.py set-target esp32s3
idf.py build
idf.py -p COM14 flash monitor
```

首次烧录会写入分区表；`storage` 分区第一次挂载时自动格式化为 FAT。

## 首次配网

1. 设备上电后优先读取 NVS 里保存的 Wi-Fi；没有或连接失败（15 秒）则开热点 **`NES-XXXX`**。
2. 手机/电脑连接该热点（默认密码 `nes12345`，可在 `menuconfig → ESP32-NES` 修改）。
3. 打开 `http://192.168.4.1/`，在「网络」里填写家里的 **2.4 GHz** Wi-Fi 名称和密码，保存。
4. 设备自动重启并加入局域网，屏幕顶部显示分配到的 IP（例如 `192.168.1.63`）。
5. 之后直接用 `http://设备IP/` 打开网页，不再需要热点。

> ESP32-S3 只支持 2.4 GHz Wi-Fi；5 GHz SSID 无法连接。
> 需要换网络时，在网页点「重新配网（清除 Wi-Fi）」，设备会重启回配网热点。

也可以把默认 Wi-Fi 写进 `menuconfig → ESP32-NES → Default home Wi-Fi SSID`（NVS 中的配置优先）。

## ROM 管理

- 网页支持拖拽/多选上传 `.nes`（≤ 4 MB），列出、删除、**启动**。
- 「蓝牙手柄」面板：显示连接状态、扫描结果（名称/地址/RSSI）、手动连接/断开。
- ROM 存在 `/storage/roms/`，掉电不丢；分区约 13 MB。
- 中文文件名已支持：菜单使用 GNU Unifont 的 GB2312 子集（7542 字形）渲染 UTF-8 文件名；
  超出 GB2312 的生僻字显示 `?`，不影响选择/启动。
- 调试截图：`GET /api/screen.bmp` 可下载当前菜单画面（320×240 BMP）。
- 本公开仓库不包含任何 NES ROM；首次使用时请通过网页上传你合法持有的 `.nes` 文件。
  固件支持在没有内置 ROM 的情况下正常启动和配网。

## 手柄

- 使用 ESP-IDF 自带 `esp_hid`（NimBLE）做 BLE HID Host，自动扫描并连接第一个 gamepad/joystick。
- **GamepadSpace-Q34B（ShanWan BM-835）已实测可用**：该手柄不提供 HID Report Map，
  固件内置一个精简 NimBLE GATT 客户端，直接发现 HID 服务并订阅 Report 特征，按固定 10 字节
  报文解析（左/右摇杆、十字键、A/B/X/Y、Start、T/LB 作为 Select）。
- 网页「蓝牙手柄」面板支持扫描、手动连接、断开，并显示已连接手柄名和扫描结果。
- 配对绑定（bond）保存在 NVS，正常情况下手柄开机后会自动回连，无需再按配对键。
- 推荐手柄：
  - **GamepadSpace-Q34B / ShanWan BM-835**（已实测）
  - **Xbox One S / Xbox Series X|S** 的蓝牙模式；
  - **8BitDo** 的 BLE 模式。
- 已验证不兼容：**北通阿修罗 2/2 Pro** 的蓝牙档为 BT Classic（Windows 设备 InstanceId 为
  `BTHENUM\...`，HID UUID `0x1124`，VID/PID `045E:02E0`），ESP32-S3 无法直连。
- 配对时请**关闭电脑/手机蓝牙**，避免手柄被原主机抢占；手柄进入快闪配对状态后再让设备连接。

### 操作

| 场景 | 操作 |
|---|---|
| 菜单 | 十字键/左摇杆上/下选择；A 或 Start 开始；B 刷新列表 |
| 菜单（无手柄） | BOOT 短按下一项；长按开始 |
| 游戏 | A/B、十字键或左摇杆 |
| Select | T 键或 LB 键 |
| 退出游戏 | **长按 Start 1.5 秒**，或长按 BOOT 0.8 秒 |

> 进入游戏后 Wi-Fi 会主动关闭（减少 2.4 GHz 共存干扰、腾出内部 RAM），网页暂时不可访问；退出游戏后自动恢复并重新入网。

## 大屏游玩

2.4 寸屏太小？有两条互不影响的大屏路径，手柄始终连接 ESP32，不需要改配到电脑/电视。

### 电脑：浏览器本地运行（`/tv`）

1. 电脑浏览器打开 `http://设备IP/tv`。
2. 选择 ROM → **开始** → **全屏**。
3. 浏览器用 `jsnes` 在本地全速运行 ROM（ROM 由 ESP32 通过 `/api/rom/file` 提供），
   画面、声音都是浏览器原生的 60fps。
4. ESP32 只通过 WebSocket（`/ws/pad`）以 60Hz 转发手柄按键；关闭页面即恢复设备菜单。

### 电视：ESP32 无头串流（`/stream`）

电视自带浏览器/WebView 的 JS 性能较弱，运行 jsnes 可能只有十几帧。此时改由 ESP32
以 **60fps 无头运行 InfoNES**，只把变化的 8×8 图块（调色板索引）和 24kHz PCM
通过 `/ws/stream` 推给电视；电视只负责 WebGL 调色板贴图和 WebAudio 播放。

- 实测：模拟器 **58–60 fps**（滚动场景约 58），音频约 **23k samples/s**；
  视觉更新约 35–60 次/秒，静止画面约 45–55 KB/s（主要是 24 kHz PCM），滚动场景约 130–190 KB/s。
- 首帧拆成 15 个 4 KB 分块发送，弱 Wi-Fi 下也不会被一个 61 KB 大包卡住。
- 串流期间 2.4" 屏显示 `TV STREAM` 和当前 ROM；关闭页面/按电视返回键即停止并回菜单。
- 电脑也可打开 `http://设备IP/stream?rom=...&gate=1&kiosk=1` 验证，但电脑优先用 `/tv`。

协议（二进制 WebSocket）：

| 类型 | 内容 |
|---|---|
| `0x01` | 图块增量：`[count u16]`，每项 `[tile u16][数据]`；bit15=RLE，bit14=2bpp，图块号 `tile & 0x3FFF` |
| `0x03` | 音频：`[samples u16][int16 LE PCM]`，24 kHz 单声道 |
| `0x04` | 调色板：64 × RGB565 LE |
| `0x06` | 首帧分块：`[offset u16][count u16][索引...]` |

> 图块编码：原始 64 字节；1–2 色图块用 RLE；3–4 色图块用“4 色调色板 + 16 字节 2bpp 位图”
> （22 字节），压缩后更大时自动退回原始数据。NES 图块本身最多 4 色。

> 电视自带浏览器需要支持 WebSocket/WebGL/WebAudio；没有 WebGL 时会回退 2D。
> 若电视网页仍不流畅，请安装下面的 Android TV APK。

### Android TV APK

`tv-app/esp32nes-tv.apk` 是电视原生启动器，不用装浏览器：

- 启动后自动发现 ESP32（UDP 广播 → 局域网扫描，失败可手动输入 IP）；
- 原生 ROM 列表，遥控器方向键选择、OK 开始；
- 游戏页全屏 WebView 加载 `/stream?rom=...&gate=1&kiosk=1`，按 OK 开始；
- 由 ESP32 跑模拟器，电视只解码图块和 PCM，实测约 58–60fps；
- 手柄仍连 ESP32，APK 只负责大屏显示；按返回键结束串流。

构建与安装见 `tv-app/README.md`。APK 为 debug 签名，适合个人侧载；最低 Android 5.0，
电视需带 Android WebView。

### 裸 ESP32-S3 无头串流固件

如果只有一块 ESP32-S3 开发板（没有 ST7789/ES8388/XL9555），可以构建 **无头电视串流版**：
保留 WiFi、BLE 手柄、FAT ROM 存储、`/stream` 和 Android TV 发现，不初始化本地屏幕和音频。

```bash
idf.py -B build_headless -DNES_HEADLESS=ON build
idf.py -B build_headless -p COMx flash
```

- 首次开机没有 WiFi 时会开 `NES-XXXX` 热点（密码 `nes12345`），连上后打开
  `http://192.168.4.1/` 配网；配好后串口会打印 `ready on LAN: http://<IP>/`。
- 没有屏幕显示 IP：可看串口、路由器 DHCP 列表，或直接用 Android TV APK 的 UDP 自动发现。
- 无本地屏幕/音频，`/api/play` 的本地启动会被忽略；请用 `/stream` 页面或 Android TV APK 游玩。
- 这是独立构建目录 `build_headless/`，不会影响正点原子整机固件。

## 内存与帧率

- InfoNES 双帧缓冲、电池存档区、APU 事件队列放 PSRAM；`ChrBuf`/`PPURAM` 留内部 RAM。
- PRG+CHR 总大小 ≤ 96 KB 时优先放内部 RAM（6502 取指快），否则回落 PSRAM。
- NimBLE 主机内存分配走 PSRAM（`CONFIG_BT_NIMBLE_MEM_ALLOC_MODE_EXTERNAL`），Wi-Fi 收/发缓冲压到 3/8，保证游戏时内部 RAM 仍有 ~115 KB。
- 帧节拍 `esp_timer` 16639 µs（NTSC 60.0988 Hz）；模拟器热循环固定在 **CPU1**，
  Wi-Fi/BLE/音频/显示在 CPU0，实测稳定 60.1 fps。
- 串流时 lwIP 与 httpd 也固定 CPU0，避免 TCP 处理抢占模拟器；TCP 发送缓冲 16 KB、
  接收窗口 8 KB。发送端只对 960 个图块做 FNV 哈希，变化图块才查调色板，音频 80 ms 一块。
- 音频：ES8388 的 `0x2E..0x31` 模拟输出音量必须显式写 `0x1E`（0 dB），否则喇叭所在的
  OUT2 默认 -45 dB；退出游戏只静音、不关闭 I2S（IDF 6 上重新打开共享双工口会导致静音）。
  欠载时用最后一个样本填充，避免爆音。

## 关键配置

- Flash：16 MB / QIO / 80 MHz；PSRAM：Octal 80 MHz。
- 分区：`factory` 3 MB + `storage` FAT ~13 MB。
- FreeRTOS 1000 Hz；主任务 8 KB 栈；任务看门狗 10 s。
- `menuconfig → ESP32-NES`：配网热点前缀/密码/信道、默认 Wi-Fi。

## 排障

- **连不上 Wi-Fi**：确认是 2.4 GHz；设备 15 秒后会回退热点，重新配网。
- **网页打不开**：看屏幕顶部 IP；确认电脑/手机与设备同一局域网。
  如果同一 SSID 的 mesh 路由把设备和你分到不同节点，节点间可能开了客户端隔离，互相 ping 不通；
  换到同一节点、关闭 AP 隔离，或使用设备配网热点。
- **手柄显示 Xbox 但连不上**：在 Windows 设备管理器看 InstanceId；`BTHENUM\...` 是蓝牙经典，
  ESP32-S3 不支持；`BTHLE\...` 才是 BLE。北通阿修罗 2 Pro 属于前者。
- **游戏中网页打不开**：正常，Wi-Fi 在游戏时关闭；退出游戏即恢复。
- **串口一开就复位**：CH340 的 DTR/RTS 接 EN/IO0，打开串口可能触发复位；抓“运行中崩溃”时注意区分。
- **声音异常**：确认 ES8388 音量（默认 70）。若完全无声，检查 `0x2E..0x31` 是否都为 `0x1E`；
  若有“呲呲”爆音，确认模拟器运行在 CPU1（本工程已固定），并检查欠载日志。
- **电视串流声音有杂音**：正常状态下 `/api/status` 的 `audio_overrun` 应保持 0，
  浏览器控制台 `window.__streamAudio` 的 `underruns` / `overflows` 也应保持 0。
  当前固件采用音频优先发送 + 设备端 2.7 s 音频环 + 网页端约 400 ms 抖动缓冲。
- **中文文件名显示 `?`**：只有超出 GB2312 的生僻字会这样，常用汉字已覆盖；
  字形由 `tools/gen_cjk_font.py` 从 GNU Unifont 生成。

## 许可证

- InfoNES：Apache-2.0，见 `licenses/InfoNES-LICENSE.txt`。
- `esp_codec_dev`：Apache-2.0，见 `licenses/esp_codec_dev-LICENSE.txt`。
- 中文字库：GNU Unifont 18.0.01 的 GB2312 + ASCII 子集，SIL OFL 1.1，
  见 `licenses/unifont-OFL-1.1.txt`；生成脚本 `tools/gen_cjk_font.py`。
- `main/input/esp_hid_gap.c/h`：来自 ESP-IDF `esp_hid_host` 示例，Unlicense OR CC0-1.0。
- `main/board/font8x8_basic.h`：Daniel Hepper，Public Domain。

## 暂未包含

SD 卡、USB 手柄、即时存档、OTA、中文 UI、LVGL。
