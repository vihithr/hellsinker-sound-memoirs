# SOUND MEMOIRS / TR · WASM

在此目录运行 `python serve.py 8080`，浏览器打开 `http://127.0.0.1:8080/`。不要直接双击 HTML：ES 模块和 Worker 需要 HTTP。静态目录可独立使用，不需要本地游戏服务或上传音乐。

选择游戏安装目录 `backboneteller` 下的 bbt.pak、bbt2.pak、bbt3.pak（若有 bbll.pak 也可加入）。选曲可点击原版 TR 网格，也可用下方列表和播放按钮；网格获得焦点时支持方向键和回车。可暂停、停止、拖动进度，或单独打开 WAV / OGG。关闭标签页即释放文件授权和本次列表。

主线复用桌面端的 PAK、解码和频谱 C++ 实现。完整曲库由用户选择的数据决定；同名资源按 loose WAV → bbll → bbt3 → bbt2 → bbt → loose OGG 解析。界面沿用原版 640×480 布局，不包括 Windows 的 SYS、本地曲库持久化、Media Foundation 格式或完整启动动画。单曲输入上限 96 MiB，目前在 Worker 内整曲解码。

图像和曲序由 `python tools/make_wasm_assets.py` 从 Windows 同源素材生成。第三方声明见播放器根目录 THIRD_PARTY_NOTICES.md。另一套 Web 主题实验在 web/，不属于本版本。
