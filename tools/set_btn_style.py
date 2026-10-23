# -*- coding: utf-8 -*-
"""把 toolkitribbonui.rbn 里每个命令按钮的显示样式改对。

起因：rbn 里所有命令都写成了
        11 mValue SmallTextOnly
     而 Creo 的样式枚举里 SmallTextOnly = 「小按钮·无图标」，
     所以 ProCmdIconSet 明明返回 0、图标文件也在，按钮上就是不画图标。

Creo 认可的五个值（从 Common Files/proe/uitools/text/compiled_resource
里的 adb_CmdDisplayType 词条挖出来）：
    DefaultStyle     跟随组的默认样式
    Small            小按钮 = 20x20 图标 + 文字，一行
    SmallTextOnly    小按钮·无图标（就是之前那个坑）
    SmallImageOnly   小按钮·只有图标，没有文字
    Large            大按钮 = 32x32 图标在文字上方

分配策略：命令数 <= 4 的组用 Large（图标大、最好认）；
          命令数 >= 5 的组用 Small（图标+文字并排，省横向宽度）。

用法： python tools/set_btn_style.py          改写 rbn
       python tools/set_btn_style.py --dry    只打印，不写文件
"""
import io
import os
import re
import sys

RBN = r"D:\H\text\ribbon\toolkitribbonui.rbn"

# 命令数 <= 4 的组里这些命令用大按钮；其余一律 Small
# ★ 2026-10-23 用户定稿：全部按钮统一 Small（图标+文字一行，最省横向宽度，
#   一组里能塞下 2~3 列）。想给某几个改回大按钮，把名字加回这个集合重跑即可。
LARGE = set()

RE_CMD = re.compile(r"^8 mName (\S+)")
RE_VAL = re.compile(r"^(11 mValue )(\S+)")

# 下拉（RbnDBCascade）里嵌套一层：容器头在 9，子命令在 12、其 mValue 在 15。
# 容器头（h_annot_note / h_scale_more）保持「无图标」，因为它自己没有图标，
# 改成 Small 会多出一块空白；子命令改成 Small，下拉里就能看到图标。
RE_CSC_CMD = re.compile(r"^12 mName (\S+)")
RE_CSC_VAL = re.compile(r"^(15 mValue )(\S+)")


def main():
    dry = "--dry" in sys.argv
    raw = open(RBN, "rb").read().decode("utf-8")
    lines = raw.splitlines(True)

    cmd_at = []          # (行号, 命令名)
    for i, ln in enumerate(lines):
        m = RE_CMD.match(ln)
        if m:
            cmd_at.append((i, m.group(1)))

    # 每个命令的 mValue 在其后 12 行内
    changed = 0
    for idx, (i, cmd) in enumerate(cmd_at):
        want = "Large" if cmd in LARGE else "Small"
        for j in range(i, min(i + 12, len(lines))):
            m = RE_VAL.match(lines[j])
            if not m:
                continue
            if m.group(2) != want:
                lines[j] = m.group(1) + want + "\n"
                changed += 1
            break

    print("命令 %d 个，改写 %d 处" % (len(cmd_at), changed))

    # 下拉子命令（注释 4 个 + 更多比例 20 个）：一律 Small，让下拉里带图标
    csc = 0
    for i, ln in enumerate(lines):
        m = RE_CSC_CMD.match(ln)
        if not m:
            continue
        for j in range(i, min(i + 12, len(lines))):
            mv = RE_CSC_VAL.match(lines[j])
            if not mv:
                continue
            if mv.group(2) != "Small":
                lines[j] = mv.group(1) + "Small\n"
                csc += 1
            break
    print("下拉子命令改写 %d 处" % csc)

    if dry:
        return 0

    out = "".join(lines)
    with open(RBN, "wb") as f:                      # 无 BOM，原样写回
        f.write(out.encode("utf-8"))
    print("已写回", RBN)
    return 0


if __name__ == "__main__":
    sys.exit(main())
