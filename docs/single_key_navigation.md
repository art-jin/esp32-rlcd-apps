# 单键导航方案（Single-Key Navigation）

针对原厂 Waveshare ESP32-S3-RLCD-4.2 裸板（仅 GPIO18 一个按键可用于 App UI）的导航设计。

## 背景：硬件事实

| 配置 | 可用按键 | 用途 |
|---|---|---|
| **裸板 3 键** | BOOT (GPIO0) / PWR / KEY (GPIO18) | BOOT 仅下载模式，PWR 硬件级电源，**只有 GPIO18 可用于 App UI** |
| **扩展 4 键** | GPIO1=PREV / GPIO3=NEXT / GPIO17=OK / GPIO18=BACK+长按 | 完整的 4 键导航，长按保留给 CodePilot STT |

> 注意：之前文档里说的"3 键"是误导。裸板真正可用于 App 的只有 GPIO18 一个键。
> BOOT 不可用作 App 导航（下载模式专用），PWR 是硬件电源开关。

## 问题：当前实现的 UX 死结

启动链路（`hello_world_main.c:138` → `app_manager.c:59`）：

```
app_main() → app_manager_init() → dispatcher_task → enter_app(APP_ID_MENU)
```

MENU 屏幕画一个高亮"日历"box + 底部提示"OK:进入"。要进入日历，**必须按 KEY_ENTER**（GPIO17），但裸板没接 GPIO17。MENU 也不响应 KEY_LONG_START。

**结论**：裸板单 App 固件下，用户卡在 MENU 屏，按 GPIO18 任何节奏都没用。

## 方案：分两阶段实施

### 阶段 1（MVP，立即实施）

**单 App 直接进日历**：`dispatcher_task` 启动时判断 `app_registry_count_enabled()==1`，是则跳过 MENU 直接 `enter_app(APP_ID_CALENDAR)`。

```c
// app_manager.c dispatcher_task 开头
app_id_t initial = (app_registry_count_enabled() == 1)
                   ? APP_ID_CALENDAR
                   : APP_ID_MENU;
enter_app(initial);
```

**影响范围**：3 行代码改动，零回归风险（多 App 时仍走 MENU 路径）。

**验证**：
- 单 App 配置（出厂默认）开机不显示 MENU，直接进日历
- KinCal fetch 正常，1Hz worker 正常

### 阶段 2（多 App 启用时实施）

**双击检测 + 单键菜单导航 + 长按退出**。

#### 2.1 keyboard.c 加双击状态机

新增事件类型 `KEY_DOUBLE_CLICK`。仅 `!s_has_back_key` 时启用双击检测：

```c
// 状态机（仅单键模式）
GPIO18 短按 → 进入 250ms 等待窗口
  ├── 窗口内再来短按 → 发 KEY_DOUBLE_CLICK
  └── 窗口超时无再来 → 发 KEY_SINGLE_CLICK（即原 KEY_USER）
```

**长按阈值**：从 500ms 提到 600ms，给双击窗口（250ms）+ 缓冲留余地。

**双击窗口调优**：250ms 是经验值（小米手环用 300ms，浏览器用 500ms 但太迟钝）。可配置项放在 keyboard.c 顶部 `#define DOUBLE_CLICK_MS 250`。

#### 2.2 menu_app.c 单键分支

```c
void menu_on_key(key_event_t key) {
    if (!keyboard_has_back_key()) {
        // 单键模式
        switch (key) {
            case KEY_USER:           // 单击 = 光标下一项
                s_cursor = (s_cursor + 1) % MENU_ITEM_COUNT;
                redraw_cursor();
                break;
            case KEY_DOUBLE_CLICK:   // 双击 = 确认进入
                app_manager_switch(s_menu_items[s_cursor]);
                break;
            case KEY_LONG_START:     // 长按 = 无操作（菜单是顶层）
            default: break;
        }
        return;
    }
    // 4 键模式：原逻辑
    ...
}
```

#### 2.3 calendar_app.c 长按返回菜单

仅当 `app_registry_count_enabled() > 1` 时，单键模式下长按 GPIO18 = 返回菜单：

```c
// calendar_on_key 新增分支
case KEY_LONG_START:
    if (!keyboard_has_back_key() && app_registry_count_enabled() > 1) {
        app_manager_switch(APP_ID_MENU);
    }
    break;
```

> 单 App 时长按仍是 no-op（菜单是空的，回不去）。

## 交互规范（最终方案）

### 单键硬件（裸板，`!has_back_key`）

| 上下文 | 短按 GPIO18 | 双击 GPIO18 | 长按 GPIO18 |
|---|---|---|---|
| MENU（多 App） | 光标下一项（循环） | 确认进入选中 App | 无操作 |
| 日历 | 清选择 / 回今日 | （保留） | 返回 MENU（仅多 App） |

### 4 键硬件（带扩展，`has_back_key`）

保持现有行为不变：
- GPIO1/3/17/43 = PREV/NEXT/ENTER/BACK
- GPIO18 短按 = KEY_USER（App 自定义）
- GPIO18 长按 = KEY_LONG_START（CodePilot STT 触发器）

## 工程量评估

| 阶段 | 改动文件 | 行数 | 风险 |
|---|---|---|---|
| 阶段 1 | `app_manager.c` | 3 行 | 零（多 App 路径不变） |
| 阶段 2 | `keyboard.h/.c` + `menu_app.c` + `calendar_app.c` | ~120 行 | 双击窗口需调优；4 键硬件零影响（条件编译） |

## 实施顺序

1. ✅ 写方案文档（本文）
2. 阶段 1：改 `app_manager.c` → build → flash → 验证
3. 阶段 2：keyboard 双击 → menu 单键分支 → calendar 长按退出 → build → flash → 验证
4. 阶段 2 验证时临时打开 `CONFIG_KINCAL_APP_CODEPILOT=y` 模拟多 App 场景

## 已知限制

1. **双击延迟**：单键用户每次短按要等 250ms 才触发单击事件。这是单键交互的固有代价。
2. **CodePilot STT 长按冲突**：单键模式下长按已用于"返回菜单"，CodePilot 的 STT 触发器在单键硬件下不可用。建议：单键硬件不启用 CodePilot（Kconfig 已经支持门控）。
3. **三击不支持**：不引入三击事件，避免状态机复杂化。
