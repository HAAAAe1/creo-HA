# -*- coding: utf-8 -*-
"""把 D:\\H 打成一个可以直接发给别人的发布包（zip）。

用法：
    python 开发资料\\tools\\pack.py                 # 自动用今天日期命名
    python 开发资料\\tools\\pack.py 2026-10-23      # 指定包名里的日期/标签

产物：D:\\H\\开发资料\\dist\\H_<标签>.zip
      zip 里根目录叫 H/，解开就是给人看的样子（只有装插件要用的东西）：
        H.dat  H.dll  setup.bat
        text\\  templates\\  symbols\\
        安装说明_超简单版.txt   安装步骤.png
        功能说明.txt           功能说明_A4.png

不发出去的：开发资料\\（源码、工具、构建脚本、LICENSE、README —— 这些走 GitHub）、
            编译产物、日志、dist、__pycache__、tools 里的调试截图。
"""
import datetime
import os
import sys
import zipfile

ROOT = r"D:\H"
TOP = "H"                            # zip 内的根目录名：跟磁盘目录同名，不改名

# 放在 zip 根、别人一眼就看到的东西
KEEP_FILES = [
    "H.dat", "H.dll", "setup.bat",
    "安装说明_超简单版.txt", "安装步骤.png",
    "功能说明.txt", "功能说明_A4.png",
]
KEEP_DIRS = ["text", "templates", "symbols"]
DEV_DIR = "开发资料"                  # 开发文件：不进包，只留在 GitHub 仓库

SKIP_DIRS = {"dist", "__pycache__", ".idea", ".vscode"}
SKIP_EXT = {".obj", ".exp", ".lib", ".pdb", ".ilk", ".res", ".zip"}
SKIP_FILES = {
    "h_load.log", "eq_curve.txt", "Thumbs.db", "desktop.ini",
    ".gitignore", ".gitattributes", ".gitmodules",
    # tools 里的调试截图，不发
    "a4_preview.png", "a4_zoom1.png", "a4_zoom2.png", "crop_right.png",
    "large_preview.png", "view_cands.png", "zoom6.png", "zoom_alt.png",
}


def add_tree(z, base, arc, n):
    for dirpath, dirnames, filenames in os.walk(base):
        dirnames[:] = [d for d in dirnames if d not in SKIP_DIRS]
        for fn in sorted(filenames):
            if fn in SKIP_FILES:
                continue
            if os.path.splitext(fn)[1].lower() in SKIP_EXT:
                continue
            full = os.path.join(dirpath, fn)
            rel = os.path.relpath(full, base).replace("\\", "/")
            name = "%s/%s/%s" % (TOP, arc, rel) if arc else "%s/%s" % (TOP, rel)
            z.write(full, name)
            n[0] += 1


def main():
    tag = sys.argv[1] if len(sys.argv) > 1 else datetime.date.today().isoformat()
    out_dir = os.path.join(ROOT, DEV_DIR, "dist")
    os.makedirs(out_dir, exist_ok=True)
    out = os.path.join(out_dir, "H_%s.zip" % tag)

    if os.path.exists(out):
        os.remove(out)

    n = [0]
    with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        for fn in KEEP_FILES:
            p = os.path.join(ROOT, fn)
            if os.path.exists(p):
                z.write(p, "%s/%s" % (TOP, fn))
                n[0] += 1
            else:
                print("  [缺] " + fn)
        for d in KEEP_DIRS:
            p = os.path.join(ROOT, d)
            if os.path.isdir(p):
                add_tree(z, p, d, n)

    print("打包完成：%s" % out)
    print("  条目 %d 个，%.2f MB" % (n[0], os.path.getsize(out) / 1048576.0))
    return 0


if __name__ == "__main__":
    sys.exit(main())
