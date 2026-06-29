# ESP32-S3-RLCD 移植参考文档

> 目标硬件：ESP32-S3 + 400×300 RLCD 显示屏
> 源模板：`mvp_main_lunar.html`（竖屏2/农，Pro/Biz 专属）
> 源分辨率：std 1072×1448 / lo 758×1024 / lg68 1236×1648 / lg7 1264×1680

本文档详细说明竖屏(农)模板的**布局结构、显示内容、数据结构、后端 API 调用方式**，供移植到 ESP32-S3-RLCD 环境时参考。

---

## 1. 竖屏(农) 整体布局

```
┌────────────────────────────────────────────────────┐
│  Header (60% / 40%)                                │
│  ┌──────────────────────┬───────────────────────┐  │
│  │ 2026年5月8日 星期五  │ 北京市  25°C 多云    │  │
│  │                      │        湿度 55%       │  │
│  └──────────────────────┴───────────────────────┘  │
│  ═════════════════════════════════════════════════  │ (3px 黑线)
├────────────────────────────────────────────────────┤
│  月历 (满宽)                                       │
│            « 2026年5月 »                            │
│  ┌────┬────┬────┬────┬────┬────┬────┐             │
│  │ 日 │ 一 │ 二 │ 三 │ 四 │ 五 │ 六 │             │
│  ├────┼────┼────┼────┼────┼────┼────┤             │
│  │    │    │    │    │  1 │  2 │  3 │             │
│  │    │    │    │    │初一│初二│初三│             │
│  ├────┼────┼────┼────┼────┼────┼────┤             │
│  │  4 │  5 │  6 │  7 │  8*│  9 │ 10 │             │
│  │初四│初五│初六│初七│初八│初九│初十│             │
│  ├────┼────┼────┼────┼────┼────┼────┤             │
│  │ 11 │ 12 │ 13 │ 14 │ 15 │ 16 │ 17 │             │
│  │十一│十二│十三│十四│十五│十六│十七│             │
│  ├────┼────┼────┼────┼────┼────┼────┤             │
│  │ 18 │ 19 │ 20 │ 21 │ 22 │ 23 │ 24 │             │
│  │十八│十九│二十│廿一│廿二│廿三│廿四│             │
│  ├────┼────┼────┼────┼────┼────┼────┤             │
│  │ 25 │ 26 │ 27 │ 28 │ 29 │ 30 │ 31 │             │
│  │廿五│廿六│廿七│廿八│廿九│三十│    │             │
│  └────┴────┴────┴────┴────┴────┴────┘             │
│  *今天=黑底白字; 休息日=灰底; 有日程=圆圈边框      │
│  *农历每月初一只显示月名如"四月"                    │
│  *传统节日覆盖农历文字(如"元宵"覆盖"十五")         │
├────────────────────────────────────────────────────┤
│  日程 (50% / 50%)                                  │ (2px 黑线)
│  ┌─────────────────────┬────────────────────────┐  │
│  │ 今日日程            │ 近期日程               │  │
│  │ 09:30 Team站会      │ 05/10 Sprint评审       │  │
│  │ 14:00 产品评审(3A)  │ 05/12 周报            │  │
│  │ ...                 │ ...                    │  │
│  └─────────────────────┴────────────────────────┘  │
├────────────────────────────────────────────────────┤
│  Footer                                            │ (1px 灰线)
│  [QR码] "早上好!"                                  │
└────────────────────────────────────────────────────┘
```

### 1.1 各区块详细说明

| 区块 | 内容 | 宽度占比 | 关键样式 |
|------|------|---------|---------|
| **Header** | 日期(左60%) + 城市/天气(右40%) | 100% | 3px 底部黑线分隔 |
| **月历** | 公历+农历双行月历表，7列表格 | 100% | 满宽，`<<`/`>>` 翻月链接(Pro/Biz) |
| **日程** | 今日(左50%) + 近期(右50%) | 100% | 2px 顶部黑线分隔，1px 竖线分隔两栏 |
| **Footer** | QR码(左) + AI Face 文字(右) | 100% | 1px 顶部灰线分隔 |

### 1.2 月历单元格样式规则

| 状态 | 视觉效果 |
|------|---------|
| 普通 | 黑字白底 |
| 今天 | **黑底白字** (bold) |
| 今天+有日程 | 黑底白字 + 加粗圆圈边框 |
| 休息日(周末/法定假日) | **浅灰背景** (#ddd) |
| 有日程 | **圆圈边框** (2px solid #999) |
| 空白(非本月) | 透明文字(占位但不可见) |
| 农历文字 | 灰色(#666)，今天时为浅灰(#e5e7eb) |

### 1.3 std profile (1072×1448) 字号参考

> ESP32 移植需将以下字号按 400×300 与 1072×1448 的比例缩放。宽度比约 **0.37** (400/1072)，高度比约 **0.21** (300/1448)。建议优先按宽度比例缩放。

| 元素 | std 字号(px) | 用途 |
|------|-------------|------|
| date_size (header) | 32 | 日期文字 → header 中缩小10% = 29px |
| city_size | 32 | 城市名 → header 中缩小10% = 29px |
| weather_detail | ~10 | 天气温湿度 (date_size × 33%) |
| month_size | 28 | 月历标题 "2026年5月" |
| cal_header_size | 18 | 星期表头 "日一二..." |
| cal_day_size | 22 | 公历日期数字 |
| lunar_size | 14 | 农历文字 |
| agenda_title_size | 22 | "今日日程"/"近期日程" |
| agenda_item_size | 18 | 日程条目文字 |
| empty_state_size | 16 | "今天没有安排" 等空态文字 |
| body_padding | 16px 20px | 页面上下左右内边距 |
| qr_size | 56 | QR 码尺寸 |

---

## 2. 后端 API 调用方式

### 2.1 核心发现：无动态数据 API

当前 Kindle 方案中，**所有显示数据由服务端一次性渲染到 HTML**，前端 JS 不发起任何数据 API 调用。ESP32 移植时需要自行决定：

- **方案A**：ESP32 请求同一个 `GET /d/{public_token}` 或 `GET /s/{short_code}` URL，解析返回的 HTML 提取数据
- **方案B**：为 ESP32 新建一个 JSON API 端点，返回结构化数据（推荐）

### 2.2 唯一的前端 HTTP 请求

```javascript
// 网络连通性探测，用于"智能刷新"
GET /health?t=<timestamp>
// 返回：{"status": "ok", "db": "connected"}  (200)
// 或：{"status": "error", "db": "unavailable"} (503)
```

用途：在刷新页面前先探测网络是否可用，避免 Kindle 休眠唤醒后网络未就绪导致错误页。

### 2.3 展示页 URL

```
GET /d/{public_token}          # 直接通过 public_token 访问（无设备绑定限制）
GET /d/{public_token}?month=YYYY-MM  # 查看指定月份（Pro/Biz）
GET /s/{short_code}            # 通过短码访问（Kindle 设备需绑定审批）
GET /health                    # 健康检查
```

### 2.4 返回内容

上述 URL 返回完整 HTML 页面（`Content-Type: text/html`），所有数据已嵌入 HTML 结构中。非 JSON 格式。

---

## 3. 服务端渲染数据结构（供新建 JSON API 参考）

以下列出 `_render_display()` 函数组装的**所有模板上下文变量**，可作为 ESP32 专用 JSON API 的返回字段设计参考。

### 3.1 完整字段清单

```json
{
  // === 基本信息 ===
  "device_name": "我的Kindle",           // 设备名称
  "timezone": "Asia/Shanghai",           // 时区
  "location": "",                        // 设备位置（备用）
  "updated_at_text": "14:30",            // 页面渲染时间 HH:MM
  "refresh_interval_seconds": 60,        // 刷新间隔（Free=300, Pro/Biz=60）

  // === 日期 ===
  "current_date_text": "2026年5月8日 星期五",  // 完整日期文字
  "lunar_date_text": "农历四月十二",            // 农历日期（Pro/Biz）

  // === 月历 ===
  "month_label": "2026年5月",             // 当前月标题
  "weekdays": ["日", "一", "二", "三", "四", "五", "六"],
  "weeks": [                              // 月历网格（6~7行×7列，0=空白）
    [0, 0, 0, 0, 1, 2, 3],
    [4, 5, 6, 7, 8, 9, 10],
    ...
  ],
  "today_day": 8,                         // 今天是几号（查看他月时为 null）
  "lunar_day_map": {                      // 每日农历/节日文字
    "1": "初一",
    "2": "初二",
    "5": "立夏",            // 节气覆盖
    "14": "情人节",          // 传统节日覆盖
    ...
  },
  "event_dates": [1, 5, 8, 15, 22],       // 有日程的日期集合
  "rest_days": [3, 4, 10, 11, 17, 18],    // 休息日（周末+法定假日-调休上班日）

  // === 月历翻页（Pro/Biz）===
  "can_prev": true,                       // 是否可翻到上月
  "can_next": true,                       // 是否可翻到下月
  "prev_month": "2026-04",               // 上月 URL 参数
  "next_month": "2026-06",               // 下月 URL 参数
  "calendar_token": "abc123...",          // 用于翻页 URL 的 public_token

  // === 天气 ===
  "weather_city": "北京",                 // 城市名（可能为空）
  "weather_summary": "北京 25°C 多云 | 湿度55%",  // 压缩摘要
  "weather_temp": 25,                     // 温度(°C, 整数)
  "weather_desc": "多云",                 // 天气描述
  "weather_humidity": 55,                 // 湿度(%), 可能为空字符串

  // === 今日日程 ===
  "events": [
    {
      "time": "09:30",                    // "HH:MM" 或 "全天"
      "title": "Team站会",                // 可能含来源前缀 "(F)" "(G)" "(D)" "(A)"
      "location": "3A会议室"              // 地点，可能为空字符串
    },
    ...
  ],  // 最多4条

  // === 近期日程 ===
  "upcoming_events": [
    {
      "date": "05/10",                    // "MM/DD"
      "time": "14:00",
      "title": "Sprint评审",
      "location": ""
    },
    ...
  ],
  "show_upcoming_paywall": false,         // Free 用户为 true（显示"升级查看更多"）

  // === Footer ===
  "ai_face_message": "早上好!",           // AI Face 文字，根据时间和日程数量变化

  // === 屏幕 profile ===
  "screen_profile": "std",               // "lo" / "std" / "lg68" / "lg7"
  "screen_w": 1072,
  "screen_h": 1448
}
```

### 3.2 日历事件的来源标记

当用户连接了 2 个以上日历源（Biz 套餐），事件标题自动加来源前缀：

| 前缀 | 来源 |
|------|------|
| `(F)` | 飞书日历 |
| `(G)` | Google Calendar |
| `(D)` | 钉钉日历 |
| `(A)` | Apple/ICS 订阅日历 |

### 3.3 AI Face 文字逻辑

根据当前小时和今日日程数量决定：

| 条件 | 文字 |
|------|------|
| 凌晨 0-5 点 | "夜深了，早点休息!" |
| 早晨 6-8 点 | "早上好!" |
| 上午 9-11 点，日程≥3 | "今天有点忙哦" |
| 上午 9-11 点 | "加油!" |
| 中午 12-13 点 | "午休时间~" |
| 下午 14-17 点 | "下午好!" |
| 傍晚 18-19 点 | "傍晚了~" |
| 晚上 20-23 点 | "晚上好!" |

---

## 4. 后端数据服务架构

### 4.1 日历事件获取流程

```
ESP32 → kincal.cn:/d/{token} 或 新 JSON API
       → _render_display()
         → _fetch_events_for_display()
           → calendar_cache (PostgreSQL, per-provider TTL)
             → 命中缓存 → 直接返回
             → 未命中 → 调用各 Provider.get_events()
               → feishu_provider / google_calendar / dingtalk_provider / ics_provider
               → 写入缓存
         → _fetch_event_dates_for_month() (翻看他月时)
         → _fetch_weather_data()
           → weather_cache (PostgreSQL, 30分钟 TTL)
             → 命中 → 直接返回
             → 未命中 → 调用和风天气 API
               → 写入缓存
         → get_holiday_dates() / get_workday_dates()
           → PostgreSQL holidays 表
         → lunarcalendar 库计算农历
         → get_customary_holidays() 传统节日覆盖
```

### 4.2 日历事件缓存

缓存表：`calendar_event_cache`
唯一键：`(user_id, provider, cache_type, cache_key)`

| 字段 | 说明 |
|------|------|
| `cache_type` | `"events"`（日期范围查询）或 `"month"`（单月查询） |
| `cache_key` | `"2026-05-08:182d"`（事件）或 `"2026-05"`（月） |
| `raw_json` | 完整的 provider 原始事件 JSON 数组 |
| `expires_at` | 过期时间 |

TTL 配置（分钟）：
- feishu / google / ics: 默认 10 分钟（`CALENDAR_CACHE_TTL_DEFAULT`）
- dingtalk: 60 分钟（`CALENDAR_CACHE_TTL_DINGTALK`）

### 4.3 天气缓存

缓存表：`weather_cache`
唯一键：`(device_id, location, provider)`

| 字段 | 说明 |
|------|------|
| `provider` | `"qweather"`（实时天气）或 `"qweather_daily"`（逐日预报） |
| `temperature` | 温度 |
| `description` | 天气描述 |
| `humidity` | 湿度 |
| `raw_json` | 扩展数据（temp_high/temp_low，或逐日预报完整 JSON） |

TTL：实时天气 30 分钟，逐日预报 2 小时。

### 4.4 节假日数据

数据库表：`holidays`

| 字段 | 说明 |
|------|------|
| `date` | 日期（Python `date` 类型） |
| `type` | `"holiday"`（法定假日）或 `"workday"`（调休上班日） |

`rest_days` 计算逻辑：`周末 ∪ 法定假日 - 调休上班日`

### 4.5 农历计算

使用 Python `lunarcalendar` 库，关键函数：

```python
from lunarcalendar import Converter, Solar

# 公历 → 农历
solar = Solar(2026, 5, 8)
lunar = Converter.Solar2Lunar(solar)
# lunar.month, lunar.day

# 月名映射
_LUNAR_MONTHS = ["正","二","三","四","五","六","七","八","九","十","冬","腊"]
# 日名映射
_LUNAR_DAYS = ["初一","初二",...,"三十"]

# 农历每月初一只显示月名（如"四月"），其余显示日名（如"十二"）
lunar_label = _LUNAR_DAYS[lunar.day - 1]
if lunar.day == 1:
    lunar_label = f"{_LUNAR_MONTHS[lunar.month - 1]}月"
```

传统节日覆盖（由 `customary_holidays.py` 处理）：

| 日期 | 覆盖文字 |
|------|---------|
| 2月14日 | 情人节 |
| 3月8日 | 妇女节 |
| 5月第2个周日 | 母亲节 |
| 6月第3个周日 | 父亲节 |
| 10月31日 | 万圣节 |
| 11月第4个周四 | 感恩节 |
| 12月25日 | 圣诞节 |
| ... | 更多见源码 |

---

## 5. ESP32 移植建议

### 5.1 推荐方案：新建 JSON API

为 ESP32 新建一个轻量端点，返回 §3.1 中的结构化数据：

```
GET /api/v1/esp32/display/{public_token}
```

返回 JSON（字段见 §3.1），ESP32 端解析后自行渲染到 400×300 LCD。

### 5.2 400×300 分辨率适配要点

- 原始 std profile 为 1072×1448，宽度比 400/1072 ≈ **0.373**
- 月历 7 列 × 5~6 行，在 400px 宽度下每列约 **57px**，需极小字号
- 建议：
  - 月历每格：公历数字 10-12px，农历文字 6-8px
  - Header 日期：12-14px
  - 日程文字：10-12px
  - 天气信息可压缩到单行
  - Footer QR 码可缩小到 32×32 或移除

### 5.3 ESP32 不需要的 Kindle 机制

以下 Kindle 专用机制在 ESP32 环境中不需要：

| 机制 | 说明 |
|------|------|
| 静音音频播放 | Kindle 防休眠用的 `<audio>` 标签 |
| scrollTo 防休眠 | Kindle 浏览器专用 |
| document.title keepalive | Kindle 浏览器专用 |
| Smart Refresh (Image preload) | Kindle 网络探测 + reload |
| 短码设备绑定 (Device Binding) | Kindle 专用安全机制 |
| screen_profile 自动检测 | Kindle UA 检测 |
| 休眠防制三重机制 | Kindle 专用 |

### 5.4 ESP32 刷新策略建议

ESP32 无需 HTML 解析和浏览器刷新，建议：

```
每 N 分钟:
  1. HTTP GET /api/v1/esp32/display/{token}
  2. 解析 JSON
  3. 重绘 LCD
```

刷新间隔沿用套餐逻辑：Free=5分钟，Pro/Biz=1分钟。

### 5.5 HTTP 请求注意事项

- 后端域名为 `kincal.cn`（或 `8.160.185.39`），HTTPS 端口 443
- 如果使用 `GET /d/{token}` 而非 JSON API，返回的是 **HTML**，需自行解析
- `/health` 端点可用于网络连通性检测
- 所有时间数据已是用户时区（`timezone` 字段指定），渲染时无需再转换

---

## 6. 文件索引

移植时可能需要参考的源码文件：

| 文件 | 用途 |
|------|------|
| `backend/app/templates/mvp_main_lunar.html` | 竖屏(农) HTML 模板 |
| `backend/app/api/v1/devices.py` | 展示页路由 + `_render_display()` + `SCREEN_PROFILES` |
| `backend/app/services/weather.py` | 和风天气服务（实时+逐日预报+缓存） |
| `backend/app/services/holidays.py` | 法定假日查询 |
| `backend/app/services/customary_holidays.py` | 传统节日覆盖 |
| `backend/app/services/calendar_cache.py` | 日历事件缓存 |
| `backend/app/services/calendar_registry.py` | Provider 注册表 |
| `backend/app/services/feishu_provider.py` | 飞书日历 Provider |
| `backend/app/services/google_calendar.py` | Google Calendar Provider |
| `backend/app/services/dingtalk_provider.py` | 钉钉日历 Provider |
| `backend/app/services/ics_provider.py` | ICS 订阅日历 Provider |
| `backend/app/services/plan.py` | 套餐限制配置 |
