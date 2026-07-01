# ESP32-RLCD-Apps

> 双语：[English](README.md) | [中文](README_zh.md)

基于 **Waveshare ESP32-S3-RLCD-4.2** 开发板的多应用固件——把 4.2 寸反射式 LCD 升级成一个个人智能终端，6 个独立应用通过屏幕菜单切换。

基于 ESP-IDF v6.0.1 开发。测试平台：ESP32-S3-WROOM-1-N16R8（16 MB Flash + 8 MB Octal PSRAM）。

## 应用一览

| 应用 | 描述 | 状态 |
|------|------|------|
| **MENU**（主菜单）| 5 项竖排列表，光标上下移动 | ✅ 已验证 |
| **日历** | 公历+农历（1901-2050）月视图、真实天气（Open-Meteo）、飞书 CalDAV 事件、室内温湿度（SHTC3）、3 键日期导航 | ✅ 已验证 |
| **小智** | 小智 AI 语音助手（WebSocket 接公共服务器）、Opus 编解码、ES8311/ES7210 音频 | ✅ 已验证 |
| **CodePilot** | Claude/Kimi 状态仪表盘，通过 WebSocket (端口 7897) 接收 PC bridge 数据，3 个 view（split/detail/notification），长按 GPIO18 触发语音转文字（复用小智 STT，扬声器静音）| ⏳ 代码完成，硬件验证待做 |
| **贪吃蛇** | Nokia 经典贪吃蛇，相对转向控制，渐进加速 | ⏳ 代码完成，硬件验证待做 |
| **俄罗斯方块** | 标准 10×20 Tetris，7 种方块 ×4 旋转，长按硬降 | ⏳ 代码完成，硬件验证待做 |

## 硬件

- **板子**：[Waveshare ESP32-S3-RLCD-4.2](https://www.waveshare.com/wiki/ESP32-S3-RLCD-4.2)
- **屏幕**：ST7306 400×300 单色反射 LCD（SPI）
- **音频**：ES8311（扬声器）+ ES7210（4 通道麦克风）走 I2S
- **传感器**：SHTC3 温湿度（I²C 地址 0x70，与 codec 共享总线）
- **按键**：
  - **原厂单键**：BOOT (GPIO0, 仅下载模式) + PWR (硬件电源) + KEY (GPIO18，**唯一**可用于 App UI 的按键)
  - **扩展 4 键**：通过 2×8 排针接入 PREV / NEXT / ENTER / BACK（GPIO1/3/17/43）

### GPIO 分配

| 功能 | GPIO |
|------|------|
| SPI CLK / MOSI / CS / DC / RST | 11 / 12 / 40 / 5 / 41 |
| I2S MCLK / BCLK / WS / DIN / DOUT | 16 / 9 / 45 / 10 / 8 |
| 扬声器放大使能 | 46 |
| I²C SDA / SCL（codec + SHTC3）| 13 / 14 |
| 电池 ADC（3 倍分压）| 2 |
| BOOT（小智语音触发）| 0 |
| PREV / NEXT / ENTER / BACK（仅 4 键扩展）| 1 / 3 / 17 / 43 |
| KEY（单键 hw：短按+长按+双击；4 键 hw：USER + 长按）| 18 |

> **硬件变体**：原厂单键硬件只有 BOOT + PWR + KEY (GPIO18)。扩展 4 键加 PREV/NEXT/ENTER/BACK。4 键标志在 AP 配网时通过 captive portal 勾选框采集，存入 NVS。
>
> GPIO43 原本是 UART0 TX。固件把 console 输出重定向到 USB-Serial/JTAG，从而释放 GPIO43 给 4 键硬件的 BACK 按钮。单键硬件把 GPIO18 走双击检测（短按、双击、长按三种手势）——交互矩阵详见 [`docs/single_key_navigation.md`](docs/single_key_navigation.md)。

## 构建和烧录

需要 ESP-IDF v6.0.1（或兼容版本）。

```bash
# 激活 ESP-IDF 环境
source ~/.espressif/tools/activate_idf_v6.0.1.sh

# 构建
idf.py build

# 烧录（端口号按实际情况调整）
idf.py -p /dev/cu.usbmodem21201 flash

# 串口监控
idf.py -p /dev/cu.usbmodem21201 monitor
```

### 首次开机 WiFi 配置

如果 NVS 里没有保存的 WiFi 凭据，设备会进入 AP 配置模式。连接到该 AP，按提示设置 SSID/密码。后续开机会自动以 STA 模式连接。

### Console 输出

日志通过 USB-Serial/JTAG 输出（与烧录同一个 USB 端口），不需要外接 UART。

## 工程结构

```
.
├── main/
│   ├── hello_world_main.c         入口 + 全局状态
│   ├── app_manager.c              Dispatcher + 协作式生命周期
│   ├── app_framework.h            app_t 接口、app_id_t 枚举
│   ├── app_registry.c             6 个 app 静态注册
│   ├── menu_app.c                 5 项菜单 UI
│   ├── calendar_app.c             日历 worker（天气/CalDAV/SHTC3）
│   ├── calendar.c                 日历渲染库
│   ├── xiaozhi_app.c              小智应用封装（生命周期）
│   ├── xiaozhi_app_display.c      小智 UI + 异步显示队列
│   ├── xiaozhi_display.c          文字/状态回调路由
│   ├── codepilot_app.c            CodePilot 3-view 仪表盘
│   ├── snake_app.c                贪吃蛇游戏
│   ├── tetris_app.c               俄罗斯方块游戏
│   ├── placeholder_app.c          通用占位（Phase D 后已不用）
│   ├── keyboard.c                 5 键 + GPIO18 长按状态机
│   ├── shtc3.c                    SHTC3 I²C 驱动
│   ├── st7306.c                   ST7306 LCD 驱动 + 像素打包
│   ├── hzk16.c                    HZK16 GB2312 字库渲染
│   ├── lunar.c                    农历算法
│   ├── weather.c / caldav.c       HTTP 客户端（Open-Meteo、飞书 CalDAV）
│   ├── wifi_manager.c             WiFi AP/STA + SNTP
│   └── battery.c                  ADC 电池读取
├── components/
│   ├── xiaozhi_core/              小智 C++ 组件（OTA/WS/Opus/pipeline）
│   └── codepilot_core/            CodePilot 协议 + state + WS server
├── spiffs/HZK16                   GB2312 字库文件（6763 字）
├── partitions.csv                 16 MB Flash 分区表
└── sdkconfig.defaults             USB-Serial/JTAG console + HTTPD WS + PSRAM
```

## 架构

```
                    ┌─────────────────┐
                    │   MENU (顶层)   │
                    └────────┬────────┘
                             │ ENTER (光标)
                             ▼
       ┌──────────┬──────────┬──────────┬──────────┬──────────┐
       ▼          ▼          ▼          ▼          ▼          ▼
     日历       小智      CodePilot    贪吃蛇    俄罗斯方块
       │          │          │          │          │
       └──────────┴──────────┴──────────┴──────────┘
                             │ BACK (Key4 短按)
                             ▼
                    ┌─────────────────┐
                    │   MENU (返回)   │
                    └─────────────────┘
```

`app_manager` 的 dispatcher 任务是唯一调用各 app `on_key` / `on_tick_1s` 钩子的线程。各 app 自己启动 worker 任务处理慢操作（HTTP 拉取、游戏循环、WS 接收）。通过 `stop_flag` + 信号量实现协作式退出，确保每次切换都干净释放资源（不做 `vTaskDelete` 强杀）。

**出厂单 App 固件**：KinCal 构建默认只启用日历（其他 App 由 `CONFIG_KINCAL_APP_*` 开关控制）。dispatcher 启动时跳过 MENU，直接进入日历——这是为了让原厂单键板子（只有 GPIO18 可用于 App UI）不会卡在需要 `KEY_ENTER` 才能离开的菜单屏。多 App 构建仍走 MENU。完整单键导航设计见 [`docs/single_key_navigation.md`](docs/single_key_navigation.md)。

## CodePilot PC Bridge

CodePilot 应用从 PC 端 bridge 接收状态更新，bridge 监控 Claude Code（可选 Kimi）进程。

```bash
# Clone bridge（或自己实现；wire format 是 WS 上的 NDJSON）
git clone https://github.com/art-jin/esp32_codepilot
cd esp32_codepilot/bridge
npm install

# 连接到 ESP32（IP 在启动日志或路由器后台查）
node bridge.js --host 192.168.1.87
```

Wire format：每个 WebSocket text frame 一个 JSON 对象，`session.update` 消息字段包括 `provider`、`status`、`current_task`、`active`、`quota_used`、`quota_total`。

## 许可证

Apache 2.0。详见 [LICENSE](LICENSE)。

## 致谢

- [Waveshare](https://www.waveshare.com/) 提供 ESP32-S3-RLCD-4.2 板子和参考示例
- [xiaozhi-esp32](https://github.com/78/xiaozhi-esp32)（作者 78）提供小智语音助手协议
- [ESP-IDF](https://docs.espressif.com/projects/esp-idf/) v6.0.1（Espressif）
- claude --resume 59e39ad1-1613-4c0d-8b76-f6c34820104a
