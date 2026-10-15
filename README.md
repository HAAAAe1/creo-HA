# creo-HA — Creo 工程图圆圈序号（球标）插件
https://github.com/user-attachments/assets/3c779267-59cf-414f-9932-e56859ec0e5a

在 Creo 工程图上**点位置放圆圈球标、编号自动管理**的 TOOLKIT 插件。
不依赖 BOM 表，纯独立符号实例，想怎么排就怎么排。

- ✅ **添加球标**：点一下放一个，编号自动 +1；支持连续放置，不用反复点按钮
- ✅ **插入球标**：中间补插时，新球标顶上该编号，后面的号全部自动顺延
- ✅ **删除球标**：删掉中间某个，其余序号自动补齐重排
- ✅ **重排序号**：一键把所有球标压实成 1、2、3…
- ✅ **一键导出图纸**：弹出 Creo 自己的「保存」浏览器，**选目录 + 输文件名**，一次生成 3D 模型 **STP**、工程图 **PDF**（600 dpi 单色）、工程图 **DWG**，三个文件同目录同名
- ✅ **视图动态调整**：仿 HelloCreo 的 `DynaView`——点按钮后在 Creo **原生「选择」对话框**（悬停高亮 + 「数据 视图」提示）里选中一个普通视图点「确定」；随后**直接移动鼠标就能全方位旋转该视图**（X/Y 角度：水平移=绕竖直轴、垂直移=绕水平轴），中键确定，Esc 恢复原状
- ✅ **公差刷子**：点选一个**源尺寸**读走它的整套公差（类型 / 上·下公差值 / 公差表 / 小数位），然后**逐个点其它尺寸就立刻刷上去**（想刷多少个刷多少个），中键或 Esc 结束
- ✅ **一份 DLL 通吃 Creo 6.0 ~ 12.0**（用 Creo 6.0 Toolkit 编译，旧版编译的程序 PTC 官方保证可在更新版本上运行）
- ✅ **加载后功能区自动出现「HA」选项卡**，不用手工拖按钮
- ✅ **路径全自动**：插件运行时自查所在目录，整个文件夹拷到哪都能用

典型场景 —— 图纸上已排好 1~18 号，发现漏了一个尺寸要插在 12 号前面：
点「插入球标」→ 点新尺寸旁的放置位置 → 点现在显示 12 的那个球标。
新球标自动变成 12 号，原 12~18 全部变成 13~19 号，位置都不用动，一次点完。

---

## 快速安装（3 步，1 分钟）

1. **放置文件**：把整个 `H` 文件夹复制到纯英文、无空格的目录，例如 `D:\H`
   （放在别的位置先双击运行一次 `setup.bat`，它会自动修正注册文件里的路径）
2. **注册插件**：记事本打开 Creo 的 `config.pro`，最后加一行：
   ```
   protkdat D:\H\H.dat
   ```
   （放在别的目录就把路径换成实际位置；也可以在 Creo 里
   工具 → 辅助应用程序 → 注册 → 选中 H.dat → 启动，但只当次生效）
3. **重启 Creo**。打开任意工程图，选项卡栏最右侧出现「**HA**」选项卡，装好了。

> 如果点按钮提示"未找到球标符号文件"：看插件目录下自动生成的 `h_load.log`，
> 里面写明了原因（符号文件是否缺失、三条加载路径各自的返回码），把日志发给作者即可定位。

## 使用方法

打开一张工程图，功能区「HA」选项卡里有两组共 6 个按钮：

**「球标」组**

| 按钮 | 行为 |
|------|------|
| **添加球标** | 点图纸位置放一个圆圈球标，编号取当前最大号 +1。**连续放置**：放完一个直接点下一个位置，全部放完按鼠标中键（滚轮按下）/ 右键 / Esc 结束 |
| **插入球标** | 先点放置位置，再点一个已有球标；新球标就用那个编号，它以及比它大的号全部自动 +1 |
| **删除球标** | 点选要删除的球标，删除后其余序号自动重排补齐 |
| **重排序号** | 把所有球标压实成 1、2、3…（补齐删除留下的空号） |

**「功能」组**

| 按钮 | 行为 |
|------|------|
| **一键导出图纸** | 弹出 Creo 原生的「保存」文件浏览器（`ProFileMdlnameSave`）：**在哪儿选目录、叫什么名字都随你**，Esc 取消。<br>· `<名字>.stp` —— 该工程图关联的 **3D 模型**（STEP）<br>· `<名字>.pdf` —— 工程图本身，**600 dpi + 单色**，导出后不弹阅读器<br>· `<名字>.dwg` —— 工程图本身（AutoCAD DWG）<br>若图纸没关联 3D 模型，只导 PDF 和 DWG 并给出提示<br>**中文名没问题**：Creo 的 DWG 导出器不认中文名，所以插件先用固定的 ASCII 临时名导出到工作目录，再由插件改名搬到你要的位置（全程 `MoveFileExW`，宽字符） |
| **视图动态调整** | 分两步：**① 点按钮后弹出 Creo 原生「选择」对话框**（悬停视图会高亮并提示「数据 视图 / 视图名称」，选好后点「确定」；取消/Esc 退出）——只接受**普通视图（General）**，投影/详图/剖视等派生视图、以及带子视图的视图会被拒绝并提示；**② 选中后直接移动鼠标即可全方位旋转视图**（不用按键）：**水平移动 = 绕竖直轴转（Y 角度）、垂直移动 = 绕水平轴转（X 角度）**，即「视图方向」对话框里的 X/Y 角度；**中键 = 确定完成**，**Esc = 取消（逆序回滚，视图精确复原）**。选择走 `ProSelect("dwg_view")` + `PRO_SELECT_DONE_REQUIRED` 选择环境；旋转走 `ProDrawingViewOrientationFromAngleSet(dwg, view, "Horizontal"/"Vertical", NULL, 增量角)`——绕**过视图原点、相对图纸固定**的轴做**相对（增量）旋转**：`Horizontal`=俯仰→X 角、`Vertical`=偏摆→Y 角、`Normal`=平面内自转；**不需要任何基准视图，所以进入时视图原地不动（零跳变）**，可连续累积、可逆（Esc 把每步逆序取负角回放，精确复原）——这正是 HelloCreo 同族的 `wfcWView2D::SetOrientationFromAngle`。**已弃用** `ProDrawingViewOrientationFromNameSet`：OTK 官方用户指南原文 *"orients the specified drawing view using saved views from the model"* ⇒ 它的 `MdlView` 只能是**模型里已保存的视图**，第一次调用就把视图强行跳到那个方向（第十版实测 `probe mdl="11" -> 0`，视图当场被拉走）；鼠标位置用 `ProMouseTrack`，按键状态用 `GetAsyncKeyState`（`ProMouseTrack` 的按键是事件语义，不能用于按住检测） |
| **公差刷子** | 刷子式两步：**① 点选【源尺寸】**（公差的来源，Esc 退出）——选到参考尺寸/从动尺寸会提示重选；**② 逐个点选要接收公差的尺寸**，点一个立刻刷一个，个数不限，**中键 / Esc 结束**。刷入内容 = 公差类型（公称/基本/极限/正负/对称/对称上标）、上/下公差值、ISO/DIN 公差表、公差小数位（「精度」），全部**逐项原样复制**（官方示例 `TestDimension.c:305/314` 就是这套 Get→Set 成对用法）。选择过滤名 `"dimension"`，选择→句柄用 `ProSelectionModelitemGet`（`ProDimension` 即 `ProModelitem` 别名）。**不做「极限值按名义尺寸换算」**：OTK 官方指南 `Dimension_Tolerances.html` 说明这些接口取回的是**公差值本身**（不是“名义值+公差”的绝对极限），按名义值换算反而会算错。公差要可见需 Creo config 选项 `tol_display yes`。 |

## 故障排查

插件目录下每次启动都会写 `h_load.log`，关键行：

- `ProCmdActionAdd(...) -> 0`：0 = 命令注册成功（返回 -28 也不影响按钮功能，可忽略）
- `ProRibbonDefinitionfileLoad(...) -> 0`：功能区加载成功。
  `-4` = rbn 文件没找到（确认 `text\ribbon\toolkitribbonui.rbn` 存在）；
  `-16` = 格式不对（按下方说明用 Creo 导出一份正规的）
- `HLoadSymbol: ... EXISTS / MISSING!`：符号文件在不在。`MISSING!` 说明发出去的包不完整，少了 `symbols` 文件夹
- `HLoadSymbol: OK via [1|2|3]`：符号最终走通哪条加载路（1=pro_symbol_dir 相对路径，2=复制到工作目录，3=绝对路径兜底）
- `HExportAct: ...`：一键导出图纸的逐步返回码。`STEP -> 0`、`DWG -> 0`、`PDF -> 0` 是成功；
  `ProFileMdlnameSave -> 0` 是浏览器正常返回（`-1` = 用户取消），
  `HExport: moved -> <完整路径>` 表示临时文件已改名搬到你要的位置，
  `HExport: MISSING source for <路径>` 才是那一个文件没落地；
  行尾 `license_missing=1` 说明缺少对应导出模块的授权。
  `PDF dpi / PDF mono / PDF no-viewer -> 0` 表示 600 dpi、单色、不弹阅读器三项设置都已生效

常见现象：

- **提示"未找到球标符号文件"**：把 `symbols\123.sym` 复制到 Creo 当前工作目录，
  或在 `config.pro` 里设 `pro_symbol_dir D:\H\symbols`（已有该行就别加第二行，直接把符号放进它指向的目录）
- **提示"请在工程图界面执行此命令"**：这三个命令只能在工程图（Drawing）模式下用
- **功能区没有「HA」选项卡**：先确认工程图模式（选项卡只在工程图出现）；
  仍没有则按 [ Creo 自定义功能区 ] 导出正规 rbn：`config.pro` 加
  `tk_enable_ribbon_custom_save yes` 重启 → 文件 → 选项 → 自定义功能区 →
  新建选项卡/组 → 命令来源选「所有 ProTk 命令」添加 6 个按钮 →
  导入/导出 → 保存辅助应用程序用户界面 → 把生成的 rbn 复制覆盖 `text\ribbon\` 下同名文件
- **页签/组名显示成英文键名（如 `H_Title`、`Balloon`、`H_Func`）**：说明功能区页签和组的
  **标签消息文件 `text\<locale>\ribbonui.txt` 缺失或没刷新**。Creo 解析页签/组标签只认这个专用文件
  （命令标签才走 `h_msg.txt`），键名就是 rbn 里的 `mLabel` 值。只要这个文件在、且重启生效即可。
  （含中文内容的用 UTF-8 + BOM；英文那份纯 ASCII 无 BOM；行尾是 LF）

## ⚠️ 发给别人之前：必须先解锁

别人打开 Creo 弹出「**应用程序 "H" 在分配到您的位置之前未被解锁**」，
不是插件写错，是 PTC 对 TOOLKIT 插件的统一规定：编译出的 DLL 默认锁定，
只有装了 Creo Parametric TOOLKIT 开发许可的电脑才能加载，分发前必须解锁。

1. 完全退出 Creo（任务管理器确认没有 `xtop.exe`）
2. 双击插件目录里的 `unlock.bat`，看到「解锁成功」即可
3. 打包发给别人

两条铁律：

- **每次重新编译 H.dll 之后都要重新跑一次 unlock.bat**（链接时会重新写入锁定标记）
- **解锁之后不要再改动 H.dll 哪怕一个字节**（改动后会被重新当成锁定状态）

解锁会占用一个 TOOLKIT 许可几秒钟（最长 15 分钟），正常现象。

## 球标符号版本注意（`symbols\123.sym`）

Creo 的文件只**向上兼容**：新版能读旧版，旧版读不了新版，`.sym` 一模一样。

- 本插件的 `123.sym` 用 **Creo 6.0.2.0** 存出（格式级别 3700），Creo 6~12 全能读
- **千万别用新版 Creo 打开它再保存** —— 保存会升级成新版格式，对方的 Creo 6 立刻读不了
- 以后换符号同理：发给谁，就用**对方那个版本或更老**的 Creo 去存

## 重新编译（可选）

需要 Visual Studio 2022（含 C++ 生成工具）和 Creo 6.0 的 Toolkit 开发包
（Creo 安装介质自带，没勾「API Toolkits / Creo Parametric TOOLKIT」组件的话用安装程序补装）：

1. 把 `makefile` 里 `CREO6_SDK` 改成 Toolkit 根目录（含 `includes` 与 `x86e_win64\obj`）
2. 双击 `build.bat`（或 `nmake /f makefile`）
3. **重新编译后记得再跑一次 `unlock.bat`**

用旧版 Toolkit 编译是刻意为之：PTC 官方规则保证旧版编译的 DLL 可在相同或更新的
Creo 上运行（"Creo Parametric build newer than Creo TOOLKIT build: always supported"），
反过来不保证。所以统一用 Creo 6.0 Toolkit 编译，一份 DLL 覆盖 6.0 ~ 12.0。

## 文件结构

```
H/
├── H.dll                        插件主程序
├── H.dat                        TOOLKIT 注册文件
├── setup.bat                    一键把 H.dat 路径改成当前文件夹
├── unlock.bat                   分发前解锁（见上文）
├── build.bat / makefile         重新编译用
├── src/
│   └── h_main.c                 插件源码
├── text/
│   ├── ribbon/toolkitribbonui.rbn   功能区定义（自动挂「HA」选项卡）
│   ├── chinese_cn/h_msg.txt         中文界面文字（命令标签 Help）
│   ├── usascii/h_msg.txt            英文界面文字
│   ├── chinese_cn/ribbonui.txt      中文功能区页签/组名（HA / 球标 / 功能）
│   └── usascii/ribbonui.txt         英文功能区页签/组名
└── symbols/
    └── 123.sym                 圆形球标符号（数字为可变文本，Creo 6 格式）
```

---

与 BOM 表无关，纯粹在工程图上点位置放圆圈序号：末尾加 → 自动 +1；
中间补插 → 后面的号自动顺延；删掉中间某个 → 其余序号自动补齐重排。
