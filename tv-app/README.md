# ESP32-NES TV APK

Android TV 端原生启动器：自动发现 ESP32、列出 ROM，选中后全屏 WebView 打开 `/stream`。
模拟器由 **ESP32 以约 58–60fps 无头运行**，只把变化的 8×8 图块（RLE/2bpp）和 24kHz PCM
推给电视；电视只做 WebGL 贴图和 WebAudio 播放。**手柄始终连接 ESP32**，不需要配对到电视。

## 构建

需要：

- Android SDK（platform `android-36`，build-tools `36.1.0`）
- JDK 17/21（脚本默认使用 Android Studio 的 JBR 21）

```bash
export ANDROID_SDK_ROOT="C:/Users/venus/AppData/Local/Android/Sdk"
bash tv-app/build_apk.sh
```

输出：`tv-app/esp32nes-tv.apk`（debug 签名，可直接侧载）。
签名密钥固定保存在 `tv-app/debug.keystore`（已在 `.gitignore` 中），后续版本可直接覆盖安装。

## 安装到电视

方式一：U 盘

1. 把 `esp32nes-tv.apk` 拷到 U 盘；
2. 电视上打开文件管理器，允许“未知来源”安装；
3. 安装并启动 **ESP32-NES TV**。

方式二：ADB（电视开启开发者选项/USB 调试或网络调试）

```bash
adb connect <电视IP>:5555
adb install -r tv-app/esp32nes-tv.apk
```

## 使用

1. 确保 ESP32 已入网，手柄已连接 ESP32；
2. 打开 APK，自动发现设备（UDP 广播 → 局域网扫描）；
3. 遥控器方向键选择 ROM，按 OK；
4. 游戏页显示“按 OK 开始”，按 OK 后开始串流并全屏显示；
5. 使用连接在 ESP32 上的手柄游玩；按电视返回键结束串流，回到 ROM 列表。

如果自动发现失败：

- 点右下角 **手动输入 IP**，输入屏幕/网页上显示的设备 IP（如 `192.168.1.63`）；
- 确认电视和 ESP32 在同一局域网、未开启 AP 隔离。

## 说明

- 需要电视支持 Android WebView（绝大多数 Android TV 自带；没有则安装/更新 “Android System WebView”）。
- 电视端需要 WebGL/WebAudio；极老机型没有 WebGL 时会回退 2D，帧率会下降。
- 最低 Android 5.0（API 21）。
- APK 使用 debug 签名，适合个人侧载；正式分发请换自己的 release 签名。
- 开始按钮需要一次遥控器 OK 手势，用于解锁 WebAudio；APK 已在 Activity 层拦截 OK。
- 串流带宽：静止画面约 45–55 KB/s（主要是音频），滚动场景约 130–190 KB/s；音频 24 kHz 单声道。
- 固件需要包含 `/stream`、`/ws/stream`、`/api/roms`、`/api/rom/file` 和 UDP discovery（端口 8266）。
- 串流期间 ESP32 的 2.4" 屏显示 `TV STREAM`，本地显示/音频路径不参与，电脑 `/tv` 不受影响。
