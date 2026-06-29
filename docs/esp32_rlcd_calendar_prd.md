# ESP32-S3-RLCD 日历显示需求说明

> 目标硬件：Waveshare ESP32-S3-RLCD-4.2
> 屏幕规格：400×300 横屏，1-bit 单色反射式 LCD (ST7306)
> 参考文档：`docs/dev_esp32_rlcd.md`（Kindle 竖屏模板）

---

## 1. 功能概述

在 ESP32-S3-RLCD-4.2 的 4.2 英寸单色横屏上，显示以下信息：

1. **月历** — 公历 + 农历双行月历表，标记今天、休息日
2. **天气** — 城市、温度、天气描述
3. **待办/日程** — 今日待办 + 近期日程

数据通过 WiFi 从后端 JSON API 获取，周期性刷新显示。

---

## 2. 横屏布局设计 (400×300)

原 Kindle 模板为竖屏 (1072×1448) 纵向堆叠布局。适配 400×300 横屏后，采用**左右分栏**结构：月历占左侧主区域，天气和待办占右侧窄栏。

```
┌──────────────────────────────────────────────────────────────────┐
│ Header (全宽, 高20px)                                           │
│ 2026年5月8日 五  │  农历四月十二  │  北京 25°C 多云              │
├────────────────────────────────────┬─────────────────────────────┤
│                                    │  今日待办                    │
│          月历 (宽280px)            │  ● 09:30 Team站会           │
│                                    │  ● 14:00 产品评审           │
│    2026年5月                       │                             │
│  日 一 二 三 四 五 六              │─────────────────────────────│
│              1  2  3               │  近期日程                    │
│   4  5  6  7 [8] 9 10             │  05/10 Sprint评审            │
│  11 12 13 14 15 16 17             │  05/12 周报                 │
│  18 19 20 21 22 23 24             │                             │
│  25 26 27 28 29 30 31             │                             │
│                                    │                             │
└────────────────────────────────────┴─────────────────────────────┘
```

### 2.1 尺寸规划

| 区域 | 位置 | 宽×高 | 说明 |
|------|------|-------|------|
| **Header** | 顶部全宽 | 400 × 20px | 日期 + 农历 + 天气，一行文字 |
| **月历** | 左侧 | 280 × 268px | 7列 × 6行网格 + 标题行 |
| **待办/日程** | 右侧 | 120 × 268px | 今日待办 + 近期日程 |

> 月历与待办区之间用 1px 竖线分隔。Header 与内容区之间用 1px 横线分隔。

### 2.2 Header (400 × 20px)

单行信息条，从左到右排列：

```
2026年5月8日 五 │ 农历四月十二 │ 北京 25°C 多云
```

- 日期信息（左）：`"2026年5月8日 五"` — 12px 字号
- 农历（中）：`"农历四月十二"` — 12px 字号
- 天气（右）：`"北京 25°C 多云"` — 12px 字号，右对齐

### 2.3 月历区 (280 × 268px)

#### 标题行

```
2026年5月
```
12px 字号，居中显示。

#### 星期表头

```
日 一 二 三 四 五 六
```
10px 字号。

#### 日期网格

7 列 × 6 行，每格 40 × 36px：
- **公历数字**（上行）：10px 字号
- **农历文字**（下行）：8px 字号

网格布局计算：
- 列宽 = 280 / 7 = 40px
- 行高 = (268 - 14(title) - 12(weekday)) / 6 ≈ 40px
- 公历数字在格子内居中偏上，农历文字在格子内居中偏下

#### 单元格样式（1-bit 单色适配）

| 状态 | 视觉效果 |
|------|---------|
| 普通 | 黑字白底 |
| 今天 | **黑底白字填充矩形**（辨识度最高） |
| 休息日 | 水平条纹背景（每隔一行画黑线） |
| 有待办 | 日期数字外围圆角方框（r=3 Bresenham 弧线），今天为白框，其他为黑框 |

> 实际实现：圆角方框标记事件日，条纹背景标记休息日，今天用黑底白字填充。

### 2.4 待办/日程区 (120 × 268px)

右侧窄栏，分为上下两部分：

> 实际布局调整为：左侧 132px（天气+代办事宜），右侧 268px（月历+状态栏）。小智已拆分为独立应用，不再集成在日历布局中。

#### 今日待办（上半区）

```
今日待办
─────────
● 09:30 Team站会
● 14:00 产品评审
● 18:00 周报
```

- 标题 `"今日待办"` — tab 式黑底白字标题头
- 每条待办：`● HH:MM 标题` — 10px 字号
- 最多显示 **5 条**，超出省略
- 无待办时显示 `"今天没有安排"` — 10px

#### 近期日程（下半区）

```
近期日程
─────────
05/10 Sprint评审
05/12 周报
```

- 标题 `"近期日程"` — 与今日待办同区域，虚线分隔
- 每条：`MM/DD 标题` — 10px 字号
- 最多显示 **4 条**
- 上下区之间用 1px 横线分隔

---

## 3. 字体方案：HZK 字库文件

采用 HZK 字库文件方案，将完整中文字库存入 Flash 的 SPIFFS 分区，运行时通过 GB2312 编码查表取位图。无需逐字生成位图、无需改代码加字。

### 3.1 字库文件

| 文件 | 字号 | 大小 | 覆盖范围 |
|------|------|------|---------|
| HZK16 | 16×16 | ~260KB | GB2312 全集（6763 汉字 + 682 符号） |
| ASC12 | 6×12 | ~1KB | ASCII 0x20-0x7E（配合紧凑文字使用） |

HZK16 覆盖所有常用中文字符（农历、节气、天气、城市名、待办标题等），一次写入永久可用。

### 3.2 存储方式

在 16MB Flash 中划分 SPIFFS 分区存放字库文件：

```
# 分区表 (partitions.csv)
# Name,    Type, SubType, Offset,   Size
nvs,       data, nvs,     0x9000,   0x6000
phy_init,  data, phy,     0xf000,   0x1000
factory,   app,  factory, 0x10000,  0x400000    # 4MB (应用固件)
storage,   data, spiffs,  0x410000, 0x3F0000    # ~3.9MB (字库+缓存)
```

烧录时将 HZK16 文件写入 SPIFFS 分区。运行时通过 `spiffs_open/read` 按需读取字符位图。

### 3.3 查表原理

```c
// GB2312 编码 → HZK16 偏移量
// 区码 q = gb[0] - 0xA0, 位码 w = gb[1] - 0xA0
// 偏移量 offset = ((q - 1) * 94 + (w - 1)) * 32

int offset = ((gb0 - 0xA1) * 94 + (gb1 - 0xA1)) * 32;
fseek(f, offset, SEEK_SET);
fread(bitmap, 1, 32, f);  // 16×16 点阵，逐行 2 字节，共 16 行
```

### 3.4 ASCII 字体

| 字号 | 用途 | 来源 |
|------|------|------|
| 8×16 ASCII | 大号文字（月历公历数字等） | 已内嵌于 `st7306.c`（4096 字节） |
| 6×12 ASCII | 紧凑文字（Header、待办时间等） | 新增 ASC12 字体表（~1KB） |

### 3.5 中文渲染字号

所有中文统一使用 HZK16 的 **16×16** 位图。如需更小字号，通过像素缩放实现：

| 显示效果 | 方法 | 用途 |
|---------|------|------|
| 16×16 原尺寸 | 直接使用 | 待办标题、UI 标题、天气 |
| 12×12 缩放 | 3/4 采样缩放 | 农历文字 |

12×12 缩放效果可接受。8×8 缩放会损失笔画，不建议使用。

### 3.6 UTF-8 → GB2312 转换

HZK 字库按 GB2312 编码索引，但 API 返回的数据是 UTF-8。需要 UTF-8 → GB2312 转换。

方案：使用 ESP-IDF 内置的 **iconv** 库，或内嵌一份 UTF-8 ↔ GB2312 映射表（~20KB）。

### 3.7 布局调整（配合 16×16 中文 + 12×12 缩放农历）

月历每格调整：

```
列宽 = 280 / 7 = 40px（不变）
每格高度 = 16(公历8×16) + 4(间距) + 12(农历12×12缩放) + 6(间距) = 38px
总行高 = 14(标题) + 12(星期) + 38×6(日期行) = 254px ✓ (在 268px 内)
剩余 14px 可分配给标题或增加行间距
```

---

## 4. 数据获取

### 4.1 JSON API

从后端 `kincal.cn` 获取结构化数据：

```
GET https://kincal.cn/api/v1/esp32/display/{public_token}
```

返回 JSON 字段（参考 `dev_esp32_rlcd.md` §3.1）：

```json
{
  "current_date_text": "2026年5月8日 星期五",
  "lunar_date_text": "农历四月十二",
  "month_label": "2026年5月",
  "weekdays": ["日","一","二","三","四","五","六"],
  "weeks": [[0,0,0,0,1,2,3],[4,5,6,7,8,9,10],...],
  "today_day": 8,
  "lunar_day_map": {"1":"初一","2":"初二","5":"立夏",...},
  "event_dates": [8, 15, 22],
  "rest_days": [3, 4, 10, 11, 17, 18],
  "weather_city": "北京",
  "weather_temp": 25,
  "weather_desc": "多云",
  "weather_humidity": 55,
  "events": [{"time":"09:30","title":"Team站会","location":""}, ...],
  "upcoming_events": [{"date":"05/10","time":"14:00","title":"Sprint评审"}, ...],
  "ai_face_message": "加油!"
}
```

### 4.2 备选方案：离线模式

如果 JSON API 不可用，ESP32 本地计算基础数据：

- **日期**：从 SNTP/NTP 获取网络时间
- **农历**：内嵌简化农历算法表（一年约 200 字节），计算当日农历
- **天气**：显示 `"未连接"` 或上次缓存数据
- **待办**：显示 `"未连接"` 或上次缓存数据

---

## 5. 程序流程

### 5.1 主循环

```
app_main()
  ├── st7306_init()               // 初始化 SPI + LCD
  ├── wifi_init()                 // 连接 WiFi (2.4GHz)
  ├── sntp_sync()                 // 同步网络时间
  ├── 显示 "连接中..." 到屏幕
  │
  └── loop (每 5 分钟):
        ├── HTTP GET JSON API
        ├── 解析 JSON
        ├── st7306_clear()         // 清空帧缓冲
        ├── draw_header()          // 绘制 Header
        ├── draw_calendar()        // 绘制月历
        ├── draw_todos()           // 绘制待办/日程
        ├── draw_separators()      // 绘制分隔线
        └── st7306_update_display() // 推送到屏幕
```

### 5.2 错误处理

| 场景 | 处理 |
|------|------|
| WiFi 断开 | 保留上次显示内容，右上角显示 `!` 标记 |
| API 请求失败 | 保留上次显示内容，下次循环重试 |
| JSON 解析失败 | 降级到离线模式（仅显示日期+月历） |
| NTP 同步失败 | 使用 ESP32 内置 RTC（上次同步时间） |

---

## 6. WiFi 配置

首次使用需配网，支持以下方式之一：

1. **SmartConfig** — 手机 APP 配网
2. **WiFi AP 模式** — ESP32 创建热点 `RLCD-Calendar-xxxx`，手机连接后访问 `http://192.168.4.1` 输入 WiFi 密码
3. **硬编码** — 在 `sdkconfig` 或代码中预设 WiFi SSID/密码（开发调试用）

WiFi 凭据保存到 NVS (Non-Volatile Storage)，断电不丢失。

---

## 7. 依赖组件

| 组件 | 用途 | ESP-IDF 内置 |
|------|------|-------------|
| `esp_wifi` | WiFi 连接 | 是 |
| `esp_netif` | 网络接口 | 是 |
| `esp_http_client` | HTTP 请求 | 是 |
| `nvs_flash` | 存储 WiFi 凭据 | 是 |
| `esp_sntp` | 网络时间同步 | 是 |
| `esp_lcd` | SPI 面板 IO（已有） | 是 |
| `cJSON` | JSON 解析 | 是 |
| `driver` | GPIO（已有） | 是 |
| `esp_spiffs` | 读取 HZK16 字库文件 | 是 |

### 7.1 `main/CMakeLists.txt` 需更新为

```cmake
idf_component_register(SRCS "hello_world_main.c" "st7306.c"
                       PRIV_REQUIRES spi_flash esp_lcd driver esp_wifi esp_netif nvs_flash esp_http_client esp_sntp json esp_spiffs
                       INCLUDE_DIRS "")
```

### 7.2 分区表

需创建 `partitions.csv`（替换默认分区表），为 SPIFFS 分配空间存放 HZK16 字库：

```
# Name,    Type, SubType, Offset,   Size, Flags
nvs,       data, nvs,     0x9000,   0x6000,
phy_init,  data, phy,     0xf000,   0x1000,
factory,   app,  factory, 0x10000,  0x400000,
storage,   data, spiffs,  0x410000, 0x3F0000,
```

在 `sdkconfig` 中设置：
```
CONFIG_PARTITION_TABLE_CUSTOM=y
CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions.csv"
```

---

## 8. 与原 Kindle 版本的差异总结

| 项目 | Kindle 竖屏 | ESP32 横屏 |
|------|-----------|-----------|
| 分辨率 | 1072×1448 | 400×300 |
| 屏幕方向 | 竖屏 | **横屏** |
| 色彩 | 16级灰度 | **1-bit 黑白** |
| 布局 | 纵向堆叠 | **左右分栏** |
| 渲染 | HTML → 浏览器 | **C 帧缓冲 → SPI** |
| 字号 | 14-32px | **8-12px** |
| 灰度样式 | 灰底、圆圈 | **方框、点标记** |
| 刷新 | 浏览器 reload | **定时 HTTP → 重绘** |
| 防休眠 | 音频/scroll | **不需要** |
| QR 码 | Footer 显示 | **移除（空间不足）** |
| 翻月 | Pro/Biz 点击 | **仅显示当月** |
| Footer AI 文字 | 显示 | **移除或合并到 Header** |

---

## 9. 已确认的决策

| 问题 | 决策 |
|------|------|
| JSON API | 未就绪，先做 P0 离线月历 |
| MINIMAL_BUILD | 关闭（引入 SPIFFS 等） |
| HTTPS 证书 | 天气 API 使用 HTTP（Open-Meteo）；CalDAV 使用 HTTPS + ESP-IDF 证书包自动验证 |
| 中文为主/英文为主 | 中文为主 |
| 配网方案 | AP 模式 + Web 页面 |
| 字体方案 | HZK16 字库文件（SPIFFS 分区），原生 16×16 + 14×14/12×12 缩放，自定义 10×20 数字位图字体（Helvetica），7×14 ASCII 缩放 |
| 天气 API | Open-Meteo（免费，无需 API key），坐标硬编码北京 |
| 布局比例 | 左 132px（天气+代办事宜）+ 右 268px（月历+状态栏），事件时间/标题分两行 |
| 待办/日程数据 | 飞书 CalDAV 直连（两步 REPORT：calendar-query + calendar-multiget） |
| CalDAV 认证 | Basic Auth，凭据硬编码（后续迁移至 NVS） |
| UTF-8→GB2312 转换 | 静态查找表（7445 条目，~28KB Flash）+ 二分查找，构建前 Python 生成 |
| 内存管理 | CalDAV 缓冲区全部堆分配（~15KB），避免栈溢出 |
| cJSON | 通过 IDF 组件管理器引入 espressif/cjson |
| 电池 | ADC1_CHANNEL_3 (GPIO2)，3x 分压，3.0V~4.12V |
| 节假日 | 硬编码 2025-2026 年数据表，含调休工作日 |
| 事件标记 | 有事件的日期用圆角方框（r=3 Bresenham 弧线）包裹日期数字，今天白框/其他黑框 |
| 语音助手 | 小智 AI 公共服务器（OTA 激活 + WebSocket），BOOT 按钮触发，无唤醒词 |
| 小智应用 | 独立全屏应用，GPIO 18 KEY 按键切换，连续多轮对话，事件驱动音频管线 |
| 应用架构 | 双应用独立运行，app_manager 管理任务生命周期，同一时刻只运行一个应用 |

## 10. 实现优先级

| 阶段 | 内容 | 依赖 | 状态 |
|------|------|------|------|
| **P0** | 离线月历（本地日期 + 农历 + HZK16 字库） | SPIFFS, 农历算法 | ✅ 完成 |
| **P1** | WiFi 配网（AP 模式 + Web 页面）+ NTP 时间同步 | WiFi, HTTP server | ✅ 完成 |
| **P2** | 天气获取 + CalDAV 待办/日程显示 | CalDAV, JSON API | ✅ 完成 |
| **P3** | XiaoZhi AI 语音助手集成 | PSRAM, I2S, WebSocket, Opus | ✅ 完成 |
| **P4** | 错误状态显示、缓存、低功耗优化 | NVS, 深度睡眠 | ⏳ 部分完成（双应用架构已实现，WiFi 断线重连待做） |

## 11. 实现进度 (2026-05-09, updated)

### 已完成功能
- **P0 离线月历**：公历+农历 Header，7×6 月历网格，今天黑底高亮，HZK16 中文字库
- **P1 WiFi 配网**：AP 模式 Web 配置页（中文），NVS 凭据存储，SNTP 时间同步
- **P1.5 实时数据**：Open-Meteo 真实天气（温度/湿度/天气描述），每秒时钟，WiFi 信号条，电池电量
- **P2 飞书日历**：CalDAV 协议直连飞书日历（HTTPS），获取真实事件，UTF-8→GB2312 运行时转换
- **布局优化**：左右分栏（132+268），左面板全高（tab 式标题头 + 天气/代办事宜，原生 16×16 字体），右面板（月历+状态栏），星期行黑底白字，事件日圆角方框标记，日期数字 10×20 位图字体，header 星期名用原生 16×16 字体
- **休息日标记**：周末+节假日灰度条纹，调休工作日排除
- **节假日数据**：2025-2026 年主要假期 + 调休工作日表
- **P3 小智 AI 语音助手**：独立全屏应用，GPIO 18 KEY 按键切换，BOOT 按钮触发对话，连续多轮对话（TTS 结束后自动重听），事件驱动音频管线，Opus 编解码，ES8311/ES7210 音频硬件
- **P4 双应用架构**：app_manager 管理日历/小智任务生命周期，GPIO 18 切换，重连清理，资源隔离

### 当前文件结构
```
main/
├── hello_world_main.c    — 入口，共享全局变量，委托 app_manager
├── app_manager.c/h       — 应用管理器：GPIO 18 KEY 切换 + 任务生命周期
├── xiaozhi_display.c     — XiaoZhi 回调路由（根据当前应用分发）
├── xiaozhi_app_display.c/h — XiaoZhi 全屏 UI（状态栏+中心图标+聊天区+提示栏）
├── st7306.c/h            — ST7306 LCD 驱动 + 图形基础库
├── hzk16.c/h             — HZK16 字库读取 + GB2312 渲染
├── lunar.c/h             — 农历算法 + GB2312 预编码字符串
├── calendar.c/h          — 月历 + 天气 + 代办事宜 + 状态栏（无小智 UI）
├── wifi_manager.c/h      — WiFi AP/STA + SNTP + RSSI
├── weather.c/h           — Open-Meteo 天气客户端
├── battery.c/h           — ADC 电池电压/电量
├── caldav.c/h            — 飞书 CalDAV 客户端 (HTTPS)
├── utf8_gb2312.c/h       — UTF-8→GB2312 运行时转换
├── utf8_gb2312_table.h   — 自动生成的映射表 (7445 条目)
├── idf_component.yml     — espressif/cjson 依赖
└── CMakeLists.txt
components/xiaozhi_core/
├── CMakeLists.txt      — 组件注册（C++ 源文件）
├── idf_component.yml   — esp_codec_dev, esp-opus, esp_websocket_client 依赖
├── include/
│   └── xiaozhi_bridge.h — C API（extern "C"）
└── src/
    ├── xiaozhi_bridge.cc — 状态机 + 事件驱动主循环 + 音频硬件初始化 + 连续对话
    ├── ota_client.cc     — OTA 激活 + NVS 凭据缓存
    ├── ws_client.cc      — WebSocket 客户端 + 帧队列 + bridge event 信号
    ├── opus_codec.cc     — Opus 编解码器封装
    ├── audio_pipeline.cc — 3 任务 + 4 队列音频管线 + Core pinning + 事件信号
    ├── button_handler.cc — BOOT 按钮 GPIO 中断
    └── config.h          — 板级引脚 + 音频参数常量
tools/
└── gen_utf8_gb2312.py  — 查找表生成脚本
```

### CalDAV 实现细节

协议流程（两步 REPORT 请求）：

1. **calendar-query**：带时间范围过滤（当前月~下月），获取匹配事件的 `.ics` href 列表
2. **calendar-multiget**：携带 href 列表，获取完整 iCal 数据（SUMMARY、DTSTART、DTEND）

关键配置：
- 服务器：`https://caldav.feishu.cn`
- 认证：Basic Auth（CalDAV 用户名+密码）
- TLS：使用 ESP-IDF 证书包（`esp_crt_bundle_attach`）自动验证
- 日历路径：硬编码（用户 UUID 已固定）
- 内存：所有大缓冲区（~15KB）堆分配，避免栈溢出
- UTF-8→GB2312：7445 条静态查找表，二分查找，~28KB Flash

### XiaoZhi AI 语音助手实现细节

**架构**：独立 C++ 组件（`components/xiaozhi_core/`），C API 桥接到主程序。作为独立应用运行，由 `app_manager` 管理生命周期。

**协议流程**：
1. OTA 激活 → POST `https://api.tenclass.net/xiaozhi/ota/` → 获取 WebSocket URL + token
2. WebSocket 连接 → Client Hello（含 `features: {"mcp": true}` + 音频参数）→ Server Hello
3. BOOT 按钮触发聆听 → 发送 `{"type":"listen","state":"start","mode":"auto"}`
4. 上行：I2S 24kHz 4ch → 解交错 ch0 → 重采样 24→16kHz → Opus 编码 → WebSocket Binary
5. 下行：WebSocket Binary → Opus 解码 → I2S 24kHz 输出
6. 服务器推送 STT 文本 + TTS 语音 → 显示文字 + 播放语音
7. TTS 结束 → 自动发送 `listen: start, mode: auto` → 回到聆听（连续多轮对话）
8. 服务器发送 `listen: stop` → 断开连接 → IDLE

**状态机（连续多轮对话）**：
```
IDLE → (BOOT) → CONNECTING → (握手) → LISTENING
LISTENING → (TTS start) → SPEAKING → (TTS stop) → LISTENING  ← 连续对话循环
LISTENING → (listen:stop) → IDLE
```

**事件驱动主循环**：
- LISTENING/SPEAKING 状态使用 `xEventGroupWaitBits` 等待事件（非轮询）
- `XZ_EVT_AUDIO_SEND`：Pipeline 有编码音频待发送
- `XZ_EVT_WS_DATA`：WS Client 收到新数据帧
- `XZ_EVT_STOP`：外部请求停止（app_manager 切换时）

**音频管线（3 个 FreeRTOS 任务）**：
| 任务 | 优先级 | 栈 | 核心 | 职责 |
|------|--------|-----|------|------|
| audio_input | 8 | 4KB | Core 0 | I2S 读 → 解交错 → 重采样 → encode_queue |
| opus_codec | 2 | 32KB | Core 1 | 解码优先 + 编码 Opus |
| audio_output | 4 | 4KB | Core 1 | playback_queue → I2S 写 |

**内存使用**：
- 日历模式：~29KB DRAM（无音频任务），0 PSRAM
- 小智模式：~127KB DRAM + ~100KB PSRAM
- Flash：二进制 ~1.29MB（4MB 分区，68% 空闲）

**显示架构**：
- 独立全屏 UI（`xiaozhi_app_display.c`）：顶部状态栏 + 中心 "小智" + 聊天文字区 + 底部提示栏
- 回调路由（`xiaozhi_display.c`）：检查当前应用，仅在小智应用激活时更新显示
- 复用 `calendar.c` 导出的 WiFi/电池图标绘制函数

**应用切换**：
- GPIO 18 KEY 按钮 → ISR → EventGroup → app_manager_task → 删除/创建任务
- 切换时全屏清屏重绘，共享帧缓冲和 SPI 显示
- 重连时调用 `xiaozhi_prepare_reconnect()` 清除残留事件位

### 待完成
- WiFi 断线自动重连
- 错误状态 UI
- NVS 数据缓存
- CalDAV 凭据迁移到 NVS（目前硬编码）
- Silk 整数重采样器（替换当前浮点重采样，降低 CPU 占用）
- 编解码任务条件变量唤醒（进一步降低延迟）
