# 工程边界与迁移顺序

桌面播放器是行为基准。浏览器有两个不同用途的入口：`wasm/` 是从同一套 C++ 核心构建的 TR 播放器，`web/` 是独立的本地音乐与版式实验。实验里的标签读取器、播放状态和组件不作为平台公共接口。两个入口均不分发游戏音频。

| 边界 | 负责什么 | 不应依赖什么 |
| --- | --- | --- |
| `src/core` | 解析 PAK 索引与范围，曲目顺序，内存中的 Vorbis/WAVE → 44.1 kHz 双声道 PCM，原版 FFT / 波形 / 电平 | HWND、音频设备、文件选择、浏览器 DOM |
| Windows 接入 | `pak.cpp/resolver.cpp` 管文件句柄、目录与覆盖优先级；`audio.cpp/local_music.cpp` 管播放设备和系统解码器 | 浏览器模块 |
| 本地曲库 | `catalog.cpp` 管格式、校验与恢复策略；`catalog_windows.cpp` 管原子换文件、Shell 元数据、CP932 和目录对话框 | PAK、渲染器 |
| Windows 画面 | 在 640×480 逻辑坐标绘制。SYS 面板独立编译，只接收绘图上下文和只读设置；调色板源在 `assets/palettes.json`；生成头文件和图集，标题几何只保留一份 | 新增文件格式或标签解码逻辑 |
| WASM 接入 | worker 只读用户选择的文件；先读索引，再按需切单曲，调用 C++ 解码。主线程管 Web Audio、交互和 Canvas，调用同一频谱核 | 扫描安装目录、Win32 路径、上传服务 |

**为什么先拆这些。** 原先 `pak.h` 和内存解码器都经由 `common.h` 带入 Windows；FFT 则直接读窗口的全局播放状态。这使简单的“在另一平台播放相同样本”也得带上半个应用。现在核心可以单独链接测试；桌面依然使用 x86 MSVC 静态链接，原曲库文件没有迁移步骤。CMake 只构建公共核心和测试，不代表已经有 Linux 桌面 GUI。

**仍要还的债。** Windows 的 LIB 搜索、选择、播放顺序和窗口输入仍靠 `ui.cpp` 协调；下一次加曲库功能前，应抽出目录条目 ID、当前播放 ID 和选择状态，用“过滤后移除当前曲目”“播放结束时列表重排”作回归。持久化目前仍以 Windows UTF-16 路径为契约；浏览器文件选择不是长期可用的路径，不能把选中文件名写回旧格式当作恢复能力。设备输出和目录授权也不适合塞进同一个跨平台 AudioEngine。

| 平台 | 优先路线 | 晋级到正式交付前的验证 |
| --- | --- | --- |
| Web | 先做 TR 原版外观和 PAK → WASM → PCM → Web Audio；局部使用 DOM 控件便于触摸和键盘。不要求 SharedArrayBuffer 或线程版 WASM | 多浏览器实测、长曲解码内存峰值、切曲延迟、LIB/SYS 功能取舍；现在按整曲解码，后续若内存超预算再做流式 |
| Linux | 首先在桌面浏览器跑同一 WASM；用独立 CMake 核心做 Linux 编译门禁。确有原生需求时再接文件选择和音频设备 | Linux 浏览器声卡、HiDPI、键盘；原生后端需独立验证，不能把核心编译成功说成 Linux 播放器已完成 |
| Android | 先用同一移动网页和系统文件选择器；窄屏下用列表选曲，避免点缩小后的 TR 字格 | 真机触摸、文件提供器、切后台与恢复、长曲内存。如锁屏长播是硬需求，再评估 Media3 服务及原生包 |

浏览器可从用户选择的 File 读取数据，见 [File API](https://developer.mozilla.org/en-US/docs/Web/API/File_API/Using_files_from_web_applications)。Android 的原生后台播放与页面生命周期不同，见 [MediaSessionService](https://developer.android.com/media/media3/session/background-playback)。这两项直接影响了文件导入和后台播放的工程选择。

**测试入口。** Windows：`build.bat` 后用 `dist\hsbgm.exe --selftest --gamedir <游戏目录>` 核查真实容器及解码。公共核心：`cmake -S . -B obj/core`、`cmake --build obj/core`、`ctest --test-dir obj/core -C Debug --output-on-failure`。浏览器：安装 Playwright 后运行 `python tests/wasm_browser.py`，可选 `--pak-dir <backboneteller目录>` 与 `--browser <Chromium路径>`；默认测试只生成临时 WAV，不需要游戏。
