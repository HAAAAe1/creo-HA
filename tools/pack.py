# -*- coding: utf-8 -*-
"""把 D:\\H 打成一个可以直接拷给别人的发布包（zip）。

用法：
    python tools/pack.py                 # 自动用今天日期命名
    python tools/pack.py 2026-10-23      # 指定包名里的日期/标签

产物：D:\\H\\dist\\H_<标签>.zip，解压后根目录是 H/（跟 D 盘上的目录同名，不改名），
      里面是 H.dat + H.dll + text/（含 ribbon 与 resource 图标）+ 模板等。

不打进去的（对照 .gitignore）：
    .git/ dist/ tools/ __pycache__/ h_load.log eq_curve.txt
    *.obj *.exp *.lib *.pdb *.ilk *.res
"""
import datetime
import os
import sys
import zipfile

ROOT = r"D:\H"
TOP = "H"                            # zip 内的根目录名：跟 D 盘上的目录同名，不改名
SKIP_DIRS = {".git", "dist", "tools", "__pycache__", ".idea", ".vscode"}
SKIP_EXT = {".obj", ".exp", ".lib", ".pdb", ".ilk", ".res", ".zip"}
SKIP_FILES = {"h_load.log", "eq_curve.txt", "Thumbs.db", "desktop.ini"}


def main():
    tag = sys.argv[1] if len(sys.argv) > 1 else datetime.date.today().isoformat()
    out_dir = os.path.join(ROOT, "dist")
    os.makedirs(out_dir, exist_ok=True)
    out = os.path.join(out_dir, "H_%s.zip" % tag)

    n = 0
    if os.path.exists(out):
        os.remove(out)
    with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        for dirpath, dirnames, filenames in os.walk(ROOT):
            dirnames[:] = [d for d in dirnames if d not in SKIP_DIRS]
            for fn in sorted(filenames):
                if fn in SKIP_FILES:
                    continue
                if os.path.splitext(fn)[1].lower() in SKIP_EXT:
                    continue
                full = os.path.join(dirpath, fn)
                rel = os.path.relpath(full, ROOT).replace("\\", "/")
                z.write(full, "%s/%s" % (TOP, rel))
                n += 1

    size = os.path.getsize(out)
    print("打包完成：%s" % out)
    print("  条目 %d 个，%.2f MB" % (n, size / 1048576.0))
    return 0


if __name__ == "__main__":
    sys.exit(main())
