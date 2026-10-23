# -*- coding: utf-8 -*-
"""把 功能说明.txt 排成一张 A4 300dpi 的 PNG（可直接打印 / 发微信）。

用法：
    python tools/make_a4_sheet.py

产物：D:\\H\\功能说明_A4.png   （2480 x 3508，A4 竖版 300dpi）
"""
import os
import re
import sys

from PIL import Image, ImageDraw, ImageFont

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, "功能说明.txt")
OUT = os.path.join(ROOT, "功能说明_A4.png")

W, H, M = 2480, 3508, 130          # A4 300dpi + 页边距
COL_GAP = 100
BAR = "━"
FONT_R = "C:/Windows/Fonts/msyh.ttc"
FONT_B = "C:/Windows/Fonts/msyhbd.ttc"

INK = (27, 34, 43)
INK2 = (86, 98, 112)
ACCENT = (31, 111, 178)
HEADBG = (237, 243, 250)
RULE = (214, 224, 234)
GRAY = (150, 160, 172)
FOOT_TITLES = ("开源声明", "免责声明")


def is_bar(s):
    s = s.strip()
    return bool(s) and set(s) <= {BAR}


# ---------------------------------------------------------------- 解析
def parse(path):
    raw = open(path, "rb").read().decode("utf-8-sig").replace("\r\n", "\n")
    lines = raw.split("\n")
    title = lines[0].strip()
    sections, cur, i, n = [], None, 0, len(lines)
    while i < n:
        ln = lines[i]
        if is_bar(ln):
            j = i + 1
            while j < n and is_bar(lines[j]):
                j += 1
            if j < n and lines[j].strip():
                cur = {"title": lines[j].strip(), "items": []}
                sections.append(cur)
                i = j + 1
                while i < n and is_bar(lines[i]):
                    i += 1
                continue
            i += 1
            continue
        if not ln.strip() or cur is None:
            i += 1
            continue
        lead = len(ln) - len(ln.lstrip("\u3000"))
        txt = re.sub("\u3000+", "\u3000", ln.strip())
        parts = [p for p in txt.split("\u3000") if p.strip()]
        if lead >= 2 and cur["items"]:                 # 缩进续行
            prev = cur["items"][-1]
            prev[2 if prev[0] == "row" else 1] += "　" + "　".join(parts)
        elif len(parts) == 2 and len(parts[0]) <= 8:   # 名称 + 说明
            cur["items"].append(["row", parts[0], parts[1]])
        else:                                          # 平铺词条
            cur["items"].append(["flat", "　".join(parts), None])
        i += 1
    body = [s for s in sections if s["title"] not in FOOT_TITLES]
    foot = [s for s in sections if s["title"] in FOOT_TITLES]
    return title, body, foot


# ---------------------------------------------------------------- 工具
def font(path, size):
    return ImageFont.truetype(path, size)


NO_HEAD = "）】」』｝〉》”’，。、；：？！·….,;:?!%)]}"   # 不能出现在行首
NO_TAIL = "（【「『｛〈《“‘([{"                              # 不能留在行尾


def wrap(draw, text, f, maxw):
    """按字宽换行：ASCII 单词不切断，中文标点不做行首。"""
    units = re.findall(r"[A-Za-z0-9][A-Za-z0-9._:/%+\-]*|.", text)
    out, line = [], ""
    for u in units:
        if line and draw.textlength(line + u, font=f) > maxw:
            if u in NO_HEAD:                      # 标点挤进当前行
                out.append(line + u)
                line = ""
            else:
                seg, carry = line, ""
                while seg and seg[-1] in NO_TAIL:     # 开括号不留在行尾
                    carry = seg[-1] + carry
                    seg = seg[:-1]
                out.append(seg.rstrip())
                line = carry + (u.lstrip() if u.isspace() else u)
        else:
            line += u
    if line.strip():
        out.append(line)
    return out


def text_at(d, xy, s, f, fill):
    d.text(xy, s, font=f, fill=fill, anchor="lm")


# ---------------------------------------------------------------- 排版 + 绘制
def build(body_size):
    title, sections, foot = parse(SRC)
    img = Image.new("RGB", (W, H), (255, 255, 255))
    d = ImageDraw.Draw(img)
    colw = (W - 2 * M - COL_GAP) // 2
    cols = [M, M + colw + COL_GAP]

    f_sub = font(FONT_R, 32)
    f_b = font(FONT_B, body_size)
    f_r = font(FONT_R, int(body_size * 0.92))
    f_hd = font(FONT_B, int(body_size * 0.95))
    lh = int(body_size * 1.52)
    head_h = int(body_size * 1.1) + 30

    # ---- 页脚尺寸（先算，才能定正文底线）
    f_ft = font(FONT_B, 32)
    f_fs = font(FONT_R, 26)
    foot_gap = 64
    half = (W - 2 * M - foot_gap) // 2
    foot_rows = []
    for sec in foot:
        segs = []
        for it in sec["items"]:
            segs += wrap(d, it[1], f_fs, half - 6)
        foot_rows.append((sec["title"], segs))
    foot_h = 34 + 44 + max(len(s) for _, s in foot_rows) * 36 + 26

    body_top = M + 186
    body_bottom = H - M - foot_h

    # ---- 名称列宽
    name_w = 0
    for s in sections:
        for it in s["items"]:
            if it[0] == "row":
                name_w = max(name_w, d.textlength(it[1], font=f_b))
    name_w = min(name_w, int(colw * 0.40))
    desc_w = colw - name_w - 34

    # ---- 行盒：先按基础行距算，再放大行距/节距把一页填满
    def make_boxes(lh_e, gap_e, sec_gap):
        out = []
        for sec in sections:
            rows, h = [], head_h + 14
            for it in sec["items"]:
                if it[0] == "row":
                    segs = wrap(d, it[2], f_r, desc_w)
                    rows.append(("row", it[1], segs))
                    h += lh_e * len(segs) + gap_e
                else:
                    segs = wrap(d, it[1], f_r, colw - 8)
                    rows.append(("flat", segs, None))
                    h += lh_e * len(segs) + gap_e
            h += sec_gap
            out.append((sec, rows, h))
        return out

    avail = body_bottom - body_top
    base = make_boxes(lh, 6, 40)
    head_all = len(base) * (head_h + 14 + 40)
    content = sum(h for _, _, h in base) - head_all
    scale = 1.0
    if content > 0:
        scale = min(1.55, max(1.0, (1.88 * avail - head_all) / float(content)))
    lh_e, gap_e, sec_gap = int(lh * scale), int(6 * scale), int(40 * scale)
    boxes = make_boxes(lh_e, gap_e, sec_gap)

    # ---- 分栏：左右尽量等高
    heights = [h for _, _, h in boxes]
    total = sum(heights)
    if total > avail * 2:
        return None
    best, acc = None, 0
    for k in range(1, len(heights) + 1):
        acc += heights[k - 1]
        rest = total - acc
        if acc <= avail and rest <= avail:
            score = abs(acc - rest)
            if best is None or score < best[0]:
                best = (score, k)
    if best is None:
        return None
    k = best[1]
    pos, y = [], body_top
    for i, (sec, rows, h) in enumerate(boxes):
        x = cols[0] if i < k else cols[1]
        if i == k:
            y = body_top
        pos.append((x, y))
        y += h

    # ---- 页眉
    d.rectangle([M, M - 26, M + 10, M + 56], fill=ACCENT)
    d.text((M + 34, M - 26), title, font=font(FONT_B, 64), fill=INK)
    sub = "Creo 6.0 – 12.0　·　源码 src/h_main.c"
    d.text((W - M - d.textlength(sub, font=f_sub), M - 6), sub, font=f_sub, fill=GRAY)
    d.line([M, M + 118, W - M, M + 118], fill=RULE, width=3)

    # ---- 正文
    for (sec, rows, h), (x, y) in zip(boxes, pos):
        d.rounded_rectangle([x, y, x + colw, y + head_h], 8, fill=HEADBG)
        d.rectangle([x, y + 10, x + 7, y + head_h - 10], fill=ACCENT)
        text_at(d, (x + 28, y + head_h // 2), sec["title"], f_hd, (18, 55, 92))
        yy = y + head_h + 14
        for kind, a, b in rows:
            if kind == "row":
                text_at(d, (x + 4, yy + lh_e // 2), a, f_b, INK)
                for k, seg in enumerate(b):
                    text_at(d, (x + name_w + 30, yy + lh_e // 2 + k * lh_e), seg, f_r, INK2)
                yy += lh_e * len(b) + gap_e
            else:
                for k, seg in enumerate(a):
                    text_at(d, (x + 4, yy + lh_e // 2 + k * lh_e), seg, f_r, INK2)
                yy += lh_e * len(a) + gap_e

    # ---- 页脚
    fy = H - M - foot_h + 20
    d.line([M, fy, W - M, fy], fill=RULE, width=3)
    fy += 30
    for k, (head, segs) in enumerate(foot_rows):
        x = M + k * (half + foot_gap)
        text_at(d, (x, fy + 18), head, f_ft, (18, 55, 92))
        yy = fy + 30
        for seg in segs:
            yy += 36
            text_at(d, (x, yy), seg, f_fs, INK2)

    # ---- 越界自检
    px = img.load()
    for x in list(range(M - 40, M - 12)) + list(range(W - M + 12, W - M + 40)):
        for yy in range(body_top, body_bottom):
            if px[x, yy] != (255, 255, 255):
                return None
    return img


def main():
    for size in (48, 46, 44, 42, 40, 38, 36, 34, 32, 30, 28, 26):
        img = build(size)
        if img is not None:
            img.save(OUT, "PNG", optimize=True, dpi=(300, 300))
            print("已生成 %s（正文字号 %dpx，%.2f MB）"
                  % (OUT, size, os.path.getsize(OUT) / 1048576.0))
            return 0
    print("放不进一页 A4：内容超长")
    return 1


if __name__ == "__main__":
    sys.exit(main())
