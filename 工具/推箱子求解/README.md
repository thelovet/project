# 推箱子 50 关自动求解（离线，在电脑上跑）

目标：给固件生成 50 关的"自动通关"方向串（上下左右），再打包进固件回放。

## 现状

- 已解：50/50，见 `solutions.txt`（每行 `关卡 步数 方向串`，`0 -` 表示还没解出）
- 求解器：[mkornreich/sokoban](https://github.com/mkornreich/sokoban)（C++，移动最优 A\*，多线程）
- 求解器卡住的 12 关（24/27/28/33/34/36/38/42/44/45/48/49/50）已用 4399 攻略视频反解补齐，
  全部在固件地图上回放校验通过；`sokoban_maps.c` 里的 `sok_demo_off/sok_demo_bits` 已按 50 关重新生成
- 我们改了它 3 处才能用（脚本自动改，不用手工）：

| 改动 | 原因 |
|---|---|
| `aligned_alloc` → `_aligned_malloc` | Windows 下 MSVC/MinGW 编译 |
| 收尾条件：箱子全到位即通关 | 原版要求"人还要站在空目标点"，我们的玩法不需要 |
| 宽表上限 `1<<25` → `1<<29` | 大箱子关卡（8~40 个箱子）状态数极多，表小了直接 `WideHash full` |

## 在一台新电脑上继续跑

```powershell
# 1. 准备（只需一次）
git clone --depth 1 https://github.com/mkornreich/sokoban "$env:TEMP\sokrepo"

# 2. 生成 50 关数据 + 打补丁（会自动把我们的地图写进 solver.cpp）
python "工具\推箱子求解\patch_solver.py" "$env:TEMP\sokrepo\solver.cpp"

# 3. 编译（Linux/macOS 用 g++；Windows 可用 zig c++ 或 MinGW）
g++ -O3 -march=native -std=c++17 -pthread "$env:TEMP\sokrepo\solver.cpp" -o "$env:TEMP\sokrepo\solver.exe"

# 4. 只跑还没解的关卡（脚本会读 solutions.txt 跳过已解）
python "工具\推箱子求解\run_solve.py" "$env:TEMP\sokrepo\solver.exe"
```

`run_solve.py` 会把新解出的结果**追加写回 `solutions.txt`**（自动校验：在原始地图上回放，必须真的通关才收录）。

## 结果怎么进固件（教程/自动演示用）

```powershell
# 1. 把 solutions.txt 压成固件数据(会打印两段 C 数组)
python "工具\推箱子求解\gen_demo_data.py"

# 2. 贴到 64MB触摸屏程序\SPI\SPI_LCD\Game\font_library\sokoban_maps.c 末尾
#    (sok_demo_off[51] 每关起始步号 + sok_demo_bits[] 每步 2bit, 每关按字节对齐)

# 3. 校验: 从 .c 回读并和 solutions.txt 逐关比对(总步数/逐关方向串)
python "工具\推箱子求解\verify_demo_data.py"
```

**★ 字节数变了要同步改 `flash_store.h` 的 `SOK_DEMO_SIZE`**：`sok_demo_bits` 不进内部 Flash
（50 关 9743 步 ≈ 2.4KB 放不下），而是和英文按钮图一起烧到外部 Flash 的按钮保留区
（`SOK_DEMO_ADDR`），所以改完必须**重烧 btn 步**（`burn_step.ps1 -Step btn`）。

未解的关长度 0 -> 固件里该关教程页的播放按钮是灰色、点了没反应（现在 50/50 已解，全都有演示）。

## 注意

- 大关卡很吃内存：表上限 21 GB（`1<<29`）。内存不够会报 `out of memory` 直接退出，改小 `cap` 即可；表不够会报 `WideHash full`，改大。
- 求解器是**内存延迟敏感**型，核多/内存带宽大才有意义；大缓存（X3D）帮助不大。
