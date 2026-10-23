# -*- coding: utf-8 -*-
"""
make_icons.py —— 从 Creo 安装目录的官方图标里挑出语义最接近的，转成插件自己的图标。

为什么这么做
------------
Pro/TOOLKIT 给自定义按钮挂图标只有一条路：ProCmdIconSet(cmd_id, "name.png")。
它是按【文件名】去三个目录里找：
    1. <Creo loadpoint>/text/resource
    2. <应用 text_dir>/resource
    3. <应用 text_dir>/<语言>/resource
我们只能可靠地控制第 2 条 ⇒ 图标文件必须落在 D:\\H\\text\\resource\\。

Creo 自带的图标在
    <Creo>/Common Files/text/resource/*.png
有 7000 多个。这里按"功能语义"挑一个最接近的，统一裁成两种尺寸：
    h_<key>.png        小图标 20x20
    h_<key>_large.png  大图标 32x32
（Creo 官方约定：小 = 16x16 或 20x20，大 = 32x32，文件名不带尺寸后缀。）

没有 PIL 也能跑：本脚本自带 PNG 解码/编码/最近邻缩放（只用 zlib）。

用法：python D:\\H\\tools\\make_icons.py [--dry]
"""

import os
import sys
import zlib
import struct

CREO_RES = r"D:\Program Files\PTC\Creo 6.0.2.0\Common Files\text\resource"
OUT_DIR = r"D:\H\text\resource"

SMALL = 20
LARGE = 32

# key -> Creo 官方图标基础名。左边是插件里的命令（H_xxx 去掉 H_ 再小写），
# 右边是 Creo 自己的图标名，语义尽量贴。
MAP = [
    # —— 会话 / 文件 ——
    ("openworkdir",    "wildfire_folders"),      # 打开工作目录：文件夹
    ("setworkdir",     "working_dir"),           # 设置工作目录
    ("openmdldir",     "open"),                  # 打开模型所在目录
    ("mdltodir",       "save_as"),               # 模型另存到目录
    ("batchbackup",    "saveas_backup"),         # 批量备份
    ("purgeold",       "delete_all_versions"),   # 清理旧版本
    ("eraseidle",      "erase_not_displayed"),   # 清除不在会话的
    ("cleanjunk",      "clean_up"),              # 清垃圾文件：扫帚
    ("export",         "export"),                # 导出文件

    # —— 模型信息 ——
    ("rename3d",       "edit_name"),              # 重命名：笔 + 名字框
    ("copyname",       "copy"),                  # 复制名称
    ("outline",        "zoom_outline"),          # 轮廓尺寸：正好是"框住模型"

    # —— 显示 / 颜色 ——
    ("colorasm",       "colorise_palette"),      # 随机上色：调色板
    ("clearcolor",     "clear_all"),             # 清除颜色
    ("viewnormal",     "orient_view"),           # 正视图：方向盘
    ("togglehide",     "drw_visibility"),        # 显隐切换：眼睛
    ("hide",           "hide"),
    ("unhide",         "lay_shown"),
    ("hideall",        "hide_all"),
    ("unhideall",      "unhide_all"),

    # —— 装配 ——
    ("openinplace",    "assembly_edit"),         # 原地打开元件：笔 + 立方体

    # —— 基准 / 几何 ——
    ("eqcrv",          "crv_tp_spline_btn"),     # 方程曲线：样条

    # —— 工程图 ——
    ("openviewinplace", "dwg_view_menu"),        # 按视图方向开 3D：图纸 + 箭头指向实体
    ("open3d",         "3d_position"),           # 打开 3D
    ("tolbrush",       "dimension_tol_symmetric"),  # 公差刷
    ("techreq",        "note_editor"),           # 技术要求：文本
    ("dynaview",       "anim_view"),             # 动态视图：动画
    ("keydim",         "auto_dimension"),        # 关键尺寸
    ("keydimc",        "clean_dimensions"),      # 取消关键尺寸
    ("add",            "dwg_create_balloon"),    # 加球标
    ("insert",         "dtl_create_ref_balloon"),  # 插球标
    ("renum",          "dtl_redistribute_balloon"),  # 球标重编号
    ("delete",         "delete"),                # 删球标

    # —— 比例 ——
    ("scale",          "drw_scale"),             # 固定档位比例
    ("scalecustom",    "scale_lin"),             # 自定义比例
]


# ----------------------------- PNG 读写 -----------------------------
def _paeth(a, b, c):
    p = a + b - c
    pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
    if pa <= pb and pa <= pc:
        return a
    return b if pb <= pc else c


def read_png(path):
    """返回 (w, h, rgba bytearray)。只支持 8bit、非隔行。"""
    data = open(path, "rb").read()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError("not a png")
    pos = 8
    width = height = None
    depth = ctype = 0
    idat = bytearray()
    palette = None
    trns = None
    while pos < len(data):
        ln, typ = struct.unpack(">I4s", data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + ln]
        pos += 12 + ln
        if typ == b"IHDR":
            width, height, depth, ctype, comp, filt, inter = struct.unpack(">IIBBBBB", body)
            if depth != 8 or inter != 0:
                raise ValueError("unsupported png depth/interlace")
        elif typ == b"PLTE":
            palette = body
        elif typ == b"tRNS":
            trns = body
        elif typ == b"IDAT":
            idat += body
        elif typ == b"IEND":
            break
    if width is None:
        raise ValueError("no IHDR")

    raw = zlib.decompress(bytes(idat))
    channels = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}[ctype]
    bpp = channels
    stride = width * bpp
    out = bytearray(height * stride)
    prev = bytearray(stride)
    p = 0
    for y in range(height):
        ft = raw[p]; p += 1
        line = bytearray(raw[p:p + stride]); p += stride
        if ft == 1:
            for i in range(bpp, stride):
                line[i] = (line[i] + line[i - bpp]) & 0xFF
        elif ft == 2:
            for i in range(stride):
                line[i] = (line[i] + prev[i]) & 0xFF
        elif ft == 3:
            for i in range(stride):
                a = line[i - bpp] if i >= bpp else 0
                line[i] = (line[i] + ((a + prev[i]) >> 1)) & 0xFF
        elif ft == 4:
            for i in range(stride):
                a = line[i - bpp] if i >= bpp else 0
                c = prev[i - bpp] if i >= bpp else 0
                line[i] = (line[i] + _paeth(a, prev[i], c)) & 0xFF
        out[y * stride:(y + 1) * stride] = line
        prev = line

    rgba = bytearray(width * height * 4)
    for i in range(width * height):
        if ctype == 6:
            rgba[i * 4:i * 4 + 4] = out[i * 4:i * 4 + 4]
        elif ctype == 2:
            rgba[i * 4:i * 4 + 3] = out[i * 3:i * 3 + 3]
            rgba[i * 4 + 3] = 255
        elif ctype == 0:
            g = out[i]
            rgba[i * 4:i * 4 + 4] = bytes((g, g, g, 255))
        elif ctype == 4:
            g, a = out[i * 2], out[i * 2 + 1]
            rgba[i * 4:i * 4 + 4] = bytes((g, g, g, a))
        elif ctype == 3:
            idx = out[i]
            r, g, b = palette[idx * 3:idx * 3 + 3]
            a = trns[idx] if (trns and idx < len(trns)) else 255
            rgba[i * 4:i * 4 + 4] = bytes((r, g, b, a))
    return width, height, rgba


def write_png(path, w, h, rgba):
    raw = bytearray()
    stride = w * 4
    for y in range(h):
        raw.append(0)
        raw += rgba[y * stride:(y + 1) * stride]

    def chunk(typ, body):
        return (struct.pack(">I", len(body)) + typ + body
                + struct.pack(">I", zlib.crc32(typ + body) & 0xFFFFFFFF))

    ihdr = struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0)
    open(path, "wb").write(b"\x89PNG\r\n\x1a\n"
                           + chunk(b"IHDR", ihdr)
                           + chunk(b"IDAT", zlib.compress(bytes(raw), 9))
                           + chunk(b"IEND", b""))


def resize(w, h, rgba, nw, nh, smooth=False):
    """缩放。

    小图（20x20，多数情况是原尺寸）用最近邻，线稿更锐利。
    大图（32x32）常常要从小图放大，最近邻会出锯齿，所以给 smooth=True
    走双线性。alpha 必须先 premultiply 再插值，否则透明区边缘会渗出白边。
    """
    if nw == w and nh == h:
        return bytearray(rgba)

    if smooth:
        out = bytearray(nw * nh * 4)

        def px(x, y):
            i = (y * w + x) * 4
            a = rgba[i + 3]
            if a == 0:
                return 0.0, 0.0, 0.0, 0.0
            k = a / 255.0
            return rgba[i] * k, rgba[i + 1] * k, rgba[i + 2] * k, float(a)

        for y in range(nh):
            sy = (y + 0.5) * h / nh - 0.5
            if sy < 0:
                sy = 0.0
            y0 = int(sy)
            if y0 > h - 1:
                y0 = h - 1
            y1 = y0 + 1 if y0 + 1 < h else y0
            fy = sy - y0
            for x in range(nw):
                sx = (x + 0.5) * w / nw - 0.5
                if sx < 0:
                    sx = 0.0
                x0 = int(sx)
                if x0 > w - 1:
                    x0 = w - 1
                x1 = x0 + 1 if x0 + 1 < w else x0
                fx = sx - x0
                r = g = b = a = 0.0
                for (xim, yim, wt) in ((x0, y0, (1 - fx) * (1 - fy)),
                                       (x1, y0, fx * (1 - fy)),
                                       (x0, y1, (1 - fx) * fy),
                                       (x1, y1, fx * fy)):
                    if wt == 0.0:
                        continue
                    pr, pg, pb, pa = px(xim, yim)
                    r += pr * wt
                    g += pg * wt
                    b += pb * wt
                    a += pa * wt
                di = (y * nw + x) * 4
                if a <= 0.0:
                    out[di:di + 4] = b"\x00\x00\x00\x00"
                    continue
                k = 255.0 / a
                out[di] = min(255, int(r * k + 0.5))
                out[di + 1] = min(255, int(g * k + 0.5))
                out[di + 2] = min(255, int(b * k + 0.5))
                out[di + 3] = min(255, int(a + 0.5))
        return out

    out = bytearray(nw * nh * 4)
    for y in range(nh):
        sy = min(h - 1, y * h // nh)
        for x in range(nw):
            sx = min(w - 1, x * w // nw)
            si = (sy * w + sx) * 4
            di = (y * nw + x) * 4
            out[di:di + 4] = rgba[si:si + 4]
    return out


# ----------------------------- 主流程 -----------------------------
def _probe(path):
    """安全度量：解不了（比如隔行/4bit 调色板）就当不可用，换下一个候选。"""
    try:
        w, h, _ = read_png(path)
        return w, h
    except Exception:
        return None


def pick_sources(base):
    """返回 (small_png, large_png) 两个文件名，缺的那个给 None。"""
    files = set(os.listdir(CREO_RES))
    small = large = None
    for suf in ("", "16x16", "20x20", "16X16", "_16x16", "_20x20"):
        f = base + suf + ".png"
        if f in files:
            d = _probe(os.path.join(CREO_RES, f))
            if d and 14 <= d[0] <= 24:
                small = f
                break
    for suf in ("_large", "_32x32"):
        f = base + suf + ".png"
        if f in files:
            d = _probe(os.path.join(CREO_RES, f))
            if d and d[0] >= 28:
                large = f
                break
    return small, large


def main():
    dry = "--dry" in sys.argv
    if not os.path.isdir(CREO_RES):
        print("找不到 Creo 资源目录:", CREO_RES)
        return 1
    if not dry:
        os.makedirs(OUT_DIR, exist_ok=True)

    done = 0
    for key, base in MAP:
        small, large = pick_sources(base)
        if small is None and large is None:
            print("!! 源图标缺失:", key, base)
            continue
        try:
            src = small or large
            w, h, rgba = read_png(os.path.join(CREO_RES, src))
        except Exception:
            try:
                src = large or small
                w, h, rgba = read_png(os.path.join(CREO_RES, src))
            except Exception:
                print("!! 两个都解不开:", key, base)
                continue
        sw = resize(w, h, rgba, SMALL, SMALL)
        if large and large != src:
            lw, lh, lrgba = read_png(os.path.join(CREO_RES, large))
        else:
            lw, lh, lrgba = w, h, rgba
            large = None
        lg = resize(lw, lh, lrgba, LARGE, LARGE, smooth=True)
        if not dry:
            write_png(os.path.join(OUT_DIR, "h_%s.png" % key), SMALL, SMALL, sw)
            write_png(os.path.join(OUT_DIR, "h_%s_large.png" % key), LARGE, LARGE, lg)
        print("%-16s <- %-28s small=%-30s large=%s"
              % (key, base, src, large or "(由小图放大)"))
        done += 1
    print("\n完成 %d / %d 个图标 -> %s" % (done, len(MAP), OUT_DIR))
    return 0


if __name__ == "__main__":
    sys.exit(main())
