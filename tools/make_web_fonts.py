"""Build small WOFF2 CJK subsets for the browser replica.

The desktop embeds the full Fusion Pixel 12px zh-Hans TTF (7.1 MB). The web
build only needs: (a) every CJK glyph used by fixed UI strings in src/, plus
(b) a common-frequency set so user ID3 metadata mostly renders. EN uses the
existing 8x11 ASCII cell atlas and needs no CJK font.

Outputs wasm/assets/fonts/fusion-zh.woff2 and fusion-ja.woff2.
Requires fonttools + brotli (bundled Python already provides both).
"""
import re
from pathlib import Path
from fontTools.ttLib import TTFont
from fontTools.subset import Subsetter, Options

root = Path(__file__).resolve().parents[1]
fonts = root / "assets/fonts"
out = root / "wasm/assets/fonts"
out.mkdir(parents=True, exist_ok=True)
src_ttf = fonts / "fusion-pixel-12px-monospaced-zh_hans.ttf"

# (a) every CJK/kana glyph appearing in the desktop source strings
text = ""
for pattern in ("*.cpp", "*.h"):
    for p in (root / "src").glob(pattern):
        text += p.read_text(encoding="utf8", errors="ignore")
ui_chars = set(c for c in text if 0x3000 <= ord(c) <= 0x30FF or 0x4E00 <= ord(c) <= 0x9FFF)

# (b) common Han frequency list (~3500). Keep the subset small but useful for
# arbitrary track titles. Kana are already covered by ui_chars and this list.
common = set(
    "的一是了我不人在他有这个上们来到时大地为子中你说生国年着就那要讲两"
    "回特代内信表化老给世位次度门任常先海通教儿原东声提立及比员解水名真"
    "论处走义各入几口认条平系气题活尔更别打女变四神总何电数安少报才结反"
    "受目太量再感建务做接必场件计管期市直德资命山金指克许统区保至队形社"
    "便空决治展马科司五基书非则听白却界达光放强即像难且权思王象完设式色"
    "路记南品住告类求据程北边死张该交规万取拉格望觉术领共确传师观清今切"
    "院让识候带导争运笑飞风步改收根干造言联持组每济车亲极林服快"
    "办让台从回更看用找到想输出入开关播放停止音量主题语言字形风格配色布局"
    "曲库搜索曲目艺人专辑标题档案系统设置帮助默认游戏音乐本地文件载入添加"
    "顺序随机循环单曲列表暂停继续下一首上一首退出返回确认取消错误重试解码"
    "版选得牌左右上下页首末空无有是否原价正悠蓝红绿黄紫灰黑白 曲目列表"
)
common = set(c for c in common if c.strip())

base = ui_chars | common
# Japanese kana/kanji additions for JA UI strings not already in source
ja_extra = set("設定言語画面構成音量出力音量配色曲名書体音楽庫再生停止一時停止")
base |= ja_extra

def build(out_name, chars):
    font = TTFont(src_ttf)
    cmap = font.getBestCmap()
    codepoints = set(ord(c) for c in chars if ord(c) in cmap)
    # Always keep ASCII digits/letters/punctuation for values and labels.
    codepoints |= set(range(0x20, 0x7F))
    options = Options()
    options.layout_features = ["*"]
    options.name_IDs = ["*"]
    options.name_legacy = True
    options.notdef_outline = True
    options.recalc_bounds = True
    options.drop_tables += ["DSIG"]
    subsetter = Subsetter(options=options)
    subsetter.populate(unicodes=codepoints)
    subsetter.subset(font)
    font.flavor = "woff2"
    dest = out / out_name
    font.save(dest)
    print(f"{dest.name}: {len(codepoints)} glyphs, {dest.stat().st_size/1024:.1f} KB")

build("fusion-zh.woff2", base)
build("fusion-ja.woff2", base | ja_extra)
