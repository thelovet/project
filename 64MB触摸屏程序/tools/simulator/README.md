---
title: HTML 模拟器（显示 + 触摸交互复刻）方案
status: 待实施（2026-09 评估定稿）
target_dir: tools/simulator/
readme_for: 下一个对话可直接按本文开工
---

# HTML 模拟器方案 v1

> **一句话目标**：在浏览器里复刻本固件的**显示 + 触摸交互**，用于快速验证菜单/手势/游戏规则，省掉"改一行就烧录一次"的循环。
> **落地位置**：`tools/simulator/`，**不参与固件编译**，不受 `编译与下载.ps1` 影响（现在 Flash 只剩 12 B，务必别把模拟器混进 `obj/` 构建）。

---

## 0. 目标 / 非目标

**目标**
- 复刻显示与触摸交互：7 个顶层模式、菜单三级（一级按钮 / 二级撤销菜单 / 撤销按钮组 / 下滑退出菜单）、手势打断与冷却
- 画面尽量用**仓库真实图片**（RGB565、调色板瓦片、1bpp 字模）
- 与 C 代码**结构同构**：函数名、变量名一致；每个移植函数注释里标出对应 C 文件与行号

**非目标（明确不做）**
- 不做像素级真机仿真：不模拟 SPI/DMA 时序、触摸噪声、待机电流、外部 Flash 读取耗时
- 不替代上机验证：**"模拟器过 ≠ 真机过"**，尤其手势边界（见 §5）

---

## 1. 现状盘点（评估阶段的实测结论，实现时以代码为准）

### 1.1 顶层模式
`main.c` 里 `MainMode s_main_mode`，取值：
`MAIN_MODE_VIDEO` / `MAIN_MODE_MENU` / `MAIN_MODE_SOKOBAN` / `MAIN_MODE_D2048` / `MAIN_MODE_TETRIS` / `MAIN_MODE_KLOTSKI` / `MAIN_MODE_FISH`
另有：设置子页 `SETTINGS_*`、`g_game_enter_exit_menu`、`g_language_index`、`g_sok_dirty` 等跨模块标志。

### 1.2 显示（240x240，RGB565，坐标 0~239）
LCD API 很少（`SPI/SPI_LCD/LCD/inc/lcd.h`），桩很好写：
| 函数 | 语义（模拟器里） |
|---|---|
| `LCD_Fill(x0,y0,x1,y1,color)` | 矩形填色 |
| `LCD_Fill_DMA(...)` | 同上（真机是 DMA，模拟器里同步填） |
| `LCD_Address_Set(x0,y0,x1,y1)` + `LCD_WR_DATA_16Bit(v)` | 设窗口 + 逐点写（维护"当前窗口游标"） |
| `LCD_DMA_Write_Buffer(...)` / `LCD_drawImageWithSize_DMA(count,pre)` | 图片流写入 |
| `LCD_Enter_Sleep()` / `LCD_Backlight_PWM_*` | 背光=整体亮度滤镜（可选） |

### 1.3 触摸
`User/inc/touch.h`：`TouchState{ x, y, ... }` + `Touch_Scan(&ts)` / `Touch_UpdateCoords()` / `Touch_IsPressed()`。
真机两套 IC 在 `touch.c` 里用宏切换（IT7259 / CST816S），带坐标滤波与去抖 —— 见 §6 待确认项 3。

### 1.4 资源（关键：分三类）
**A. 仓库内 C 数组（可直接转 canvas）**
- 推箱子瓦片：`sok_img_bg` / `sok_img_box` / `sok_img_box_on_target`（palette + 索引型）、`sok_img_player`（RGB565）
- 按钮图：`sok_img_prev` / `sok_img_next` / `sok_img_restart`（+ 各自 `_en` 英文版）、`sok_img_undo`
- 其它：`player_imgs`（9 阶段 RGB565 字节流）、`fish_imgs`（10 条）、`sokoban_maps`（关卡）、`chinese_fonts`（24x24 1bpp）、`g_menu_glyph_ext`（追加字模 21 字）

**B. 外部 Flash 保留区（真机烧录进去的，但源在仓库里，可再生成）**
| 资源 | 大小 | 生成源 |
|---|---|---|
| `BTN_IMG_EN`（上一关/下一关/重新开始 英文三张） | 3 x 1860 B | `上一关图片.txt` + 生成脚本 |
| `BTN_IMG_UNDO_CN` / `BTN_IMG_UNDO_EN` | 各 5580 B | `gen_btn_undo.py` |
| `MENU_GLYPH`（菜单字模 21 字） | 1512 B | `chinese_fonts.c` 的 `g_menu_glyph_ext` |

**C. 视频（唯一"仓库里不全"的资源）**
`SPI/SPI_LCD/User/src/video_chunk.c` 只有 **1 块**（53,248 B），整体 **57 块 / 3,011,488 B**；
`g_chunk_meta[6] = {offset, size, index, total, blocks, crc16}`，`VIDEO_SEGMENT_COUNT 4`；
由 `gen_video_chunk.py` 生成，`burn_all.ps1` 写外部 Flash。

---

## 2. 目录与文件职责

```
tools/simulator/
  index.html          圆屏外壳(240x240) + 调试面板 + canvas
  lcd.js              帧缓冲 + 全部 LCD_* 桩（§3.1）
  touch.js            指针事件 -> 10ms TouchState，含手势脚本回放（§3.2）
  device.js           主循环(60fps) + 外设桩：RTC/电量/电池图标/背光/待机
  gen_consts.py       从 .h 提取 #define -> port/consts.js（§3.4）
  gen_assets.py       从 .c/.txt 提取图片 -> assets/*.png + index.json（§3.5）
  assets/             生成物（建议不入库，或只入库小图）
  port/
    main.js           主循环与模式分派（对应 User/src/main.c）
    menu.js           菜单框/行/主菜单（对应 Game/games_or_menu/menu.c）
    sokoban.js        推箱子（对应 .../sokoban.c）★ 优先
    d2048.js / tetris.js / klotski.js / fish.js
    settings.js       设置子页
    video.js          视频（P3 才做，P0 用静态帧占位）
```

---

## 3. 实现约定（重点）

### 3.1 lcd.js
- 内部 `Uint16Array(240*240)` 帧缓冲；每帧把 RGB565 转 RGBA（用 65536 项查表）后 `putImageData`
- `LCD_Fill` / `LCD_Fill_DMA` 直接填缓冲；`LCD_Address_Set` + `LCD_WR_DATA_16Bit` 维护窗口游标
- 图片类函数按 (x, y, w, h) 把源数组写进缓冲，支持"越界裁剪"（真机 `sok_draw_btn_clipped` 这类逻辑依赖它）
- 绘制用到的 `LCD_CS_CLR/SET`、`LCD_CLEAR_MENU_AREA()` 等宏按空实现或直接映射

### 3.2 touch.js
- 指针事件（mouse + touch）进采样队列；主循环按 **10ms** 节流产出 `TouchState`，与真机轮询同频
- `Touch_UpdateCoords()` 语义 = "长按期间读一次最新坐标"，模拟器里就是取队列最新点
- **必须提供脚本化手势**：`sim.play([{t:0,y:200},{t:16,y:180},...])`，用于回归"今天修的那几条"（见 P1 验收）
- 去抖/滤波参数从 `touch.c` 抄（注释标行号），先照抄、后按需简化

### 3.3 主循环与时间模型
- `requestAnimationFrame` 当 60fps 帧
- `Delay_Ms(500)` 这类阻塞（如通关后 0.5s）→ 用"阻塞帧计数"实现，否则通关画面会错位
- `HOLD_START_US(200ms)` / `HOLD_REPEAT_US(130ms)` / `HOLD_COOLDOWN_FRAMES(100)` → 按帧数换算
- 冷却类变量（`s_menu_cooldown` / `g_menu_cooldown`，单位是"帧"）**原样照抄**，别改成毫秒

### 3.4 常量同步（防双份维护漂移）
- `gen_consts.py` 从这些头文件提取 `#define`：`Game/inc/menu.h`（`MENU_SWIPE_START_Y=40`、`MENU_SWIPE_TRIGGER_DIST=5`、`MENU_SWIPE_COMMIT_DIST=20`、`MENU_DRAG_MAX_STEP=4`、`MENU_H=230`、`RESTART_MENU_*`、`VIDEO_MENU_*`…）、`sokoban.h`（`SOK_BTN_*`、`SOK_SWIPE_Y=230`、`SOK_SLIDE_SPEED`、`SOK_UNDO_*`、`HOLD_*`）、`touch.h`、`config.h`
- 输出 `port/consts.js`；**每次改 C 常量后重跑**，不要手抄

### 3.5 资源生成（P0 就要做）
- `gen_assets.py` 解析三类源：
  1. `uint16_t arr[N]` → RGB565 数组（**小端按字节**还原）
  2. `palette[N]` + `uint8_t arr[M]` → 调色板索引型瓦片
  3. `chinese_fonts.c` 的 1bpp 24x24 字模 → 按位展开成图
- 输出 `assets/*.png` + `assets/index.json`（含尺寸、类型、来源文件）；约定文件名与 C 数组同名，便于对照
- 顺带把 `上一关图片.txt` / `玩家.txt` / `鱼图片.txt` 里的 `image_data[]` 块导出成 PNG，便于人工核对

### 3.6 调试面板（这东西的价值一半在这）
- 状态显示：`s_main_mode`、`s_menu_visible`、`s_lvl2_visible`、`s_undo_visible`、`s_undo_group`、`s_menu_sliding`、`s_menu_slide_anim`、`s_menu_slide_y`、`s_panel_kind`、`s_menu_cooldown`、`s_exit_menu_visible`、`s_exit_sliding`、`s_exit_dragging`、`s_exit_drag_y`、`s_exit_from_undo`、`g_menu_cooldown`、`g_touch_down`
- **手势日志**：每帧的 `pressed / x / y / dy / s_swipe_origin_y`，可导出/回放（复现"有时候没反应"这类问题时非常关键）
- 手动触发按钮：低电、待机/唤醒、通关、切换语言

---

## 4. 分期计划（按改动量/风险，不排时间）

| 期 | 交付物 | 验收标准 | 改动量 | 风险 |
|---|---|---|---|---|
| **P0** | `index.html` + `lcd.js` + `touch.js` + `device.js` + `gen_consts.py` + `gen_assets.py`；主菜单 + 视频静态占位 | 浏览器里能看到圆屏界面；主菜单上下划能出/收；能切进推箱子 | 中 | 低 |
| **P1** | `port/sokoban.js` 全量（地图/瓦片/按钮/二级撤销菜单/撤销按钮组/下滑退出菜单/手势打断与冷却） | 用脚本化手势**复现并验证**三条：① 面板滑动/动画中下划能打断并开退出菜单 ② 退出菜单滑入中，中间区域上划不再误关（仅静止态生效）③ 退出菜单关闭后"从哪来回哪去"（3 个按钮 / 撤销菜单 / 撤销的 3 个按钮） | 中 | 低 |
| **P2** | `port/d2048.js` / `tetris.js` / `klotski.js` / `fish.js` | 4 个游戏的菜单与基本操作可用 | 中大 | 低 |
| **P3** | 视频真帧：补齐 57 块或改用源视频，复刻分块解码 | 视频模式能连续播放（或明确降级为静态） | 大 | **高** |
| **P4** | 设置页、低电/电池图标、待机与唤醒、存档 | 与真机观感一致（细节级） | 小~中 | 低 |

**建议开工顺序**：P0 -> P1（先把你在调的部分做成"可回归测试"），之后再决定 P2/P3 的优先级。

---

## 5. 风险与对策

| 风险 | 说明 | 对策 |
|---|---|---|
| 双份维护漂移 | C 改了 JS 没跟上，模拟器反而误导判断 | 常量用 §3.4 脚本生成；移植函数注释标 C 行号；每次改完 C 在提交信息里提醒"需同步 sim" |
| 时序/触摸不等价 | 真机有 10ms 轮询、坐标滤波、DMA 分块刷屏；模拟器会简化 | 冷却/阈值等**语义量原样照抄**；只简化 I/O 层；验收时把"模拟器结论"标注为待上机确认 |
| 图片格式坑 | 调色板瓦片、RGB565 小端、1bpp 字模各有坑 | 统一在 `gen_assets.py` 里处理并输出 PNG；先用 `fish_imgs`/`sok_img_prev` 这类简单图校准 |
| 视频资源不全 | 仓库只有 1/57 块 | P3 前不碰；要做就先确认能否拿到源视频/帧序列 |
| 别影响固件构建 | 12 B Flash 余量已到极限 | 模拟器全部放 `tools/simulator/`，生成物不入 `obj/`，不上库大文件 |
| 性能 | 若逐像素模拟 DMA 分块会卡 | 用 `ImageData` 批量写 + 整帧一次 `putImageData`；不做逐点 JS 调用 |

---

## 6. 开工前待确认（4 项）

1. **视频**：P3 给源视频/帧序列，还是先用静态帧占位？
2. **外部 Flash 那几张按钮图/字模**：用仓库导出 txt 重新生成，还是直接给 PNG？
3. **触摸 IC**：真机用的是 `IT7259` 还是 `CST816S`？（决定 `touch.c` 的滤波/去抖要不要照抄）
4. **落地位置**：默认 `tools/simulator/`（本文假设）；生成的大图是否入库？

---

## 7. 关键文件索引（实现时按这个定位）

| 文件 | 作用 | 移植难度 |
|---|---|---|
| `SPI/SPI_LCD/User/src/main.c` | 模式分派、主循环、待机/低电、设置进入退出 | 中（依赖外设桩） |
| `SPI/SPI_LCD/Game/games_or_menu/menu.c` | 菜单框/行、主菜单、退出菜单滑入滑出、共用上划/下滑判定 | 中 |
| `SPI/SPI_LCD/Game/games_or_menu/sokoban.c` | 推箱子全部逻辑（含三级菜单与手势） | 中（约 2100 行） |
| `SPI/SPI_LCD/Game/games_or_menu/d2048.c` / `tetris.c` / `klotski.c` / `fish.c` | 其它 4 个游戏 | 中 |
| `SPI/SPI_LCD/LCD/inc/lcd.h` | 需要打桩的全部显示接口 | 低 |
| `SPI/SPI_LCD/User/inc/touch.h` + `User/src/touch.c` | 触摸状态与去抖/滤波 | 低 |
| `SPI/SPI_LCD/Game/font_library/*.c` | 图片/字模数组（资源来源） | 低（脚本处理） |
| `SPI/SPI_LCD/User/src/video_chunk.c` | 视频单块数据 + `g_chunk_meta` | 高（P3） |
| `SPI/SPI_LCD/User/inc/flash_store.h` | 外部 Flash 保留区地址布局（BTN_IMG_* / MENU_GLYPH_*） | 低 |
| `编译与下载.ps1` | 真机构建/烧录（模拟器不参与） | -- |

---

## 8. 备注：本仓库的编码/case 注意

- `sokoban.c` / `chinese_fonts.c` / `sok_img_prev.c` 是 **GBK**；`menu.c` / `chinese_fonts.h` / `flash_store.h` / `config.h` / 本备忘录 是 **UTF-8**。**改文件前先确认编码**（详见备忘录 §42.3、§43.5）
- 本仓库发生过"内存崩溃把源码中文写成 `?`"的事故；模拟器文档/代码建议统一用 **UTF-8 + CRLF**，与备忘录一致
- 生成物（`assets/*.png`）如入库会显著增大仓库，建议只入库脚本 + 小图
