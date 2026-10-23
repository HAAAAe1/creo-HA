# -*- coding: utf-8 -*-
"""生成 安装步骤.png —— 一页 A4 的三步安装图，给不看文字的人。"""
import os

from PIL import Image, ImageDraw, ImageFont

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
if not os.path.exists(os.path.join(ROOT, "H.dll")):        # 脚本在 开发资料\tools 下
    ROOT = os.path.dirname(ROOT)
OUT = os.path.join(ROOT, "安装步骤.png")

W, H, M = 1654, 2339, 110          # A4 200dpi
FONT_R = "C:/Windows/Fonts/msyh.ttc"
FONT_B = "C:/Windows/Fonts/msyhbd.ttc"

INK = (27, 34, 43)
INK2 = (80, 92, 106)
ACCENT = (31, 111, 178)
CARD = (237, 243, 250)
RULE = (214, 224, 234)
GRAY = (150, 160, 172)
WARN = (198, 45, 20)               # 强调用的红

D = "D:\\H"

# (标题, 正文, 强调行)
STEPS = [
    ("把整个 H 文件夹放到 " + D,
     "打开能看到 H.dll、text、templates 就对了。\n"
     "路径里不要有中文和空格。\n"
     "换盘了（比如 E:\\H）？双击一次 setup.bat。",
     None),
    ("Creo → 文件 → 选项 → 配置编辑器",
     "在最后加一行，然后保存：\n"
     "        protkdat " + D + "\\H.dat",
     "换盘了，就换成你文件的实际位置！"),
    ("关掉 Creo，重新打开",
     "随便开一张工程图\n"
     "→ 功能区最右边多出【HA】页签，装好了。",
     None),
]

TIPS = [
    ("提示“未找到球标符号文件”", "把 H 文件夹里生成的 h_load.log 发给作者，里面写了原因"),
    ("提示插件未解锁", "找作者要一份已解锁的 H.dll 覆盖进来"),
    ("加完还是没有 HA 页签", "① 确认 Creo 全退了（任务管理器里没有 xtop.exe）   ② 改的是 Creo 真正读的那份 config.pro"),
]


def f(path, size):
    return ImageFont.truetype(path, size)


def wrap(d, text, font, maxw):
    out, line = [], ""
    for ch in text:
        if line and d.textlength(line + ch, font=font) > maxw:
            out.append(line)
            line = ch
        else:
            line += ch
    if line:
        out.append(line)
    return out


def main():
    img = Image.new("RGB", (W, H), (255, 255, 255))
    d = ImageDraw.Draw(img)

    d.rectangle([M, 96, M + 10, 178], fill=ACCENT)
    d.text((M + 34, 92), "H 插件 · 安装 3 步", font=f(FONT_B, 62), fill=INK)
    sub = "照着做，2 分钟"
    fs = f(FONT_R, 34)
    d.text((W - M - d.textlength(sub, font=fs), 124), sub, font=fs, fill=GRAY)
    d.line([M, 210, W - M, 210], fill=RULE, width=3)

    cx = M + 52
    f_num = f(FONT_B, 46)
    f_title = f(FONT_B, 46)
    f_desc = f(FONT_R, 34)
    f_emph = f(FONT_B, 42)                  # 强调行：更大更粗
    inner_w = W - M - (cx + 90) - 40

    plans, ys, y = [], [], 300
    for t1, body, emph in STEPS:
        tl = wrap(d, t1, f_title, inner_w)
        bl = []
        for ln in body.split("\n"):
            bl += wrap(d, ln, f_desc, inner_w)
        el = wrap(d, emph, f_emph, inner_w) if emph else []
        h = 62 + 62 * len(tl) + 12 + 54 * len(bl) + 40
        if el:
            h += 20 + 62 * len(el)
        plans.append((tl, bl, el, h))
        ys.append(y)
        y += h + 96

    d.line([cx, ys[0], cx, ys[-1] + plans[-1][3] - 40], fill=(222, 232, 242), width=8)

    for i, (tl, bl, el, h) in enumerate(plans):
        y = ys[i]
        d.ellipse([cx - 44, y - 44, cx + 44, y + 44], fill=ACCENT)
        num = str(i + 1)
        d.text((cx - d.textlength(num, font=f_num) / 2, y - 34), num, font=f_num, fill=(255, 255, 255))

        x = cx + 90
        d.rounded_rectangle([x - 24, y - 58, W - M, y - 58 + h], 14, fill=CARD)
        yy = y - 40
        for seg in tl:
            d.text((x, yy), seg, font=f_title, fill=(18, 55, 92))
            yy += 62
        yy += 12
        for seg in bl:
            d.text((x, yy), seg, font=f_desc, fill=INK2)
            yy += 54
        if el:
            yy += 20
            for seg in el:
                d.text((x, yy), seg, font=f_emph, fill=WARN)
                yy += 62

    by = ys[-1] + plans[-1][3] + 70
    d.line([M, by - 44, W - M, by - 44], fill=RULE, width=3)
    d.text((M, by), "装不上怎么办", font=f(FONT_B, 38), fill=(18, 55, 92))
    yy = by + 76
    f_k = f(FONT_B, 30)
    f_v = f(FONT_R, 30)
    for k, v in TIPS:
        d.text((M, yy), k, font=f_k, fill=INK)
        yy += 48
        for seg in wrap(d, v, f_v, W - 2 * M - 24):
            d.text((M + 30, yy), seg, font=f_v, fill=INK2)
            yy += 44
        yy += 26

    d.line([M, H - M - 56, W - M, H - M - 56], fill=RULE, width=2)
    foot = "详见「安装说明_超简单版.txt」 · 功能清单见「功能说明.txt」/「功能说明_A4.png」"
    d.text((M, H - M - 38), foot, font=f(FONT_R, 26), fill=GRAY)

    if os.path.exists(OUT):
        os.remove(OUT)
    img.save(OUT, "PNG", optimize=True, dpi=(200, 200))
    print("已生成 %s（%.2f MB）" % (OUT, os.path.getsize(OUT) / 1048576.0))


if __name__ == "__main__":
    main()
