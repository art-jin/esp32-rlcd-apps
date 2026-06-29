# Waveshare ESP32-S3-RLCD-4.2 月历显示开发说明

## 项目概述

在 Waveshare ESP32-S3-RLCD-4.2 开发板上显示中文月历，功能包括：
- 公历+农历日期显示，月历网格（6行×7列），今天高亮
- 有事件日期圆角方框标记（r=3 Bresenham 弧线），休息日灰度条纹标记（周末+节假日，排除调休工作日）
- WiFi AP 配网（首次启动）+ STA 自动连接 + SNTP 时间同步
- 实时天气（Open-Meteo API）
- 飞书日历事件（CalDAV 协议直接获取），代办事宜+近期日程（原生 16×16 字体，两行布局）
- 底部状态栏：实时时钟（每秒）、WiFi 信号条、电池电量
- **小智 AI 语音助手**：独立应用，按 KEY（GPIO 18）切换，BOOT（GPIO 0）触发对话，连续多轮对话，事件驱动音频管线
- **双应用架构**：日历与小智独立运行，GPIO 18 KEY 按键切换，app_manager 管理任务生命周期

## 硬件信息

| 项目 | 规格 |
|------|------|
| 开发板 | Waveshare ESP32-S3-RLCD-4.2 |
| MCU | ESP32-S3-WROOM-1-N16R8 |
| Flash | 16MB |
| PSRAM | 8MB |
| LCD 控制器 | ST7306 (Sitronix) |
| 屏幕分辨率 | 400 x 300 (横屏) / 300 x 400 (竖屏) |
| 屏幕类型 | 反射式单色 LCD，1-bit 黑白 |
| 帧缓冲大小 | 15,000 字节 |

### SPI 引脚定义

| 功能 | GPIO |
|------|------|
| CLK (SCK) | GPIO 11 |
| MOSI (SDA) | GPIO 12 |
| CS | GPIO 40 |
| DC (数据/命令) | GPIO 5 |
| RST (复位) | GPIO 41 |

## 软件环境

| 项目 | 版本/路径 |
|------|-----------|
| ESP-IDF | v6.0.1 |
| IDF 路径 | `~/.espressif/v6.0.1/esp-idf/` |
| 环境激活 | `source ~/.espressif/tools/activate_idf_v6.0.1.sh` |
| 编译命令 | `python "$IDF_PATH/tools/idf.py" build` |
| 烧录命令 | `python "$IDF_PATH/tools/idf.py" -p /dev/cu.usbmodem21201 flash` |
| 串口设备 | `/dev/cu.usbmodem21201` |

## 开发步骤

### 1. 项目基础配置

修改 `sdkconfig`，将 Flash 大小从默认 2MB 改为 16MB：

```
CONFIG_ESPTOOLPY_FLASHSIZE_16MB=y
CONFIG_ESPTOOLPY_FLASHSIZE="16MB"
```

### 2. 文件结构

```
hello_world/
├── CMakeLists.txt              # 根构建文件 (启用 MINIMAL_BUILD)
├── sdkconfig                   # 项目配置 (Flash 16MB, target: esp32s3)
├── partitions.csv              # 自定义分区表 (4MB app + ~4MB SPIFFS)
├── tools/
│   └── gen_utf8_gb2312.py      # UTF-8→GB2312 查找表生成脚本
├── docs/
│   ├── esp32_rlcd_calendar_prd.md  # 产品需求文档 (PRD)
│   └── dev_esp32_rlcd.md           # Kindle 竖屏移植参考
├── spiffs/
│   └── HZK16                   # GB2312 中文字库 (267,616 字节)
└── main/
    ├── CMakeLists.txt          # 组件注册
    ├── idf_component.yml       # IDF 组件依赖 (espressif/cjson)
    ├── hello_world_main.c      # 主程序入口，定义共享全局变量，委托 app_manager
    ├── app_manager.c/h         # 应用管理器：GPIO 18 KEY 切换，任务生命周期管理
    ├── xiaozhi_display.c       # XiaoZhi 回调路由（根据当前应用分发）
    ├── xiaozhi_app_display.c/h # XiaoZhi 全屏 UI（状态栏+中心图标+聊天区+提示栏）
    ├── st7306.c/h              # ST7306 LCD 驱动 + 图形基础库
    ├── hzk16.c/h               # HZK16 字库读取 + GB2312 渲染
    ├── lunar.c/h               # 农历算法 (1901-2050) + GB2312 字符串表
    ├── calendar.c/h            # 月历+天气+待办+状态栏 渲染（无小智 UI）
    ├── wifi_manager.c/h        # WiFi AP/STA + SNTP + RSSI 查询
    ├── weather.c/h             # Open-Meteo 天气客户端 (HTTP + cJSON)
    ├── battery.c/h             # ADC 电池电压/电量读取 (GPIO2)
    ├── caldav.c/h              # 飞书 CalDAV 客户端 (HTTPS + iCal 解析)
    ├── utf8_gb2312.c/h         # UTF-8→GB2312 运行时转换 (二分查找表)
    └── utf8_gb2312_table.h     # 自动生成的 Unicode→GB2312 映射表 (7445 条目)
```

### 3. CMakeLists.txt

`main/CMakeLists.txt`：

```cmake
idf_component_register(SRCS "hello_world_main.c" "app_manager.c" "xiaozhi_display.c"
                        "xiaozhi_app_display.c" "st7306.c" "hzk16.c" "lunar.c"
                        "calendar.c" "wifi_manager.c" "battery.c" "weather.c"
                        "caldav.c" "utf8_gb2312.c"
                       PRIV_REQUIRES spi_flash esp_lcd driver spiffs
                                    esp_wifi nvs_flash esp_netif esp_http_server
                                    esp_http_client esp_adc esp-tls xiaozhi_core
                       INCLUDE_DIRS "")
```

cJSON 通过 IDF 组件管理器引入：`main/idf_component.yml` 中声明 `espressif/cjson: "*"`

### 4. ST7306 驱动实现

#### 4.1 SPI 通信层

使用 ESP-IDF 的 `esp_lcd` 面板 IO SPI 接口，自动处理 DC 信号线切换：

```c
spi_bus_initialize(SPI3_HOST, &bus_cfg, SPI_DMA_CH_AUTO);
esp_lcd_new_panel_io_spi(SPI3_HOST, &io_cfg, &io_handle);
```

关键配置：
- SPI 主机：SPI3_HOST
- 时钟频率：10 MHz
- SPI 模式：0
- DMA 通道：自动分配

#### 4.2 LCD 初始化序列

ST7306 的初始化需要发送 27 条命令，完整序列来自 Waveshare 官方示例：

```c
send_cmd(0xD6, (uint8_t[]){0x17, 0x02}, 2);  // NVM 加载控制
send_cmd(0xD1, (uint8_t[]){0x01}, 1);         // 升压使能
send_cmd(0xC0, (uint8_t[]){0x11, 0x04}, 2);   // 栅极电压控制
send_cmd(0xC1, (uint8_t[]){0x69,0x69,0x69,0x69}, 4); // VSHP 设置
send_cmd(0xC2, (uint8_t[]){0x19,0x19,0x19,0x19}, 4); // VSLP 设置
// ... (共 27 条命令)
send_cmd_no_params(0x29);  // 开启显示
```

#### 4.3 像素格式 — 2×4 块状打包

ST7306 使用特殊的像素打包格式，每 8 个像素（2 宽 × 4 高）压缩为 1 字节：

```
横屏模式 (400 x 300):
inv_y   = 299 - y
byte_x  = x / 2          (范围: 0 ~ 199)
block_y = inv_y / 4       (范围: 0 ~ 74)
index   = byte_x * 75 + block_y
local_x = x % 2
local_y = inv_y % 4
bit     = 7 - (local_y * 2 + local_x)
```

每个字节的位排列：

```
BIT7  BIT6  BIT5  BIT4  BIT3  BIT2  BIT1  BIT0
y=3   y=2   y=3   y=2   y=3   y=2   y=3   y=2
x=0   x=0   x=1   x=1   x=0   x=0   x=1   x=1
```

#### 4.4 帧缓冲更新

```c
void st7306_update_display(void) {
    send_cmd(0x2A, (uint8_t[]){0x12, 0x2A}, 2);  // 列地址 (25 个位置)
    send_cmd(0x2B, (uint8_t[]){0x00, 0xC7}, 2);  // 行地址 (200 行)
    esp_lcd_panel_io_tx_color(io, 0x2C, framebuf, 15000); // 写入内存
}
```

列地址范围 0x12~0x2A (25 个位置) × 3 字节/位置 × 200 行 = 15,000 字节。

### 5. 字体与文字渲染

#### 5.1 ASCII 字体

使用标准 8×16 点阵字体，覆盖 0x00~0x7F 共 128 个字符。每个字符 16 字节，总大小 2,048 字节。
另外提供 7×14 缩放版本（`st7306_draw_char_7x14`）和 10×20 自定义位图数字字体（`st7306_draw_digit_10x20`，从 Helvetica 20pt 生成，像素级完美无锯齿）。

#### 5.2 中文字符 "的"

为 UTF-8 编码的 "的" (0xE7 0x9A 0x84) 制作 16×16 点阵位图 (32 字节)。使用 Python Pillow 库从系统字体 (STHeiti Light) 生成：

```python
from PIL import Image, ImageDraw, ImageFont
img = Image.new('1', (16, 16), 0)
draw = ImageDraw.Draw(img)
font = ImageFont.truetype('/System/Library/Fonts/STHeiti Light.ttc', 15)
draw.text((x_off, y_off), '的', font=font, fill=1)
# 转换为 2 字节/行的 C 数组
```

#### 5.3 UTF-8 文字渲染

支持混合 ASCII + 中文字符的渲染：

- ASCII 字符：1 字节 UTF-8 → 8px 宽
- 中文字符：3 字节 UTF-8 → 16px 宽
- 所有字符统一 16px 高

```c
void st7306_draw_text(int x, int y, const char *text, int color) {
    while (*text) {
        unsigned char c = (unsigned char)*text;
        if (c < 0x80) {
            draw_char_8x16(cur_x, y, c, color);
            cur_x += 8;
            text++;
        } else if ((c & 0xF0) == 0xE0) {
            // 检测 "的" (E7 9A 84)
            if (text[0]==0xE7 && text[1]==0x9A && text[2]==0x84) {
                draw_char_16x16(cur_x, y, font_de_16x16, color);
                cur_x += 16;
            }
            text += 3;
        }
    }
}
```

### 6. 主程序

```c
void app_main(void) {
    st7306_init();                    // 初始化 SPI + LCD

    const char *msg = "Arthur\xe7\x9a\x84Hello World";
    int w = st7306_text_width(msg);   // 计算文字宽度
    int x = (400 - w) / 2;           // 水平居中
    int y = (300 - 16) / 2;          // 垂直居中

    st7306_draw_text(x, y, msg, ST7306_COLOR_BLACK);
    st7306_update_display();          // 推送到屏幕
}
```

文字尺寸计算：
- "Arthur" = 6 × 8 = 48px
- "的" = 16px
- "Hello World" = 11 × 8 = 88px
- 总宽度 = 152px，水平起始 = (400 - 152) / 2 = 124px

## 参考资源

| 资源 | 用途 |
|------|------|
| Waveshare 官方示例 | ST7306 初始化序列、SPI 引脚定义 |
| ESP-IDF `esp_lcd` 组件 | SPI 面板 IO 通信框架 |
| ESP-IDF SSD1306 驱动 | 单色 LCD 驱动参考实现 |
| musicaJack/ST73xx_Reflective_Lcd | 8×16 ASCII 字体数据 |
| Python Pillow | 中文字符点阵生成 |

---

## 7. XiaoZhi AI 语音助手

### 7.1 概述

小智 AI 语音助手作为独立应用运行，通过 KEY 按钮（GPIO 18）在日历和小智之间切换。小智应用内通过 BOOT 按钮（GPIO 0）触发语音对话。支持连续多轮对话（TTS 结束后自动回到 LISTENING 状态）。使用 OTA 激活获取 WebSocket 地址，Opus 编解码音频，ES8311（扬声器）+ ES7210（麦克风）音频硬件。

### 7.1.1 应用管理器 (app_manager)

`app_manager.c/h` 控制两个应用的生命周期切换：

```
┌─────────────┐  GPIO 18 按下  ┌─────────────┐
│  日历应用    │ ◄──────────────► │  小智应用    │
│ (update_task)│                 │(xiaozhi_task)│
│  无音频任务  │                 │  + 音频管线  │
└─────────────┘                  └─────────────┘
       ▲                               ▲
       └──── app_manager_task ─────────┘
              (GPIO 18 ISR + 切换逻辑)
```

- **GPIO 18 ISR**：下降沿中断 + 300ms 防抖，设置 EventGroup 位 `APP_EVT_SWITCH`
- **app_manager_task**（6KB 栈，优先级 2）：阻塞等待切换事件
- **日历 → 小智**：删除 `update_task` → 首次调用 `xiaozhi_init()` / 后续调用 `xiaozhi_prepare_reconnect()` → 创建 xiaozhi_run 任务（32KB 栈）
- **小智 → 日历**：`xiaozhi_stop_listening()` → 等待 200ms → 删除 xiaozhi_task → `xiaozhi_disable_audio()` → 全量重绘日历 → 创建 `update_task`
- 同一时刻只有一个应用运行，任务删除/创建（非挂起/恢复）

### 7.2 组件结构

```
components/xiaozhi_core/
├── CMakeLists.txt          # idf_component_register（C++ 源文件）
├── idf_component.yml       # 依赖：esp_codec_dev, esp-opus, esp_websocket_client, cjson
├── include/
│   └── xiaozhi_bridge.h    # C API（extern "C"）
└── src/
    ├── xiaozhi_bridge.cc   # 状态机 + 主事件循环 + 音频硬件初始化
    ├── ota_client.cc       # OTA 激活 → WebSocket URL + token
    ├── ws_client.cc        # WebSocket 客户端（esp_websocket_client）
    ├── opus_codec.cc       # Opus 编解码器封装
    ├── audio_pipeline.cc   # 3 个 FreeRTOS 任务 + 4 个队列
    ├── button_handler.cc   # BOOT 按钮 GPIO 中断 + 防抖
    └── config.h            # 板级引脚定义 + 音频/协议常量
```

### 7.3 音频硬件

| 功能 | GPIO | 说明 |
|------|------|------|
| I2S MCLK | 16 | 主时钟 |
| I2S WS | 45 | 字选择 |
| I2S BCLK | 9 | 位时钟 |
| I2S DIN | 10 | 麦克风（ES7210 → ESP32） |
| I2S DOUT | 8 | 扬声器（ESP32 → ES8311） |
| PA 使能 | 46 | 功放开关 |
| I2C SDA | 13 | 编解码器控制 |
| I2C SCL | 14 | 编解码器控制 |

### 7.4 音频管线

```
上行（聆听）：
I2S RX (24kHz 4ch, Core 0) → 解交错 ch0 → 重采样 24→16kHz (960 samples)
→ encode_queue → OpusEncoder → send_queue → WebSocket → 服务器
                                       ↓ (信号 bridge event XZ_EVT_AUDIO_SEND)

下行（回复）：
服务器 → WebSocket → 剥离帧头 → decode_queue
                ↓ (信号 bridge event XZ_EVT_WS_DATA)
→ OpusDecoder (优先处理) → playback_queue → I2S TX (24kHz)
```

**核心优化**：
- 音频输入任务固定到 Core 0，编解码+主循环在 Core 1（匹配厂家模式）
- OpusCodecTask 中解码优先于编码处理（降低播放延迟）
- Pipeline 和 WS Client 通过 bridge event group 信号主循环，实现事件驱动
- 重连时队列自动清理（`Init()` 先删除旧队列再创建新的）

### 7.5 协议流程

1. **OTA 激活**：POST `https://api.tenclass.net/xiaozhi/ota/`，JSON 含 board/flash_size/mac/chip_info/version/user_agent
2. **WebSocket 连接**：URL+token 来自 OTA，headers 含 Authorization/Protocol-Version/Device-Id/Client-Id
3. **Client Hello**：`{"type":"hello","version":2,"transport":"websocket","features":{"mcp":true},"audio_params":{"format":"opus","sample_rate":16000,"channels":1,"frame_duration":60}}`
4. **Server Hello**：返回 session_id + 服务端音频参数
5. **聆听**：发送 `{"type":"listen","state":"start"}` + Opus 音频流
6. **回复**：接收 STT 文本 + TTS 语音（Opus 编码）

### 7.6 状态机（连续多轮对话）

```
XZ_IDLE → (BOOT 按钮) → XZ_CONNECTING → (握手成功) → XZ_LISTENING
XZ_LISTENING → (TTS start) → XZ_SPEAKING → (TTS stop) → XZ_LISTENING  ← 连续对话循环
XZ_LISTENING → (listen:stop from server) → 断开连接 → XZ_IDLE
任何状态 → (连接失败) → XZ_ERROR → (5s) → XZ_IDLE
```

**连续对话机制**：
- TTS stop 后不再断开连接，而是回到 LISTENING 状态
- 自动发送 `{"type":"listen","state":"start","mode":"auto"}` 通知服务器重新开始聆听
- 排空 send_queue 中残留的上行音频（避免旧音频干扰下一轮）
- 服务器发送 `listen:stop` 时才真正断开连接回到 IDLE
- 匹配厂家 XiaoZhi V2.1.0 固件行为

**事件驱动主循环**：
```cpp
// LISTENING/SPEAKING 状态下使用事件驱动（非轮询）
EventBits_t bits = xEventGroupWaitBits(events,
    XZ_EVT_STOP | XZ_EVT_AUDIO_SEND | XZ_EVT_WS_DATA,
    pdTRUE, pdFALSE, pdMS_TO_TICKS(100));
```
- `XZ_EVT_STOP`：外部请求停止（app_manager 切换时调用）
- `XZ_EVT_AUDIO_SEND`：Pipeline 有编码完成的音频待发送
- `XZ_EVT_WS_DATA`：WS Client 收到新数据帧
- 延迟从 ~50ms（轮询间隔）降至接近 0

### 7.7 显示架构

小智应用使用独立全屏 UI（`xiaozhi_app_display.c`），不再集成在日历面板中。

**回调路由**（`xiaozhi_display.c`）：
```c
void xiaozhi_on_text(const char *text) {
    if (app_manager_current_app() == APP_XIAOZHI) {
        xiaozhi_app_update_text(text);
        st7306_update_display();
    }
}
```

**小智全屏布局 (400×300)**：
| 区域 | 位置 | 内容 |
|------|------|------|
| 顶部状态栏 | y=0~28 | WiFi 信号条 + 时钟 + 电池图标+电量 |
| 中心图标区 | y=40~180 | 16px "小智" 文字 + 状态文字（待机/聆听中.../回复中.../连接中...） |
| 聊天文字区 | y=210~266 | AI 回复文字，14px 字体，自动换行，最多 3 行 |
| 底部提示栏 | y=268~300 | "GP18:切换日历" + "BOOT:开始对话"（12px） |

**状态对应显示**：
- IDLE：中心 "小智" + "待机"
- CONNECTING：中心 "小智" + "连接中..."
- LISTENING：中心 "小智" + "聆听中..."，聊天区显示 STT 文字
- SPEAKING：中心 "小智" + "回复中..."，聊天区显示 TTS 文字
- ERROR：中心 "小智" + "连接错误"

复用 `calendar.c` 导出的 `calendar_draw_wifi_bars()` 和 `calendar_draw_battery_icon()` 函数。

### 7.8 关键实现细节

- **PSRAM**：Opus 编码器/解码器状态、音频 I/O 缓冲区分配在 PSRAM
- **Binary Protocol V1**：服务器分配 version=1，直接发送 Opus 裸数据（无 V2 头）
- **帧队列**：WebSocket 事件处理器将完整帧 malloc 拷贝到环形队列（MAX_FRAMES=16），防止数据覆盖。`Close()` 时释放所有帧缓冲并重置索引
- **I2S 通道管理**：`xiaozhi_init()` 首次初始化 I2S 通道，后续重连不需重复初始化。`xiaozhi_disable_audio()` 禁用 I2S 通道用于切回日历时
- **重连清理**：`xiaozhi_prepare_reconnect()` 清除残留 EventGroup 位并重置状态为 IDLE，防止第二次切换时因残留事件导致异常行为
- **NVS 持久化**：UUID 存储在 `board` namespace，OTA 凭据存储在 `xz_ws` namespace
- **组件依赖**：`espressif/esp_codec_dev ~1.5`, `78/esp-opus-encoder ~2.4.1`, `espressif/esp_websocket_client ~1.3`
- **内存预算**：日历模式 ~29KB DRAM（无音频任务），小智模式 ~127KB DRAM + ~100KB PSRAM
