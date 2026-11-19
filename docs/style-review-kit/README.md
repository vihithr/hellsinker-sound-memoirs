# SOUND MEMOIRS 样式设计评审包

本包聚合播放器**画面样式设计**相关的全部代码、配色、美术生成脚本与设计文档，供外部 AI / 设计师独立评审提意见。拷贝本目录即可脱离原仓库阅读。不含平台渲染后端（GDI/WASM 传输/Canvas 回放）——评审对象是"画什么、什么颜色、什么形状、什么节奏"，不是"怎么画到屏幕上"。

## 1. 背景与审美基准（评审前必读）

- 复刻对象：HellSinker.（弹幕射击游戏）内置音乐播放器 "SOUND MEMOIRS"。逻辑分辨率 640×480，像素风：硬边图元、8×11 字格、最近邻缩放、无抗锯齿、无渐变（径向眩光是唯一例外，见 §5）。
- **Original 布局与 ORIGINAL 配色是 1:1 复刻基准，不可改**；其余 6 布局 / 4 主题是自由创作区。
- 用户（甲方）多轮评审收敛出的审美原则，评审时请当作硬约束：
  1. **信息量 = 信息类型丰富**，不是像素密度。要素不重叠、不无意义堆叠（"填充"曾被否决）。
  2. **运动匀速，音乐塑造形状**：放大/旋转/滚动速度不得被能量调制；音频只决定几何（宽度/摆幅/形状）。
  3. **动态要有跃动感**：安静段真的安静、重拍有大形变（整圈撑开/底线闪光），不能常年拉满。
  4. **硬边两色分段**是原版语言：柱体 = 当前色 bar + 衰减尾 barLit，2px 段 + 1px 缝。高度渐变染色、白色峰值帽（Hi-Fi LCD 味）均被否决。
  5. NEON 主题要高饱和撞色（"吵就是冲击力"）；逆向手册只是 Original 的参数记录，不是通用美学规范。
  6. 航灯类光点：圆形径向眩光、亮度+边缘模糊同步呼吸、谷值留底光、任何相位不得露出方块本体、颜色契合主题（向 ink 提亮不推纯白）。

## 2. 目录

```
style-review-kit/
├── README.md                    ← 本文件
├── visual-style-code-map.md     ← 样式清单 + 原仓库代码定位（行号）
├── design/
│   ├── visual-motion-roadmap.md ← 动效设计交接文档（含 HELIX/机械DNA 原始规划、
│   │                              起落时间参数、保留/放弃决策记录）
│   ├── make_themed_art.py       ← 字格图集按主题重染色的生成脚本
│   └── make_themed_title.py     ← 标题位图三色桶最近色重映射脚本
├── assets/palettes.json         ← 5 主题全部色值（设计的唯一颜色源）
├── src/
│   ├── scene.cpp                ← ★ 主体：7 布局 + 原版组件 + 侧翼的全部样式代码
│   ├── stage.cpp / stage.h      ← F7 三种中央演出 + 动态包络（节奏设计）
│   ├── layout.h / layout.cpp    ← 布局枚举、三语名、原版几何常量
│   ├── themes.generated.h       ← palettes.json 生成的 C++ 色表
│   ├── themed_art_runs.h        ← 原版标题/轴线游程矢量美术数据
│   ├── scene.h / scene_state.h  ← 绘制器输入（评审时当参考，样式不在此）
│   └── core/spectrum.h/.cpp     ← 43 段频谱/波形/包络的定义（样式的输入信号）
└── web/
    ├── screen.css               ← 网页外壳样式（主题 CSS 变量、晕影、侧栏）
    ├── index.html               ← 外壳结构（侧栏按钮/帮助层）
    └── chrome-theme-excerpt.js  ← app.js 摘录：5 主题外壳配色映射 + applyChrome
```

## 3. 样式代码导读（scene.cpp 内函数）

| 样式 | 函数 | 设计要点 |
|---|---|---|
| Original（基准） | sceneRender 主体 + PaintLevelMeter/PaintSideWave/PaintFrame/PaintSpectrum/PaintTitle/PaintGrid… | 原版坐标/颜色/相位逐字保留 |
| Field 全幅场 | PaintFieldLayout | 43 分段柱+镜像尾、点阵底纹、角框、低频能量条、重拍底线闪光、上波形随冲击撑大 |
| Stack 三栏 | PaintStackLayout | 电平表（休眠刻度点）、中央频谱窗、46 曲列表三态、4px 进度轨+暂停闪烁播放头 |
| Ring 环形 | PaintRingLayout | 43 径向楔（3×3 块 4px 节距）、进度弧、XY 相位图（自动增益+余晖）、外圈活波形环、四角信息块 |
| Cross 四象限 | PaintCrossLayout | L/R 示波、43 频段横梯、传输读数+迷你 XY 窗、冲击轴闪、每象限一个职责 |
| Sink 沉井 | PaintHelixLayout | 双股=声道波形绕相位投影（横偏移=波形×强度摆幅）、垂直匀速 80px/s、深度计 -0000M..-9999M、右侧电平+低频梯 |
| Dive 隧道 | PaintDiveLayout | 真透视 13/(1-u)；每层=正八边形航灯框+＃字（无限直线被八边形线段求交裁剪，边上均匀缠 DNA 双螺旋 2.5 周期）；层间 8 连线载 L/R 波形（奇偶）；航灯径向眩光呼吸（sin³、1.8s、层间错相）；远透明近实 (1-u)^1.6 |
| 侧翼 F8 | PaintWings | 原版电平 / 折叠波形（奇偶双色行）/ Ribbon（边缘随包络加热）/ Matrix（点阵随包络加热） |
| F7 演出 | stage.cpp: eclipse/strata/phase + calibration | 星蚀（低频撑环）/ 地层（历史频谱纵深）/ 相位（自动增益 XY）+ 仪器纸底纹 |

## 4. 配色体系（palettes.json）

每主题 22 个语义色位：outer/panel（底）、bar/barLit（频谱两色）、frame/wave（线框/波形）、strip/stripHot（电平/热点）、ink/digit/hint（文字三级）、title/axisLabel、sel*（选区）、seam。设计规则：
- 柱体永远 bar+barLit 两色硬切；热点 stripHot 只给峰值/拍点/高亮。
- 横档/键类细线用 stripHot→ink 混 30% 保证亮暗主题都有对比。
- 深度/远近用"向 panel 混色"模拟透明（像素风不用真 alpha，眩光除外）。
- 网页外壳（侧栏/帮助层）经 CSS 变量跟随主题（web/chrome-theme-excerpt.js 的 CHROME 表）。

## 5. 节奏与动态参数（stage.cpp / scene.cpp）

- 冲击包络：起音 22ms / 回落 240ms；持续低频 45ms/750ms；基底 180ms/1.1s。
- 电平重映射 bandLevel：去 12% 噪底 + gamma1.5（仅自定义布局）。
- XY 自动增益：120ms 攻 / 1.6s 放，上限 4×。
- Sink：8Hz 采样、10px 节距、匀速 80px/s；绕相位 = 波形均值×2.4 + 0.35 怠速。
- Dive：出生 0.42/s、旋转 0.55rad/s + 层间 0.5 相位、航灯 sin³ 脉冲 1.8s、底光 0.22。
- 侧翼快包络：30ms/300ms；折叠波强拍占翼宽 70–85%。

## 6. 想请评审方回答的问题

1. 各布局的构图/信息分工是否成立？哪个最弱、怎么改（在"不堆叠"约束下）？
2. Dive 的＃字+八边形+DNA 边+波形壁组合是否自洽？层间节奏（0.42/s、6 层）观感建议？
3. 5 主题配色有无对比度/和谐度问题？NEON 的撞色策略是否到位？
4. "匀速运动+音乐塑形状"原则下，还有哪些表达张力的手段没用上？
5. 像素风约束（硬边、两色分段、无渐变）与眩光例外的边界是否合理？
6. 整体"格调"：与原版气质（冷峻仪器感）相比，自定义布局是否跑偏？

## 7. 已知未决项（诚实清单）

- Ring 墨水占比仍偏低（中等音量 ~4%），结构性克制是否过度待评。
- 亮色主题（ORIGINAL/PAPER）下深度混色淡出的层次感弱于暗色主题。
- 消息提示行仅 ASCII（字格图集无 CJK）；LIB 名走动态像素字体。
- Sink/Dive 静音时仍有怠速运动（绕相位 +0.35 / 静态层），是否该完全静止待评。