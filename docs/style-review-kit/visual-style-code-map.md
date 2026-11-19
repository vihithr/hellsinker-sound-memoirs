# SOUND MEMOIRS 播放器视觉样式与代码定位

本文档列出播放器当前全部画面样式（布局 / 演出 / 侧翼 / 主题）及其对应的实现代码位置，供外部审阅与评估。所有路径相对于 `player/` 目录。行号为撰写时的快照，可能随后续改动漂移，以函数名为准。

## 0. 架构背景（审阅前必读）

- **单一共享绘制器**：桌面端（Win32/GDI）与网页端（WASM/Canvas）共用同一份绘制代码 `src/scene.cpp`。它只通过平台无关原语 `src/gfx.h`（`gfx_rect / gfx_line / gfx_polyline / gfx_blit / gfx_text / gfx_alpha_rect / gfx_push_clip` 等）输出指令。
  - 桌面后端：`src/gfx_win32.cpp`（GDI 直接绘制）。
  - WASM 后端：`wasm/core/gfx_wasm.cpp`（把指令编码进缓冲），由 `wasm/scene.js` 在 Canvas 2D 上回放。
- **状态契约**：`src/scene_state.h` 定义 `SceneState` POD，桌面每帧打包、网页 JS 按固定字节偏移写入。`static_assert` 锁定偏移，新字段只能追加在尾部。
- **几何常量**：`src/layout.h` / `src/layout.cpp` 定义布局枚举与原版面板几何（原版坐标逐字保留，是 1:1 复刻基准）。
- **主题**：`assets/palettes.json`（5 套配色）→ 生成 `src/themes.generated.h`（`Theme` 结构 + `kThemes[]`）。绘制器经 `Th()` 取当前主题色。
- **动态状态**：`src/stage.h` 的 `stage::Memory` 保存跨帧包络/历史（冲击、持续低频、XY 自动增益、声道历史、波形绕相位等），由 `src/stage.cpp` 的 `Memory::advance` 每帧推进。绘制器只读。
- **入口分发**：`sceneRender`（`src/scene.cpp:1726`）按 `page` 与 `layoutMode` 分发到各布局绘制器；`stageMode` 决定原版面板中央的演出。

## 1. 布局（F9 切换，`layout::Mode`，`src/layout.h:14`）

| # | 枚举 | 名称 | 绘制函数 | 位置 |
|---|------|------|----------|------|
| 0 | `Original` | 原版面板（1:1 复刻基准，不可改） | `sceneRender` 主体 + 下列原版组件 | `src/scene.cpp:1726` |
| 1 | `Field` | 全幅场 | `PaintFieldLayout` | `src/scene.cpp:538` |
| 2 | `Stack` | 三栏仪表架 | `PaintStackLayout` | `src/scene.cpp:678` |
| 3 | `Ring` | 环形表盘 | `PaintRingLayout` | `src/scene.cpp:798` |
| 4 | `Cross` | 四象限仪表 | `PaintCrossLayout` | `src/scene.cpp:965` |
| 5 | `Helix` | SINK 沉井（双声道波形螺旋竖井） | `PaintHelixLayout` | `src/scene.cpp:1114` |
| 6 | `Dive` | DIVE 坠入（第一视角 DNA 隧道） | `PaintDiveLayout` | `src/scene.cpp:1284` |

三语显示名在 `src/layout.cpp` 的 `kNames`。网页端布局名单与循环在 `wasm/app.js` 的 `LAYOUTS` / `cycleLayout`。

### 原版组件（布局 0 内部，均为 1:1 复刻，勿动）
- 电平表 `PaintLevelMeter` `src/scene.cpp:230`
- 侧波形 `PaintSideWave` `src/scene.cpp:245`
- 面板框 `PaintFrame` `src/scene.cpp:264`
- 频谱窗 `PaintSpectrum` `src/scene.cpp:282`
- 标题位图 `PaintTitle` `src/scene.cpp:342`
- 选区/网格/信息/提示 `PaintSelection` `:354`、`PaintGrid` `:395`、`PaintInfo` `:408`、`PaintHints` `:458`
- 频道栏 `channelRail` `src/scene.cpp:1513`

## 2. 中央演出（F7，`stageMode`，仅原版面板中央）

实现于 `src/stage.cpp`，经 `stage::paint`（`:232`）分发，共用仪器底纹 `calibration`（`:199`）。

| mode | 名称 | 函数 | 位置 |
|------|------|------|------|
| 0 | 原版频谱 | `PaintSpectrum` | `src/scene.cpp:282` |
| 1 | Occultation 星蚀 | `eclipse` | `src/stage.cpp:133` |
| 2 | Strata 地层 | `strata` | `src/stage.cpp:158` |
| 3 | Phase 相位 | `phase` | `src/stage.cpp:173` |

## 3. 侧翼（F8，`wingMode`，原版面板左右）

`PaintWings`（`src/scene.cpp:465`）按 style 分支：
- style 1：原版电平+侧波形（`PaintLevelMeter`+`PaintSideWave`）
- style 2：折叠波形（奇偶双色行 + 边缘刻度）
- style 3：Ribbon 双线带谱（边缘随声道包络加热变色）
- style 4：Matrix 点阵（点亮单元随包络加热变色）

## 4. 主题（F6 / 侧栏，5 套）

- 配色源：`assets/palettes.json`（ORIGINAL / DARK / NEON / PAPER / BLUEPRINT）
- 生成头：`src/themes.generated.h`
- 主题标题位图：`assets/themed/title_*.bmp`，生成脚本 `tools/make_themed_title.py`
- 字格图集：`assets/themed/cells_*.bmp`，生成脚本 `tools/make_themed_art.py`
- 网页外壳跟随主题的 CSS 变量：`wasm/app.js` 的 `CHROME` / `applyChrome`，样式 `wasm/screen.css`

## 5. 动态系统（驱动"张力"与响应）

`src/stage.h` `Memory`（`:21`）+ `src/stage.cpp` `Memory::advance`（`:32`）：
- `impact` / `lowSustain` / `lowFloor`：低频冲击包络（快起慢落），驱动各布局的重拍大形变与闪帧。
- `wing[2]`：声道快包络，驱动侧翼行程与变色。
- `xyGain`：XY 相位图平滑自动增益（快攻慢放），避免轻声挤成一坨。
- `meterPeak[2]`：慢落峰值，用于 SINK 齿轨刻痕。
- `histL/histR/histBeat`（`kHist=64`）：8Hz 声道历史 + 拍点标记，SINK 晶格与 DIVE 层形状的数据源。
- `histWaveL/histWaveR` + `windL/windR`：带符号波形采样 + 连续绕相位，SINK 股线横偏移 = 波形本身。
- `bandLevel`（`src/stage.h` 内联）：自定义布局的电平重映射（去噪底 + gamma），仅非原版使用。

## 6. 两个新布局的算法要点（评估重点）

### SINK（`PaintHelixLayout`，`src/scene.cpp:1114`）
- 竖井内两条股线，横偏移 = 该 rung 的带符号波形采样 × 强度摆幅；L/R 反相形成交叉晶格。
- 垂直方向严格匀速滚动（8 rung/s × 10px），无拍点调制；波形只塑造形状。
- 左侧深度计把播放位置显示为下潜深度；右侧 live 电平 + 低频梯。

### DIVE（`PaintDiveLayout`，`src/scene.cpp:1284`）
- 真透视：层尺寸 = `13 / max(0.008, 1-u)`，u→1 时发散，层在回收前已完全掠出屏幕（连续下坠，不"撞屏消失"）。
- 每层 = 正八边形定位框（8 个硬边闪烁航灯）+ 内部＃字。＃的四条边是**无限长直线**，用线段求交裁剪到八边形边界（`clipLine`），DNA 双螺旋在整段可见长度均匀缠绕，端点硬截在边上。
- 层间隧道壁：上层 8 顶点连下层对应顶点，奇数顶点载 L 波形、偶数载 R，垂直于连线位移 → 活示波器壁。
- 深度亮度单调：远层 `(1-u)^1.6` 淡出（cap 0.96），近层实。
- 横档色 = `stripHot` 向 `ink` 混 30%，保证全主题对比度。
- 旋转 0.55 rad/s + 每层 0.5 相位偏移；放大/旋转匀速，音乐只塑造形状不调制运动。

## 7. 网页外壳与交互

- 页面结构：`wasm/index.html`；样式 `wasm/screen.css`（黑底居中 640×480、侧栏浮动、主题变量、晕影）。
- 逻辑：`wasm/app.js`（播放/暂停/上下曲/双击播放/进度拖动/LIB 顺序模式/布局命中 `hitLayout`/进度轨 `LAYOUT_SEEK`/测试钩子 `window.__hsTest`）。
- 解码 worker：`wasm/worker.js`（OGG/WAV 走 WASM，MP3/M4A/FLAC 等走主线程 `decodeAudioData`）。
- 指令回放：`wasm/scene.js`。

## 8. 验证与基准

- 原版像素基准：`tools/verify_scene_pixels.py`（回放指令与桌面 `--shot` BMP 对照，支持 `HS_DUMP/HS_THEME/HS_OUT` 环境变量审查非基准帧）。当前 mean_abs_diff≈0.0006、2 像素差。
- 场景帧冒烟：`wasm/core/verify_scene.mjs`（26 帧，含全部布局/演出/主题/页）。
- 桌面构建：`build.bat` → `dist/hsbgm.exe`；WASM 构建：`wasm/core/build.ps1`。
- 部署同步：`tools/sync_site.ps1`（仓库根 + `dist-static/` 双目标）。