# 测试用例（Test Cases）

固件功能验证测试用例集。每个用例包含前置条件、操作步骤、预期结果。

> **状态约定**：⏳ 未执行 / ✅ 通过 / ❌ 失败 / 🚫 阻塞

---

## TC-1: 启动路由（阶段 1）

### TC-1.1: 单 App + 4 键硬件 → 直接进日历

| 项 | 内容 |
|---|---|
| 前置 | `CONFIG_KINCAL_APP_*=n`（仅 Calendar）；NVS `has_back_key=1`；KinCal 已配对 |
| 步骤 | 上电启动 → 等待 ~25s（WiFi+SNTP+KinCal） |
| 预期 | 不显示 MENU 屏；直接显示日历月视图；左面板显示天气+事件；右面板月历高亮今日 |
| 串口日志 | `Entering app: 日历`（不出现 `Entering app: 主菜单`） |
| 验证点 | 串口 grep `Entering app` 应只匹配 Calendar |

### TC-1.2: 单 App + 单键硬件 → 直接进日历

| 项 | 内容 |
|---|---|
| 前置 | `CONFIG_KINCAL_APP_*=n`；NVS `has_back_key=0`；KinCal 已配对 |
| 步骤 | 上电启动 → 等待 ~25s |
| 预期 | 同 TC-1.1，启动日志额外显示 `Has BACK key: no` 和 `+ double-click (250ms window)` |
| 验证点 | 串口 grep `skipped — no 4-key hardware` 应匹配 GPIO43 |

### TC-1.3: 多 App + 4 键硬件 → 进 MENU

| 项 | 内容 |
|---|---|
| 前置 | `CONFIG_KINCAL_APP_CODEPILOT=y`；NVS `has_back_key=1` |
| 步骤 | 上电启动 |
| 预期 | 显示 MENU 屏，列出至少 2 项（Calendar + CodePilot），光标默认在 Calendar，底部提示 `PREV/NEXT:选择 OK:进入` |
| 串口日志 | `Entering app: 主菜单` |

### TC-1.4: 多 App + 单键硬件 → 进 MENU（需双击进入）

| 项 | 内容 |
|---|---|
| 前置 | `CONFIG_KINCAL_APP_CODEPILOT=y`；NVS `has_back_key=0` |
| 步骤 | 上电启动 |
| 预期 | 显示 MENU 屏，底部提示 `KEY:选择 双击:进入` |
| 验证点 | 单击 GPIO18 → 光标下移；双击 GPIO18 → 进入选中 App |

---

## TC-2: 双击检测状态机（阶段 2）

> 单键硬件下 GPIO18 三种手势的检测准确性

### TC-2.1: 单次短按 → KEY_USER（延迟 250ms）

| 项 | 内容 |
|---|---|
| 前置 | 单键模式（`has_back_key=0`） |
| 步骤 | 在 MENU 屏按一下 GPIO18（按下 100ms 后释放） |
| 预期 | 释放后约 250ms 触发 `Key event: USER(short)`；光标下移一项 |
| 验证点 | 串口应在释放后 ~250ms（不是立即）出现 `Key event: USER(short)` |

### TC-2.2: 双击 → KEY_DOUBLE_CLICK

| 项 | 内容 |
|---|---|
| 前置 | 单键模式；MENU 屏 |
| 步骤 | 快速按两次 GPIO18（按下-释放-按下-释放，两次按下间隔 < 250ms） |
| 预期 | 第二次释放时立即触发 `Key event: DOUBLE`；进入选中 App |
| 验证点 | 串口应只看到 `Key event: DOUBLE`，**不应**同时出现 `USER(short)` |

### TC-2.3: 两次独立短按（间隔 > 250ms）→ 两个 KEY_USER

| 项 | 内容 |
|---|---|
| 前置 | 单键模式；MENU 屏 |
| 步骤 | 按 GPIO18 → 等 400ms → 再按一次 |
| 预期 | 第一次释放后 250ms 触发 `USER(short)`，第二次释放后再 250ms 触发 `USER(short)`；光标下移两次 |
| 验证点 | 串口出现两次 `USER(short)`，**不出现** `DOUBLE` |

### TC-2.4: 长按 → KEY_LONG_START / KEY_LONG_END

| 项 | 内容 |
|---|---|
| 前置 | 单键模式；Calendar 屏，多 App 配置 |
| 步骤 | 按住 GPIO18 不放（> 600ms）→ 释放 |
| 预期 | 按下 500ms 后触发 `LONG_START`；释放时触发 `LONG_END`；屏幕切换到 MENU |
| 验证点 | 单 App 配置下应只看到 `LONG_START/LONG_END`，**不切换**屏幕（no-op） |

### TC-2.5: 短按 + 长按组合（手势不串扰）

| 项 | 内容 |
|---|---|
| 前置 | 单键模式；MENU 屏 |
| 步骤 | 短按一次（释放后等 400ms）→ 长按 |
| 预期 | 短按 250ms 后 → `USER(short)`，光标下移；长按 500ms 后 → `LONG_START`，no-op |
| 验证点 | 两个手势独立识别，互不影响 |

### TC-2.6: 4 键模式短按无延迟

| 项 | 内容 |
|---|---|
| 前置 | 4 键模式（`has_back_key=1`） |
| 步骤 | 短按 GPIO18 |
| 预期 | 释放后立即触发 `USER(short)`，无 250ms 延迟 |
| 验证点 | 串口时间戳：按下释放与 `Key event` 日志间隔应 < 50ms |

---

## TC-3: MENU 单键导航

### TC-3.1: 短按循环光标

| 项 | 内容 |
|---|---|
| 前置 | 单键模式；MENU 屏（多 App） |
| 步骤 | 单击 GPIO18 多次（每次间隔 > 400ms 避免误判双击） |
| 预期 | 光标依次下移到下一项；到末尾后回到第一项 |
| 验证点 | 屏幕 `>` 指示符位置正确 |

### TC-3.2: 双击进入 App

| 项 | 内容 |
|---|---|
| 前置 | 单键模式；MENU 屏；光标在 Calendar |
| 步骤 | 双击 GPIO18 |
| 预期 | 立即切换到 Calendar App，显示日历月视图 |
| 串口日志 | `DOUBLE: switching to app 1`（Calendar） |

### TC-3.3: 长按 no-op

| 项 | 内容 |
|---|---|
| 前置 | 单键模式；MENU 屏 |
| 步骤 | 长按 GPIO18（> 500ms）→ 释放 |
| 预期 | 屏幕不切换；光标不动 |
| 验证点 | 不应切换到任何 App |

---

## TC-4: Calendar 单键行为

### TC-4.1: 单 App + 短按（无选择）→ no-op

| 项 | 内容 |
|---|---|
| 前置 | 单 App 固件；单键模式；Calendar 屏；无日期选中 |
| 步骤 | 短按 GPIO18 |
| 预期 | 屏幕无变化；串口 `BACK no-op (single-app firmware, no selection)` |

### TC-4.2: 单 App + 短按（有选择）→ 清选择回今日

| 项 | 内容 |
|---|---|
| 前置 | 单 App 固件；单键模式；Calendar 屏；通过 PREV/NEXT 选中某日（在 4 键扩展硬件上做这个测试） |
| 步骤 | 短按 GPIO18 |
| 预期 | 选择框消失；光标回到今日；事件列表回到今日 |
| 串口日志 | 不出现 `BACK no-op`（因为是有选择的情况） |

### TC-4.3: 多 App + 长按 → 返回 MENU

| 项 | 内容 |
|---|---|
| 前置 | `CONFIG_KINCAL_APP_CODEPILOT=y`；单键模式；Calendar 屏 |
| 步骤 | 长按 GPIO18（> 500ms） |
| 预期 | 切换到 MENU 屏 |
| 串口日志 | `switching: 日历 -> 主菜单` |

### TC-4.4: 单 App + 长按 → no-op

| 项 | 内容 |
|---|---|
| 前置 | 单 App 固件；单键模式；Calendar 屏 |
| 步骤 | 长按 GPIO18 |
| 预期 | 屏幕不切换（无可去的 MENU） |
| 串口日志 | 不出现 `switching:` |

---

## TC-5: 4 键硬件回归（防止阶段 2 破坏原有功能）

### TC-5.1: MENU 4 键导航

| 项 | 内容 |
|---|---|
| 前置 | 4 键模式；MENU 屏（多 App） |
| 步骤 | 按 PREV / NEXT / ENTER |
| 预期 | PREV/NEXT 移动光标；ENTER 进入选中 App；底部提示 `PREV/NEXT:选择 OK:进入` |

### TC-5.2: Calendar 4 键日期导航

| 项 | 内容 |
|---|---|
| 前置 | 4 键模式；Calendar 屏 |
| 步骤 | 按 PREV / NEXT / ENTER / BACK |
| 预期 | PREV/NEXT 选日；ENTER 确认；BACK 清选择或返回菜单（多 App） |

### TC-5.3: GPIO18 短按 → KEY_USER 立即（4 键模式）

| 项 | 内容 |
|---|---|
| 前置 | 4 键模式；Calendar 屏 |
| 步骤 | 短按 GPIO18 |
| 预期 | 释放后立即触发 KEY_USER（被 dispatcher 别名为 KEY_BACK）；行为同 BACK |

### TC-5.4: GPIO18 长按 → CodePilot STT 触发

| 项 | 内容 |
|---|---|
| 前置 | 4 键模式；CodePilot 屏；`CONFIG_KINCAL_APP_CODEPILOT=y` |
| 步骤 | 长按 GPIO18（> 500ms） |
| 预期 | 触发 XiaoZhi STT，扬声器静音 |
| 串口日志 | CodePilot/XiaoZhi STT 启动 |

### TC-5.5: GPIO43 (BACK) 物理按键工作

| 项 | 内容 |
|---|---|
| 前置 | 4 键模式；Calendar 屏（多 App） |
| 步骤 | 按 GPIO43 (BACK) 物理键 |
| 预期 | 同 BACK 逻辑：清选择或返回菜单 |

---

## TC-6: KinCal 数据流回归

### TC-6.1: 正常 fetch + 显示

| 项 | 内容 |
|---|---|
| 前置 | KinCal 已配对（短码 NDLCPX 或类似） |
| 步骤 | 上电启动，等待 1 分钟 |
| 预期 | 启动后 ~30s 第一次 fetch；之后每 60s（Pro/Biz）或 300s（Free）刷新 |
| 串口日志 | `OK status=200` + `events_today=N upcoming=M` + `refresh=60s` |

### TC-6.2: 短码无效（404）

| 项 | 内容 |
|---|---|
| 前置 | NVS 中短码改为无效值（如 `XXXXXX`） |
| 步骤 | 重启，观察屏幕和日志 |
| 预期 | fetch 失败；日历保留本地静态数据；下次刷新继续重试 |
| 串口日志 | `KinCal fetch failed: ESP_ERR_HTTP_FETCH_ERROR` 或类似 |

### TC-6.3: WiFi 未配 → AP 模式

| 项 | 内容 |
|---|---|
| 前置 | 擦除 NVS（`esptool.py erase_region`） |
| 步骤 | 重启；手机连 `RLCD-XXXX`；浏览器访问 `192.168.4.1` |
| 预期 | 屏幕显示 "请连接 WiFi RLCD-XXXX 配置"；AP 配网页面正常显示，包含 KinCal 短码输入框和 4 键硬件勾选框 |

### TC-6.4: 4 键勾选写入 NVS

| 项 | 内容 |
|---|---|
| 前置 | AP 配网中 |
| 步骤 | 在配网页面勾选"我有 4 键硬件"，提交 |
| 预期 | 重启后 `Has BACK key: yes`，GPIO43 注册为 KEY_BACK |

### TC-6.5: 不勾选 4 键（默认单键）

| 项 | 内容 |
|---|---|
| 前置 | AP 配网中 |
| 步骤 | 在配网页面不勾选"4 键硬件"，提交 |
| 预期 | 重启后 `Has BACK key: no`，GPIO43 skipped，GPIO18 启用双击检测 |

---

## TC-7: UI 视觉验证

### TC-7.1: 事件标题字体清晰（16×16）

| 项 | 内容 |
|---|---|
| 前置 | Calendar 屏，有事件 |
| 步骤 | 肉眼检查左面板事件标题 |
| 预期 | 中文字符清晰无模糊；最多显示 7 个汉字（截断） |
| 验证点 | 与之前 14×14 模式对比应明显更清晰 |

### TC-7.2: 月历压缩后日格可读

| 项 | 内容 |
|---|---|
| 前置 | Calendar 屏 |
| 步骤 | 肉眼检查月格（CELL_W=33） |
| 预期 | 日期数字（10×20 bitmap）完整显示；事件标记可见；休息日有视觉区分 |
| 验证点 | 不应出现日期数字被裁切或与事件标记重叠 |

### TC-7.3: 中文显示无乱码

| 项 | 内容 |
|---|---|
| 前置 | 各 App 屏 |
| 步骤 | 肉眼检查所有 GB2312 中文字符 |
| 预期 | "天气"、"代办事宜"、"选择"、"进入"、"双击"等显示正确 |
| 验证点 | 不应有方块、问号或错位 |

### TC-7.4: MENU 提示按硬件切换

| 项 | 内容 |
|---|---|
| 前置 | MENU 屏（多 App） |
| 步骤 | 分别在单键/4 键硬件下检查底部提示 |
| 预期 | 单键：`KEY:选择  双击:进入`；4 键：`PREV/NEXT:选择  OK:进入` |

---

## TC-8: 故障模式

### TC-8.1: WiFi 断开后行为

| 项 | 内容 |
|---|---|
| 前置 | Calendar 运行中，KinCal 已 fetch 过 |
| 步骤 | 拔路由器电源 / 关 WiFi |
| 预期 | 日历保留最后一次成功的数据；屏幕不崩；定时刷新继续尝试，失败后退避 |
| 串口日志 | 周期性 `KinCal fetch failed` |

### TC-8.2: 服务端不可达（DNS 失败）

| 项 | 内容 |
|---|---|
| 前置 | 修改 NVS 让 `kincal_host` 指向不存在域名 |
| 步骤 | 重启 |
| 预期 | fetch 失败但其他功能（日历渲染、SHTC3、状态栏）正常 |

### TC-8.3: 看门狗不触发

| 项 | 内容 |
|---|---|
| 前置 | 长时间运行 |
| 步骤 | 让设备运行 10 分钟，观察是否触发 `Task watchdog` |
| 预期 | 不应出现 WDT 复位或 assert |
| 串口日志 | 不出现 `abort()` 或 `WDT` 字样 |

---

## 测试执行顺序建议

1. **冒烟测试**（必做）：TC-1.1, TC-1.2, TC-6.1
2. **阶段 1 验证**：TC-1.3, TC-1.4
3. **阶段 2 单键交互**：TC-2.1～TC-2.6, TC-3.1～TC-3.3, TC-4.1～TC-4.4
4. **回归测试**：TC-5.1～TC-5.5
5. **数据流**：TC-6.2～TC-6.5
6. **UI 视觉**：TC-7.1～TC-7.4
7. **故障模式**：TC-8.1～TC-8.3

---

## 测试矩阵（硬件 × 配置）

| 配置 \ 硬件 | 单键裸板 | 4 键扩展 |
|---|---|---|
| 单 App（KinCal 工厂） | TC-1.2, TC-4.1, TC-4.2, TC-4.4 | TC-1.1, TC-5.1～5.5 |
| 多 App（启用 CodePilot） | TC-1.4, TC-3.1～3.3, TC-4.3 | TC-1.3, TC-5.1～5.5 |

---

## 失败处理

任一用例失败时：
1. 记录失败现象（屏幕截图、串口日志片段）
2. 标记状态为 ❌ 并填写失败原因
3. 在对应模块的 issue tracker 开 bug
4. 修复后必须重跑该用例所在组的所有用例（防止回归）
