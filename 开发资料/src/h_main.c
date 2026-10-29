/*===========================================================================*\
  FILE    : h_main.c
  PRODUCT : H  —— Creo 工程图圆圈序号工具（TOOLKIT 插件）
            - 不依赖 BOM 表，独立的符号实例
            - 点一下放一个圆圈球标，编号自动 +1（1、2、3…）
            - 删除任意球标后自动重排（删 1，原 2 变 1）
            - 符号载体：symbols\123.sym（圆形球标，可变文本 = 数字）

  BUILD   : 面向 Creo 6.0 Toolkit 编译（MSVC x64）
            PTC 官方兼容规则：用较旧版本的 Toolkit 编译出的 DLL，可以在
            相同或更新的 Creo 上运行（"Creo Parametric build newer than
            Creo TOOLKIT build: This is always supported"）。
            反过来（用新版 Toolkit 编译、跑在旧版 Creo 上）不受支持。
            所以本插件统一用 Creo 6.0 的 Toolkit 编译 —— 一份 DLL 同时
            兼容 Creo 6.0 / 7 / 8 / 9 / 10 / 11 / 12 各版本。

  ★ 可移植性设计（为分发给别人）：
    1) DLL 不再依赖任何硬编码绝对路径。运行时用 GetModuleFileName 反查
       自身所在目录，日志、符号目录全部相对它计算。
       -> 整个 H 文件夹拷到任何位置（C 盘/D 盘/中文路径均可）都能直接用。
    2) 符号文件从  <插件目录>\symbols  加载，对方【零配置】。
       注意：ProDrawingDtlsymdefRetrieve 的 filepath 官方定义是"相对
       pro_symbol_dir 的相对路径"（没设才用工作目录）——所以传绝对路径、
       或切换工作目录，都命中不了。改用带来源参数的官方 API：
           ProSolidDtlsymdefRetrieve(..., PRO_DTLSYMDEF_SRC_PATH, 绝对目录, ...)
       官方枚举注释即 "absolute path"。另保留两条兜底，详见 HLoadSymbol。
    3) 界面消息文件名 h_msg.txt（宽字符串 + 带扩展名，见下方说明）。
    4) 功能区按钮不靠手工拖拽：同目录 text\toolkitribbonui.rbn 会被 Creo
       在加载插件时自动读取，直接生成名为 H 的选项卡。
\*===========================================================================*/

#include <ProToolkit.h>
#include <ProTKRunTime.h>
#include <ProUI.h>
#include <ProMdl.h>
#include <ProDrawing.h>
#include <ProDrawingView.h>
#include <ProSelection.h>
#include <ProSelbuffer.h>
#include <ProMenuBar.h>
#include <ProRibbon.h>
#include <ProUtil.h>
#include <ProObjects.h>
#include <ProArray.h>
#include <ProWstring.h>
#include <ProDtlsyminst.h>
#include <ProDtlsymdef.h>
#include <ProSolid.h>
#include <ProLayer.h>
#include <ProSurface.h>
#include <ProSurfacedata.h>
#include <ProGeomitemdata.h>
#include <ProAsmcomp.h>
#include <ProAsmcomppath.h>
#include <ProFeatType.h>
#include <ProFeature.h>
#include <ProElement.h>
#include <ProReference.h>
#include <ProDtmCrv.h>
#include <ProCsys.h>
#include <ProCsysdata.h>
#include <ProFamtable.h>
#include <ProFaminstance.h>
#include <ProParameter.h>
#include <ProParamval.h>
#include <ProUIMessage.h>

/* 注：这里曾经有一大段直接调 Win32 的「Esc 监听 + 光标离开图形区监听」代码，
   是为了配合自定义的跟随预览循环。现在拾取已换回 Creo 原生的
   ProMousePickGet（见 HPickPlacePoint），Esc / 缩放 / 切换命令全由 Creo
   自己处理，这段 Win32 兜底代码整体删除。 */

#include <ProDtlitem.h>
#include <ProDtlattach.h>
#include <ProDtlnote.h>
#include <ProMessage.h>
#include <ProWindows.h>
#include <ProGraphic.h>
#include <ProDimension.h>
#include <ProView.h>
#include <ProPDF.h>
#include <windows.h>
#include <shellapi.h>
#include <wincodec.h>        /* 标准件预览：PNG 解码（GDI+ 头文件只支持 C++，WIC 才认 C） */
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <wchar.h>
#include <stdlib.h>
#include <math.h>

/*---------------------------------------------------------------------*\
  常量
\*---------------------------------------------------------------------*/

/* 消息文件：ProMessageDisplay 与 ProCmdDesignate 的写法【完全一样】——
   参数类型是 ProFileName（= wchar_t[]，宽字符串），
   文件名必须【带扩展名、不带路径】。
   传窄字符串会让 Creo 按 wchar_t* 解析成乱码文件名，消息区就报
   “未找到消息'xxx'”，插件看起来像卡死；少写 .txt 则返回
   -28 = PRO_TK_MSG_NOT_FOUND。 */
#define MSGFILE_NAME "h_msg.txt"

/* 功能区定义文件（必须放在 <插件目录>\text\ribbon\ 下，见 ProRibbon.h） */
#define RBNFILE_NAME "toolkitribbonui.rbn"

/* 符号文件（放在 <插件目录>\symbols 下，不带扩展名传给 API；
   文件名 = SYMNAME + ".sym"，且文件【内部名】= 文件名的全大写，
   这是 Creo 自己的规矩，所以这里必须与 symbols\123.sym 完全对应）。

   ★ 版本规矩（踩过坑）：
   Creo 的文件只向上兼容 —— 新版能读旧版，旧版读不了新版。
   所以这个符号必须用【目标机器上最老的那个 Creo 版本】存出来。
   曾经用过的 111.sym 是 Creo 12.4 存的（头部 "#Creo TM 12 ... 12.4.0.0"，
   格式级别 4300，正文还带 symbol_font / p_sym_param 等 Creo 8/9 才有的字段），
   在 Creo 6 上 ProDrawingDtlsymdefRetrieve 必然失败。
   现在的 123.sym 是用 Creo 6.0.2.0 存的（格式级别 3700），
   没有任何新版本字段 —— Creo 6/7/8/9/10/11/12 全部可读。
   ⚠ 以后千万不要用新版 Creo 打开它再另存，那会把它升级成新版格式，
     同事的 Creo 6 就又读不了了。 */
#define SYMNAME     "123"

/* 命令 ID —— 统一加 H_ 前缀，避免与其它插件的命令重名。
   这三个名字必须与 text\toolkitribbonui.rbn 里的 mName 以及
   h_msg.txt 里的标签键完全一致。 */
#define CMD_ADD      "H_AddBalloon"
#define CMD_INSERT   "H_InsertBalloon"
#define CMD_DELETE   "H_DeleteBalloon"
#define CMD_RENUM    "H_RenumberBalloon"
#define CMD_EXPORT   "H_ExportFiles"
#define CMD_DYNAVIEW "H_DynaView"
#define CMD_TOLBRUSH "H_TolBrush"
#define CMD_TECHREQ "H_TechReq"
#define CMD_KEYDIM  "H_KeyDim"
#define CMD_KEYDIMC "H_KeyDimClear"
#define CMD_OUTLINE      "H_Outline"
#define CMD_COPYNAME     "H_CopyName"   /* 复制模型名到剪贴板（模型信息组） */
#define CMD_HIDE         "H_Hide"
#define CMD_UNHIDE       "H_Unhide"
#define CMD_UNHIDEALL    "H_UnhideAll"
#define CMD_TOGGLEHIDE   "H_ToggleHide"
#define CMD_HIDEALL      "H_HideAll"
#define CMD_COLORASM     "H_ColorAsm"   /* 现已合并为「随机着色」，名字沿用旧名 */
#define H_SEL_MAX 512
#define CMD_RENAME3D     "H_Rename"
#define CMD_CLEARCOLOR   "H_ClearColor"
#define CMD_OPENINPLACE  "H_OpenInPlace"
#define CMD_VIEWNORMAL   "H_ViewNormal"
/* 工程图页的两个命令（2026-10-23）：
     H_OpenViewInPlace = 原位打开：选中一个图纸视图 -> 按【该视图的方向】打开 3D；
     H_Open3D          = 打开 3D  ：打开图纸对应的 3D 模型，【不动方向】。
   注意与 H_OpenInPlace 区分：那个是 3D 页的"装配里选中元件 -> 用装配当前视角
   把该零件单独开一个窗口"，语义不同，命令名也必须是新的。 */
#define CMD_OPENVIEWINPLACE "H_OpenViewInPlace"
#define CMD_OPEN3D          "H_Open3D"
#define CMD_BATCHBACKUP  "H_BatchBackup"
#define CMD_EQCRV        "H_EqCurve"
#define CMD_STDLIB       "H_StdLib"      /* 标准件库（本机自用，不进发布包） */

#define CMD_OPENWORKDIR "H_OpenWorkDir"
#define CMD_SETWORKDIR  "H_SetWorkDir"
#define CMD_OPENMDLDIR  "H_OpenMdlDir"
#define CMD_MDLTODIR    "H_MdlToDir"
#define CMD_PURGEOLD    "H_PurgeOld"
#define CMD_ERASEIDLE   "H_EraseIdle"
#define CMD_CLEANJUNK   "H_CleanJunk"
#define CMD_SCALE_1_1 "H_Scale_1_1"
#define CMD_SCALE_1_2 "H_Scale_1_2"
#define CMD_SCALE_1_3 "H_Scale_1_3"
#define CMD_SCALE_1_4 "H_Scale_1_4"
#define CMD_SCALE_2_1 "H_Scale_2_1"
#define CMD_SCALE_3_1 "H_Scale_3_1"
#define CMD_SCALE_4_1 "H_Scale_4_1"
#define CMD_SCALE_2_3 "H_Scale_2_3"
#define CMD_SCALE_2_5 "H_Scale_2_5"
#define CMD_SCALE_1_5 "H_Scale_1_5"
#define CMD_SCALE_1_6 "H_Scale_1_6"
#define CMD_SCALE_1_8 "H_Scale_1_8"
#define CMD_SCALE_1_10 "H_Scale_1_10"
#define CMD_SCALE_1_12 "H_Scale_1_12"
#define CMD_SCALE_1_15 "H_Scale_1_15"
#define CMD_SCALE_1_20 "H_Scale_1_20"
#define CMD_SCALE_3_2 "H_Scale_3_2"
#define CMD_SCALE_5_2 "H_Scale_5_2"
#define CMD_SCALE_5_1 "H_Scale_5_1"
#define CMD_SCALE_6_1 "H_Scale_6_1"
#define CMD_SCALE_8_1 "H_Scale_8_1"
#define CMD_SCALE_10_1 "H_Scale_10_1"
#define CMD_SCALE_12_1 "H_Scale_12_1"
#define CMD_SCALE_15_1 "H_Scale_15_1"
#define CMD_SCALE_20_1 "H_Scale_20_1"


/* 自定义比例（弹输入框） */
#define CMD_SCALE_CUSTOM "H_ScaleCustom"

/*---------------------------------------------------------------------*\
  全局
\*---------------------------------------------------------------------*/
static ProDtlsymdef g_symdef;                       /* 符号定义（绑定到某张绘图） */
static ProBoolean  g_symdef_valid = PRO_B_FALSE;
static ProMdl      g_symdef_owner = NULL;           /* g_symdef 属于哪张绘图；换图必须重载 */
static ProFileName g_msg_file;                      /* 宽字符串形式的 h_msg.txt */
static char        g_plugin_dir[PRO_PATH_SIZE] = "";/* DLL 所在目录 */
static char        g_symbols_dir[PRO_PATH_SIZE] = "";/* <DLL目录>\symbols */

/*---------------------------------------------------------------------*\
  日志（写到 DLL 同目录，便于排查；写不进去就静默跳过）
\*---------------------------------------------------------------------*/
static void HLog(const char *fmt, ...)
{
    char    path[PRO_PATH_SIZE];
    FILE   *f;
    va_list ap;

    if (g_plugin_dir[0] == '\0')
        return;
    sprintf(path, "%s\\h_load.log", g_plugin_dir);
    f = fopen(path, "a");
    if (!f)
        return;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fclose(f);
}

/*---------------------------------------------------------------------*\
  运行时定位自身目录：GetModuleHandleEx 拿本模块句柄，
  GetModuleFileName 拿完整路径，再砍掉文件名。
  这是纯 Win32 做法，和 Creo 版本无关，也不依赖 argv / 注册文件。
\*---------------------------------------------------------------------*/
static void HInitPluginDir(void)
{
    HMODULE hm = NULL;
    char    path[MAX_PATH];
    DWORD   n;
    char   *p;

    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCSTR)&HInitPluginDir, &hm))
        return;
    n = GetModuleFileNameA(hm, path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH)
        return;
    p = strrchr(path, '\\');
    if (p)
        *p = '\0';
    strncpy(g_plugin_dir, path, sizeof(g_plugin_dir) - 1);
    g_plugin_dir[sizeof(g_plugin_dir) - 1] = '\0';

    sprintf(g_symbols_dir, "%s\\symbols", g_plugin_dir);
}

/*---------------------------------------------------------------------*\
  统一提示输出（g_msg_file 在 user_initialize 里初始化）
\*---------------------------------------------------------------------*/
/* 前置声明：HUiInfoW 的实现在后面，清理垃圾要用 */
static void HUiInfoW(wchar_t *title, wchar_t *msg);

static void HMsg(char *key)
{
    ProMessageDisplay(g_msg_file, key);
}

/*---------------------------------------------------------------------*\
  工具：取当前模型（必须是工程图）
\*---------------------------------------------------------------------*/
static ProError HGetDrawing(ProMdl *p_drawing)
{
    ProError   status;
    ProMdlType mdl_type;

    status = ProMdlCurrentGet(p_drawing);
    if (status != PRO_TK_NO_ERROR)
        return status;
    ProMdlTypeGet(*p_drawing, &mdl_type);
    if (mdl_type != PRO_MDL_DRAWING)
        return PRO_TK_BAD_INPUTS;
    return PRO_TK_NO_ERROR;
}

/*---------------------------------------------------------------------*\
  工具：为符号实例设置可变文本值
  遍历符号定义的 notes，找出所有 \xxx\ 可变文本占位符，
  逐个创建 vartext（prompt = 占位符内容, value = 编号）并 Set 进 data
\*---------------------------------------------------------------------*/
static ProError HSetVarTexts(ProMdl drawing, ProDtlsyminstdata data, int number)
{
    ProError        err;
    ProDtlnote     *p_notes = NULL;
    ProDtlnoteline *p_lines = NULL;
    ProDtlnotetext *p_texts = NULL;
    ProDtlvartext   var_text = NULL;
    ProDtlvartext  *p_var_texts = NULL;
    int  n_notes = 0, n_lines = 0, n_texts = 0, n_vts = 0;
    int  i, j, k;
    ProLine     line;
    ProCharLine string, text;
    char  buf[32];
    wchar_t wbuf[PRO_LINE_SIZE];

    err = ProDrawingDtlnotesCollect((ProDrawing)drawing, &g_symdef,
                                    PRO_VALUE_UNUSED, &p_notes);
    if (err != PRO_TK_NO_ERROR && err != PRO_TK_E_NOT_FOUND)
    {
        HLog("HSetVarTexts: notesCollect -> %d FAILED\n", (int)err);
        return err;
    }

    err = ProArrayAlloc(0, sizeof(ProDtlvartext), 1, (ProArray*)&p_var_texts);
    if (err != PRO_TK_NO_ERROR)
        return err;

    sprintf(buf, "%d", number);
    ProStringToWstring(wbuf, buf);

    if (p_notes)
    {
        ProArraySizeGet((ProArray)p_notes, &n_notes);
        for (i = 0; i < n_notes; i++)
        {
            ProDtlnotedata nd = NULL;
            err = ProDtlnoteDataGet(&p_notes[i], &g_symdef, PRODISPMODE_NUMERIC, &nd);
            if (err != PRO_TK_NO_ERROR) continue;
            err = ProDtlnotedataLinesCollect(nd, &p_lines);
            if (err == PRO_TK_NO_ERROR && p_lines)
            {
                ProArraySizeGet((ProArray)p_lines, &n_lines);
                for (j = 0; j < n_lines; j++)
                {
                    err = ProDtlnotelineTextsCollect(p_lines[j], &p_texts);
                    if (err == PRO_TK_NO_ERROR && p_texts)
                    {
                        ProArraySizeGet((ProArray)p_texts, &n_texts);
                        for (k = 0; k < n_texts; k++)
                        {
                            ProDtlnotetextStringGet(p_texts[k], line);
                            ProWstringToString(string, line);
                            /* 可变文本占位符：\xxx\（首尾都是反斜杠） */
                            if (string[0] == '\\' &&
                                string[strlen(string) - 1] == '\\' &&
                                strlen(string) >= 3)
                            {
                                memset(text, '\0', sizeof(ProCharLine));
                                strncpy(text, &string[1], strlen(string) - 2);
                                ProStringToWstring(line, text);
                                ProDtlvartextAlloc(line, wbuf, &var_text);
                                if (var_text)
                                {
                                    ProArrayObjectAdd((ProArray*)&p_var_texts,
                                                      PRO_VALUE_UNUSED, 1, &var_text);
                                    n_vts++;
                                }
                            }
                            ProDtlnotetextFree(p_texts[k]);
                        }
                        ProArrayFree((ProArray*)&p_texts);
                        p_texts = NULL;
                    }
                }
                for (j = 0; j < n_lines; j++)
                    ProDtlnotelineFree(p_lines[j]);
                ProArrayFree((ProArray*)&p_lines);
                p_lines = NULL;
            }
        }
        ProArrayFree((ProArray*)&p_notes);
        p_notes = NULL;
    }

    if (n_vts > 0)
    {
        /* Set 接管 vartext 元素所有权，之后只 Free 数组外壳 */
        ProDtlsyminstdataVartextsSet(data, p_var_texts);
        ProArrayFree((ProArray*)&p_var_texts);
    }
    else if (p_var_texts)
    {
        ProArrayFree((ProArray*)&p_var_texts);
    }

    /* 这行是「换图纸后创建失败」的关键指标：
       vts=0 表示一个可变文本占位符都没读到 -> 编号根本设不进去，
       接下来 ProDtlsyminstCreate 多半要失败。正常情况下 123.sym
       带一个 \xxx\ 占位符，这里应当是 vts=1。 */
    HLog("HSetVarTexts: notes=%d vts=%d number=%d\n", n_notes, n_vts, number);
    return PRO_TK_NO_ERROR;
}

/*---------------------------------------------------------------------*\
  文件存在性检查（诊断用）
\*---------------------------------------------------------------------*/
static int HFileExists(const char *path)
{
    DWORD attr = GetFileAttributesA(path);
    return (attr != INVALID_FILE_ATTRIBUTES &&
            !(attr & FILE_ATTRIBUTE_DIRECTORY)) ? 1 : 0;
}

/*---------------------------------------------------------------------*\
  加载符号定义（111.sym）—— 目标：接收方零配置，且加载路径可远程诊断。

  【为什么原做法不成立】
  ProDrawingDtlsymdefRetrieve 的 filepath 官方定义（ProDtlsymdef.h:194）：
      "The path to the file with a symbol definition. This path is a
       RELATIVE path to the user symbol directory, which is defined as
       either the value of the configuration option 'pro_symbol_dir' or
       the Creo Parametric working directory if 'pro_symbol_dir' is
       not set."
  即：传绝对路径会当成"相对 pro_symbol_dir 的路径"；而只要用户设了
  pro_symbol_dir，"." 就指向那个目录、与工作目录无关。所以
  "绝对路径"和"切工作目录"两条路都命中不了 —— 这正是别人机器上
  报"未找到球标符号文件"的原因。

  【现在按顺序试三条，任一条成功即返回】
    ① ProSolidDtlsymdefRetrieve + PRO_DTLSYMDEF_SRC_PATH（绝对目录）
       —— 官方枚举注释即 "absolute path"，零配置主路径。
    ② 相对 pro_symbol_dir 的 "."  —— 兼容把 111.sym 放进 custom-symbol 的用户。
    ③ 把自带 111.sym 复制到 Creo 工作目录后再用 "."（pro_symbol_dir 未设时有效）。
\*---------------------------------------------------------------------*/
static ProError HLoadSymbol(ProMdl drawing)
{
    ProError status;
    ProName  wname;
    ProPath  wp;
    char     symfile[PRO_PATH_SIZE];

    ProStringToWstring(wname, SYMNAME);
    ProStringToWstring(wp, ".");

    /* 先给日志一个明确答案：自带符号文件到底在不在 */
    sprintf(symfile, "%s\\%s.sym", g_symbols_dir, SYMNAME);
    HLog("HLoadSymbol: %s -> %s\n", symfile,
         HFileExists(symfile) ? "EXISTS" : "MISSING!");

    /* 日志带上绘图名 —— 「换图之后才失败」这类问题靠它一眼看穿 */
    {
        ProName dn;
        char    cd[PRO_NAME_SIZE];
        if (ProMdlNameGet(drawing, dn) == PRO_TK_NO_ERROR)
        {
            ProWstringToString(cd, dn);
            HLog("HLoadSymbol: drawing=%s\n", cd);
        }
    }

    /* ⓪ 当前绘图里已经加载过同名符号？直接复用它的句柄。
          ★ 必须放在 Retrieve 之前（2026-10-14 实测补上）：
          ProDrawingDtlsymdefRetrieve 对「已存在」的符号返回
          PRO_TK_E_FOUND(-5)，官方文档原话 "The specified symbol exist
          but was not retrieved" —— 输出参数此时不保证被填充，拿到的
          句柄不可用。而「换图纸后重新加载」正好次次命中这种情况，
          所以不能只靠 Retrieve，要先在绘图里查一遍。 */
    {
        ProDtlsymdef *defs = NULL;
        if (ProDrawingDtlsymdefsCollect((ProDrawing)drawing, &defs)
                == PRO_TK_NO_ERROR && defs != NULL)
        {
            int n_defs = 0, id;
            ProArraySizeGet((ProArray)defs, &n_defs);
            for (id = 0; id < n_defs; id++)
            {
                ProDtlsymdefdata dd = NULL;
                ProName          dname;
                char             cname[PRO_NAME_SIZE];
                if (ProDtlsymdefDataGet(&defs[id], &dd) != PRO_TK_NO_ERROR
                        || dd == NULL)
                    continue;
                if (ProDtlsymdefdataNameGet(dd, dname) != PRO_TK_NO_ERROR)
                    continue;
                ProWstringToString(cname, dname);
                if (_stricmp(cname, SYMNAME) == 0)
                {
                    g_symdef       = defs[id];
                    g_symdef_valid = PRO_B_TRUE;
                    ProArrayFree((ProArray*)&defs);
                    HLog("HLoadSymbol: [0] already loaded in this drawing "
                         "(\"%s\") -> OK\n", cname);
                    return PRO_TK_NO_ERROR;
                }
            }
            ProArrayFree((ProArray*)&defs);
        }
    }

    /* ① 相对 pro_symbol_dir 检索（官方示例 TestSymDefMenu.c 用的就是 "."） */
    status = ProDrawingDtlsymdefRetrieve(drawing, wp, wname,
                                         PRO_VALUE_UNUSED, PRO_B_TRUE, &g_symdef);
    HLog("HLoadSymbol: [1] \".\" -> %d\n", (int)status);
    if (status == PRO_TK_NO_ERROR)
    {
        g_symdef_valid = PRO_B_TRUE;
        HLog("HLoadSymbol: OK via [1] pro_symbol_dir\n");
        return status;
    }

    /* ② 把自带符号复制到 Creo 工作目录，再用 "." 重试
          （覆盖对方没设 pro_symbol_dir 的机器） */
    {
        ProPath wcwd;
        char    cwd[PRO_PATH_SIZE], dst[PRO_PATH_SIZE];
        if (ProDirectoryCurrentGet(wcwd) == PRO_TK_NO_ERROR)
        {
            ProWstringToString(cwd, wcwd);
            sprintf(dst, "%s\\%s.sym", cwd, SYMNAME);
            if (!HFileExists(dst) && HFileExists(symfile))
                CopyFileA(symfile, dst, FALSE);
            status = ProDrawingDtlsymdefRetrieve(drawing, wp, wname,
                                                 PRO_VALUE_UNUSED, PRO_B_TRUE, &g_symdef);
            HLog("HLoadSymbol: [2] copy to cwd %s -> %d\n", cwd, (int)status);
            if (status == PRO_TK_NO_ERROR)
            {
                g_symdef_valid = PRO_B_TRUE;
                HLog("HLoadSymbol: OK via [2] cwd copy\n");
                return status;
            }
        }
    }

    /* ③ 官方 API + 绝对目录（PRO_DTLSYMDEF_SRC_PATH 枚举注释即 "absolute path"）
          注意：该 API 形参是 ProSolid，对图纸是否适用未经实测，故放在最后兜底。 */
    if (g_symbols_dir[0] != '\0')
    {
        ProPath wabs;
        ProStringToWstring(wabs, g_symbols_dir);
        status = ProSolidDtlsymdefRetrieve((ProSolid)drawing,
                                           PRO_DTLSYMDEF_SRC_PATH,
                                           wabs, wname, PRO_VALUE_UNUSED,
                                           PRO_B_TRUE, &g_symdef);
        HLog("HLoadSymbol: [3] SRC_PATH abs dir -> %d\n", (int)status);
        if (status == PRO_TK_NO_ERROR)
        {
            g_symdef_valid = PRO_B_TRUE;
            HLog("HLoadSymbol: OK via [3] absolute dir\n");
            return status;
        }
    }

    HLog("HLoadSymbol: FAILED (3 ways tried). dir=%s\n", g_symbols_dir);
    return PRO_TK_E_NOT_FOUND;
}

/*---------------------------------------------------------------------*\
  遍历所有符号实例，收集编号 -> 用于自动分配新编号
\*---------------------------------------------------------------------*/
typedef struct
{
    int  count;
    int *numbers;
    int  max;
} HScan;

static ProError HScanAction(ProDtlitem *item, ProError status, ProAppData data)
{
    ProDtlsyminst *syminst = (ProDtlsyminst*)item;
    HScan         *scan = (HScan*)data;
    ProDtlsyminstdata sdata = NULL;
    ProDtlvartext     *vts = NULL;
    int   n = 0, i;
    ProLine prompt, value;
    int   num;

    if (ProDtlsyminstDataGet(syminst, PRODISPMODE_NUMERIC, &sdata) != PRO_TK_NO_ERROR)
        return PRO_TK_NO_ERROR;
    if (sdata == NULL)
        return PRO_TK_NO_ERROR;
    /* ProDtlsyminstDataGet 返回内部引用，不要 Free */

    if (ProDtlsyminstdataVartextsCollect(sdata, &vts) == PRO_TK_NO_ERROR && vts)
    {
        ProArraySizeGet(vts, &n);
        for (i = 0; i < n; i++)
        {
            ProDtlvartextDataGet(vts[i], prompt, value);
            num = _wtoi(value);
            if (num > 0)
            {
                scan->count++;
                ProArrayObjectAdd((ProArray*)&scan->numbers, PRO_VALUE_UNUSED, 1, &num);
                if (num > scan->max)
                    scan->max = num;
            }
        }
        for (i = 0; i < n; i++)
            ProDtlvartextFree(vts[i]);
        ProArrayFree((ProArray*)&vts);
    }
    return PRO_TK_NO_ERROR;
}

static void HScanFree(HScan *s)
{
    if (s->numbers)
        ProArrayFree((ProArray*)&s->numbers);
    s->numbers = NULL;
}

/*---------------------------------------------------------------------*\
  球标编号的  读 / 写 / 整体顺延  —— 「插入式编号」的核心工具
\*---------------------------------------------------------------------*/

/* 读一个球标的编号（读不到或内容不是数字则返回 0） */
static int HBalloonNumberGet(ProDtlsyminst *syminst)
{
    ProDtlsyminstdata sdata = NULL;
    ProDtlvartext    *vts = NULL;
    int   nv = 0, j, num = 0;
    ProLine prompt, value;

    if (ProDtlsyminstDataGet(syminst, PRODISPMODE_NUMERIC, &sdata) != PRO_TK_NO_ERROR
        || sdata == NULL)
        return 0;
    if (ProDtlsyminstdataVartextsCollect(sdata, &vts) == PRO_TK_NO_ERROR && vts)
    {
        ProArraySizeGet(vts, &nv);
        for (j = 0; j < nv; j++)
        {
            ProDtlvartextDataGet(vts[j], prompt, value);
            if (_wtoi(value) > 0) { num = _wtoi(value); break; }
        }
        for (j = 0; j < nv; j++)
            ProDtlvartextFree(vts[j]);
        ProArrayFree((ProArray*)&vts);
    }
    return num;
}

/* 把一个球标的编号改成 number */
static ProError HBalloonNumberSet(ProDtlsyminst *syminst, int number)
{
    ProError          err;
    ProDtlsyminstdata sdata = NULL;
    ProDtlvartext    *vts = NULL;
    ProDtlvartext    *new_list = NULL;
    int   nv = 0, k;
    ProLine prompt, value;
    char  buf[32];

    err = ProDtlsyminstDataGet(syminst, PRODISPMODE_NUMERIC, &sdata);
    if (err != PRO_TK_NO_ERROR || sdata == NULL)
        return PRO_TK_GENERAL_ERROR;

    err = ProArrayAlloc(0, sizeof(ProDtlvartext), 1, (ProArray*)&new_list);
    if (err != PRO_TK_NO_ERROR)
        return err;

    if (ProDtlsyminstdataVartextsCollect(sdata, &vts) == PRO_TK_NO_ERROR && vts)
    {
        ProArraySizeGet(vts, &nv);
        for (k = 0; k < nv; k++)
        {
            ProDtlvartext newvt = NULL;
            ProDtlvartextDataGet(vts[k], prompt, value);
            if (_wtoi(value) > 0)
            {
                sprintf(buf, "%d", number);
                ProStringToWstring(value, buf);
            }
            ProDtlvartextAlloc(prompt, value, &newvt);
            if (newvt)
                ProArrayObjectAdd((ProArray*)&new_list, PRO_VALUE_UNUSED, 1, &newvt);
        }
        for (k = 0; k < nv; k++)
            ProDtlvartextFree(vts[k]);
        ProArrayFree((ProArray*)&vts);
    }
    /* Set 接管 vartext 元素所有权，只 Free 数组外壳 */
    ProDtlsyminstdataVartextsSet(sdata, new_list);
    ProArrayFree((ProArray*)&new_list);

    ProDtlsyminstErase(syminst);
    ProDtlsyminstModify(syminst, sdata);
    ProDtlsyminstShow(syminst);
    return PRO_TK_NO_ERROR;
}

/* 把所有编号 >= from_num 的球标整体平移 delta（插入时 delta = +1）
   逐个实例只读写一次，所以与遍历顺序无关，不会重复平移。 */
static int HShiftNumbers(ProMdl drawing, int from_num, int delta)
{
    ProDtlsyminst *insts = NULL;
    int   n = 0, i, done = 0;

    if (ProDrawingDtlsyminstsCollect((ProDrawing)drawing, PRO_VALUE_UNUSED, &insts)
            != PRO_TK_NO_ERROR || insts == NULL)
        return 0;
    ProArraySizeGet((ProArray)insts, &n);
    for (i = 0; i < n; i++)
    {
        int num = HBalloonNumberGet(&insts[i]);
        if (num >= from_num &&
            HBalloonNumberSet(&insts[i], num + delta) == PRO_TK_NO_ERROR)
            done++;
    }
    ProArrayFree((ProArray*)&insts);
    HLog("HShiftNumbers: from=%d delta=%d affected=%d\n", from_num, delta, done);
    return done;
}

/*---------------------------------------------------------------------*\
   打印并返回「符号文件自带的附着点」。

   ★ 这里【只读不写】—— 绝不覆盖符号的附着点。

   为什么：Creo 放置符号实例时，是拿符号定义自带的附着点去对齐鼠标位置的。
   官方示例 pt_graphics\TestDtlsyminst.c 的 ProTestAttachGet() 就是先
   ProDtlsymdefdataAttachGet() 把附着点读出来，再把它的【类型】交给
   ProDtlsyminstdataAttachtypeSet()；放置位置则由
   ProDtlsyminstdataAttachmentSet() 用鼠标拾取点决定。

   之前我们额外调用了 ProDtlsyminstdataDefattachSet()，把附着点强改成
   FREE @ 符号原点 (0,0,0)，前提假设是「圆心画在符号原点」。实测该假设
   不成立 —— 结果球标偏得更远（本机 Creo 12 也偏）。所以退回官方做法：
   附着点完全交给符号文件，插件一个字节都不改。

   返回值：符号定义里第一个附着点的类型（读不到时退回 FREE）。
\*---------------------------------------------------------------------*/
static ProDtlsymdefattachType HDumpSymdefAttach(void)
{
    ProDtlsymdefdata       d   = NULL;
    ProDtlsymdefattach    *arr = NULL;
    ProDtlsymdefattachType ret = PROSYMDEFATTACHTYPE_FREE;
    int   i, n = 0;

    /* 字段含义见 ProDtlsymdefattachGet() 文档：
       type=FREE 等用 location；type=ON_ITEM/NORM_ITEM 用 entity_id。 */
    if (ProDtlsymdefDataGet(&g_symdef, &d) != PRO_TK_NO_ERROR || d == NULL)
    {
        HLog("SymdefAttach: DataGet failed, fallback FREE\n");
        return ret;
    }
    if (ProDtlsymdefdataAttachGet(d, &arr) != PRO_TK_NO_ERROR || arr == NULL)
    {
        HLog("SymdefAttach: none / AttachGet failed, fallback FREE\n");
        ProDtlsymdefdataFree(d);
        return ret;
    }

    ProArraySizeGet((ProArray)arr, &n);
    HLog("SymdefAttach: count=%d\n", n);
    for (i = 0; i < n; i++)
    {
        ProDtlsymdefattachType t   = PROSYMDEFATTACHTYPE_FREE;
        int                    eid = 0;
        double                 par = 0.0;
        ProVector              loc;
        loc[0] = loc[1] = loc[2] = 0.0;
        if (ProDtlsymdefattachGet(arr[i], &t, &eid, &par, loc)
                == PRO_TK_NO_ERROR)
        {
            HLog("  [%d] type=%d entity_id=%d param=%.4f "
                 "loc=(%.4f, %.4f, %.4f)\n",
                 i, (int)t, eid, par, loc[0], loc[1], loc[2]);
            if (i == 0)
                ret = t;
        }
    }
    ProArrayFree((ProArray*)&arr);
    ProDtlsymdefdataFree(d);

    HLog("SymdefAttach: use symbol's own attach point, type=%d "
         "(NOT overridden)\n", (int)ret);
    return ret;
}

/* 在 point 处创建一个编号为 number 的球标（「添加」与「插入」共用） */
static ProError HCreateBalloon(ProMdl drawing, ProView over_view,
                               ProVector point, int number)
{
    ProDtlsyminstdata data = NULL;
    ProDtlattach      attach = NULL;
    ProDtlsyminst     syminst;
    ProError          sub;

    if (ProDtlsyminstdataAlloc(drawing, &data) != PRO_TK_NO_ERROR)
    {
        HLog("HCreateBalloon: [1] dataAlloc FAILED\n");
        return PRO_TK_GENERAL_ERROR;
    }
    ProDtlsyminstdataDefSet(data, &g_symdef);

    /* ★ 附着【类型】取符号文件自带的（官方 TestDtlsyminst.c 的做法）；
          放置位置仍由下面的 ProDtlattachAlloc() 用鼠标点决定。
          绝不调用 ProDtlsyminstdataDefattachSet() 去覆盖附着点。 */
    ProDtlsyminstdataAttachtypeSet(data, HDumpSymdefAttach());
    ProDtlsyminstdataDisplayedSet(data, PRO_B_TRUE);

    /* 每一步单独记错误码：失败时日志能直接指出是哪一环，
       不用再靠猜（之前这里静默返回，排查全靠反推）。 */
    sub = HSetVarTexts(drawing, data, number);
    if (sub != PRO_TK_NO_ERROR)
    {
        HLog("HCreateBalloon: [2] SetVarTexts -> %d FAILED\n", (int)sub);
        ProDtlsyminstdataFree(data);
        return PRO_TK_GENERAL_ERROR;
    }

    sub = ProDtlattachAlloc(PRO_DTLATTACHTYPE_FREE, over_view, point, NULL, &attach);
    if (sub != PRO_TK_NO_ERROR)
    {
        HLog("HCreateBalloon: [3] attachAlloc -> %d FAILED "
             "(view=%p pt=%.1f,%.1f)\n",
             (int)sub, (void*)over_view, point[0], point[1]);
        ProDtlsyminstdataFree(data);
        return PRO_TK_GENERAL_ERROR;
    }
    ProDtlsyminstdataAttachmentSet(data, attach);
    ProDtlattachFree(attach);           /* attach 已被复制，立即释放 */

    sub = ProDtlsyminstCreate(drawing, data, &syminst);
    if (sub != PRO_TK_NO_ERROR)
    {
        HLog("HCreateBalloon: [4] syminstCreate -> %d FAILED\n", (int)sub);
        ProDtlsyminstdataFree(data);
        return PRO_TK_GENERAL_ERROR;
    }
    ProDtlsyminstShow(&syminst);
    HLog("HCreateBalloon: number=%d OK\n", number);
    ProDtlsyminstdataFree(data);
    return PRO_TK_NO_ERROR;
}

/*---------------------------------------------------------------------*\
  通用：拾取「放置点」（添加球标 / 插入球标共用）

  ★ 2026-10-09 定稿：换回 Creo 原生拾取 ★

  前几版的做法是自己画一个「跟着鼠标走的预览圈」：用 ProMouseTrack 死循环
  采样鼠标位置，再用异或（complement）绘图模式画圆 + 编号。手感是好，但
  代价是**必然的**，不是实现得不好：

    动作函数（用户点按钮后 Creo 调用的那段代码）跑在 Creo 的界面线程上。
    要让圆跟着鼠标走，就得 for(;;) 一直不返回 —— 界面线程被占住，Creo
    自己的消息循环就停摆了。于是滚轮缩放、点别的功能区按钮、点标题栏关闭
    全都没反应：消息只会堆在队列里，没人取走。这不是「优先级高」，是死
    循环方案的固有代价。

  而 TOOLKIT 并不提供「原生预览」。翻遍官方示例（包括 pt_examples\
  pt_graphics\TestDtlsyminst.c 里 PTC 自己造符号实例的流程）：标准做法就是
  ProMousePickGet 拾一个点、再 ProDtlsyminstCreate 创建，没有别的花样。
  所以这是一道选择题：

        「跟随鼠标的预览圈」  与  「原生手感」  不可兼得。

  用户要的是后者 —— 跟 Creo 自带符号功能一致：能滚轮缩放、点别的功能就
  切走、关窗口正常。所以这里换回原生拾取 ProMousePickGet：

    · 它是 Creo 自己的拾取会话，界面消息循环照常运转
      → 滚轮缩放、中键平移都正常；
    · ProGraphic.h:598 官方原话「You must check the return value to make
      sure the user did not select a command.」→ 用户在拾取期间点了别的
      功能区按钮，本函数会带回错误码，我们立刻退出，让那个功能正常启动
      （这就是「点别的功能就切走」）；
    · Esc / 中键 / 右键 由 Creo 原生中断，同样带回非 0 返回码。

  交互：左键       -> 确认，point 有效，返回 PRO_TK_NO_ERROR
        中键/右键  -> 取消，返回 PRO_TK_ABORT
        Esc        -> 取消，返回 PRO_TK_ABORT
        点别的功能 -> 取消（Creo 带回错误码）

  preview_number 参数保留但不再使用（原生拾取没有自定义预览），
  只为调用处改动最小、将来想加回预览时留个位置。
\*---------------------------------------------------------------------*/
static ProError HPickPlacePoint(int preview_number, ProVector point)
{
    ProError       status;
    ProMouseButton btn;

    (void)preview_number;

    btn      = PRO_NO_BUTTON;
    point[0] = point[1] = point[2] = 0.0;

    /* expected_button 传 PRO_ANY_BUTTON：左键放置、中键/右键结束都要能
       收回来，只有拿到实际按下的键才能区分（ProGraphic.h:601）。 */
    status = ProMousePickGet(PRO_ANY_BUTTON, &btn, point);
    ProMessageClear();

    HLog("HPickPlacePoint: ProMousePickGet status=%d btn=%d point=(%.1f,%.1f)\n",
         (int)status, (int)btn, point[0], point[1]);

    if (status != PRO_TK_NO_ERROR)
    {
        /* 用户点了别的命令 / 按了 Esc —— Creo 已把控制权收走，直接让位 */
        HLog("HPickPlacePoint: cancelled (other command or abort)\n");
        return PRO_TK_ABORT;
    }
    if (btn != PRO_LEFT_BUTTON)
    {
        HLog("HPickPlacePoint: btn=%d -> cancel\n", (int)btn);
        return PRO_TK_ABORT;
    }
    return PRO_TK_NO_ERROR;
}

/* 扫描现有球标，返回下一个可用编号（最大值 +1，每轮重扫，保证始终连号） */
static int HNextNumber(ProMdl drawing)
{
    HScan scan;
    int   n;

    scan.count = 0; scan.numbers = NULL; scan.max = 0;
    ProDrawingDtlsyminstVisit((ProDrawing)drawing, PRO_VALUE_UNUSED,
                              HScanAction, NULL, (ProAppData)&scan);
    n = scan.max + 1;
    HScanFree(&scan);
    return n;
}

/*---------------------------------------------------------------------*\
  命令1：添加一个球标（追加在末尾）
\*---------------------------------------------------------------------*/
static uiCmdAccessState HAddAccess(uiCmdAccessMode access_mode)
{
    return ACCESS_AVAILABLE;
}

static int HAddAct(uiCmdCmdId command, uiCmdValue *p_value, void *p_push_command_data)
{
    ProError       status;
    ProMdl         drawing;
    ProVector      point;
    int            new_number;
    int            sheet;
    ProView        over_view;
    int            placed;

    status = HGetDrawing(&drawing);
    if (status != PRO_TK_NO_ERROR)
    {
        HMsg("BALLOON_NO_DRAWING");
        return 0;
    }

    /* ★ 符号定义是「绘图级」句柄（ProDrawingDtlsymdefRetrieve 的第一个
       参数就是绘图）。只在启动时加载一次、之后跨图复用是错的 ——
       2026-10-14 实测：在某张图上加载后，切到另一张图（例如新建工程图）
       继续用旧句柄，ProDrawingDtlnotesCollect 取不到可变文本占位符，
       vartexts 一个字都设不进去，ProDtlsyminstCreate 随即失败，
       界面上就是「操作失败，请检查后重试」。所以只要绘图变了就重载。 */
    if (!g_symdef_valid || g_symdef_owner != drawing)
    {
        if (HLoadSymbol(drawing) != PRO_TK_NO_ERROR)
        {
            HMsg("BALLOON_NO_SYMBOL");
            return 0;
        }
        g_symdef_owner = drawing;
        HLog("HAddAct: symbol (re)loaded for this drawing\n");
    }

    /* ★★ 连续放置（原生拾取）★★

       交互：
         左键       -> 放下一个球标（编号自动 = 现有最大号 + 1），继续等下一个点
         中键/右键  -> 结束
         Esc        -> 结束
         点别的功能 -> 结束，并让那个功能正常启动

       为什么这样就够了：拾取用的是 Creo 原生的 ProMousePickGet，它自己的
       拾取会话里滚轮缩放、命令切换都是活的（详见 HPickPlacePoint 的说明）。
       本函数只负责「放一个、再要下一个点」，连续放不用回去点功能区按钮。

       退出条件：HPickPlacePoint 返回非 0（中键 / 右键 / Esc / 点了别的功能 /
       出错）或 HCreateBalloon 出错，都立刻收尾，不留半截状态。 */
    ProDrawingCurrentSheetGet((ProDrawing)drawing, &sheet);
    ProDrawingBackgroundViewGet((ProDrawing)drawing, sheet, &over_view);

    placed = 0;

    for (;;)
    {
        HMsg(placed == 0 ? "BALLOON_PICK" : "BALLOON_PICK_MORE");
        ProWindowRepaint(PRO_VALUE_UNUSED);   /* 先刷出提示，用户才知道怎么操作 */

        /* 原生拾取选点：左键确认返回 0；中键 / 右键 / Esc / 点了别的功能 /
           出错一律返回非 0，这时直接收尾离开 */
        if (HPickPlacePoint(0, point) != PRO_TK_NO_ERROR)
        {
            HLog("HAddAct: placement ended, placed=%d\n", placed);
            break;
        }

        /* 落点确认后重新取号（每轮重扫，编号始终连号） */
        new_number = HNextNumber(drawing);
        HLog("HAddAct: place (%.1f,%.1f) number=%d placed=%d\n",
             point[0], point[1], new_number, placed);

        if (HCreateBalloon(drawing, over_view, point, new_number) != PRO_TK_NO_ERROR)
        {
            HLog("HAddAct: FAILED\n");
            HMsg("BALLOON_FAILED");
            break;                      /* 出错就退出，不要卡在坏状态里 */
        }
        placed++;
        ProWindowRepaint(PRO_VALUE_UNUSED);
        HMsg("BALLOON_ADD_OK");
    }

    HLog("HAddAct: finished, placed=%d\n", placed);
    if (placed > 0)
        HMsg("BALLOON_ADD_MULTI_OK");   /* 收尾给个明确反馈 */
    return 0;
}


/*---------------------------------------------------------------------*\
  命令2：插入一个球标（序号顺延）

  用途：图纸上已经排好 1..N 号，现在在中间补了一个尺寸，希望新尺寸的
  球标拿到中间某个号（比如 12），而原来的 12..N 自动变成 13..N+1。

  交互：① 点新球标的放置位置
        ② 点一个已有球标 —— 新球标就插在它前面，用它原来的编号；
           它以及编号更大的全部 +1。
  为什么不用手输数字：这里只用到本插件已经验证可用的拾取/选择接口，
  没有任何新的输入类 API，最稳。
\*---------------------------------------------------------------------*/
static int HInsertAct(uiCmdCmdId command, uiCmdValue *p_value, void *p_push_command_data)
{
    ProError       status;
    ProMdl         drawing;
    ProVector      point;
    ProView        over_view;
    ProSelection  *sel = NULL;
    int            n_sels = 0, sheet, insert_num;
    ProDtlsyminst  ref_inst;

    status = HGetDrawing(&drawing);
    if (status != PRO_TK_NO_ERROR)
    {
        HMsg("BALLOON_NO_DRAWING");
        return 0;
    }
    /* 同 HAddAct：换图纸（新建工程图等）后旧句柄失效，必须按绘图重载 */
    if (!g_symdef_valid || g_symdef_owner != drawing)
    {
        if (HLoadSymbol(drawing) != PRO_TK_NO_ERROR)
        {
            HMsg("BALLOON_NO_SYMBOL");
            return 0;
        }
        g_symdef_owner = drawing;
        HLog("HInsertAct: symbol (re)loaded for this drawing\n");
    }

    ProDrawingCurrentSheetGet((ProDrawing)drawing, &sheet);
    ProDrawingBackgroundViewGet((ProDrawing)drawing, sheet, &over_view);

    /* ① 放置位置：和「添加球标」一样，用 Creo 原生拾取。
       中键 / 右键 / Esc / 点了别的功能 都能取消。 */
    HMsg("BALLOON_INSERT_PICK");
    ProWindowRepaint(PRO_VALUE_UNUSED);
    if (HPickPlacePoint(-1, point) != PRO_TK_NO_ERROR)
    {
        HLog("HInsertAct: placement cancelled by user\n");
        return 0;
    }
    HLog("HInsertAct: placement at (%.1f,%.1f)\n", point[0], point[1]);

    /* ② 参照球标（新球标插在它前面）
       ProSelect() 走的是 Creo 自己的交互机制，Esc / 中键取消由它内部处理，
       不需要我们额外做取消通道。 */
    HMsg("BALLOON_INSERT_REF");
    ProWindowRepaint(PRO_VALUE_UNUSED);
    sel    = NULL;
    n_sels = 0;
    status = ProSelect((char*)"dtl_symbol", 1, NULL, NULL, NULL, NULL, &sel, &n_sels);
    ProMessageClear();
    HLog("HInsertAct: ProSelect status=%d n_sels=%d\n", (int)status, n_sels);
    if (status != PRO_TK_NO_ERROR || n_sels < 1 || sel == NULL)
        return 0;
    if (ProSelectionModelitemGet(sel[0], (ProModelitem*)&ref_inst) != PRO_TK_NO_ERROR)
        return 0;

    insert_num = HBalloonNumberGet(&ref_inst);
    HLog("HInsertAct: insert at %d\n", insert_num);
    if (insert_num <= 0)
    {
        HMsg("BALLOON_NO_BALLOON");
        return 0;
    }

    /* 先把该号及其后整体 +1，再建新球标，这样新球标不会被自己顶走；
       万一新建失败就整体 -1 回滚，不给用户留半截状态。 */
    HShiftNumbers(drawing, insert_num, +1);
    if (HCreateBalloon(drawing, over_view, point, insert_num) != PRO_TK_NO_ERROR)
    {
        HShiftNumbers(drawing, insert_num + 1, -1);
        HLog("HInsertAct: FAILED, rolled back\n");
        HMsg("BALLOON_FAILED");
        return 0;
    }
    ProWindowRepaint(PRO_VALUE_UNUSED);
    HMsg("BALLOON_INSERT_OK");
    return 0;
}

/*---------------------------------------------------------------------*\
  重排全部球标编号为 1..n（按原编号升序）
\*---------------------------------------------------------------------*/
typedef struct
{
    int           orig;
    ProDtlsyminst syminst;
} HRenumItem;

static ProError HRenumberAll(ProMdl drawing)
{
    ProError       status;
    ProDtlsyminst *insts = NULL;
    HRenumItem    *items = NULL;
    int            n = 0, i, j, n_items = 0;

    status = ProDrawingDtlsyminstsCollect((ProDrawing)drawing, PRO_VALUE_UNUSED, &insts);
    if (insts == NULL)
        return PRO_TK_E_NOT_FOUND;
    ProArraySizeGet((ProArray)insts, &n);
    if (n == 0)
    {
        ProArrayFree((ProArray*)&insts);
        return PRO_TK_E_NOT_FOUND;
    }

    items = (HRenumItem*)malloc(sizeof(HRenumItem) * n);
    if (!items)
    {
        ProArrayFree((ProArray*)&insts);
        return PRO_TK_E_NOT_FOUND;
    }

    /* 第一遍：筛选数值型球标，记录原编号 + 句柄副本 */
    for (i = 0; i < n; i++)
    {
        ProDtlsyminstdata sdata = NULL;
        ProDtlvartext *vts = NULL;
        int  nv = 0, num = 0, found = 0;
        ProLine prompt, value;

        if (ProDtlsyminstDataGet(&insts[i], PRODISPMODE_NUMERIC, &sdata) != PRO_TK_NO_ERROR)
            continue;
        if (sdata == NULL) continue;
        if (ProDtlsyminstdataVartextsCollect(sdata, &vts) == PRO_TK_NO_ERROR && vts)
        {
            ProArraySizeGet(vts, &nv);
            for (j = 0; j < nv; j++)
            {
                ProDtlvartextDataGet(vts[j], prompt, value);
                if (_wtoi(value) > 0) { num = _wtoi(value); found = 1; break; }
            }
            for (j = 0; j < nv; j++)
                ProDtlvartextFree(vts[j]);
            ProArrayFree((ProArray*)&vts);
        }
        if (found)
        {
            items[n_items].orig = num;
            items[n_items].syminst = insts[i];
            n_items++;
        }
    }

    if (n_items == 0)
    {
        free(items);
        ProArrayFree((ProArray*)&insts);
        return PRO_TK_E_NOT_FOUND;
    }
    HLog("HRenumber: total=%d\n", n_items);

    /* 按原编号升序排序（冒泡） */
    for (i = 0; i < n_items - 1; i++)
        for (j = 0; j < n_items - 1 - i; j++)
            if (items[j].orig > items[j + 1].orig)
            {
                HRenumItem t = items[j];
                items[j] = items[j + 1];
                items[j + 1] = t;
            }

    /* 赋新编号 1..n，逐个 Erase→Modify→Show */
    for (i = 0; i < n_items; i++)
    {
        ProDtlsyminstdata sdata = NULL;
        ProDtlvartext *vts = NULL;
        ProDtlvartext *new_list = NULL;
        int  nv = 0, k;
        ProLine prompt, value;
        char buf[32];

        if (ProDtlsyminstDataGet(&items[i].syminst, PRODISPMODE_NUMERIC, &sdata) != PRO_TK_NO_ERROR || sdata == NULL)
            continue;

        ProArrayAlloc(0, sizeof(ProDtlvartext), 1, (ProArray*)&new_list);
        if (ProDtlsyminstdataVartextsCollect(sdata, &vts) == PRO_TK_NO_ERROR && vts)
        {
            ProArraySizeGet(vts, &nv);
            for (k = 0; k < nv; k++)
            {
                ProDtlvartext newvt = NULL;
                ProDtlvartextDataGet(vts[k], prompt, value);
                if (_wtoi(value) > 0)
                {
                    sprintf(buf, "%d", i + 1);
                    ProStringToWstring(value, buf);
                }
                ProDtlvartextAlloc(prompt, value, &newvt);
                if (newvt)
                    ProArrayObjectAdd((ProArray*)&new_list, PRO_VALUE_UNUSED, 1, &newvt);
            }
            for (k = 0; k < nv; k++)
                ProDtlvartextFree(vts[k]);
            ProArrayFree((ProArray*)&vts);
        }
        /* Set 接管 vartext 元素所有权，只 Free 外壳 */
        ProDtlsyminstdataVartextsSet(sdata, new_list);
        ProArrayFree((ProArray*)&new_list);

        ProDtlsyminstErase(&items[i].syminst);
        ProDtlsyminstModify(&items[i].syminst, sdata);
        ProDtlsyminstShow(&items[i].syminst);
    }

    free(items);
    ProArrayFree((ProArray*)&insts);
    ProWindowRepaint(PRO_VALUE_UNUSED);
    return PRO_TK_NO_ERROR;
}

static int HRenumberAct(uiCmdCmdId command, uiCmdValue *p_value, void *p_push_command_data)
{
    ProError status;
    ProMdl   drawing;

    status = HGetDrawing(&drawing);
    if (status != PRO_TK_NO_ERROR)
    {
        HMsg("BALLOON_NO_DRAWING");
        return 0;
    }
    if (HRenumberAll(drawing) == PRO_TK_E_NOT_FOUND)
    {
        HMsg("BALLOON_NO_BALLOON");
        return 0;
    }
    HMsg("BALLOON_RENUM_OK");
    return 0;
}

/*---------------------------------------------------------------------*\
  命令3：删除单个球标，删除后自动重排
\*---------------------------------------------------------------------*/
static int HDeleteAct(uiCmdCmdId command, uiCmdValue *p_value, void *p_push_command_data)
{
    ProError       status;
    ProMdl         drawing;
    ProSelection  *sel = NULL;
    int            n_sels = 0;
    ProDtlsyminst  syminst;

    status = HGetDrawing(&drawing);
    if (status != PRO_TK_NO_ERROR)
    {
        HMsg("BALLOON_NO_DRAWING");
        return 0;
    }

    /* 过滤类型：dtl_symbol。
       ProSelect 的一切非 PRO_TK_NO_ERROR 返回值都必须“立刻返回、把控制权
       交还 Creo”。文档要求：遇到 PRO_TK_PICK_ABOVE(-14) 必须马上返回；
       用户取消返回 PRO_TK_USER_ABORT。否则命令会在 Creo 的“选择会话”
       还没收尾时继续调用 Erase/Delete/重排/重绘，把状态栏的“选择过滤器”
       留在异常状态（过滤器空白、什么都选不中、Esc 也退不出来）。 */
    sel    = NULL;
    n_sels = 0;
    status = ProSelect((char*)"dtl_symbol", 1, NULL, NULL, NULL, NULL, &sel, &n_sels);
    HLog("HDeleteAct: ProSelect status=%d n_sels=%d\n", (int)status, n_sels);
    if (status != PRO_TK_NO_ERROR)
        return 0;
    if (n_sels < 1 || sel == NULL)
        return 0;

    /* ProSelect 返回的 selection 由 Creo 在命令作用域自动管理，不要手动 Free */
    if (ProSelectionModelitemGet(sel[0], (ProModelitem*)&syminst) != PRO_TK_NO_ERROR)
        return 0;

    status = ProDtlsyminstErase(&syminst);
    HLog("HDeleteAct: Erase -> %d\n", (int)status);
    status = ProDtlsyminstDelete(&syminst);
    HLog("HDeleteAct: Delete -> %d\n", (int)status);

    if (status == PRO_TK_NO_ERROR)
    {
        HMsg("BALLOON_DELETE_OK");
        ProWindowRepaint(PRO_VALUE_UNUSED);
        /* 删除后自动重排：删了 1，原 2 自动变 1 */
        if (HRenumberAll(drawing) != PRO_TK_E_NOT_FOUND)
            HMsg("BALLOON_RENUM_OK");
    }
    return 0;
}

/*---------------------------------------------------------------------*\
  命令5：一键导出 —— 把当前工程图关联的 3D 模型导成 STEP、
          工程图本身导成 PDF 和 DWG，三个文件统一用用户选定的名字，
          存放位置也由用户在 Creo 原生的"另存为"浏览器里选。

  【为什么全程用宽字符 + Win32 的 W 版 API】
    · 实测：Creo 的 DWG 导出器吃不下中文文件名。所以插件**从不把中文名
      交给 Creo** —— 先用固定 ASCII 临时名导到工作目录，再用 MoveFileExW
      搬到用户选的位置并改成用户要的名字。
    · 同理 GetFileAttributesA / MoveFileA 这类窄字符 API 在中文路径下会把
      编码搞错，一律改用 GetFileAttributesW / MoveFileExW，和
      ProPath / ProMdlFileName（都是 wchar_t）天然对齐。
    · ProStringToWstring / ProWstringToString（ProUtil.h:468/495）官方注明
      "accepts only strings in the active encoding of the TOOLKIT
      application"、"does not change the encoding" —— 编码语义**依赖运行时
      环境**，不能拿它当 UTF-8 转换器使。所以窄字符只留给日志。

  接口依据（全部在 D:\Creo6Toolkit 头文件里查证过，2026-10-14）：
    · ProFileMdlnameSave(dialog_label, filter_string, shortcut_path_arr,
                         shortcut_name_arr, default_path, pre_sel_file_name,
                         r_selected_file)                   —— ProUtil.h:1666
        "Prompts the standard file browser interface of Creo Parametric"
        —— 就是「文件 > 另存为」那个浏览器：能选目录、能输文件名，
        返回的 r_selected_file 是完整路径（ProPath）；
        PRO_TK_USER_ABORT = 用户取消。（DEPRECATED 的 ProFileSave 是前身。）
    · ProOutputFileMdlnameWrite(ProMdl, ProMdlFileName, ProImportExportFile,
                                arg1..arg4)                 —— ProUtil.h:1114
        第一个参数是 ProMdl（**不是** ProSolid），所以【工程图可以直接传】；
        PRO_STEP_FILE = 22、PRO_DWG_FILE = 34（ProUtil.h:106 / 119）。
        name 参数**要带扩展名**（doc:1140 —— 只有 IGES/DXF 才不写、自动加）；
        只给文件名时文件落在 Creo 当前工作目录。
        官方用法两处：
          PTWCServerUpdateDXF.c:45  对 DXF —— 后四个参数全 NULL；
          TestInterface.c:410       对 STEP —— arg2 保持 NULL。
        本插件先按头文件:1193（arg2 = int* 指向"数量"）传 &1，
        没落地再按官方示例传 NULL 重试。
    · ProPDFExport(ProMdl, ProPath, ProPDFOptions)            —— ProPDF.h:242
        output_file 要求**完整路径**（含扩展名）；
        model 必须已加载且显示在窗口里 —— 我们的工程图正好满足。
    · ProDrawingSolidsCollect(ProDrawing, ProSolid**)         —— ProDrawing.h:1571
        取工程图里"用户显式添加过"的模型；程序集只返回根程序集。
    · ProMessageStringRead(int max, wchar_t *buffer)          —— ProMessage.h:291
        读一行键盘输入；**必须先 ProMessageDisplay 提示**；
        用户按 Esc -> PRO_TK_MSG_USER_QUIT，直接回车 -> PRO_TK_GENERAL_ERROR。
\*---------------------------------------------------------------------*/
static uiCmdAccessState HExportAccess(uiCmdAccessMode access_mode)
{
    return ACCESS_AVAILABLE;
}

/* 拼 <dir>\<base><ext>。成功返回 0，放不下返回 -1。
   dir 已经以分隔符结尾时不再补一个 —— ProDirectoryCurrentGet 返回的工作
   目录就带尾部反斜杠，直接拼会得到 "…\CREO12\\H_export_tmp.stp"（Windows
   虽然容错，但日志脏、个别 API 也挑剔）。 */
static int HPathJoinW(wchar_t *out, int cap, const wchar_t *dir,
                      const wchar_t *base, const wchar_t *ext)
{
    size_t dl  = wcslen(dir);
    int    sep = (dl > 0 && (dir[dl - 1] == L'\\' || dir[dl - 1] == L'/'))
                     ? 0 : 1;

    if ((int)(dl + (size_t)sep + wcslen(base) + wcslen(ext) + 1) > cap)
        return -1;

    if (sep)
        swprintf(out, (size_t)cap, L"%ls\\%ls%ls", dir, base, ext);
    else
        swprintf(out, (size_t)cap, L"%ls%ls%ls", dir, base, ext);
    return 0;
}

/* 宽字符版存在性检查（窄字符 API 在中文路径下不可靠） */
static int HFileExistsW(const wchar_t *path)
{
    DWORD attr = GetFileAttributesW(path);

    return (attr != INVALID_FILE_ATTRIBUTES &&
            !(attr & FILE_ATTRIBUTE_DIRECTORY)) ? 1 : 0;
}

/* 宽 -> 窄，只用于写日志（h_load.log 是窄字符文件）。
   用 Creo 自己的转换函数跟随运行时编码，比 printf("%ls") 靠谱。 */
static void HLogW(const char *tag, const wchar_t *w)
{
    char buf[PRO_PATH_SIZE * 4];

    if (w == NULL)
    {
        HLog("%s(null)\n", tag);
        return;
    }
    ProWstringToString(buf, (wchar_t *)w);
    HLog("%s%s\n", tag, buf);
}

/* 把 path 拆成「目录」+「叶子名（已去扩展名、已清非法字符）」。
   fallback_dir 供"只有文件名、没有目录"的情况使用。
   返回 0 成功，-1 失败（空名 / 放不下）。 */
static int HSplitPathW(const wchar_t *path, const wchar_t *fallback_dir,
                       wchar_t *dir, int dir_cap, wchar_t *leaf, int leaf_cap)
{
    const wchar_t *s, *last = NULL, *name, *dot = NULL;
    size_t         n;
    int            k;

    if (path == NULL || path[0] == L'\0')
        return -1;

    for (s = path; *s != L'\0'; s++)
        if (*s == L'\\' || *s == L'/')
            last = s;

    if (last != NULL)
    {
        n = (size_t)(last - path);
        if (n == 0 || n >= (size_t)dir_cap)
            return -1;
        wmemcpy(dir, path, n);
        dir[n] = L'\0';
        name = last + 1;
    }
    else
    {
        if (wcslen(fallback_dir) >= (size_t)dir_cap)
            return -1;
        wcscpy(dir, fallback_dir);
        name = path;
    }

    if (name[0] == L'\0' || wcslen(name) >= (size_t)leaf_cap)
        return -1;

    for (s = name; *s != L'\0'; s++)
        if (*s == L'.')
            dot = s;            /* 取最后一个点：用户多打的扩展名一律去掉 */
    n = (dot != NULL && dot != name) ? (size_t)(dot - name) : wcslen(name);
    wmemcpy(leaf, name, n);
    leaf[n] = L'\0';

    for (k = 0; leaf[k] != L'\0'; k++)
        if (leaf[k] == L'\\' || leaf[k] == L'/' || leaf[k] == L':' ||
            leaf[k] == L'*'  || leaf[k] == L'?' || leaf[k] == L'"' ||
            leaf[k] == L'<'  || leaf[k] == L'>' || leaf[k] == L'|')
            leaf[k] = L'_';

    return (leaf[0] == L'\0') ? -1 : 0;
}

/* 把刚导出的临时文件搬到用户要的位置并改成目标名，同时删掉源文件。
   Creo 有时写成 <tmp><ext><ext>、有时完全不补扩展名 —— 三种都试。
   返回 1 = 目标文件已就位。

   ★★ 跨盘是必须支持的（2026-10-14 用户实测踩到）★★
     临时文件一定在 Creo 工作目录（比如 D:\…\CREO12\），而用户完全可能把
     导出位置选到另一个盘（比如 C:\Users\H\Desktop）。MoveFileExW 在不带
     MOVEFILE_COPY_ALLOWED 时底层就是 rename 语义，跨卷会直接失败并返回
     ERROR_NOT_SAME_DEVICE(17) —— 日志里那串 "MoveFileExW failed (err=17)"
     就是这个。第一次导出到工作目录所在的 D 盘所以成功，第二次选桌面
     （C 盘）就三个文件全灭，看起来像"第一次能、第二次不能"的怪现象。
     修法两层：
       ① 带上 MOVEFILE_COPY_ALLOWED（官方语义：跨卷时自动退化成
          CopyFile + DeleteFile）；
       ② 万一某环境仍不认，自己再来一遍 CopyFileW + DeleteFileW。 */
static int HMoveExported(const wchar_t *src_dir, const wchar_t *tmp_base,
                         const wchar_t *dst_dir, const wchar_t *dst_base,
                         const wchar_t *ext)
{
    wchar_t src[PRO_PATH_SIZE * 2], dst[PRO_PATH_SIZE * 2];
    wchar_t ext2[PRO_MDLEXTENSION_SIZE * 2];
    int     i;

    if (HPathJoinW(dst, (int)(sizeof(dst) / sizeof(dst[0])),
                   dst_dir, dst_base, ext) != 0)
    {
        HLog("HExport: destination path too long, skipped\n");
        return 0;
    }

    /* 注意：这里**不能**因为"目标已存在"就直接返回成功 —— 那个旧文件可能
       是上一次导出的成果，跟本轮导出成不成功无关，会把失败伪装成成功。
       目标已存在交给 MOVEFILE_REPLACE_EXISTING / CopyFileW(FALSE) 覆盖。 */

    swprintf(ext2, sizeof(ext2) / sizeof(ext2[0]), L"%ls%ls", ext, ext);

    for (i = 0; i < 3; i++)
    {
        const wchar_t *e = (i == 0) ? ext : ((i == 1) ? ext2 : L"");
        DWORD          err_move;

        if (HPathJoinW(src, (int)(sizeof(src) / sizeof(src[0])),
                       src_dir, tmp_base, e) != 0)
            continue;
        if (!HFileExistsW(src))
            continue;

        /* ① 同卷走 rename、跨卷自动退化成复制+删除（靠 COPY_ALLOWED） */
        if (MoveFileExW(src, dst, MOVEFILE_REPLACE_EXISTING |
                                    MOVEFILE_COPY_ALLOWED))
        {
            HLogW("HExport: moved -> ", dst);
            return 1;
        }
        err_move = GetLastError();

        /* ② 兜底：自己跨卷复制，成功后删源 */
        if (CopyFileW(src, dst, FALSE))
        {
            if (!DeleteFileW(src))
                HLog("HExport: copied but source left behind (err=%lu)\n",
                     (unsigned long)GetLastError());
            HLogW("HExport: copied (cross-volume) -> ", dst);
            return 1;
        }

        HLog("HExport: move failed (err=%lu), copy failed (err=%lu)\n",
             (unsigned long)err_move, (unsigned long)GetLastError());
        HLogW("  from: ", src);
        HLogW("  to  : ", dst);
    }

    HLogW("HExport: MISSING source for ", dst);
    return 0;
}

/* 删掉工作目录里上一轮可能残留的临时文件。
   同样三种扩展名写法都试 —— 否则 Creo 写成 <tmp>.stp.stp 时会留个旧文件，
   下一轮 HMoveExported 可能把它当成"刚导出的"搬给用户（内容其实是旧的）。 */
static void HRemoveTemp(const wchar_t *dir, const wchar_t *base,
                        const wchar_t *ext)
{
    wchar_t path[PRO_PATH_SIZE * 2];
    wchar_t ext2[PRO_MDLEXTENSION_SIZE * 2];
    int     i;

    swprintf(ext2, sizeof(ext2) / sizeof(ext2[0]), L"%ls%ls", ext, ext);

    for (i = 0; i < 3; i++)
    {
        const wchar_t *e = (i == 0) ? ext : ((i == 1) ? ext2 : L"");

        if (HPathJoinW(path, (int)(sizeof(path) / sizeof(path[0])),
                       dir, base, e) == 0 && HFileExistsW(path))
            DeleteFileW(path);
    }
}

static int HExportAct(uiCmdCmdId command, uiCmdValue *p_value, void *p_push_command_data)
{
    static const wchar_t TMPBASE[] = L"H_export_tmp";   /* 给 Creo 的 ASCII 名 */
    static const wchar_t *const EXTS[3] = { L".stp", L".dwg", L".pdf" };

    ProError  status;
    ProMdl    drawing;
    ProSolid *solids = NULL;
    ProPath   wcwd, wsel, wpdf;
    wchar_t   ddir[PRO_PATH_SIZE];              /* 目标目录（用户选的） */
    wchar_t   dbase[PRO_FILE_MDLNAME_SIZE];     /* 目标文件名（不含扩展名） */
    int       n_solids = 0, narg = 1;
    int       step_ok = 0, pdf_ok = 0, dwg_ok = 0, no_license = 0;
    int       i;

    status = HGetDrawing(&drawing);
    if (status != PRO_TK_NO_ERROR)
    {
        HMsg("BALLOON_NO_DRAWING");
        return 0;
    }

    /* ① Creo 当前工作目录 —— 所有导出先落这里，随后再搬到用户选的位置 */
    if (ProDirectoryCurrentGet(wcwd) != PRO_TK_NO_ERROR)
    {
        HLog("HExportAct: cannot get cwd\n");
        HMsg("EXPORT_FAILED");
        return 0;
    }
    HLogW("HExportAct: cwd    = ", wcwd);

    /* ② 让用户选目录 + 输名字：Creo 原生的"另存为"浏览器 */
    wsel[0] = L'\0';
    status = ProFileMdlnameSave(L"选择导出位置与文件名",
                                L"*.stp,*.dwg,*.pdf", NULL, NULL,
                                wcwd, NULL, wsel);
    HLog("HExportAct: ProFileMdlnameSave -> %d\n", (int)status);
    HLogW("HExportAct: selected= ", wsel);

    if (status == PRO_TK_USER_ABORT)
    {
        HLog("HExportAct: user cancelled the file browser\n");
        return 0;                                   /* 取消：安静退出 */
    }
    if (status != PRO_TK_NO_ERROR)
    {
        /* 浏览器不可用（不同版本/环境可能返回别的码）—— 退化成只问名字、
           落到 Creo 当前工作目录，功能不至于整个不能用。
           顺带支持在名字里直接写完整路径。 */
        ProMdlFileName wname;

        HLog("HExportAct: browser failed -> fallback to name prompt\n");
        HMsg("EXPORT_ASK_NAME");
        ProWindowRepaint(PRO_VALUE_UNUSED);
        wname[0] = L'\0';
        status = ProMessageStringRead(100, wname);
        ProMessageClear();
        if (status != PRO_TK_NO_ERROR)
        {
            HLog("HExportAct: name input cancelled (status=%d)\n", (int)status);
            return 0;
        }
        if (HSplitPathW(wname, wcwd, ddir, PRO_PATH_SIZE,
                        dbase, PRO_FILE_MDLNAME_SIZE) != 0)
        {
            HLog("HExportAct: bad name -> abort\n");
            HMsg("EXPORT_FAILED");
            return 0;
        }
    }
    else if (HSplitPathW(wsel, wcwd, ddir, PRO_PATH_SIZE,
                         dbase, PRO_FILE_MDLNAME_SIZE) != 0)
    {
        HLog("HExportAct: cannot split the selected path -> abort\n");
        HMsg("EXPORT_FAILED");
        return 0;
    }

    HLogW("HExportAct: dir    = ", ddir);
    HLogW("HExportAct: name   = ", dbase);

    /* ③ 清掉上一轮可能残留的临时文件，免得把旧的搬给用户 */
    for (i = 0; i < 3; i++)
        HRemoveTemp(wcwd, TMPBASE, EXTS[i]);

    /* ④ STEP：工程图关联的 3D 模型（程序集只会返回根程序集） */
    if (ProDrawingSolidsCollect((ProDrawing)drawing, &solids) == PRO_TK_NO_ERROR
            && solids != NULL)
        ProArraySizeGet((ProArray)solids, &n_solids);
    HLog("HExportAct: drawing models=%d\n", n_solids);

    if (n_solids <= 0)
    {
        HMsg("EXPORT_NO_MODEL");
        if (solids) ProArrayFree((ProArray*)&solids);
    }
    else
    {
        ProMdlFileName wtmp2;

        swprintf(wtmp2, PRO_FILE_MDLNAME_SIZE, L"%ls.stp", TMPBASE);
        status = ProOutputFileMdlnameWrite((ProMdl)solids[0], wtmp2,
                                           PRO_STEP_FILE, NULL, &narg, NULL, NULL);
        HLog("HExportAct: STEP -> %d\n", (int)status);
        if (status == PRO_TK_NO_LICENSE)
            no_license = 1;
        step_ok = HMoveExported(wcwd, TMPBASE, ddir, dbase, L".stp");

        /* arg2 的传法官方自己有两套说法，两种都试：
           · 头文件 ProUtil.h:1193 —— PRO_STEP_FILE 的 arg2 是 int* 指向"数量"；
           · 官方示例 TestInterface.c:410 —— switch(export_type) 里
             **根本没有 PRO_STEP_FILE 分支**，p_arg2 保持 NULL。
           第一次没落地就按官方示例再试一次 NULL，不猜。 */
        if (!step_ok)
        {
            ProMdlFileName wtmp3;

            swprintf(wtmp3, PRO_FILE_MDLNAME_SIZE, L"%ls.stp", TMPBASE);
            status = ProOutputFileMdlnameWrite((ProMdl)solids[0], wtmp3,
                                               PRO_STEP_FILE, NULL, NULL, NULL, NULL);
            HLog("HExportAct: STEP(retry arg2=NULL) -> %d\n", (int)status);
            if (status == PRO_TK_NO_LICENSE)
                no_license = 1;
            step_ok = HMoveExported(wcwd, TMPBASE, ddir, dbase, L".stp");
        }
        ProArrayFree((ProArray*)&solids);
    }

    /* ⑤ DWG：工程图本身。Creo 只看见 ASCII 临时名，中文名由我们搬过去 */
    {
        ProMdlFileName wtmp2;

        swprintf(wtmp2, PRO_FILE_MDLNAME_SIZE, L"%ls.dwg", TMPBASE);
        status = ProOutputFileMdlnameWrite(drawing, wtmp2, PRO_DWG_FILE,
                                           NULL, NULL, NULL, NULL);
        HLog("HExportAct: DWG -> %d\n", (int)status);
        if (status == PRO_TK_NO_LICENSE)
            no_license = 1;
        dwg_ok = HMoveExported(wcwd, TMPBASE, ddir, dbase, L".dwg");
    }

    /* ⑥ PDF：工程图本身（output_file 要完整路径）
          固定 600 dpi + 单色；顺手关掉"导出后自动弹阅读器"。
          选项依据（ProPDF.h）：
            PRO_PDFOPT_RASTER_DPI    :157  Int, 范围 100-600, 默认 300
            PRO_PDFOPT_COLOR_DEPTH   :152  Enum ProPDFColorDepth, 默认 _COLOR
                                           枚举值 :43  _COLOR / _GRAY / _MONO
            PRO_PDFOPT_LAUNCH_VIEWER :158  Boolean, 默认 TRUE
          枚举类选项也走 ProPDFoptionsIntpropertySet（:320 注释
          "Sets the value for an integer or enum property"）。 */
    if (HPathJoinW(wpdf, PRO_PATH_SIZE, wcwd, TMPBASE, L".pdf") != 0)
    {
        HLog("HExportAct: pdf path too long\n");
    }
    else
    {
        ProPDFOptions opts = NULL;

        if (ProPDFoptionsAlloc(&opts) == PRO_TK_NO_ERROR && opts != NULL)
        {
            HLog("HExportAct: PDF dpi -> %d\n",
                 (int)ProPDFoptionsIntpropertySet(opts, PRO_PDFOPT_RASTER_DPI, 600));
            HLog("HExportAct: PDF mono -> %d\n",
                 (int)ProPDFoptionsIntpropertySet(opts, PRO_PDFOPT_COLOR_DEPTH,
                                                  PRO_PDF_CD_MONO));
            HLog("HExportAct: PDF no-viewer -> %d\n",
                 (int)ProPDFoptionsBoolpropertySet(opts, PRO_PDFOPT_LAUNCH_VIEWER,
                                                   PRO_B_FALSE));
        }
        else
        {
            opts = NULL;                /* 拿不到就用默认配置导 */
        }
        status = ProPDFExport(drawing, wpdf, opts);
        HLog("HExportAct: PDF -> %d\n", (int)status);
        if (opts != NULL)
            ProPDFoptionsFree(opts);
        if (status == PRO_TK_NO_LICENSE)
            no_license = 1;
        pdf_ok = HMoveExported(wcwd, TMPBASE, ddir, dbase, L".pdf");
    }

    HLog("HExportAct: result step=%d pdf=%d dwg=%d license_missing=%d\n",
         step_ok, pdf_ok, dwg_ok, no_license);

    if (step_ok && pdf_ok && dwg_ok)
        HMsg("EXPORT_OK");
    else if (no_license)
        HMsg("EXPORT_NO_LICENSE");
    else
        HMsg("EXPORT_PARTIAL");

    ProWindowRepaint(PRO_VALUE_UNUSED);
    return 0;
}


/*---------------------------------------------------------------------*\
  入口
\*---------------------------------------------------------------------*/
/*---------------------------------------------------------------------\
  命令：视图动态调整
  交互：点按钮 -> Creo 原生「选择」对话框选中一个普通视图（悬停高亮，
        点「确定」）-> 选中后【直接移动鼠标】即全方位旋转：
            鼠标上下位移 -> 绕图纸水平轴转（俯仰 = X 角）
            鼠标左右位移 -> 绕图纸竖直轴转（偏摆 = Y 角）
        -> 中键 = 确定完成（保留当前朝向）
        -> Esc  = 取消（把已下发的每一步逆序取负角重放，精确恢复原朝向）
  拾取机制（2026-10-15）：ProSelect("dwg_view", -1, ..., sel_env, ...)
        + PRO_SELECT_DONE_REQUIRED 选择环境 = 与 HelloCreo 同一套原生交互。
  ★ 旋转 API 定案（2026-10-15，OTK 官方用户指南 user_guide/View_Orientation.html）：
        ProDrawingViewOrientationFromAngleSet(dwg, view, rot_ref_name, sel, angle)
        = 绕【过视图原点、相对图纸固定】的轴旋转，不需要任何基准视图
          => 天然无跳变、可连续累积、可逆。
            rot_ref_name 合法值（ProDrawingView.h:200 源码注释原文）：
                "Normal"      图纸法向轴 = 平面内自转（in-plane）
                "Horizontal"  图纸水平轴 = 俯仰（X 角）  ★ 本命令主用
                "Vertical"    图纸竖直轴 = 偏摆（Y 角）  ★ 本命令主用
                "Edge/Axis"   绕选中的边或轴（sel 非 NULL）
            sel 传 NULL 即可（非 Edge/Axis 类型忽略该参数）。
        · 为什么必须弃用 ProDrawingViewOrientationFromNameSet：
          OTK 指南原文「orients the specified drawing view using saved views
          from the model」=> mdl_view_name 必须是【模型里已保存的视图】，
          第一次调用就会把视图强行跳到该保存视图的方向（跳变），而且根本
          无法表达「保持当前方向」。第十版实测日志 probe mdl="11" -> 0，
          即视图当场被拉到模型保存视图 "11" 上 => 用户判定失败。
        · ProViewRotate / ProViewMatrixSet 对「绘图 + 绘图视图」实测均返回
          -1，不可用（14:13 / 14:17 实测，勿再试）。
  每次改完 ProDrawingViewRegenerate + ProWindowRepaint。
  按键状态：GetAsyncKeyState（ProMouseTrack 的 button_pressed 是事件
        语义，按住期间再读恒为 0，不能用于按住/松开判定）。
        本命令不需要按键：视图选中后移动鼠标即旋转。
\*---------------------------------------------------------------------*/
static uiCmdAccessState HDynaViewAccess(uiCmdAccessMode access_mode)
{
    (void)access_mode;
    return ACCESS_AVAILABLE;
}

/* 灵敏度（度/像素）：鼠标位移 -> 增量角。0.5 = 拖 200px 转 100°（拖满半屏约转 200°），
   跟手偏快、适合大角度翻转；历史上 0.2（细腻）→ 0.35（轻快）→ 0.5（当前，用户指定）。
   嫌慢/嫌快只改这一个值。 */
#define HDYNAVIEW_3D_SENS 0.5
/* 两个轴的符号：实测哪个方向与鼠标相反，就把对应值改成 -1.0，不必动别处。
   SIGN_X = -1.0：水平轴（俯仰）反向 —— 用户 2026-10-15 实测"上下方向反过来"。 */
#define HDYNAVIEW_SIGN_X  (-1.0)     /* 水平轴 -> X 角（鼠标上下） */
#define HDYNAVIEW_SIGN_Y  (+1.0)     /* 竖直轴 -> Y 角（鼠标左右） */
/* 节流：增量角先攒着，够 HDYNAVIEW_MIN_STEP 度才下发一次（少刷屏、跟手）。 */
#define HDYNAVIEW_MIN_STEP 0.5
/* Esc 逆序回滚栈上限（每下发一步记一条；超出则只提示，放弃整段回滚）。 */
#define HDYNAVIEW_UNDO_MAX 8192
/* 连续失败多少次就认定 API 不可用、直接退出 */
#define HDYNAVIEW_FAIL_LIMIT 8
/* ★ FromAngleSet 的角度语义开关（唯一需要改的地方）：
      0 = 【相对/增量】语义（默认，官方指南「around an axis」+ 对话框上是
          「旋转」按钮，都指向"按这个角转一下"）= 每次下发本帧位移增量。
      1 = 【绝对/累计】语义（万一实测"只微微晃、转不起来"就是它）= 每次
          下发累计角，Esc 直接把两轴归零。
   实测若与预期不符，只把下面的 0 改成 1 重编即可，不必动其它代码。 */
#define HDYNAVIEW_ABSOLUTE 0

#define HDYNAVIEW_AX_X 1
#define HDYNAVIEW_AX_Y 2

/* 一条已下发的旋转（Esc 回滚用；绝对语义下只用来判断"是否动过"） */
typedef struct
{
    int    axis;      /* HDYNAVIEW_AX_X / HDYNAVIEW_AX_Y */
    double angle;     /* 已下发的角（度）；相对语义回滚时取负 */
} HDynaViewStep;

/* 下发一次旋转：axis 决定绕哪根图纸轴，angle 为角度（度）。
   rot_ref_name 是 ProName（= wchar_t[PRO_NAME_SIZE]，ProSizeConst.h:63），
   所以必须用宽字面量，不能传窄串。 */
static ProError HDynaViewRotate(ProDrawing drawing, ProView view, int axis, double angle)
{
    ProName ref;

    if (axis == HDYNAVIEW_AX_X)       wcscpy(ref, L"Horizontal");
    else if (axis == HDYNAVIEW_AX_Y)  wcscpy(ref, L"Vertical");
    else                              return (PRO_TK_BAD_INPUTS);

    return ProDrawingViewOrientationFromAngleSet(drawing, view, ref, NULL, angle);
}

static int HDynaViewAct(uiCmdCmdId command, uiCmdValue *p_value, void *p_push_command_data)
{
    ProError              status;
    ProMdl                drawing;
    ProSelection         *sels = NULL;
    int                   n_sels = 0;
    ProSelectionEnvOption env_opts[3];
    ProSelectionEnv       sel_env = NULL;
    ProView               view = NULL;
    ProViewType           vtype;
    ProView              *children = NULL;
    int                   n_children = 0;
    ProMouseButton        btn;
    ProPoint3d            cur;
    double                last_x = 0.0, last_y = 0.0; /* 上帧鼠标位置（算位移增量） */
    int                   have_last = 0;              /* 是否已建立位移基准 */
    double                pend_x = 0.0, pend_y = 0.0; /* 待下发的 X/Y 增量角（节流用） */
    double                tot_x = 0.0, tot_y = 0.0;   /* 累计 X/Y 角（绝对语义/日志用） */
    double                ax_x = 0.0, ax_y = 0.0;     /* 本次实际下发的角 */
    int                   do_x = 0, do_y = 0;         /* 本次是否下发该轴 */
    HDynaViewStep        *steps = NULL;               /* 已下发步序列（Esc 回滚用） */
    int                   n_steps = 0;
    int                   n_ok = 0, n_fail = 0;
    int                   k;

    (void)command; (void)p_value; (void)p_push_command_data;

    status = HGetDrawing(&drawing);
    if (status != PRO_TK_NO_ERROR) { HMsg("BALLOON_NO_DRAWING"); return 0; }

    /* 原生选择环境：DONE_REQUIRED = 弹出「选择/确定/取消」对话框（与 HelloCreo 一致），
       允许右键查询选择，禁止框选（视图要点选）。官方示例同款写法（TestSelect.c:670）。 */
    env_opts[0].attribute = PRO_SELECT_DONE_REQUIRED;    env_opts[0].value = 0;
    env_opts[1].attribute = PRO_SELECT_BY_MENU_ALLOWED;  env_opts[1].value = 1;
    env_opts[2].attribute = PRO_SELECT_BY_BOX_ALLOWED;   env_opts[2].value = 0;
    status = ProSelectionEnvAlloc(env_opts, 3, &sel_env);
    HLog("HDynaViewAct: SelectionEnvAlloc -> %d\n", (int)status);

    /* ============ 阶段 1：原生选择会话（悬停高亮 + 确定对话框） ============ */
    for (;;)
    {
        HMsg("DYNAVIEW_PICK");
        ProWindowRepaint(PRO_VALUE_UNUSED);

        status = ProSelect((char*)"dwg_view", -1, NULL, NULL, sel_env, NULL, &sels, &n_sels);
        HLog("HDynaViewAct: ProSelect -> %d n=%d sels=%p\n",
             (int)status, n_sels, (void*)sels);
        if (status != PRO_TK_NO_ERROR || n_sels < 1 || sels == NULL)
        {
            HLog("HDynaViewAct: selection ended without a pick\n");
            if (sel_env) ProSelectionEnvFree(sel_env);
            return 0;
        }
        HLog("HDynaViewAct: picked %d item(s)\n", n_sels);

        status = ProSelectionViewGet(sels[0], &view);
        if (status != PRO_TK_NO_ERROR || view == NULL)
        {
            HLog("HDynaViewAct: SelectionViewGet -> %d\n", (int)status);
            HMsg("DYNAVIEW_NO_VIEW");
            continue;                        /* 提示后重新选 */
        }

        /* 校验：必须是普通视图（派生视图的朝向不是自由定义的） */
        status = ProDrawingViewTypeGet((ProDrawing)drawing, view, &vtype);
        if (status != PRO_TK_NO_ERROR || vtype != PRO_VIEW_GENERAL)
        {
            HLog("HDynaViewAct: view type=%d (not general)\n", (int)vtype);
            HMsg("DYNAVIEW_NOT_GENERAL");
            continue;
        }

        /* 校验：不能有关联子视图（HelloCreo：有关联视图不允许动态调整） */
        children = NULL; n_children = 0;
        status = ProDrawingViewChildrenGet((ProDrawing)drawing, view, &children, &n_children);
        if (status == PRO_TK_NO_ERROR && n_children > 0)
        {
            HLog("HDynaViewAct: view has %d children -> reject\n", n_children);
            if (children) ProArrayFree((ProArray*)&children);
            HMsg("DYNAVIEW_HAS_CHILDREN");
            continue;
        }
        if (children) ProArrayFree((ProArray*)&children);
        break;                               /* 拿到合法视图 */
    }
    if (sel_env) { ProSelectionEnvFree(sel_env); sel_env = NULL; }

    HLog("HDynaViewAct: view ready (general, no children), angle mode=%s\n",
         HDYNAVIEW_ABSOLUTE ? "absolute" : "relative");

    /* ============ 阶段 2：交互主循环（选中即拖，相对旋转） ============
         - 移动鼠标 = 旋转：上下位移 -> 绕图纸水平轴（X 角）
                            左右位移 -> 绕图纸竖直轴（Y 角）
         - 中键     = 确定完成（保留当前朝向）
         - Esc      = 取消：把已下发的每一步按【逆序、取负角】重放一遍，
                      精确回到原朝向（旋转轴固定在图纸上，旋转可逆）。
       注意：FromAngleSet 是【相对】旋转，所以进入本阶段【不做任何
       初始化调用】——视图原地不动，不存在跳变。 */
    steps = (HDynaViewStep*)malloc(sizeof(HDynaViewStep) * HDYNAVIEW_UNDO_MAX);
    HMsg("DYNAVIEW_DRAG");
    ProWindowRepaint(PRO_VALUE_UNUSED);

    for (;;)
    {
        status = ProMouseTrack(0, &btn, cur);
        if (status != PRO_TK_NO_ERROR) break;          /* Esc 或出错：走回滚 */

        if (btn & PRO_MIDDLE_BUTTON)                   /* 中键：确定完成 */
        {
            HLog("HDynaViewAct: done, steps=%d ok=%d fail=%d tot=(%.1f,%.1f)\n",
                 n_steps, n_ok, n_fail, tot_x, tot_y);
            if (steps) { free(steps); steps = NULL; }
            HMsg("DYNAVIEW_OK");
            return 0;
        }

        if (!have_last)                                /* 首帧：建立位移基准 */
        {
            last_x = cur[0]; last_y = cur[1];
            have_last = 1;
            Sleep(5);
            continue;
        }

        /* 本帧位移 -> 增量角（屏幕像素，Y 轴向下为正） */
        pend_x += (cur[1] - last_y) * HDYNAVIEW_3D_SENS * HDYNAVIEW_SIGN_X;
        pend_y += (cur[0] - last_x) * HDYNAVIEW_3D_SENS * HDYNAVIEW_SIGN_Y;
        last_x = cur[0]; last_y = cur[1];

        if (fabs(pend_x) < HDYNAVIEW_MIN_STEP && fabs(pend_y) < HDYNAVIEW_MIN_STEP)
        {
            Sleep(5);
            continue;
        }

        tot_x += pend_x;
        tot_y += pend_y;

#if HDYNAVIEW_ABSOLUTE
        /* 绝对语义：两轴每次都发累计角，视图才会持续跟着鼠标走 */
        do_x = 1;        do_y = 1;
        ax_x = tot_x;    ax_y = tot_y;
#else
        /* 相对语义：只发本帧增量（够阈值的那个轴） */
        do_x = (fabs(pend_x) >= HDYNAVIEW_MIN_STEP);
        do_y = (fabs(pend_y) >= HDYNAVIEW_MIN_STEP);
        ax_x = pend_x;   ax_y = pend_y;
#endif

        if (do_x)                                      /* 俯仰：绕图纸水平轴 */
        {
            status = HDynaViewRotate((ProDrawing)drawing, view, HDYNAVIEW_AX_X, ax_x);
            HLog("HDynaViewAct: rotX d=%.2f tot=%.1f -> %d\n", ax_x, tot_x, (int)status);
            if (status == PRO_TK_NO_ERROR)
            {
                n_ok++;
                if (steps != NULL && n_steps < HDYNAVIEW_UNDO_MAX)
                {
                    steps[n_steps].axis  = HDYNAVIEW_AX_X;
                    steps[n_steps].angle = ax_x;
                    n_steps++;
                }
            }
            else n_fail++;
            pend_x = 0.0;
        }

        if (do_y)                                      /* 偏摆：绕图纸竖直轴 */
        {
            status = HDynaViewRotate((ProDrawing)drawing, view, HDYNAVIEW_AX_Y, ax_y);
            HLog("HDynaViewAct: rotY d=%.2f tot=%.1f -> %d\n", ax_y, tot_y, (int)status);
            if (status == PRO_TK_NO_ERROR)
            {
                n_ok++;
                if (steps != NULL && n_steps < HDYNAVIEW_UNDO_MAX)
                {
                    steps[n_steps].axis  = HDYNAVIEW_AX_Y;
                    steps[n_steps].angle = ax_y;
                    n_steps++;
                }
            }
            else n_fail++;
            pend_y = 0.0;
        }

        /* 一次都没成功、又连续失败够多次 => API 不可用，别让用户干耗 */
        if (n_ok == 0 && n_fail >= HDYNAVIEW_FAIL_LIMIT)
        {
            HLog("HDynaViewAct: FromAngleSet unavailable (fail=%d) -> abort\n", n_fail);
            if (steps) { free(steps); steps = NULL; }
            HMsg("DYNAVIEW_FAILED");
            return 0;
        }

        ProDrawingViewRegenerate((ProDrawing)drawing, view);
        ProWindowRepaint(PRO_VALUE_UNUSED);
        Sleep(5);
    }

    /* ============ Esc / 跟踪出错：恢复原朝向 ============ */
    HLog("HDynaViewAct: rollback %d step(s), ok=%d fail=%d tot=(%.1f,%.1f)\n",
         n_steps, n_ok, n_fail, tot_x, tot_y);
#if HDYNAVIEW_ABSOLUTE
    /* 绝对语义：两轴角直接归零就是原始朝向 */
    if (n_steps > 0)
    {
        status = HDynaViewRotate((ProDrawing)drawing, view, HDYNAVIEW_AX_X, 0.0);
        HLog("HDynaViewAct: reset X -> %d\n", (int)status);
        status = HDynaViewRotate((ProDrawing)drawing, view, HDYNAVIEW_AX_Y, 0.0);
        HLog("HDynaViewAct: reset Y -> %d\n", (int)status);
    }
#else
    /* 相对语义：逆序取负角重放，精确抵消 */
    for (k = n_steps - 1; k >= 0; k--)
    {
        status = HDynaViewRotate((ProDrawing)drawing, view, steps[k].axis, -steps[k].angle);
        if (status != PRO_TK_NO_ERROR)
            HLog("HDynaViewAct: rollback step %d -> %d\n", k, (int)status);
    }
#endif
    if (n_steps > 0)
    {
        ProDrawingViewRegenerate((ProDrawing)drawing, view);
        ProWindowRepaint(PRO_VALUE_UNUSED);
    }
    if (steps) { free(steps); steps = NULL; }

    if (n_ok == 0 && n_fail > 0) { HMsg("DYNAVIEW_FAILED"); return 0; }
    HMsg("DYNAVIEW_CANCEL");
    return 0;
}



/*---------------------------------------------------------------------\
  命令：公差刷子
  交互：点按钮 -> 点选【源尺寸】（公差的来源）-> 之后每点一个尺寸就立刻把
        源尺寸的公差配置刷过去，可连续点任意多个 -> 中键 / Esc 结束。
  刷入内容（逐项"读一个写一个"，全部走官方 API）：
        ProDimensionToltypeGet  / Set   公差类型（公称/基本/极限/正负/对称/对称上标）
        ProDimensionToleranceGet/ Set   上/下公差值
        ProDimensionDecimalsGet / Set   公差小数位（界面上的「精度」）
        ProDimensionTollabelGet / Set   ISO/DIN 公差表（类型 + 表名 + 列号）
  证据链（均来自本机官方 SDK，非推测）：
    · 官方示例 protk_appls/pt_examples/pt_dbase/TestDimension.c:305/314 ——
      ProDimensionToleranceGet 读进变量 → ProDimensionToleranceSet 原样写回；
      :334 ToltypeSet、:340/:346 DecimalsGet/Set 同款。这就是"刷"的标准写法。
    · 选择过滤名 "dimension"（TestDimension.c:168）；选择 → 句柄用
      ProSelectionModelitemGet（TestDimension.c:168/1098）；ProDimension 就是
      ProModelitem 的 typedef 别名（ProObjects.h:419-429），PRO_DIMENSION=8
      （ProObjects.h:32）。
  ★ 为什么【不做】"极限值按名义尺寸换算"：OTK 官方用户指南
    user_guide/Dimension_Tolerances.html 说明，这些接口取回的是公差值本身
    （例子：上下公差 0.123456 时"显示值"取回 0.12），不是"名义值+公差"的绝对
    极限；所以逐项原样复制才对，任何按名义值换算都会算错。
  ★ 公差是否显示由 config 选项 tol_display 决定（API 只有
    ProDimensionIsToleranceDisplayed 可读、没有 Set），源尺寸若没开显示，
    刷完也看不见 —— 日志会把 displayed 打出来。
  ★ 参考尺寸/从动尺寸不能设公差，ToltypeGet 会返回 BAD_CONTEXT，据此拦掉。
\*---------------------------------------------------------------------*/
static uiCmdAccessState HTolBrushAccess(uiCmdAccessMode access_mode)
{
    (void)access_mode;
    return ACCESS_AVAILABLE;
}

/* 源尺寸的公差配置（刷子刷的就是这一整套） */
typedef struct
{
    ProDimToleranceType type;                 /* 公差类型 */
    double              upper;                /* 上公差（极限上限 / 正偏差） */
    double              lower;                /* 下公差（极限下限 / 负偏差） */
    int                 decimals;             /* 公差小数位 */
    int                 has_decimals;         /* DecimalsGet 是否成功 */
    ProToleranceTable   table_type;           /* ISO/DIN 公差表类型 */
    wchar_t             table[PRO_NAME_SIZE]; /* 公差表名（孔/轴表才用） */
    int                 column;               /* 公差表列号 */
    int                 has_label;            /* 是否真的有公差表标签 */
    ProBoolean          displayed;            /* 公差当前是否显示（只读） */
} HTolBrushSpec;

/* 从源尺寸读出整套公差配置 */
static ProError HTolBrushRead(ProDimension *dim, HTolBrushSpec *spec)
{
    ProError status;

    memset(spec, 0, sizeof(HTolBrushSpec));
    spec->table_type = PROTOLTABLE_NONE;
    spec->table[0]   = L'\0';

    status = ProDimensionToltypeGet(dim, &spec->type);
    if (status != PRO_TK_NO_ERROR)
        return status;                       /* 参考尺寸会返回 PRO_TK_BAD_CONTEXT */

    status = ProDimensionToleranceGet(dim, &spec->upper, &spec->lower);
    if (status != PRO_TK_NO_ERROR)
        return status;

    status = ProDimensionDecimalsGet(dim, &spec->decimals);
    spec->has_decimals = (status == PRO_TK_NO_ERROR) ? 1 : 0;

    status = ProDimensionTollabelGet(dim, &spec->table_type,
                                     spec->table, &spec->column);
    spec->has_label = (status == PRO_TK_NO_ERROR) ? 1 : 0;

    spec->displayed = PRO_B_FALSE;
    ProDimensionIsToleranceDisplayed(dim, &spec->displayed);
    return PRO_TK_NO_ERROR;
}

/* 把整套公差配置写到目标尺寸；返回第一个失败码 */
/* 公差快照：用来做「写前 / 写后」回读对比，判定到底写进去没有。
   ★ 只报"成功"是靠不住的 —— 必须 Get 回来对上账才算真刷上。 */
typedef struct
{
    int                 ok;      /* Get 是否成功 */
    ProDimToleranceType type;    /* 公差类型 */
    double              upper;   /* 上公差 */
    double              lower;   /* 下公差 */
    ProBoolean          disp;    /* 该尺寸的公差当前是否显示 */
} HTolSnap;

/* 读尺寸 owner 的身份，排"对象定位错"用 */
static void HTolOwnerRead(ProDimension *dim, int *mdl_type, int *mdl_id)
{
    *mdl_type = -1;
    *mdl_id   = -1;
    if (dim->owner == NULL) return;
    {
        ProMdlType t = PRO_MDL_UNUSED;
        ProMdlIdGet(dim->owner, mdl_id);
        ProMdlTypeGet(dim->owner, &t);
        *mdl_type = (int)t;
    }
}

static void HTolSnapRead(ProDimension *dim, ProDrawing dwg, HTolSnap *s)
{
    s->ok    = 0;
    s->type  = PRO_TOL_DEFAULT;
    s->upper = 0.0;
    s->lower = 0.0;
    s->disp  = PRO_B_FALSE;

    if (ProDimensionToltypeGet(dim, &s->type) != PRO_TK_NO_ERROR) return;
    if (ProDimensionToleranceGet(dim, &s->upper, &s->lower) != PRO_TK_NO_ERROR) return;

    /* 绘图尺寸有专用查询（ProDimension.h:1221 "Indicates whether a drawing
       dimension's tolerance is visible or not"），比通用版更准，优先用。 */
    if (dwg != NULL)
        ProDrawingDimensionIsToleranceDisplayed(dwg, dim, &s->disp);
    else
        ProDimensionIsToleranceDisplayed(dim, &s->disp);

    s->ok = 1;
}

/* 把整套公差配置写到目标尺寸；返回第一个失败码。
   ★ dwg 用于查询「绘图尺寸的公差是否可见」。
   ★ 本函数把「写前 / 写后」都回读一遍打进日志：这是排「点了没反应」的唯一硬证据 ——
     VERDICT=WRITTEN 说明 API 真写进去了（那问题在显示/刷新）；
     VERDICT=NOT-WRITTEN 说明 API 只报成功没落地（那是对象定位/上下文问题）。 */
static ProError HTolBrushApply(HTolBrushSpec *spec, ProDimension *dim, ProDrawing dwg)
{
    ProError status;
    ProError first = PRO_TK_NO_ERROR;
    HTolSnap before, after;
    int      own_type = -1, own_id = -1;
    ProBoolean driven = PRO_B_FALSE;
    double   disp_up = 0.0, disp_lo = 0.0;
    int      written;

    HTolSnapRead(dim, dwg, &before);

    /* 先定类型、再写值 —— 顺序反了在「极限/正负」之间切换时值会被重新解释 */
    status = ProDimensionToltypeSet(dim, spec->type);
    if (status != PRO_TK_NO_ERROR) first = status;

    status = ProDimensionToleranceSet(dim, spec->upper, spec->lower);
    if (status != PRO_TK_NO_ERROR && first == PRO_TK_NO_ERROR) first = status;

    if (spec->has_decimals)
    {
        status = ProDimensionDecimalsSet(dim, spec->decimals);
        if (status != PRO_TK_NO_ERROR && first == PRO_TK_NO_ERROR) first = status;
    }

    if (spec->has_label)
    {
        status = ProDimensionTollabelSet(dim, spec->table_type,
                                         spec->table, spec->column);
        if (status != PRO_TK_NO_ERROR && first == PRO_TK_NO_ERROR) first = status;
    }

    HTolSnapRead(dim, dwg, &after);

    /* 显示的（四舍五入后）上下极限：看界面数值有没有跟着变 */
    disp_up = 0.0; disp_lo = 0.0;
    ProDimensionDisplayedToleranceGet(dim, &disp_up, &disp_lo);

    written = (after.ok &&
               after.type == spec->type &&
               after.upper > spec->upper - 1e-9 && after.upper < spec->upper + 1e-9 &&
               after.lower > spec->lower - 1e-9 && after.lower < spec->lower + 1e-9);

    HLog("HTolBrushApply: id=%d rc=%d\n", dim->id, (int)first);
    HLog("HTolBrushApply:   before ok=%d type=%d up=%.4f lo=%.4f disp=%d\n",
         before.ok, (int)before.type, before.upper, before.lower, (int)before.disp);
    HLog("HTolBrushApply:   want   type=%d up=%.4f lo=%.4f\n",
         (int)spec->type, spec->upper, spec->lower);
    HLog("HTolBrushApply:   after  ok=%d type=%d up=%.4f lo=%.4f disp=%d dispval=%.4f/%.4f\n",
         after.ok, (int)after.type, after.upper, after.lower, (int)after.disp,
         disp_up, disp_lo);
    HTolOwnerRead(dim, &own_type, &own_id);
    driven = PRO_B_FALSE;
    ProDimensionIsDriving(dim, &driven);
    HLog("HTolBrushApply:   owner mdltype=%d mdlid=%d driven=%d\n",
         own_type, own_id, (int)driven);

    HLog("HTolBrushApply:   VERDICT=%s\n",
         written ? "WRITTEN" : (after.ok ? "NOT-WRITTEN" : "READBACK-FAILED"));

    return first;
}

static int HTolBrushAct(uiCmdCmdId command, uiCmdValue *p_value, void *p_push_command_data)
{
    ProError              status;
    ProMdl                drawing;
    ProSelection         *sels = NULL;
    int                   n_sels = 0;
    ProSelectionEnvOption env_opts[3];
    ProSelectionEnv       sel_env = NULL;
    ProDimension          src;
    ProDimension          dim;
    HTolBrushSpec         spec;
    int                   n_applied = 0, n_fail = 0, n_skip = 0;
    ProView               view = NULL;

    (void)command; (void)p_value; (void)p_push_command_data;

    status = HGetDrawing(&drawing);
    if (status != PRO_TK_NO_ERROR) { HMsg("BALLOON_NO_DRAWING"); return 0; }

    /* 单选即返回（不要「确定」对话框）：刷子是连点操作，弹框会卡手。
       中键 / Esc = 结束刷。 */
    env_opts[0].attribute = PRO_SELECT_DONE_REQUIRED;    env_opts[0].value = 0;
    env_opts[1].attribute = PRO_SELECT_BY_MENU_ALLOWED;  env_opts[1].value = 1;
    env_opts[2].attribute = PRO_SELECT_BY_BOX_ALLOWED;   env_opts[2].value = 0;
    status = ProSelectionEnvAlloc(env_opts, 3, &sel_env);
    HLog("HTolBrushAct: SelectionEnvAlloc -> %d\n", (int)status);

    /* ============ 阶段 1：点选源尺寸 ============ */
    HMsg("TOLBRUSH_PICK_SRC");
    ProWindowRepaint(PRO_VALUE_UNUSED);

    status = ProSelect((char*)"dimension", 1, NULL, NULL, sel_env, NULL, &sels, &n_sels);
    HLog("HTolBrushAct: src ProSelect -> %d n=%d\n", (int)status, n_sels);
    if (status != PRO_TK_NO_ERROR || n_sels < 1 || sels == NULL)
    {
        HLog("HTolBrushAct: no source dimension -> exit\n");
        /* ★ 不 free sels：ProSelect 返回的是 Creo 的静态内存
           （ProSelection.h:495-500），free 它会破坏堆当场崩溃 */
        if (sel_env) ProSelectionEnvFree(sel_env);
        return 0;
    }

    status = ProSelectionModelitemGet(sels[0], &src);
    sels = NULL;   /* ★ 只清指针，绝不 ProSelectionarrayFree —— 见上 */

    HLog("HTolBrushAct: src ModelitemGet -> %d type=%d id=%d\n",
         (int)status, (int)src.type, src.id);
    if (status != PRO_TK_NO_ERROR || src.type != PRO_DIMENSION)
    {
        HMsg("TOLBRUSH_NOT_DIM");
        if (sel_env) ProSelectionEnvFree(sel_env);
        return 0;
    }

    status = HTolBrushRead(&src, &spec);
    if (status != PRO_TK_NO_ERROR)
    {
        HLog("HTolBrushAct: read source tolerance -> %d (reference dim?)\n", (int)status);
        HMsg("TOLBRUSH_SRC_REF");
        if (sel_env) ProSelectionEnvFree(sel_env);
        return 0;
    }
    HLog("HTolBrushAct: src spec type=%d upper=%.4f lower=%.4f decimals=%d(%d) "
         "label=%d(%d/%ls/%d) displayed=%d\n",
         (int)spec.type, spec.upper, spec.lower, spec.decimals, spec.has_decimals,
         spec.has_label, (int)spec.table_type, spec.table, spec.column,
         (int)spec.displayed);

    /* ============ 阶段 2：连点目标尺寸，点一个刷一个 ============ */
    HMsg("TOLBRUSH_TARGET");
    ProWindowRepaint(PRO_VALUE_UNUSED);

    for (;;)
    {
        sels = NULL; n_sels = 0;
        status = ProSelect((char*)"dimension", 1, NULL, NULL, sel_env, NULL, &sels, &n_sels);
        if (status != PRO_TK_NO_ERROR || n_sels < 1 || sels == NULL)
        {
            HLog("HTolBrushAct: target selection ended -> stop\n");
            break;                     /* ★ 同样不 free sels */
        }

        status = ProSelectionModelitemGet(sels[0], &dim);

        /* 顺手拿到这个尺寸所在的视图：公差改了之后要重生成该视图，
           绘图的显示列表才会重算（尺寸来自 3D 模型时尤其必须）。 */
        view = NULL;
        ProSelectionViewGet(sels[0], &view);

        sels = NULL;                   /* ★ 不 free，只清指针 */

        if (status != PRO_TK_NO_ERROR || dim.type != PRO_DIMENSION)
        {
            n_skip++;
            HLog("HTolBrushAct: target is not a dimension -> skip\n");
            continue;
        }
        if (dim.id == src.id && dim.owner == src.owner)
        {
            n_skip++;
            HLog("HTolBrushAct: target == source -> skip\n");
            continue;
        }

        status = HTolBrushApply(&spec, &dim, (ProDrawing)drawing);
        if (status == PRO_TK_NO_ERROR)
        {
            n_applied++;
            HLog("HTolBrushAct: applied to dim id=%d\n", dim.id);
            HMsg("TOLBRUSH_APPLIED");
        }
        else
        {
            n_fail++;
            HLog("HTolBrushAct: apply to dim id=%d -> %d (reference dim?)\n",
                 dim.id, (int)status);
            HMsg("TOLBRUSH_TARGET_REF");
        }

        /* 刷新链：重生成视图（重算显示列表）+ Repaint（走 View>Repaint）
           + Refresh（清临时图形）。只 Repaint 不够 —— 显示列表不重算，
           尺寸文字就不会重画，表现就是"点了没反应"。 */
        if (view != NULL)
        {
            status = ProDrawingViewRegenerate((ProDrawing)drawing, view);
            HLog("HTolBrushAct: view regenerate -> %d\n", (int)status);
        }
        ProWindowRepaint(PRO_VALUE_UNUSED);
        ProWindowRefresh(PRO_VALUE_UNUSED);
    }

    if (sel_env) { ProSelectionEnvFree(sel_env); sel_env = NULL; }

    HLog("HTolBrushAct: done, applied=%d failed=%d skipped=%d\n",
         n_applied, n_fail, n_skip);
    if (n_applied > 0) HMsg("TOLBRUSH_OK");
    else               HMsg("TOLBRUSH_CANCEL");
    return 0;
}

/*---------------------------------------------------------------------*\
  重点尺寸 / 取消重点尺寸：在尺寸文本【前面】加一个 ★，或者把它去掉

  交互（两个命令完全一样）：点按钮 -> 消息行提示 -> 连点尺寸
        （点一个处理一个）-> 中键 / Esc 结束
        ★ 拾取会话照抄公差刷子（HTolBrushAct）：PRO_SELECT_DONE_REQUIRED=0
          单选即返回，不弹「确定」框；中键 / Esc 由 ProSelect 自己结束。

  ★ 实现选的是【尺寸前缀】ProDimensionPrefixSet，不是改尺寸文本：
      - Creo 的尺寸 = 前缀 + 数值 + 后缀，前缀是独立字段（功能区「前缀」框、
        右键 Properties 里都能看到），改它不会破坏由模型带出来的数值文本；
      - ProDimensionTextSet 是「Modify > Text」的等价物，会把整个文本覆盖掉
        （还得自己写 @D 之类的占位符），风险大，不用。
      - 前缀/后缀 API 在 ProDimension.h:974/994/1014/1036。
  ★ 加：原有前缀原样保留，★ 只插到最前面（"2x" -> "★2x"）；
       已经有 ★ 的尺寸跳过，不重复叠（幂等）。
  ★ 取消：删掉前缀里【所有】★，其余字符原样保留；没有 ★ 就跳过。
  ★ 改完必须 ProDrawingViewRegenerate（只 Repaint 等于"点了没反应"，
    公差刷子踩过同一个坑，见那段注释）。
\*---------------------------------------------------------------------*/

#define KEYDIM_MARK L"★"               /* U+2605 BLACK STAR */

#define KEYDIM_ADD    0
#define KEYDIM_CLEAR  1

/* 打标记。*already 输出 1 表示之前就标过（本次没改）。 */
static ProError HKeyDimMark(ProDimension *dim, int *already)
{
    ProLine  old_prefix;
    ProLine  new_prefix;
    ProError status;

    *already = 0;
    old_prefix[0] = L'\0';

    /* 没有前缀时这里会回 PRO_TK_E_NOT_FOUND(-5)，不是错，当空串处理 */
    status = ProDimensionPrefixGet(dim, old_prefix);
    if (status != PRO_TK_NO_ERROR)
        old_prefix[0] = L'\0';

    if (wcsstr(old_prefix, KEYDIM_MARK) != NULL)
    {
        *already = 1;
        HLog("HKeyDimMark: id=%d already marked, skip\n", dim->id);
        return PRO_TK_NO_ERROR;
    }

    /* ★ 放最前面，原有前缀（比如 "2x"）接在后面 */
    if (swprintf_s(new_prefix, PRO_LINE_SIZE, L"%s%s",
                   KEYDIM_MARK, old_prefix) < 0)
        return PRO_TK_GENERAL_ERROR;

    status = ProDimensionPrefixSet(dim, new_prefix);
    /* ⚠ 不打印 old_prefix 本身：HLog 的 %ls 遇到空宽串会吃掉行尾换行，
       导致这条日志和下一行黏在一起（之前踩过）。只打长度。 */
    HLog("HKeyDimMark: id=%d old_len=%d set -> %d\n",
         dim->id, (int)wcslen(old_prefix), (int)status);
    return status;
}

/* 去标记：删掉前缀里所有 ★，剩下的字符原样保留。
   *had 输出 1 表示确实有 ★ 被删掉。 */
static ProError HKeyDimUnmark(ProDimension *dim, int *had)
{
    ProLine  old_prefix;
    ProLine  new_prefix;
    ProError status;
    int      i, k = 0;

    *had = 0;
    old_prefix[0] = L'\0';

    status = ProDimensionPrefixGet(dim, old_prefix);
    if (status != PRO_TK_NO_ERROR)
        old_prefix[0] = L'\0';         /* 根本没前缀 -> 自然也没标记 */

    if (wcsstr(old_prefix, KEYDIM_MARK) == NULL)
    {
        HLog("HKeyDimUnmark: id=%d no mark, skip\n", dim->id);
        return PRO_TK_NO_ERROR;
    }

    for (i = 0; old_prefix[i] != L'\0' && k < PRO_LINE_SIZE - 1; i++)
    {
        if (old_prefix[i] == KEYDIM_MARK[0])
            continue;                  /* 扔掉这一个 ★ */
        new_prefix[k++] = old_prefix[i];
    }
    new_prefix[k] = L'\0';

    *had = 1;
    status = ProDimensionPrefixSet(dim, new_prefix);
    HLog("HKeyDimUnmark: id=%d old_len=%d new_len=%d set -> %d\n",
         dim->id, (int)wcslen(old_prefix), k, (int)status);
    return status;
}

/* 两个命令共用的主循环。mode = KEYDIM_ADD / KEYDIM_CLEAR */
static int HKeyDimRun(int mode)
{
    ProError              status;
    ProMdl                drawing;
    ProSelection         *sels = NULL;
    int                   n_sels = 0;
    ProSelectionEnvOption env_opts[3];
    ProSelectionEnv       sel_env = NULL;
    ProDimension          dim;
    ProView               view = NULL;
    int                   n_done = 0, n_noop = 0, n_fail = 0;

    char *msg_pick   = (mode == KEYDIM_ADD) ? "KEYDIM_PICK"    : "KEYDIMC_PICK";
    char *msg_done   = (mode == KEYDIM_ADD) ? "KEYDIM_APPLIED" : "KEYDIMC_CLEARED";
    char *msg_noop   = (mode == KEYDIM_ADD) ? "KEYDIM_ALREADY" : "KEYDIMC_NOMARK";
    char *msg_fail   = (mode == KEYDIM_ADD) ? "KEYDIM_FAIL"    : "KEYDIMC_FAIL";
    char *msg_ok     = (mode == KEYDIM_ADD) ? "KEYDIM_OK"      : "KEYDIMC_OK";
    char *msg_cancel = (mode == KEYDIM_ADD) ? "KEYDIM_CANCEL"  : "KEYDIMC_CANCEL";

    status = HGetDrawing(&drawing);
    if (status != PRO_TK_NO_ERROR)
    {
        HLog("HKeyDimRun: not a drawing -> %d\n", (int)status);
        HMsg("BALLOON_NO_DRAWING");
        return 0;
    }

    env_opts[0].attribute = PRO_SELECT_DONE_REQUIRED;    env_opts[0].value = 0;
    env_opts[1].attribute = PRO_SELECT_BY_MENU_ALLOWED;  env_opts[1].value = 1;
    env_opts[2].attribute = PRO_SELECT_BY_BOX_ALLOWED;   env_opts[2].value = 0;
    status = ProSelectionEnvAlloc(env_opts, 3, &sel_env);
    HLog("HKeyDimRun: mode=%d SelectionEnvAlloc -> %d\n", mode, (int)status);

    HMsg(msg_pick);
    ProWindowRepaint(PRO_VALUE_UNUSED);

    for (;;)
    {
        int flag = 0;

        sels = NULL; n_sels = 0;
        status = ProSelect((char*)"dimension", 1, NULL, NULL, sel_env, NULL,
                           &sels, &n_sels);
        if (status != PRO_TK_NO_ERROR || n_sels < 1 || sels == NULL)
        {
            HLog("HKeyDimRun: selection ended -> stop\n");
            break;
        }

        status = ProSelectionModelitemGet(sels[0], &dim);

        /* 顺手取尺寸所在视图：改完要重生成该视图，显示列表才会重算 */
        view = NULL;
        ProSelectionViewGet(sels[0], &view);

        sels = NULL;   /* ★ Creo 的静态内存，只清指针，绝不 ProSelectionarrayFree */

        if (status != PRO_TK_NO_ERROR || dim.type != PRO_DIMENSION)
        {
            n_fail++;
            HLog("HKeyDimRun: not a dimension -> skip\n");
            HMsg("KEYDIM_NOT_DIM");
            continue;
        }

        if (mode == KEYDIM_ADD)
            status = HKeyDimMark(&dim, &flag);
        else
            status = HKeyDimUnmark(&dim, &flag);

        if (status != PRO_TK_NO_ERROR)
        {
            n_fail++;
            HLog("HKeyDimRun: dim id=%d -> %d (reference/driven?)\n",
                 dim.id, (int)status);
            HMsg(msg_fail);
        }
        else if (flag)
        {
            n_done++;
            HMsg(msg_done);
        }
        else
        {
            n_noop++;
            HMsg(msg_noop);
        }

        /* 刷新链：重生成视图 -> Repaint -> Refresh（和公差刷子完全一致） */
        if (view != NULL)
        {
            status = ProDrawingViewRegenerate((ProDrawing)drawing, view);
            HLog("HKeyDimRun: view regenerate -> %d\n", (int)status);
        }
        ProWindowRepaint(PRO_VALUE_UNUSED);
        ProWindowRefresh(PRO_VALUE_UNUSED);
    }

    if (sel_env) { ProSelectionEnvFree(sel_env); sel_env = NULL; }

    HLog("HKeyDimRun: mode=%d done, changed=%d skipped=%d failed=%d\n",
         mode, n_done, n_noop, n_fail);
    if (n_done > 0) HMsg(msg_ok);
    else            HMsg(msg_cancel);
    return 0;
}

static int HKeyDimAct(uiCmdCmdId command, uiCmdValue *p_value, void *p_push_command_data)
{
    (void)command; (void)p_value; (void)p_push_command_data;
    return HKeyDimRun(KEYDIM_ADD);
}

static int HKeyDimClearAct(uiCmdCmdId command, uiCmdValue *p_value, void *p_push_command_data)
{
    (void)command; (void)p_value; (void)p_push_command_data;
    return HKeyDimRun(KEYDIM_CLEAR);
}

#define TPL_FOLDER L"templates"
#define TPL_SUBDIR L"技术要求"

/*---------------------------------------------------------------------*\
  技术要求：从模板文件夹里挑一个 .txt，整段作为【一条注解】放到图纸上

  交互：点按钮 -> Creo 原生文件浏览器（一开始就停在
        <插件目录>\templates\技术要求）-> 选一个 .txt
        -> 提示「点击放置位置」-> 左键落点，整段文字成为一条注解

  ★ 注解 API 全部照官方 pt_userguide\ptu_dwg\UgNoteCreate.c：
      ProDtlnotedataAlloc -> 每行一个 ProDtlnoteline（行内挂一个 ProDtlnotetext）
      -> ProDtlnotedataColorSet -> ProDtlnotedataAttachmentSet(PRO_DTLATTACHTYPE_FREE)
      -> ProDtlnoteCreate(drawing, NULL, notedata, &note) -> ProDtlnoteShow
    多行 = 反复 ProDtlnotedataLineAdd（官方只演示一行，多行是同一套接口）。
  ★ 字高：先 ProSolidDefaulttextheightGet 取默认字高，再除以图纸变换矩阵的
    [0][0] 做单位换算（官方同一个写法）。不这样做，不同比例图纸上字会忽大忽小。
  ★ PRO_LINE_SIZE = 81（ProSizeConst.h:19）=> 每行最多 80 个宽字符，超出截断。
  ★ 文件编码：UTF-8 BOM > 合法 UTF-8 > 系统 ANSI(GBK)
    —— 记事本「UTF-8」和「ANSI」两种保存方式都能读。
  ★ 放置点和球标共用 HPickPlacePoint（ProMousePickGet），
    拾取期间滚轮/切命令都是活的，见那段注释。
\*---------------------------------------------------------------------*/

static void HPluginDirW(ProPath out)
{
    HMODULE hm = NULL;

    out[0] = L'\0';
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCWSTR)&HPluginDirW, &hm))
    {
        DWORD n = GetModuleFileNameW(hm, out, PRO_PATH_SIZE);
        if (n == 0 || n >= PRO_PATH_SIZE)
        {
            out[0] = L'\0';
            return;
        }
    }
    {
        wchar_t *p = wcsrchr(out, L'\\');
        if (p != NULL)
            *p = L'\0';
    }
}

/* 模板目录 = <插件目录>\templates\技术要求，缺哪层建哪层 */
static void HTplDirW(ProPath out)
{
    ProPath base;

    HPluginDirW(base);
    if (base[0] == L'\0')
        wcscpy_s(base, PRO_PATH_SIZE, L".");

    swprintf_s(out, PRO_PATH_SIZE, L"%s\\%s", base, TPL_FOLDER);
    CreateDirectoryW(out, NULL);            /* 已存在时返回失败，忽略 */

    wcsncat_s(out, PRO_PATH_SIZE, L"\\", 1);
    wcsncat_s(out, PRO_PATH_SIZE, TPL_SUBDIR, _TRUNCATE);
    CreateDirectoryW(out, NULL);
}

/* 读模板 -> 按行切成宽字符行（lines 由调用方提供，容量 maxlines）
   返回行数；<=0 表示读失败或空文件 */
static int HReadTplLinesW(const wchar_t *path, ProLine *lines, int maxlines)
{
    HANDLE   h;
    DWORD    got  = 0;
    char    *buf  = NULL;
    wchar_t *wb   = NULL;
    int      size, wlen, i, n = 0, off = 0, cp = CP_UTF8;
    BOOL     utf8_bom = FALSE;

    h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL,
                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE)
    {
        HLog("HReadTplLinesW: open failed (%lu)\n", (unsigned long)GetLastError());
        return -1;
    }

    size = (int)GetFileSize(h, NULL);
    if (size <= 0 || size > 256 * 1024)
    {
        HLog("HReadTplLinesW: bad size %d\n", size);
        CloseHandle(h);
        return -1;
    }

    buf = (char *)malloc(size + 1);
    if (buf == NULL) { CloseHandle(h); return -1; }
    if (!ReadFile(h, buf, (DWORD)size, &got, NULL)) got = 0;
    CloseHandle(h);
    buf[(int)got] = '\0';
    if (got < 1) { free(buf); return -1; }

    if (got >= 3 && (unsigned char)buf[0] == 0xEF &&
        (unsigned char)buf[1] == 0xBB && (unsigned char)buf[2] == 0xBF)
    {
        utf8_bom = TRUE;
        off = 3;
    }
    if (!utf8_bom)
    {
        /* 严格试 UTF-8；不合法就按系统代码页（中文 Windows = GBK）解释 */
        if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                buf + off, -1, NULL, 0) <= 0)
            cp = CP_ACP;
    }

    wlen = MultiByteToWideChar(cp, 0, buf + off, -1, NULL, 0);
    if (wlen <= 1) { free(buf); return -1; }

    wb = (wchar_t *)malloc((wlen + 1) * sizeof(wchar_t));
    if (wb == NULL) { free(buf); return -1; }
    MultiByteToWideChar(cp, 0, buf + off, -1, wb, wlen + 1);
    free(buf);
    wlen--;                                  /* 去掉结尾 \0 的计数 */

    i = 0;
    while (i < wlen && n < maxlines)
    {
        int k = 0;

        while (i < wlen && wb[i] != L'\r' && wb[i] != L'\n')
        {
            if (k < PRO_LINE_SIZE - 1)
                lines[n][k++] = wb[i];
            i++;
        }
        while (k > 0 && (lines[n][k - 1] == L' ' || lines[n][k - 1] == L'\t'))
            k--;
        lines[n][k] = L'\0';
        if (k == 0)
            wcscpy_s(lines[n], PRO_LINE_SIZE, L" ");   /* 空行留一行高，保住排版 */
        n++;

        if (i < wlen && wb[i] == L'\r') i++;
        if (i < wlen && wb[i] == L'\n') i++;
    }
    free(wb);

    while (n > 1 && lines[n - 1][0] == L' ')      /* 去掉文件末尾的空行 */
        n--;

    HLog("HReadTplLinesW: cp=%d bom=%d lines=%d\n", cp, (int)utf8_bom, n);
    return n;
}

/* 把 lines[0..n) 建成一条注解，FREE 挂在 point 处 */
static ProError HCreateNoteFromLines(ProMdl drawing, ProView over_view,
                                     ProVector point, ProLine *lines, int n)
{
    ProDtlnotedata notedata = NULL;
    ProDtlattach   attach   = NULL;
    ProDtlnote     note;
    ProColor       color;
    double         height = 2.5;
    ProError       sub;
    int            i, sheet;

    if (ProDtlnotedataAlloc(drawing, &notedata) != PRO_TK_NO_ERROR)
    {
        HLog("HCreateNoteFromLines: [1] dataAlloc FAILED\n");
        return PRO_TK_GENERAL_ERROR;
    }

    /* 默认字高 + 图纸变换单位换算（官方 UgNoteCreate.c 同款） */
    {
        double    h_def = 0.0;
        ProMatrix m;
        ProName   w_size;

        if (ProSolidDefaulttextheightGet((ProSolid)drawing, &h_def) == PRO_TK_NO_ERROR
            && h_def > 0.0
            && ProDrawingCurrentSheetGet((ProDrawing)drawing, &sheet) == PRO_TK_NO_ERROR
            && ProDrawingSheetTrfGet((ProDrawing)drawing, sheet, w_size, m) == PRO_TK_NO_ERROR
            && m[0][0] > 0.0)
        {
            height = h_def / m[0][0];
        }
        HLog("HCreateNoteFromLines: default height=%.4f -> %.4f\n", h_def, height);
    }

    for (i = 0; i < n; i++)
    {
        ProDtlnoteline line = NULL;
        ProDtlnotetext text = NULL;

        if (ProDtlnotelineAlloc(&line) != PRO_TK_NO_ERROR ||
            ProDtlnotetextAlloc(&text) != PRO_TK_NO_ERROR)
        {
            HLog("HCreateNoteFromLines: [2] line/text alloc FAILED at %d\n", i);
            ProDtlnotedataFree(notedata);
            return PRO_TK_GENERAL_ERROR;
        }
        ProDtlnotetextHeightSet(text, height);
        ProDtlnotetextWidthSet(text, 0.8);
        ProDtlnotetextSlantSet(text, 0.0);
        ProDtlnotetextThicknessSet(text, 0.0);
        ProDtlnotetextStringSet(text, lines[i]);
        ProDtlnotelineTextAdd(line, text);
        ProDtlnotedataLineAdd(notedata, line);
    }

    color.method     = PRO_COLOR_METHOD_TYPE;
    color.value.type = PRO_COLOR_LETTER;
    ProDtlnotedataColorSet(notedata, &color);

    sub = ProDtlattachAlloc(PRO_DTLATTACHTYPE_FREE, over_view, point, NULL, &attach);
    if (sub != PRO_TK_NO_ERROR)
    {
        HLog("HCreateNoteFromLines: [3] attachAlloc -> %d FAILED\n", (int)sub);
        ProDtlnotedataFree(notedata);
        return sub;
    }
    ProDtlnotedataAttachmentSet(notedata, attach);
    /* ★ 这里【不要】ProDtlattachFree：官方 UgNoteCreate.c:239-240 同样不释放，
       释放后 notedata 里可能留着悬挂指针 -> 建注解时崩。一次性的小泄漏无妨。 */

    sub = ProDtlnoteCreate(drawing, NULL, notedata, &note);
    ProDtlnotedataFree(notedata);
    if (sub != PRO_TK_NO_ERROR)
    {
        HLog("HCreateNoteFromLines: [4] noteCreate -> %d FAILED\n", (int)sub);
        return sub;
    }

    sub = ProDtlnoteShow(&note);
    HLog("HCreateNoteFromLines: noteCreate OK, show -> %d, %d lines\n",
         (int)sub, n);
    ProDrawingtreeRefresh((ProDrawing)drawing, PRO_VALUE_UNUSED);
    return PRO_TK_NO_ERROR;
}

static int HTechReqAct(uiCmdCmdId command, uiCmdValue *p_value, void *p_push_command_data)
{
    ProError  status;
    ProMdl    drawing;
    ProPath   tpl_dir, sel;
    ProView   over_view;
    ProVector point;
    int       sheet, nlines;
    static ProLine lines[64];

    (void)command;
    (void)p_value;
    (void)p_push_command_data;

    status = HGetDrawing(&drawing);
    if (status != PRO_TK_NO_ERROR)
    {
        HLog("HTechReqAct: not a drawing -> %d\n", (int)status);
        HMsg("TECHREQ_NO_DRAWING");
        return 0;
    }

    HTplDirW(tpl_dir);
    HLog("HTechReqAct: template dir = %ls\n", tpl_dir);

    sel[0] = L'\0';
    status = ProFileMdlnameOpen(L"选择技术要求模板", L"*.txt", NULL, NULL,
                                tpl_dir, NULL, sel);
    HLog("HTechReqAct: file open -> %d\n", (int)status);
    if (status == PRO_TK_USER_ABORT)
        return 0;                          /* 用户取消：安静退出 */
    if (status != PRO_TK_NO_ERROR)
    {
        /* ★ 兜底：个别版本/环境可能不接受 *.txt 过滤器（把 txt 当非法扩展名），
           这时退回「显示所有文件」再问一次，别让整个功能不可用。 */
        HLog("HTechReqAct: retry browser without filter\n");
        status = ProFileMdlnameOpen(L"选择技术要求模板", L"", NULL, NULL,
                                    tpl_dir, NULL, sel);
        HLog("HTechReqAct: file open (no filter) -> %d\n", (int)status);
        if (status == PRO_TK_USER_ABORT)
            return 0;
        if (status != PRO_TK_NO_ERROR)
        {
            HMsg("TECHREQ_NO_TEMPLATE");
            return 0;
        }
    }

    nlines = HReadTplLinesW(sel, lines, 64);
    if (nlines <= 0)
    {
        HMsg("TECHREQ_EMPTY");
        return 0;
    }

    ProDrawingCurrentSheetGet((ProDrawing)drawing, &sheet);
    ProDrawingBackgroundViewGet((ProDrawing)drawing, sheet, &over_view);

    HMsg("TECHREQ_ASK");
    ProWindowRepaint(PRO_VALUE_UNUSED);

    if (HPickPlacePoint(-1, point) != PRO_TK_NO_ERROR)
    {
        HLog("HTechReqAct: placement cancelled\n");
        HMsg("TECHREQ_CANCEL");
        return 0;
    }

    if (HCreateNoteFromLines(drawing, over_view, point, lines, nlines)
        != PRO_TK_NO_ERROR)
    {
        HMsg("TECHREQ_FAILED");
        return 0;
    }

    ProDwgSheetRegenerate((ProDrawing)drawing, sheet);
    ProWindowRepaint(PRO_VALUE_UNUSED);
    ProWindowRefresh(PRO_VALUE_UNUSED);
    HMsg("TECHREQ_OK");
    return 0;
}

/*---------------------------------------------------------------------*\
  会话：打开工作目录 / 设置工作目录 / 打开模型目录 / 模型转目录
        / 批量删旧版 / 删除闲置模型 / 清理垃圾

  ★ 除「删除闲置模型」外全是 Win32 文件操作（ShellExecuteW /
    FindFirstFileW / DeleteFileW），路径一律宽字符 —— 中文目录安全。
    窄字符文件 API 在中文路径下会静默失败（一键导出踩过同样的坑）。

  ★ 危险动作（批量删旧版 / 清理垃圾）【先扫描、再要用户确认】：
    确认走消息行输入（ProMessageStringRead），必须输入 Y 才动手，
    直接回车 = 取消。绝不静默删除用户的文件。

  ★ 删除范围严格限定在【当前工作目录】+【白名单扩展名】：
    · 删旧版只认 .prt/.asm/.drw/.frm/.sec/.lay/.mfg 的「.数字」后缀，
      且必须同目录存在同名的无编号文件才删（否则那份编号文件是唯一副本）；
    · 清理垃圾只删 .err/.out/.inf/.idx/.xpr/.bak/.dwl/.m_p/.lst/.tst。
\*---------------------------------------------------------------------*/

/* 用资源管理器打开目录或文件。ShellExecuteW 返回值 > 32 才算成功。 */
static int HShellOpenW(const wchar_t *path)
{
    INT_PTR r;

    if (path == NULL || path[0] == L'\0')
        return -1;
    r = (INT_PTR)ShellExecuteW(NULL, L"open", path, NULL, NULL, SW_SHOWNORMAL);
    HLog("HShellOpenW -> %ld\n", (long)r);
    return (r > 32) ? 0 : -1;
}

/* 就地把 path 截成它的目录部分 */
static void HDirOfW(wchar_t *path)
{
    wchar_t *p, *last = NULL;

    for (p = path; *p != L'\0'; p++)
        if (*p == L'\\' || *p == L'/')
            last = p;
    if (last != NULL && last != path)
        *last = L'\0';
}

/* 消息行二选一确认：返回 1 = 用户输入了 Y/y，0 = 其它（含 Esc / 空回车） */
static int HConfirmYes(const char *ask_key)
{
    wchar_t ans[PRO_LINE_SIZE];
    ProError status;

    HMsg((char *)ask_key);
    ProWindowRepaint(PRO_VALUE_UNUSED);

    ans[0] = L'\0';
    status = ProMessageStringRead(PRO_LINE_SIZE - 1, ans);
    ProMessageClear();
    HLog("HConfirmYes: read -> %d, first=%d\n", (int)status, (int)ans[0]);

    if (status != PRO_TK_NO_ERROR)
        return 0;
    return (ans[0] == L'Y' || ans[0] == L'y') ? 1 : 0;
}

/* ★ ProName 是 wchar_t[PRO_NAME_SIZE]（ProSizeConst.h:65），不是窄字符 ——
   ProDirectoryChoose 的对话框标签直接给 L"" 宽串即可，不需要转码。 */

static int HInListW(const wchar_t *ext, const wchar_t **list, int n)
{
    int i;

    for (i = 0; i < n; i++)
        if (_wcsicmp(ext, list[i]) == 0)
            return 1;
    return 0;
}

/* 从文件名里拆出「.ext」部分（最后一个点之后）。放不下返回 -1。 */
static int HExtOfW(const wchar_t *name, wchar_t *ext, int cap)
{
    const wchar_t *dot = NULL, *p;
    size_t        n;

    for (p = name; *p != L'\0'; p++)
        if (*p == L'.')
            dot = p;
    if (dot == NULL || dot == name)
        return -1;
    n = wcslen(dot + 1);
    if (n == 0 || (int)n >= cap)
        return -1;
    wmemcpy(ext, dot + 1, n);
    ext[n] = L'\0';
    return 0;
}

/*--------------------------------------------------------------------*\
  批量删旧版：扫 dir，找 name.ext.N（N 全数字，ext 在白名单里），
  且同目录存在 name.ext，才算旧版本。do_delete=0 只统计。
  返回命中数（删除模式返回命中数，不代表全部删成功，失败会记日志）。
\*--------------------------------------------------------------------*/
static int HPurgeDirW(const wchar_t *dir, int do_delete)
{
    static const wchar_t *ok_ext[] = { L"prt", L"asm", L"drw", L"frm",
                                       L"sec", L"lay", L"mfg" };
    wchar_t         pat[PRO_PATH_SIZE], full[PRO_PATH_SIZE], base[PRO_PATH_SIZE];
    wchar_t         ext[32];
    WIN32_FIND_DATAW fd;
    HANDLE          h;
    wchar_t        *d1, *d2, *p;
    int             n = 0, len;

    if (HPathJoinW(pat, PRO_PATH_SIZE, dir, L"*", L".*") != 0)
        return 0;

    h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE)
    {
        HLog("HPurgeDirW: nothing matched\n");
        return 0;
    }

    do
    {
        const wchar_t *name = fd.cFileName;

        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            continue;

        /* 从右往左找最后两个点： name . ext . ver */
        d2 = NULL;
        d1 = NULL;
        for (p = (wchar_t *)name; *p != L'\0'; p++)
            if (*p == L'.') { d1 = d2; d2 = p; }
        if (d1 == NULL || d2 == NULL || d1 == d2)
            continue;

        /* d2 之后必须全是数字，且至少一位 */
        for (p = d2 + 1; *p != L'\0'; p++)
            if (*p < L'0' || *p > L'9')
                break;
        if (*p != L'\0' || p == d2 + 1)
            continue;

        /* 扩展名白名单 */
        len = (int)(d2 - d1 - 1);
        if (len <= 0 || len >= 32)
            continue;
        wmemcpy(ext, d1 + 1, (size_t)len);
        ext[len] = L'\0';
        if (!HInListW(ext, ok_ext, (int)(sizeof(ok_ext) / sizeof(ok_ext[0]))))
            continue;

        /* 基础名（含扩展名、去掉 .N）必须在同目录存在 */
        len = (int)(d1 - name);
        if (len <= 0 || len >= PRO_PATH_SIZE - 1)
            continue;
        wmemcpy(base, name, (size_t)len);
        base[len] = L'\0';
        if (HPathJoinW(full, PRO_PATH_SIZE, dir, base, L"") != 0)
            continue;
        if (!HFileExistsW(full))
        {
            HLogW("HPurgeDirW: skip (no base file) ", name);
            continue;
        }

        n++;
        if (!do_delete)
            continue;

        if (HPathJoinW(full, PRO_PATH_SIZE, dir, name, L"") != 0)
            continue;
        if (DeleteFileW(full))
            HLogW("HPurgeDirW: purged ", full);
        else
            HLog("HPurgeDirW: delete FAILED err=%lu\n",
                 (unsigned long)GetLastError());
    } while (FindNextFileW(h, &fd));

    FindClose(h);
    HLog("HPurgeDirW: %s %d file(s)\n", do_delete ? "deleted" : "found", n);
    return n;
}

/*--------------------------------------------------------------------*\
  清理垃圾：只删白名单里的临时文件扩展名，不碰模型 / 图纸 / 任何其它文件。
\*--------------------------------------------------------------------*/
/*--------------------------------------------------------------------*\
  清理垃圾：判定规则（★ 不能只看扩展名 —— trail.txt.369 的扩展名是 "369"）

    A 类 = 纯临时，删了不影响任何东西：
      · trail.txt / trail.txt.N        —— 轨迹文件，每启动一次加一个
      · std.out / std.err              —— Creo 标准输出/错误（运行时被占用，删不掉属正常）
      · *_log.xml                      —— Creo 12 每次开模型生成的日志
      · current_session.pro            —— 会话快照，下次启动重建
      · *.err .out .inf .idx .xpr .bak .dwl .m_p .lst .tst —— 老 Pro/E 临时文件

    × 不碰：*.log（可能是用户/别的插件留的记录，含插件自己的 h_load.log）、
           *.prt.N / *.asm.N（旧版本，归「批量删旧版」管）、
           config.pro / *.dtl / *.pnt / *.sym / *.dat（配置资源）
    × 只扫工作目录第一层，不进子目录
\*--------------------------------------------------------------------*/
typedef struct h_clean_stat
{
    int found;        /* 命中数 */
    int removed;      /* 真正删掉的 */
    int skipped;      /* 删失败（多为被 Creo 占用 err=32） */
} HCleanStat;

static int HIsJunkW(const wchar_t *name)
{
    static const wchar_t *ok_ext[] = { L"err", L"out", L"inf", L"idx", L"xpr",
                                       L"bak", L"dwl", L"m_p", L"lst", L"tst" };
    wchar_t ext[32];
    size_t  len;

    if (name == NULL || name[0] == L'\0')
        return 0;

    /* 插件自己的调试日志，排错要用 */
    if (_wcsicmp(name, L"h_load.log") == 0)
        return 0;

    /* ① 轨迹文件：trail.txt / trail.txt.N */
    if (_wcsnicmp(name, L"trail.txt", 9) == 0)
        return 1;

    /* ② Creo 12 的模型日志：*_log.xml */
    len = wcslen(name);
    if (len > 8 && _wcsicmp(name + len - 8, L"_log.xml") == 0)
        return 1;

    /* ③ 会话快照 */
    if (_wcsicmp(name, L"current_session.pro") == 0)
        return 1;

    /* ④ 老 Pro/E 临时文件，按扩展名 */
    if (HExtOfW(name, ext, 32) != 0)
        return 0;
    return HInListW(ext, ok_ext, (int)(sizeof(ok_ext) / sizeof(ok_ext[0])));
}

/* do_delete = 0 只统计；1 真删。★ 计数只在真正删掉时才加。 */
static int HCleanDirW(const wchar_t *dir, int do_delete, HCleanStat *st)
{
    wchar_t         pat[PRO_PATH_SIZE], full[PRO_PATH_SIZE];
    WIN32_FIND_DATAW fd;
    HANDLE          h;

    if (dir == NULL || st == NULL)
        return 0;
    if (HPathJoinW(pat, PRO_PATH_SIZE, dir, L"*", L".*") != 0)
        return 0;

    h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE)
    {
        HLog("HCleanDirW: nothing matched\n");
        return 0;
    }

    do
    {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            continue;
        if (!HIsJunkW(fd.cFileName))
            continue;

        st->found++;
        if (!do_delete)
            continue;
        if (HPathJoinW(full, PRO_PATH_SIZE, dir, fd.cFileName, L"") != 0)
            continue;

        if (DeleteFileW(full))
        {
            st->removed++;
            HLogW("HCleanDirW: removed ", full);
        }
        else
        {
            DWORD e = GetLastError();
            st->skipped++;
            HLog("HCleanDirW: delete FAILED err=%lu  (skip)\n",
                 (unsigned long)e);
            HLogW("HCleanDirW:   file = ", full);
        }
    } while (FindNextFileW(h, &fd));

    FindClose(h);
    HLog("HCleanDirW: found=%d removed=%d skipped=%d\n", st->found,
         st->removed, st->skipped);
    return st->found;
}

/* 打开工作目录（资源管理器） */
static int HOpenWorkDirAct(uiCmdCmdId command, uiCmdValue *p_value,
                           void *p_push_command_data)
{
    ProPath dir;

    (void)command;
    (void)p_value;
    (void)p_push_command_data;

    dir[0] = L'\0';
    if (ProDirectoryCurrentGet(dir) != PRO_TK_NO_ERROR || dir[0] == L'\0')
    {
        HMsg("SESSION_NO_WORKDIR");
        return 0;
    }
    HLogW("HOpenWorkDirAct: dir = ", dir);

    if (HShellOpenW(dir) != 0)
    {
        HMsg("SESSION_OPEN_FAIL");
        return 0;
    }
    HMsg("SESSION_WORKDIR_OPENED");
    return 0;
}

/* 设置工作目录 */
static int HSetWorkDirAct(uiCmdCmdId command, uiCmdValue *p_value,
                          void *p_push_command_data)
{
    ProPath   sel;
    ProError  status;

    (void)command;
    (void)p_value;
    (void)p_push_command_data;

    sel[0] = L'\0';
    status = ProDirectoryChoose(L"设置工作目录", NULL, NULL, NULL, sel);
    HLog("HSetWorkDirAct: choose -> %d\n", (int)status);
    if (status == PRO_TK_USER_ABORT)
        return 0;
    if (status != PRO_TK_NO_ERROR || sel[0] == L'\0')
    {
        HMsg("SESSION_SETDIR_FAIL");
        return 0;
    }
    HLogW("HSetWorkDirAct: picked = ", sel);

    status = ProDirectoryChange(sel);
    HLog("HSetWorkDirAct: change -> %d\n", (int)status);
    if (status != PRO_TK_NO_ERROR)
    {
        HMsg("SESSION_SETDIR_FAIL");
        return 0;
    }
    HMsg("SESSION_SETDIR_OK");
    return 0;
}

/* 打开当前模型所在目录 */
static int HOpenMdlDirAct(uiCmdCmdId command, uiCmdValue *p_value,
                          void *p_push_command_data)
{
    ProMdl    mdl;
    wchar_t   path[PRO_PATH_SIZE];
    ProError  status;

    (void)command;
    (void)p_value;
    (void)p_push_command_data;

    status = ProMdlCurrentGet(&mdl);
    if (status != PRO_TK_NO_ERROR || mdl == NULL)
    {
        HLog("HOpenMdlDirAct: no current model -> %d\n", (int)status);
        HMsg("SESSION_NO_MDL");
        return 0;
    }

    /* ProMdlDataGet 在 Creo 12 里已废弃（返回 -34 OBSOLETE_FUNC），
       改用 ProMdlDirectoryPathGet 直接拿"模型所在目录"。 */
    memset(path, 0, sizeof(path));
    status = ProMdlDirectoryPathGet(mdl, path);
    HLog("HOpenMdlDirAct: dirPathGet -> %d\n", (int)status);
    if (status != PRO_TK_NO_ERROR || path[0] == L'\0')
    {
        HMsg("SESSION_NO_MDL");
        return 0;
    }
    HLogW("HOpenMdlDirAct: model dir = ", path);
    if (path[0] == L'\0' || HShellOpenW(path) != 0)
    {
        HMsg("SESSION_OPEN_FAIL");
        return 0;
    }
    HMsg("SESSION_MDLDIR_OPENED");
    return 0;
}

/* 模型转目录：把会话里所有零件/装配/工程图备份到选定目录 */
static int HMdlToDirAct(uiCmdCmdId command, uiCmdValue *p_value,
                        void *p_push_command_data)
{
    ProPath    sel;
    ProError   status;
    ProMdlType types[3];
    int        t, i, n, ok = 0, fail = 0;

    (void)command;
    (void)p_value;
    (void)p_push_command_data;

    sel[0] = L'\0';
    status = ProDirectoryChoose(L"选择目标目录", NULL, NULL, NULL, sel);
    HLog("HMdlToDirAct: choose -> %d\n", (int)status);
    if (status == PRO_TK_USER_ABORT)
        return 0;
    if (status != PRO_TK_NO_ERROR || sel[0] == L'\0')
    {
        HMsg("SESSION_SETDIR_FAIL");
        return 0;
    }
    HLogW("HMdlToDirAct: target = ", sel);

    types[0] = PRO_MDL_PART;
    types[1] = PRO_MDL_ASSEMBLY;
    types[2] = PRO_MDL_DRAWING;

    for (t = 0; t < 3; t++)
    {
        ProMdl *arr = NULL;

        n = 0;
        status = ProSessionMdlList(types[t], &arr, &n);
        HLog("HMdlToDirAct: list type=%d -> %d, n=%d\n", (int)types[t],
             (int)status, n);
        if (status != PRO_TK_NO_ERROR || arr == NULL || n <= 0)
        {
            if (arr != NULL)
                ProArrayFree((ProArray *)&arr);
            continue;
        }

        for (i = 0; i < n; i++)
        {
            status = ProMdlnameBackup(arr[i], sel);
            HLog("HMdlToDirAct: backup #%d type=%d -> %d\n", i,
                 (int)types[t], (int)status);
            if (status == PRO_TK_NO_ERROR)
                ok++;
            else
                fail++;
        }
        ProArrayFree((ProArray *)&arr);
    }

    HLog("HMdlToDirAct: ok=%d fail=%d\n", ok, fail);
    if (ok <= 0 && fail <= 0)
    {
        HMsg("SESSION_NO_MDL");
        return 0;
    }
    if (ok > 0)
        HMsg("SESSION_BACKUP_OK");
    else
        HMsg("SESSION_BACKUP_FAIL");
    return 0;
}

/* 批量删旧版 */
static int HPurgeOldAct(uiCmdCmdId command, uiCmdValue *p_value,
                        void *p_push_command_data)
{
    ProPath dir;
    int     n;

    (void)command;
    (void)p_value;
    (void)p_push_command_data;

    dir[0] = L'\0';
    if (ProDirectoryCurrentGet(dir) != PRO_TK_NO_ERROR || dir[0] == L'\0')
    {
        HMsg("SESSION_NO_WORKDIR");
        return 0;
    }
    HLogW("HPurgeOldAct: dir = ", dir);

    n = HPurgeDirW(dir, 0);
    HLog("HPurgeOldAct: scan n=%d\n", n);
    if (n <= 0)
    {
        HMsg("SESSION_PURGE_NONE");
        return 0;
    }

    if (!HConfirmYes("SESSION_PURGE_ASK"))
    {
        HMsg("SESSION_CANCEL");
        return 0;
    }

    n = HPurgeDirW(dir, 1);
    HLog("HPurgeOldAct: deleted n=%d\n", n);
    HMsg("SESSION_PURGE_OK");
    return 0;
}

/* 删除闲置模型（没显示在任何窗口里的） */
static int HEraseIdleAct(uiCmdCmdId command, uiCmdValue *p_value,
                         void *p_push_command_data)
{
    ProError status;

    (void)command;
    (void)p_value;
    (void)p_push_command_data;

    status = ProMdlEraseNotDisplayed();
    HLog("HEraseIdleAct: erase -> %d\n", (int)status);

    if (status == PRO_TK_NO_ERROR)
        HMsg("SESSION_ERASE_OK");
    else if (status == PRO_TK_E_BUSY)
        HMsg("SESSION_BUSY");
    else
        HMsg("SESSION_ERASE_FAIL");
    return 0;
}

/* 清理垃圾临时文件 */
static int HCleanJunkAct(uiCmdCmdId command, uiCmdValue *p_value,
                         void *p_push_command_data)
{
    ProPath    dir;
    HCleanStat st;
    wchar_t    title[64], info[PRO_LINE_SIZE];

    (void)command;
    (void)p_value;
    (void)p_push_command_data;

    dir[0] = L'\0';
    if (ProDirectoryCurrentGet(dir) != PRO_TK_NO_ERROR || dir[0] == L'\0')
    {
        HMsg("SESSION_NO_WORKDIR");
        return 0;
    }
    HLogW("HCleanJunkAct: dir = ", dir);

    /* ① 先扫描，把"要删多少个"摆出来 */
    memset(&st, 0, sizeof(st));
    HCleanDirW(dir, 0, &st);
    HLog("HCleanJunkAct: scan found=%d\n", st.found);

    if (st.found <= 0)
    {
        HMsg("SESSION_CLEAN_NONE");
        return 0;
    }

    wcscpy(title, L"清理垃圾");
    swprintf(info, (size_t)PRO_LINE_SIZE,
             L"当前工作目录下找到 %d 个垃圾文件：\n"
             L"trail.txt* / std.out / std.err / *_log.xml / current_session.pro\n"
             L"以及 *.err .inf .idx .xpr .bak .dwl .m_p .lst .tst\n\n"
             L"不会动 *.log、旧版本和配置文件。确定删除吗？",
             st.found);
    HUiInfoW(title, info);

    if (!HConfirmYes("SESSION_CLEAN_ASK"))
    {
        HMsg("SESSION_CANCEL");
        return 0;
    }

    /* ② 真删，计数如实 */
    memset(&st, 0, sizeof(st));
    HCleanDirW(dir, 1, &st);
    HLog("HCleanJunkAct: removed=%d skipped=%d\n", st.removed, st.skipped);

    if (st.skipped > 0)
        swprintf(info, (size_t)PRO_LINE_SIZE,
                 L"已删除 %d 个。\n%d 个被 Creo 占用没删掉"
                 L"（std.out 之类，关掉 Creo 再清一次就行）。",
                 st.removed, st.skipped);
    else
        swprintf(info, (size_t)PRO_LINE_SIZE,
                 L"已删除 %d 个垃圾文件。", st.removed);
    HUiInfoW(title, info);

    HMsg(st.removed > 0 ? "SESSION_CLEAN_OK" : "SESSION_CLEAN_FAIL");
    return 0;
}

/*---------------------------------------------------------------------*\
  绘图比例：1:1 大按钮 + 6 个小的 + 「更多比例」下拉 + 「自定义比例」

  ★ 比例是【图纸级】的，走 ProDrawingScaleSet(drawing, NULL, -1, scale)：
      第 2 参 solid 传 NULL = 用图纸的默认模型（ProDrawing.h 原文）；
      第 3 参 sheet 传 -1 = 当前图纸。
    Creo 的 scale 定义是「图上长度 / 实物长度」，所以 1:3 传 1.0/3.0、
    2.5:1 传 2.5。

  ★ 改完必须 ProDwgSheetRegenerate 重建该图纸的显示列表 —— 只 Repaint
    等于「只擦屏不重算」，视图不会重新缩放（公差刷子踩过同一个坑）。
    该 API 要求目标图纸正显示在窗口里，传 ProDrawingCurrentSheetGet 拿到的
    当前图纸号即可满足。

  ★ 视图若自己设过「自定义比例」(ProDrawingViewScaleIsUserdefined 为真)，
    图纸比例改了它也不会跟着变 —— 这是 Creo 原生行为，不是本命令的问题。

  ★ 所有固定档位共用一个 HScaleAct：action 回调第一个参数就是 uiCmdCmdId
    （ProUICmd.h:134 uiCmdCmdActFn 签名），拿它反查下标即可。

  ★ 自定义比例走消息行输入（ProMessageStringRead），不用 ProUIDialog + .res
    对话框：少一个资源文件、少一类编码风险，和一键导出问文件名同一套交互。
\*---------------------------------------------------------------------*/
typedef struct
{
    const char *name;   /* 命令名，同时是 h_msg.txt 里的标签键 */
    double      scale;  /* 图上长度 / 实物长度 */
} HScaleItem;

static const HScaleItem g_scale_items[] =
{
    /* 前 7 个 = 组里直接显示的按钮（1 大 + 6 小），顺序照 Creo 原生排法 */
    { "H_Scale_1_1",   1.0 },
    { "H_Scale_1_2",   0.5 },
    { "H_Scale_1_3",   1.0 / 3.0 },
    { "H_Scale_1_4",   0.25 },
    { "H_Scale_2_1",   2.0 },
    { "H_Scale_3_1",   3.0 },
    { "H_Scale_4_1",   4.0 },
    /* 其余 18 个由 Creo 自动收进组名下拉栏（见 rbn 注释） */
    { "H_Scale_2_3",   2.0 / 3.0 },
    { "H_Scale_2_5",   0.4 },
    { "H_Scale_1_5",   0.2 },
    { "H_Scale_1_6",   1.0 / 6.0 },
    { "H_Scale_1_8",   0.125 },
    { "H_Scale_1_10",  0.1 },
    { "H_Scale_1_12",  1.0 / 12.0 },
    { "H_Scale_1_15",  1.0 / 15.0 },
    { "H_Scale_1_20",  0.05 },
    { "H_Scale_3_2",   1.5 },
    { "H_Scale_5_2",   2.5 },
    { "H_Scale_5_1",   5.0 },
    { "H_Scale_6_1",   6.0 },
    { "H_Scale_8_1",   8.0 },
    { "H_Scale_10_1",  10.0 },
    { "H_Scale_12_1",  12.0 },
    { "H_Scale_15_1",  15.0 },
    { "H_Scale_20_1",  20.0 }
};

#define H_SCALE_COUNT ((int)(sizeof(g_scale_items) / sizeof(g_scale_items[0])))

static uiCmdCmdId g_scale_cmd[H_SCALE_COUNT];
static uiCmdCmdId g_scale_custom_cmd;

static uiCmdAccessState HScaleAccess(uiCmdAccessMode access_mode)
{
    (void)access_mode;
    return ACCESS_AVAILABLE;
}

/* 设比例 -> 写后回读 -> 重建图纸 -> 刷新。固定档位与自定义共用。 */
static void HScaleApply(ProDrawing drw, double scale, const char *tag)
{
    int      sheet = 1;
    ProError status;
    double   old_scale = 0.0;

    ProDrawingCurrentSheetGet(drw, &sheet);
    ProDrawingScaleGet(drw, NULL, -1, &old_scale);

    status = ProDrawingScaleSet(drw, NULL, -1, scale);
    HLog("HScaleApply: %s sheet=%d  scale %.5f -> %.5f   set -> %d\n",
         tag, sheet, old_scale, scale, (int)status);
    if (status != PRO_TK_NO_ERROR)
    {
        HMsg("SCALE_FAILED");
        return;
    }

    {
        double chk  = 0.0;
        double diff;

        if (ProDrawingScaleGet(drw, NULL, -1, &chk) == PRO_TK_NO_ERROR)
        {
            diff = chk - scale;
            if (diff < 0.0) diff = -diff;
            HLog("HScaleApply: readback %.6f  want %.6f  VERDICT=%s\n",
                 chk, scale, (diff < 1.0e-9) ? "WRITTEN" : "NOT-WRITTEN");
        }
        else
        {
            HLog("HScaleApply: readback FAILED\n");
        }
    }

    status = ProDwgSheetRegenerate(drw, sheet);
    HLog("HScaleApply: sheet regenerate(%d) -> %d\n", sheet, (int)status);

    ProWindowRepaint(PRO_VALUE_UNUSED);
    ProWindowRefresh(PRO_VALUE_UNUSED);

    HMsg("SCALE_OK");
}

static int HScaleAct(uiCmdCmdId command, uiCmdValue *p_value, void *p_push_command_data)
{
    int      i, idx = -1;
    ProError status;
    ProMdl   drawing;

    (void)p_value;
    (void)p_push_command_data;

    for (i = 0; i < H_SCALE_COUNT; i++)
    {
        if (g_scale_cmd[i] == command)
        {
            idx = i;
            break;
        }
    }
    if (idx < 0)
    {
        HLog("HScaleAct: unknown command id %p\n", (void *)command);
        return 0;
    }

    status = HGetDrawing(&drawing);
    if (status != PRO_TK_NO_ERROR)
    {
        HLog("HScaleAct: not a drawing -> %d\n", (int)status);
        HMsg("SCALE_NO_DRAWING");
        return 0;
    }

    HScaleApply((ProDrawing)drawing, g_scale_items[idx].scale,
                g_scale_items[idx].name);
    return 0;
}

/*--------------------------------------------------------------- 自定义比例

  ★ 用消息行输入（ProMessageStringRead），不走 ProUIDialog + .res 对话框：
    少一个资源文件、少一类编码风险，而且和一键导出问文件名是同一个交互。
    返回码：Esc -> PRO_TK_MSG_USER_QUIT，直接回车 -> PRO_TK_GENERAL_ERROR，
    两者都按取消处理，不弹错误。
\*---------------------------------------------------------------------*/
/* "1:2.5" / "2.5:1" / "0.4" / "1/3" / 全角冒号 都能认；scale = a / b */
static int HParseRatio(const wchar_t *w, double *out)
{
    wchar_t  buf[PRO_LINE_SIZE];
    wchar_t *p;
    wchar_t *colon;
    double   a, b;

    if (w == NULL || out == NULL)
        return -1;

    wcsncpy(buf, w, PRO_LINE_SIZE - 1);
    buf[PRO_LINE_SIZE - 1] = L'\0';

    for (p = buf; *p != L'\0'; p++)
    {
        if (*p == L'\uff1a' || *p == L'/' || *p == L'\\')
            *p = L':';
    }

    colon = wcschr(buf, L':');
    if (colon == NULL)
    {
        wchar_t *end = NULL;

        a = wcstod(buf, &end);
        if (end == buf || !(a > 0.0))
            return -1;
        *out = a;
        return 0;
    }

    *colon = L'\0';
    a = wcstod(buf, NULL);
    b = wcstod(colon + 1, NULL);
    if (!(a > 0.0) || !(b > 0.0))
        return -1;

    *out = a / b;
    return 0;
}

static int HScaleCustomAct(uiCmdCmdId command, uiCmdValue *p_value, void *p_push_command_data)
{
    ProError status;
    ProMdl   drawing;
    wchar_t  buf[PRO_LINE_SIZE];
    double   scale = 1.0;
    double   cur   = 0.0;

    (void)command;
    (void)p_value;
    (void)p_push_command_data;

    status = HGetDrawing(&drawing);
    if (status != PRO_TK_NO_ERROR)
    {
        HLog("HScaleCustomAct: not a drawing -> %d\n", (int)status);
        HMsg("SCALE_NO_DRAWING");
        return 0;
    }

    if (ProDrawingScaleGet((ProDrawing)drawing, NULL, -1, &cur) == PRO_TK_NO_ERROR)
        HLog("HScaleCustomAct: current scale=%.6f\n", cur);

    HMsg("SCALE_ASK");
    ProWindowRepaint(PRO_VALUE_UNUSED);

    buf[0] = L'\0';
    status = ProMessageStringRead(PRO_LINE_SIZE - 1, buf);
    ProMessageClear();
    HLog("HScaleCustomAct: read -> %d\n", (int)status);

    if (status != PRO_TK_NO_ERROR)
    {
        HMsg("SCALE_CANCEL");
        return 0;
    }

    if (HParseRatio(buf, &scale) != 0 || !(scale > 0.0001) || !(scale < 10000.0))
    {
        HLog("HScaleCustomAct: bad input, parsed scale=%.6f\n", scale);
        HMsg("SCALE_INVALID");
        return 0;
    }

    HScaleApply((ProDrawing)drawing, scale, "H_ScaleCustom");
    return 0;
}



/*=====================================================================*\
  3D 模型功能（功能区【3D】组）
  ---------------------------------------------------------------------
  · 轮廓尺寸   ProSolidOutlineGet，弹窗报 长/宽/高（模型单位）
  · 显示控制   ProModelitemHide / ProModelitemUnhide / 遍历特征全部显示
  · 重命名     ProMdlnameRename，同名工程图跟着改
  · 随机着色   写到零件外观（装配逐元件→零件，同一零件同色），颜色偏亮
  · 清除着色   对零件外观传 NULL = 恢复默认外观
  · 原位打开   记住装配当前视图矩阵 -> 打开零件 -> 把矩阵套上去
  · 批量备份   ProMdlnameBackup（连带全部从属模型）
\*=====================================================================*/

/* 取当前要操作的 3D 模型：
   1) 当前窗口是零件/装配 -> 就是它
   2) 当前窗口是工程图   -> 退回到该图“当前模型”（出图时不用切窗口）
   3) 其它               -> 失败 */
static ProError HGetSolid3D(ProMdl *p_mdl, ProMdlType *p_type)
{
    ProMdl     mdl = NULL;
    ProMdlType t   = PRO_MDL_UNUSED;

    if (p_mdl == NULL)
        return PRO_TK_BAD_INPUTS;
    *p_mdl = NULL;
    if (p_type != NULL)
        *p_type = PRO_MDL_UNUSED;

    if (ProMdlCurrentGet(&mdl) != PRO_TK_NO_ERROR || mdl == NULL)
        return PRO_TK_BAD_INPUTS;
    if (ProMdlTypeGet(mdl, &t) != PRO_TK_NO_ERROR)
        return PRO_TK_BAD_INPUTS;

    if (t == PRO_MDL_PART || t == PRO_MDL_ASSEMBLY)
    {
        *p_mdl = mdl;
        if (p_type != NULL)
            *p_type = t;
        return PRO_TK_NO_ERROR;
    }

    if (t == PRO_MDL_DRAWING)
    {
        ProSolid solid = NULL;

        if (ProDrawingCurrentsolidGet((ProDrawing)mdl, &solid) == PRO_TK_NO_ERROR
            && solid != NULL)
        {
            ProMdlType st = PRO_MDL_UNUSED;
            if (ProMdlTypeGet((ProMdl)solid, &st) == PRO_TK_NO_ERROR
                && (st == PRO_MDL_PART || st == PRO_MDL_ASSEMBLY))
            {
                HLog("HGetSolid3D: fallback to drawing current solid\n");
                *p_mdl = (ProMdl)solid;
                if (p_type != NULL)
                    *p_type = st;
                return PRO_TK_NO_ERROR;
            }
        }
        HLog("HGetSolid3D: drawing has no current solid\n");
    }
    return PRO_TK_BAD_INPUTS;
}

/* 一个按钮的模态提示框（标题为宽字符字面量，正斜杠不存在转义问题） */
static void HUiInfoW(wchar_t *title, wchar_t *msg)
{
    ProArray            btns = NULL;
    ProUIMessageButton  b    = PRO_UI_MESSAGE_OK;
    ProUIMessageButton  pick = PRO_UI_MESSAGE_OK;

    if (ProArrayAlloc(0, sizeof(ProUIMessageButton), 1, &btns) == PRO_TK_NO_ERROR
        && btns != NULL)
    {
        ProArrayObjectAdd(&btns, -1, 1, &b);
        ProUIMessageDialogDisplay(PROUIMESSAGE_INFO, title, msg,
                                  (ProUIMessageButton *)btns,
                                  PRO_UI_MESSAGE_OK, &pick);
        ProArrayFree(&btns);
    }
    else
    {
        HLog("HUiInfoW: ProArrayAlloc failed, dialog skipped\n");
    }
}

/* 原生选择：过滤器名各版本略有差异，只有“报错”才降级重试，
   用户自己按确定/取消（n=0）不会重复弹框。
   ★ 返回的数组是 Creo 的静态内存，只能置 NULL，绝不许 free。 */
static ProSelection g_hsel[H_SEL_MAX];
static int           g_hsel_n = 0;

/* 可选过滤器的版本：filters 按优先级逐个试，只有"报错"才降级。
   ★★ 过滤器必须是 Creo 认得的名字（见 protk_appls 官方示例）：
      prt_or_asm / part / asm / feature / surface / edge / face / point /
      curve / csys / datum / axis / dimension / annot_elem，可用逗号组合。
   ★ 没有 "any"，也没有空串 —— 传 "" 或 "any" 的结果是"什么都选不上"。 */
static int HSelectItemsF(int max_count, char *tag, char **filters, int nf,
                         ProSelection **p_sels, int *p_n)
{
    int          i, n = 0;
    ProError     status = PRO_TK_BAD_INPUTS;
    ProSelection *sels = NULL;

    if (p_sels != NULL)
        *p_sels = NULL;
    if (p_n != NULL)
        *p_n = 0;

    /* ① 先看"选择缓冲区"：用户先选好对象再点按钮，这是 Creo 的习惯操作。
       缓冲区数组必须 ProSelectionarrayFree（和 ProSelect 的静态数组正好相反）。 */
    sels = NULL;
    status = ProSelbufferSelectionsGet(&sels);
    n = 0;
    if (status == PRO_TK_NO_ERROR && sels != NULL)
        ProArraySizeGet(sels, &n);
    HLog("%s: selbuffer -> %d, n=%d\n", tag, (int)status, n);
    if (status == PRO_TK_NO_ERROR && n > 0)
    {
        int m = 0;

        if (n > H_SEL_MAX)
            n = H_SEL_MAX;
        /* ★★ ProSelectionarrayFree 会**连每个 ProSelection 一起释放**
           （ProSelection.h:266 原文 "also frees each member ProSelection"）。
           只拷指针再 free 数组 ⇒ g_hsel 里全是野指针 ⇒ 一用就崩。
           必须先用 ProSelectionCopy 深拷贝。 */
        for (i = 0; i < n; i++)
        {
            ProSelection copy = NULL;

            if (ProSelectionCopy(sels[i], &copy) == PRO_TK_NO_ERROR
                && copy != NULL)
                g_hsel[m++] = copy;
        }
        ProSelectionarrayFree(sels);
        sels = NULL;
        g_hsel_n = m;
        if (m <= 0)
            return 0;
        if (p_sels != NULL)
            *p_sels = g_hsel;
        if (p_n != NULL)
            *p_n = m;
        return 1;
    }
    if (sels != NULL)
    {
        ProSelectionarrayFree(sels);
        sels = NULL;
    }

    /* ② 没有预选才进入交互选择 */
    HMsg(max_count == 1 ? "TD_PICK_ONE" : "TD_PICK_TIP");
    for (i = 0; i < nf; i++)
    {
        sels = NULL;
        n    = 0;
        status = ProSelect(filters[i], max_count, NULL, NULL, NULL, NULL,
                           &sels, &n);
        HLog("%s: ProSelect(\"%s\") -> %d, n=%d\n", tag, filters[i],
             (int)status, n);
        if (status == PRO_TK_NO_ERROR)
        {
            if (p_sels != NULL)
                *p_sels = sels;
            if (p_n != NULL)
                *p_n = n;
            return (n > 0) ? 1 : 0;
        }
    }
    return 0;
}

/* 只读"选择缓冲区"（不弹交互拾取），给「一键」类命令用：
   用户先选了对象就用它，没选就直接返回 0，不会打断操作。 */
static int HSelbufferOnly(char *tag, ProSelection **p_sels, int *p_n)
{
    ProSelection *sels = NULL;
    ProError      status;
    int           i, n = 0, m = 0;

    if (p_sels != NULL)
        *p_sels = NULL;
    if (p_n != NULL)
        *p_n = 0;

    status = ProSelbufferSelectionsGet(&sels);
    if (status == PRO_TK_NO_ERROR && sels != NULL)
        ProArraySizeGet(sels, &n);
    HLog("%s: selbuffer(only) -> %d, n=%d\n", tag, (int)status, n);
    if (status != PRO_TK_NO_ERROR || sels == NULL || n <= 0)
    {
        if (sels != NULL)
            ProSelectionarrayFree(sels);
        return 0;
    }
    if (n > H_SEL_MAX)
        n = H_SEL_MAX;
    for (i = 0; i < n; i++)
    {
        ProSelection copy = NULL;

        if (ProSelectionCopy(sels[i], &copy) == PRO_TK_NO_ERROR && copy != NULL)
            g_hsel[m++] = copy;
    }
    ProSelectionarrayFree(sels);
    g_hsel_n = m;
    if (m <= 0)
        return 0;
    if (p_sels != NULL)
        *p_sels = g_hsel;
    if (p_n != NULL)
        *p_n = m;
    return 1;
}

/* 默认过滤器组合（原来的行为，所有既有命令都用它） */
static int HSelectItems(int max_count, char *tag,
                        ProSelection **p_sels, int *p_n)
{
    static char *filters[3] = { "prt_or_asm",
                                "part,asm,feature",
                                "feature" };

    return HSelectItemsF(max_count, tag, filters, 3, p_sels, p_n);
}

/* 释放 HSelectItems 从"选择缓冲区"深拷出来的那批 ProSelection。
   ProSelect 返回的是 Creo 静态内存（绝不能 free），所以只处理 == g_hsel 的情况。 */
static void HSelectFree(ProSelection *sels)
{
    int i;

    if (sels != g_hsel || g_hsel_n <= 0)
        return;
    for (i = 0; i < g_hsel_n; i++)
    {
        if (g_hsel[i] != NULL)
        {
            ProSelection s = g_hsel[i];

            ProSelectionFree(&s);
            g_hsel[i] = NULL;
        }
    }
    g_hsel_n = 0;
}

/*----------------------------- 1. 轮廓尺寸 -----------------------------*/
static int HOutlineAct(uiCmdCmdId command, uiCmdValue *p_value,
                       void *p_push_command_data)
{
    ProMdl     mdl  = NULL;
    ProMdlType type = PRO_MDL_UNUSED;
    Pro3dPnt   ext[2];
    ProError   status;
    double     dx, dy, dz;
    ProMdlName nm;
    wchar_t    msg[900];

    (void)command;
    (void)p_value;
    (void)p_push_command_data;

    if (HGetSolid3D(&mdl, &type) != PRO_TK_NO_ERROR)
    {
        HLog("HOutlineAct: no 3D model\n");
        HMsg("TD_NOT_3D");
        return 0;
    }

    memset(ext, 0, sizeof(ext));
    status = ProSolidOutlineGet((ProSolid)mdl, ext);
    HLog("HOutlineAct: outlineGet -> %d\n", (int)status);
    if (status != PRO_TK_NO_ERROR)
    {
        ProMatrix mtx;
        int       i, j;

        for (i = 0; i < 4; i++)
            for (j = 0; j < 4; j++)
                mtx[i][j] = (i == j) ? 1.0 : 0.0;
        status = ProSolidOutlineCompute((ProSolid)mdl, mtx, NULL, 0, ext);
        HLog("HOutlineAct: outlineCompute -> %d\n", (int)status);
    }
    if (status != PRO_TK_NO_ERROR)
    {
        HMsg("TD_OUTLINE_FAIL");
        return 0;
    }

    dx = fabs(ext[1][0] - ext[0][0]);
    dy = fabs(ext[1][1] - ext[0][1]);
    dz = fabs(ext[1][2] - ext[0][2]);
    HLog("HOutlineAct: dx=%.3f dy=%.3f dz=%.3f\n", dx, dy, dz);

    nm[0] = L'\0';
    ProMdlMdlnameGet(mdl, nm);
    swprintf(msg, sizeof(msg) / sizeof(msg[0]),
             L"%ls\n\nX = %.3f\nY = %.3f\nZ = %.3f\n\n(unit: model unit)",
             nm, dx, dy, dz);
    HUiInfoW(L"Outline / 轮廓尺寸", msg);
    return 0;
}

/*-------------- 3. 复制模型名称到剪贴板（2026-10-23 新增） -------------
   用途：把当前模型的名字（含扩展名，如 PRT0006.PRT）放进 Windows 剪贴板，
         贴到 Excel、邮件、资源管理器重命名框里都不用再手敲。
         取的就是「当前活动模型」：装配复制装配名，零件复制零件名，图纸复制图纸名。
         不看选择集 —— 手边选着什么不影响结果，装配里也一样。

   ★ 剪贴板三条硬规矩（踩过的人不少，这里一次性写死）：
     1) OpenClipboard(NULL) 挂到调用线程自己的窗口；插件命令跑在 Creo 主
        线程，能拿到。返回 FALSE 说明剪贴板正被别的程序占用（Office、输入法、
        剪贴板管理器都比想象中频繁），等 60ms 重试一次，还不行才报失败。
     2) ★ SetClipboardData 成功后，那块 HGLOBAL 的所有权就交给系统了，
        绝对不能再 GlobalFree —— 否则系统剪贴板内容被提前释放，之后每次
        粘贴都可能随机失败，且不报错。
     3) 只放 CF_UNICODETEXT。用 CF_TEXT 的话中文模型名会变成乱码。
   ------------------------------------------------------------------ */
static int HClipPutW(const wchar_t *s)
{
    HGLOBAL hg;
    size_t  cb;
    LPVOID  p;
    int     ok = 0;

    if (s == NULL || s[0] == L'\0')
        return 0;

    cb = (wcslen(s) + 1) * sizeof(wchar_t);
    hg = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, cb);
    if (hg == NULL)
    {
        HLog("HClipPutW: GlobalAlloc(%u) failed\n", (unsigned)cb);
        return 0;
    }

    p = GlobalLock(hg);
    if (p == NULL)
    {
        HLog("HClipPutW: GlobalLock failed\n");
        GlobalFree(hg);
        return 0;
    }
    memcpy(p, s, cb);
    GlobalUnlock(hg);

    if (!OpenClipboard(NULL))
    {
        /* 剪贴板经常被别的程序瞬间占着（Office、输入法、各种剪贴板管理器），
           等 60ms 再试一次命中率明显提高；仍然失败才认输。 */
        HLog("HClipPutW: OpenClipboard busy, retry after 60ms\n");
        Sleep(60);
        if (!OpenClipboard(NULL))
        {
            HLog("HClipPutW: OpenClipboard failed twice, err=%lu\n",
                 (unsigned long)GetLastError());
            GlobalFree(hg);
            return 0;
        }
    }

    /* 必须先 EmptyClipboard，否则旧内容一直留着，粘贴出来还是上一次的 */
    if (EmptyClipboard() && SetClipboardData(CF_UNICODETEXT, hg) != NULL)
    {
        ok = 1;
    }
    else
    {
        HLog("HClipPutW: empty/set failed, err=%lu\n",
             (unsigned long)GetLastError());
    }
    CloseClipboard();

    if (!ok)
        GlobalFree(hg);         /* 只有没交出去才由自己回收 */

    HLog("HClipPutW: len=%d ok=%d\n", (int)wcslen(s), ok);
    return ok;
}

static int HCopyNameAct(uiCmdCmdId command, uiCmdValue *p_value,
                        void *p_push_command_data)
{
    ProMdl         mdl = NULL;
    ProMdlType     type = PRO_MDL_UNUSED;
    ProMdlName     nm;
    ProMdlExtension ext;
    ProPath        origin;
    wchar_t        full[PRO_MDLNAME_SIZE + PRO_MDLEXTENSION_SIZE + 4];
    wchar_t        msg[1400];
    const wchar_t *ts = L"模型";
    int            ok;

    (void)command;
    (void)p_value;
    (void)p_push_command_data;

    /* 只认当前活动模型，不看选择集 —— 用户不该因为手边恰好还选着某个元件，
       就复制到别的名字。装配复制装配名，零件复制零件名，
       工程图就复制图纸名（DRW…），不取图纸背后的模型。
       ★ 与同组另两个按钮「重命名」「轮廓尺寸」不同：那两个走 HGetSolid3D，
         在图纸里会自动落到图纸背后的模型上；这里刻意不用它 —— 用户要的就是
         图纸本身的名字。 */
    if (ProMdlCurrentGet(&mdl) != PRO_TK_NO_ERROR)
        mdl = NULL;

    if (mdl == NULL)
    {
        HLog("HCopyNameAct: no current model\n");
        HUiInfoW(L"Copy Name / 复制名称",
                 L"当前没有打开的模型。\n\n请先打开一个零件 / 装配 / 工程图再点这个按钮。");
        return 0;
    }

    nm[0]  = L'\0';
    ext[0] = L'\0';
    if (ProMdlMdlnameGet(mdl, nm) != PRO_TK_NO_ERROR || nm[0] == L'\0')
    {
        HLog("HCopyNameAct: ProMdlMdlnameGet failed\n");
        HUiInfoW(L"Copy Name / 复制名称", L"取不到模型名称，详见 h_load.log。");
        return 0;
    }

    /* 扩展名拿不到不影响主体，退化成纯名 */
    if (ProMdlExtensionGet(mdl, ext) != PRO_TK_NO_ERROR)
        ext[0] = L'\0';

    /* 统一成 NAME.EXT。
       ProMdlExtensionGet 一般回 "PRT"（不带点），但个别版本/多 CAD 模型
       会带点或带回空，所以这里两种都兼容，避免出现 "A..PRT" 或 "A.PRT.PRT" */
    if (ext[0] == L'\0')
        swprintf(full, sizeof(full) / sizeof(full[0]), L"%ls", nm);
    else if (ext[0] == L'.')
        swprintf(full, sizeof(full) / sizeof(full[0]), L"%ls%ls", nm, ext);
    else
        swprintf(full, sizeof(full) / sizeof(full[0]), L"%ls.%ls", nm, ext);

    if (ProMdlTypeGet(mdl, &type) == PRO_TK_NO_ERROR)
    {
        switch (type)
        {
        case PRO_MDL_PART:     ts = L"零件"; break;
        case PRO_MDL_ASSEMBLY: ts = L"装配"; break;
        case PRO_MDL_DRAWING:  ts = L"工程图"; break;
        case PRO_MDL_LAYOUT:   ts = L"布局"; break;
        default:               ts = L"模型"; break;
        }
    }

    origin[0] = L'\0';
    if (ProMdlOriginGet(mdl, origin) != PRO_TK_NO_ERROR)
        origin[0] = L'\0';

    ok = HClipPutW(full);
    HLogW("HCopyNameAct: copied name = ", full);

    if (ok)
    {
        swprintf(msg, sizeof(msg) / sizeof(msg[0]),
                 L"已复制到剪贴板：\n\n%ls\n\n类型：%ls\n目录：%ls",
                 full, ts,
                 (origin[0] != L'\0') ? origin : L"（尚未保存到磁盘）");
    }
    else
    {
        swprintf(msg, sizeof(msg) / sizeof(msg[0]),
                 L"复制失败：剪贴板被其他程序占用。\n\n名称是：%ls\n\n"
                 L"等一秒把正在复制的窗口关掉，再点一次即可。",
                 full);
    }

    HUiInfoW(L"Copy Name / 复制名称", msg);
    return 0;
}

/*-------------------------- 2. 隐藏 / 取消隐藏 --------------------------*/
/*----------------------- 2. 显示控制（4 个命令） -----------------------
  ★ ProModelitemHide/Unhide 的参数是【层项目】。官方示例
    pt_geardesign/GearDesign.c 里就是 ProModelitemInit(..., PRO_CURVE, ...)
    再 ProModelitemHide，隐藏的是曲线。
  ★ 而从 ProSelectionModelitemGet 取到的 item 常常是"零件模型"(PRO_PART)，
    对装配元件会返回 PRO_TK_INVALID_TYPE。
  ⇒ 先直连试一次；失败就用 ProSelectionAsmcomppathGet 拿元件路径，
    重建成 type=PRO_FEAT_COMPONENT / owner=装配 / id=元件id 的 ProAsmcomp 再试。
\*----------------------------------------------------------------------*/
#define H_VIS_HIDE   0
#define H_VIS_SHOW   1
#define H_VIS_TOGGLE 2

static ProError HHideApply(ProModelitem *item, int mode)
{
    ProBoolean hidden = PRO_B_FALSE;
    ProError   e;

    if (item == NULL)
        return PRO_TK_BAD_INPUTS;

    if (mode == H_VIS_TOGGLE)
    {
        /* 反隐藏 = 切换。
           ★ ProModelitemIsHidden 对装配元件并不可靠（经常恒回 FALSE），
             所以再加一层：Hide 若返回 NO_CHANGE，说明它本来就是隐藏态，
             那就改做 Unhide —— 用返回值兜底，不单纯依赖 IsHidden。 */
        e = ProModelitemIsHidden(item, &hidden);
        HLog("HHideApply: toggle isHidden -> %d, hidden=%d\n", (int)e,
             (int)hidden);

        if (e == PRO_TK_NO_ERROR && hidden == PRO_B_TRUE)
        {
            e = ProModelitemUnhide(item);
            HLog("HHideApply: toggle UNHIDE -> %d\n", (int)e);
            return e;
        }

        e = ProModelitemHide(item);
        HLog("HHideApply: toggle HIDE -> %d\n", (int)e);
        if (e == PRO_TK_NO_CHANGE)          /* 其实已经是隐藏态了 */
        {
            e = ProModelitemUnhide(item);
            HLog("HHideApply: toggle HIDE=NO_CHANGE -> retry UNHIDE -> %d\n",
                 (int)e);
        }
        return e;
    }

    if (mode == H_VIS_HIDE)
        return ProModelitemHide(item);
    return ProModelitemUnhide(item);
}

/* 按元件 id 在装配里找到对应的元件 feature，再隐藏它本身 */
typedef struct h_find_feat_ctx
{
    int      id;
    int      mode;
    int      found;
    ProError err;
} HFindFeatCtx;

static ProError HFindFeatVisit(ProFeature *feat, ProError status,
                               ProAppData app)
{
    HFindFeatCtx *fc = (HFindFeatCtx *)app;
    ProFeattype   ftype = 0;

    if (status != PRO_TK_NO_ERROR || feat == NULL || fc == NULL)
        return PRO_TK_NO_ERROR;
    if (feat->id != fc->id)
        return PRO_TK_NO_ERROR;
    if (ProFeatureTypeGet(feat, &ftype) != PRO_TK_NO_ERROR
        || ftype != PRO_FEAT_COMPONENT)
        return PRO_TK_NO_ERROR;

    fc->found++;
    fc->err = HHideApply((ProModelitem *)feat, fc->mode);
    return PRO_TK_NO_ERROR;
}

static int HVisFromSelection(ProSelection sel, int mode, char *tag)
{
    ProModelitem   item;
    ProAsmcomppath path;
    ProAsmcomp     comp;
    ProError       e;

    memset(&item, 0, sizeof(item));
    if (ProSelectionModelitemGet(sel, &item) == PRO_TK_NO_ERROR)
    {
        e = HHideApply(&item, mode);
        HLog("%s: direct type=%d id=%d -> %d\n", tag, (int)item.type,
             item.id, (int)e);
        if (e == PRO_TK_NO_ERROR || e == PRO_TK_NO_CHANGE)
            return 1;
    }

    memset(&path, 0, sizeof(path));
    if (ProSelectionAsmcomppathGet(sel, &path) == PRO_TK_NO_ERROR
        && path.table_num > 0 && path.owner != NULL)
    {
        int comp_id = path.comp_id_table[path.table_num - 1];

        /* ② ★ 实测只有 PRO_FEATURE(3) 被 ProModelitemHide/Unhide 接受；
              PRO_FEAT_COMPONENT(1000) 一定回 -18 INVALID_TYPE，别再试了。
              Creo 内部元件就是一个 feature，id 用 comp id。 */
        memset(&comp, 0, sizeof(comp));
        comp.type  = PRO_FEATURE;
        comp.id    = comp_id;
        comp.owner = (ProMdl)path.owner;
        e = HHideApply((ProModelitem *)&comp, mode);
        HLog("%s: asfeat(3) id=%d depth=%d -> %d\n", tag, comp_id,
             path.table_num, (int)e);
        if (e == PRO_TK_NO_ERROR || e == PRO_TK_NO_CHANGE)
            return 1;

        /* ③ 兜底：遍历装配特征，按 id 找到元件 feature 本身再隐藏 */
        {
            HFindFeatCtx fc;

            memset(&fc, 0, sizeof(fc));
            fc.id   = comp_id;
            fc.mode = mode;
            ProSolidFeatVisit((ProSolid)path.owner, HFindFeatVisit, NULL,
                              (ProAppData)&fc);
            HLog("%s: byvisit id=%d found=%d -> %d\n", tag, comp_id,
                 fc.found, (int)fc.err);
            if (fc.found > 0
                && (fc.err == PRO_TK_NO_ERROR || fc.err == PRO_TK_NO_CHANGE))
                return 1;
        }
    }
    return 0;
}

/* 显示控制（隐藏/显示/反隐藏/全部隐藏/全部显示）每次操作后统一跑一次重新生成，
   再刷左侧模型树。用户 2026-10-22 要求："每次操作后都自动执行一次重新生成"，
   目的就是让左边状态树跟着刷新。
   ★ ProSolidRegenerate 只重算模型，**不会**刷模型树；刷树要用
     ProTreetoolRefresh(ProMdl)（ProMdl.h:1324）。官方在 ProAsmcomp.h:630 的
     See Also 里把 ProWindowRepaint + ProTreetoolRefresh 列为改元件后的标配。 */
static void HVisRegenerate(char *tag)
{
    ProMdl     mdl  = NULL;
    ProMdlType type = PRO_MDL_UNUSED;
    ProError   e;

    if (HGetSolid3D(&mdl, &type) != PRO_TK_NO_ERROR || mdl == NULL)
        return;
    e = ProSolidRegenerate((ProSolid)mdl, PRO_REGEN_NO_FLAGS);
    HLog("%s: regenerate -> %d\n", tag, (int)e);
    e = ProTreetoolRefresh(mdl);
    HLog("%s: treeRefresh -> %d\n", tag, (int)e);
}

/* ============ 「选中就生效」的连点拾取 ============
   2026-10-23 用户要求：隐藏/显示"不要确认这一步，选中就隐藏"。
   ProSelect 默认在选满之后还要等用户按中键（Done），这一步无法从
   ProSelect 的参数里去掉，但可以用【选择环境】关掉：
     · PRO_SELECT_DONE_REQUIRED = 0  ⇒ 选满 max_count 立即返回，不等 Done
     · PRO_SELECT_HIDE_SEL_DLG = 1   ⇒ 不弹"选择"浮动面板（那个也要确认）
   两个属性在 ProSelection.h:167-172 的 ProSelectionEnvAttr 里，
   用 ProSelectionEnvAlloc(array, count, &env) 组装（注意是普通数组 + 个数，
   不是 ProArray），用完 ProSelectionEnvFree。
   有了 DONE_REQUIRED=0，每轮只选 1 个就立刻返回 ⇒ 点中即隐藏；
   外面套一层循环 ⇒ 连点连藏，ESC / 中键退出。 */
#define H_VIS_PICK_MAX  200

static void HVisPickLoop(int mode, char *tag, int *p_ok, int *p_fail)
{
    static char *filters[3] = { "prt_or_asm",
                                "part,asm,feature",
                                "feature" };
    ProSelectionEnvOption envopts[2];
    ProSelectionEnv       env = NULL;
    int                   i, f;

    envopts[0].attribute = PRO_SELECT_DONE_REQUIRED;
    envopts[0].value     = PRO_B_FALSE;      /* 不等 Done */
    envopts[1].attribute = PRO_SELECT_HIDE_SEL_DLG;
    envopts[1].value     = PRO_B_TRUE;       /* 不弹选择面板 */
    if (ProSelectionEnvAlloc(envopts, 2, &env) != PRO_TK_NO_ERROR)
        env = NULL;
    HLog("%s: pickEnv -> %s\n", tag, env != NULL ? "ok" : "alloc failed");

    for (i = 0; i < H_VIS_PICK_MAX; i++)
    {
        ProSelection *sels   = NULL;
        ProError      status = PRO_TK_BAD_INPUTS;
        int           n      = 0;

        /* 过滤器按优先级降级：只有"报错"才换下一个 */
        for (f = 0; f < 3; f++)
        {
            sels   = NULL;
            n      = 0;
            status = ProSelect(filters[f], 1, NULL, NULL, env, NULL,
                               &sels, &n);
            HLog("%s: loop ProSelect(\"%s\") -> %d, n=%d\n", tag, filters[f],
                 (int)status, n);
            if (status == PRO_TK_NO_ERROR)
                break;
        }
        if (status != PRO_TK_NO_ERROR || n <= 0 || sels == NULL)
            break;                            /* ESC / Quit / 什么都没选 */

        if (HVisFromSelection(sels[0], mode, tag))
            (*p_ok)++;
        else
            (*p_fail)++;

        sels = NULL;                          /* ProSelect 静态内存，只能置空 */
        ProWindowRepaint(PRO_VALUE_UNUSED);   /* 立刻看到效果 */
    }

    if (env != NULL)
        ProSelectionEnvFree(env);
}

static int HVisRun(int mode, char *tag)
{
    ProSelection *sels = NULL;
    int           n = 0, i, ok = 0, fail = 0;

    /* ① 先看预选：用户在图上选好再点按钮 ⇒ 直接处理，一个框都不弹 */
    if (HSelbufferOnly(tag, &sels, &n))
    {
        HLog("%s: use preselect n=%d\n", tag, n);
    }
    else
    {
        /* ② 没有预选：进入连点模式，选中即生效 */
        HMsg(mode == H_VIS_SHOW ? "TD_PICK_SHOW_TIP" : "TD_PICK_HIDE_TIP");
        HVisPickLoop(mode, tag, &ok, &fail);
        sels = NULL;
        n    = 0;
        goto finish;
    }

    for (i = 0; i < n; i++)
    {
        if (HVisFromSelection(sels[i], mode, tag))
            ok++;
        else
            fail++;
    }
    HSelectFree(sels);
    sels = NULL;

finish:
    HVisRegenerate(tag);
    ProWindowRepaint(PRO_VALUE_UNUSED);
    HLog("%s: ok=%d fail=%d mode=%d\n", tag, ok, fail, mode);

    /* ★ 反隐藏(SHOW)成功/失败要弹自己的文案：TD_UNHIDE_OK/FAIL 早在消息表里
       备好了（"已恢复显示"/"恢复显示失败"），以前误用 TD_HIDE_* 会显示
       "已隐藏选中对象"——话术指错。 */
    if (ok <= 0)
        HMsg(mode == H_VIS_TOGGLE ? "TD_TOGGLE_FAIL"
                                  : (mode == H_VIS_SHOW ? "TD_UNHIDE_FAIL"
                                                        : "TD_HIDE_FAIL"));
    else
        HMsg(mode == H_VIS_TOGGLE ? "TD_TOGGLE_OK"
                                  : (mode == H_VIS_SHOW ? "TD_UNHIDE_OK"
                                                        : "TD_HIDE_OK"));
    return 0;
}

static int HHideAct(uiCmdCmdId command, uiCmdValue *p_value,
                    void *p_push_command_data)
{
    (void)command;
    (void)p_value;
    (void)p_push_command_data;
    return HVisRun(H_VIS_HIDE, "HHideAct");
}

static int HUnhideAct(uiCmdCmdId command, uiCmdValue *p_value,
                      void *p_push_command_data)
{
    (void)command;
    (void)p_value;
    (void)p_push_command_data;
    return HVisRun(H_VIS_SHOW, "HUnhideAct");
}

static int HVisAllRun(int mode, char *tag);   /* 定义在后面：全部隐藏/显示 */

static int HToggleHideAct(uiCmdCmdId command, uiCmdValue *p_value,
                          void *p_push_command_data)
{
    (void)command;
    (void)p_value;
    (void)p_push_command_data;
    /* ★ 反隐藏 = 全模型批量切换（用户 2026-10-22 明确定义）：
       所有隐藏的对象显示出来、当前显示的对象隐藏掉；
       不再走"选中单一对象"的路径。 */
    return HVisAllRun(H_VIS_TOGGLE, "HToggleHideAct");
}

/* 全部隐藏 / 全部显示 / 反隐藏（全模型切换）：遍历模型里所有特征，
   元件按 feat 自身的 PRO_FEATURE 类型直接处理 */
typedef struct h_vis_all_ctx
{
    int mode;
    int ok;
    int fail;
    int seen;
    int comps;
} HVisAllCtx;

static ProError HVisAllVisit(ProFeature *feat, ProError status, ProAppData app)
{
    HVisAllCtx *ctx = (HVisAllCtx *)app;
    ProFeattype ftype = 0;
    ProError    e;

    if (status != PRO_TK_NO_ERROR || feat == NULL || ctx == NULL)
        return PRO_TK_NO_ERROR;
    ctx->seen++;

    /* ★ feat->type 本身就是 PRO_FEATURE(3)，实测这个 type 才被
       ProModelitemHide/Unhide 接受；拼成 PRO_FEAT_COMPONENT(1000) 会回 -18 INVALID_TYPE。
       所以直接用 feat，不再重建 ProAsmcomp。 */
    if (ProFeatureTypeGet(feat, &ftype) == PRO_TK_NO_ERROR
        && ftype == PRO_FEAT_COMPONENT)
        ctx->comps++;
    else if (ctx->mode == H_VIS_TOGGLE)
        return PRO_TK_NO_ERROR;  /* ★ 反隐藏只翻元件（用户 2026-10-22），
                                    基准面/坐标系等非元件特征一律不动 */

    e = HHideApply((ProModelitem *)feat, ctx->mode);

    if (e == PRO_TK_NO_ERROR)
        ctx->ok++;
    else if (e != PRO_TK_NO_CHANGE)
        ctx->fail++;
    return PRO_TK_NO_ERROR;
}

static int HVisAllRun(int mode, char *tag)
{
    ProMdl     mdl  = NULL;
    ProMdlType type = PRO_MDL_UNUSED;
    HVisAllCtx ctx;

    if (HGetSolid3D(&mdl, &type) != PRO_TK_NO_ERROR)
    {
        HMsg("TD_NOT_3D");
        return 0;
    }

    memset(&ctx, 0, sizeof(ctx));
    ctx.mode = mode;
    ProSolidFeatVisit((ProSolid)mdl, HVisAllVisit, NULL, (ProAppData)&ctx);
    HLog("%s: seen=%d comps=%d ok=%d fail=%d\n", tag, ctx.seen, ctx.comps,
         ctx.ok, ctx.fail);

    HVisRegenerate(tag);
    ProWindowRepaint(PRO_VALUE_UNUSED);
    if (mode == H_VIS_HIDE)
        HMsg(ctx.ok > 0 ? "TD_HIDEALL_OK" : "TD_HIDEALL_NONE");
    else if (mode == H_VIS_TOGGLE)
        HMsg(ctx.ok > 0 ? "TD_TOGGLE_OK" : "TD_TOGGLE_FAIL");
    else
        HMsg(ctx.ok > 0 ? "TD_UNHIDE_OK" : "TD_UNHIDE_FAIL");
    return 0;
}

static int HHideAllAct(uiCmdCmdId command, uiCmdValue *p_value,
                       void *p_push_command_data)
{
    (void)command;
    (void)p_value;
    (void)p_push_command_data;
    return HVisAllRun(H_VIS_HIDE, "HHideAllAct");
}

static int HUnhideAllAct(uiCmdCmdId command, uiCmdValue *p_value,
                         void *p_push_command_data)
{
    (void)command;
    (void)p_value;
    (void)p_push_command_data;
    return HVisAllRun(H_VIS_SHOW, "HUnhideAllAct");
}

/*---------------------------- 3. 重命名 -------------------------------*/
static int HRename3DAct(uiCmdCmdId command, uiCmdValue *p_value,
                        void *p_push_command_data)
{
    ProMdl     mdl  = NULL;
    ProMdlType type = PRO_MDL_UNUSED;
    ProMdlName old_name, new_name;
    wchar_t    buf[PRO_LINE_SIZE];
    ProError   status;
    ProMdl     drw = NULL;

    (void)command;
    (void)p_value;
    (void)p_push_command_data;

    if (HGetSolid3D(&mdl, &type) != PRO_TK_NO_ERROR)
    {
        HMsg("TD_NOT_3D");
        return 0;
    }

    old_name[0] = L'\0';
    ProMdlMdlnameGet(mdl, old_name);
    HLogW("HRename3DAct: old = ", old_name);

    HMsg("TD_RENAME_ASK");
    ProWindowRepaint(PRO_VALUE_UNUSED);

    buf[0] = L'\0';
    status = ProMessageStringRead(PRO_MDLNAME_SIZE - 1, buf);
    ProMessageClear();
    HLog("HRename3DAct: read -> %d\n", (int)status);
    if (status != PRO_TK_NO_ERROR || buf[0] == L'\0')
    {
        HMsg("TD_CANCEL");
        return 0;
    }

    wcsncpy(new_name, buf, PRO_MDLNAME_SIZE - 1);
    new_name[PRO_MDLNAME_SIZE - 1] = L'\0';

    status = ProMdlnameRename(mdl, new_name);
    HLog("HRename3DAct: rename model -> %d\n", (int)status);
    if (status != PRO_TK_NO_ERROR)
    {
        HMsg("TD_RENAME_FAIL");
        return 0;
    }

    /* 同名工程图跟着改（找不到就算了，不影响零件本身） */
    if (old_name[0] != L'\0'
        && ProMdlnameInit(old_name, PRO_MDL_DRAWING, &drw) == PRO_TK_NO_ERROR
        && drw != NULL)
    {
        status = ProMdlnameRename(drw, new_name);
        HLog("HRename3DAct: rename drawing -> %d\n", (int)status);
    }

    ProWindowRepaint(PRO_VALUE_UNUSED);
    HMsg("TD_RENAME_OK");
    return 0;
}

/*---------------- 4/5. 随机着色 / 清除着色 ----------------*/
/* 2026-10-22 定稿（用户原话）：「装配着色和零件着色合并，改名随机着色，
   从零件层直接随机着色，随机颜色范围调亮点，清除着色直接清除零件着色」。
   ⇒ 组件级（元件级）外观在 Creo 12.4 实测 7 种 ProAsmitem 构造全回
   -18(INVALID_TYPE)（含官方 ProAsmcompAsmitemInit 的产出、顶层元件空 path），
   这条路彻底放弃。着色一律写到【零件模型】外观：
     当前模型是零件 → 直接给该零件上色；
     当前模型是装配 → 遍历元件（子装配递归），对元件引用的零件模型上色，
                       同一零件的多个实例拿同一个颜色。
   代价：颜色存在零件里 ⇒ 零件也要存盘才永久保留（提示语已写明）。 */
#define H_COLOR_MAX_MDL 512
#define H_COLOR_RANDOM  0     /* 随机着色：每个零件一个随机色 */
#define H_COLOR_CLEAR   1     /* 清除着色：恢复默认外观 */

typedef struct h_color_ctx
{
    int    mode;                         /* H_COLOR_RANDOM / H_COLOR_CLEAR */
    ProMdl done[H_COLOR_MAX_MDL];        /* 已分配的零件（同一零件多实例同色） */
    ProVector done_rgb[H_COLOR_MAX_MDL]; /* 每个零件固定的随机色 */
    int    applied[H_COLOR_MAX_MDL];     /* 本次命令里已设过外观的零件（去重） */
    int    n_done;
    int    fail;
    int    seen_feat;                    /* 遍历到的特征数（诊断） */
    int    comp_ok;                      /* 认出来的元件数（诊断） */
    int    part_ok;                      /* 写成功的零件数（成功判定） */
    double hue_base;                     /* 本次命令的色相起点（随机） */
    int    hue_idx;                      /* 色相缓存针（黄金角步进） */
} HColorCtx;

static void HHsvToRgb(double h, double s, double v, ProVector rgb)
{
    double c, x, m, r = 0.0, g = 0.0, b = 0.0;
    int    seg;

    h = h - floor(h);
    c = v * s;
    x = c * (1.0 - fabs(fmod(h * 6.0, 2.0) - 1.0));
    m = v - c;
    seg = (int)(h * 6.0);
    if (seg < 0) seg = 0;
    if (seg > 5) seg = 5;
    switch (seg)
    {
    case 0:  r = c; g = x; b = 0; break;
    case 1:  r = x; g = c; b = 0; break;
    case 2:  r = 0; g = c; b = x; break;
    case 3:  r = 0; g = x; b = c; break;
    case 4:  r = x; g = 0; b = c; break;
    default: r = c; g = 0; b = x; break;
    }
    rgb[0] = r + m;
    rgb[1] = g + m;
    rgb[2] = b + m;
}

/* ============ 随机色生成 ============
   2026-10-23 改版（用户反馈「颜色有点暗，随机性不够强」）：
   ① 色相不再纯 rand —— 改成【黄金角步进】0.6180339887 铺开：
      n 个零件天然均匀分布在色环上，任意相邻两个相差约 222°，不会撞色、
      也不会出现「一片同色系」；起点 hue_base 每次命令随机 ⇒ 每次点整体配色不同。
   ② 饱和度提到 0.55~1.00（原 0.50~0.89）、明度拉到 0.95~1.00，更艳、区分更明显。
   ③ 感知亮度保底：HSV 的 V 是 max(R,G,B)，纯蓝 V=1 时 Rec.709 luma 只有 0.07，
      看起来就是「暗」。这里按 luma 给暗色补白，补到 0.42 为止；补白量封顶 0.45，
      避免蓝紫全部变成粉彩、反而更没区分度。
   ④ 每会话只 srand 一次（原有逻辑保留）：连点（<16ms）不会落同一 tick 复读。 */
#define H_COLOR_HUE_STEP   0.618033988749895   /* 黄金分割共轭 */
#define H_COLOR_MIN_LUMA   0.50                /* 感知亮度下限 */
#define H_COLOR_MAX_WHITE  0.55                /* 补白量上限（防粉彩化） */

static int h_color_seeded = 0;

static void HColorSeedOnce(void)
{
    if (!h_color_seeded)
    {
        srand((unsigned)GetTickCount());
        h_color_seeded = 1;
    }
}

static double HLuma(ProVector c)
{
    return 0.2126 * c[0] + 0.7152 * c[1] + 0.0722 * c[2];
}

static void HColorRandRgb(HColorCtx *ctx, ProVector rgb)
{
    double h, s, v, lum, t;
    int    i;

    /* 1) 色相：黄金角铺开 + ±7° 抖动（抖动要远小于步进，不破坏均匀性） */
    h  = ctx->hue_base + H_COLOR_HUE_STEP * (double)ctx->hue_idx;
    h += ((double)(rand() % 400) / 10000.0) - 0.02;
    ctx->hue_idx++;
    h = h - floor(h);

    /* 2) 饱和度 / 明度：高饱和 + 高亮 */
    s = 0.55 + (double)(rand() % 450) / 1000.0;   /* 0.550 ~ 0.999 */
    v = 0.95 + (double)(rand() % 60) / 1000.0;    /* 0.950 ~ 1.000 */
    if (s > 1.0) s = 1.0;
    if (v > 1.0) v = 1.0;

    HHsvToRgb(h, s, v, rgb);

    /* 3) 感知亮度保底 */
    lum = HLuma(rgb);
    if (lum < H_COLOR_MIN_LUMA && lum < 0.999)
    {
        t = (H_COLOR_MIN_LUMA - lum) / (1.0 - lum);
        if (t > H_COLOR_MAX_WHITE)
            t = H_COLOR_MAX_WHITE;
        for (i = 0; i < 3; i++)
            rgb[i] = rgb[i] + t * (1.0 - rgb[i]);
    }

    HLog("HColorRandRgb: h=%.3f s=%.3f v=%.3f luma=%.3f -> rgb=(%.3f %.3f %.3f)\n",
         h, s, v, lum, rgb[0], rgb[1], rgb[2]);
}

/* 按 idx 组装一套外观属性（颜色来自 done_rgb[idx]，同一零件同色） */
static void HColorBuildProps(HColorCtx *ctx, int idx,
                             ProSurfaceAppearanceProps *props)
{
    int k = (idx >= 0 && idx < ctx->n_done) ? idx : 0;

    memset(props, 0, sizeof(*props));
    /* 2026-10-23：用户反馈偏暗 ⇒ 环境光从 0.30 提到 0.55、漫反射降到 0.45
       （两者仍合计 1.0）。ambient 不受光照方向影响，背光面不会再发黑，
       这是颜色看着「暗」的主要来源之一。 */
    props->ambient      = 0.55;
    props->diffuse      = 0.45;
    props->highlite     = 0.30;
    props->shininess    = 0.20;
    props->transparency = 0.00;
    props->reflection   = 0.00;

    props->color_rgb[0] = ctx->done_rgb[k][0];
    props->color_rgb[1] = ctx->done_rgb[k][1];
    props->color_rgb[2] = ctx->done_rgb[k][2];
    props->highlight_color[0] = ctx->done_rgb[k][0];
    props->highlight_color[1] = ctx->done_rgb[k][1];
    props->highlight_color[2] = ctx->done_rgb[k][2];
}

/* 零件 → 颜色序号。同一零件的多个实例拿到同一个序号 = 同色；都返回 >= 0。
   ★ 颜色在“注册新零件”时随一次，存进 done_rgb —— 不能每次 apply 都随机，
     否则同一零件的多个实例会各拿一个颜色。 */
static int HColorIndexOf(HColorCtx *ctx, ProMdl mdl)
{
    int i;

    for (i = 0; i < ctx->n_done; i++)
        if (ctx->done[i] == mdl)
            return i;
    if (ctx->n_done >= H_COLOR_MAX_MDL - 1)
        return ctx->n_done - 1;
    ctx->done[ctx->n_done] = mdl;
    HColorRandRgb(ctx, ctx->done_rgb[ctx->n_done]);
    return ctx->n_done++;
}

/* 零件级外观写入（唯一通道）。返回 Set 的 status。 */
static ProError HColorApply(ProAsmitem *ai, HColorCtx *ctx, int idx)
{
    ProSurfaceAppearanceProps props, rd;
    ProError  status;

    if (ai == NULL || ctx == NULL)
        return PRO_TK_BAD_INPUTS;

    if (ctx->mode == H_COLOR_CLEAR)
    {
        status = ProMdlVisibleSideAppearancepropsSet(ai, 0, NULL);
        HLog("HColorApply: itemtype=%d id=%d clear -> %d\n", (int)ai->item.type,
             ai->item.id, (int)status);
        if (status != PRO_TK_NO_ERROR)
            ctx->fail++;
        return status;
    }

    HColorBuildProps(ctx, idx, &props);
    status = ProMdlVisibleSideAppearancepropsSet(ai, 0, &props);

    /* 写后回读：切开“没写进去”和“没刷新” */
    memset(&rd, 0, sizeof(rd));
    if (ProMdlVisibleSideAppearancepropsGet(ai, 0, &rd) == PRO_TK_NO_ERROR)
        HLog("HColorApply: itemtype=%d idx=%d set -> %d, readback (%.2f,%.2f,%.2f)\n",
             (int)ai->item.type, idx, (int)status, rd.color_rgb[0],
             rd.color_rgb[1], rd.color_rgb[2]);
    else
        HLog("HColorApply: itemtype=%d idx=%d set -> %d, readback FAILED\n",
             (int)ai->item.type, idx, (int)status);
    if (status != PRO_TK_NO_ERROR)
        ctx->fail++;
    return status;
}

/* ============ 组件级（元件级）外观：已废弃 ============
   2026-10-22 实测：7 种 ProAsmitem 构造（PRO_FEATURE / PRO_FEAT_COMPONENT ×
   owner=直接父装配 / 顶层装配 × path 空 / 1 级 / 追加本级，含官方
   ProAsmcompAsmitemInit 的产出）全部返回 -18(PRO_TK_INVALID_TYPE)；
   ProSelectionAlloc 对最规范的空 path 能 alloc，但 1 级 path 直接 alloc FAILED。
   结论：Creo 12.4 的 Pro/TOOLKIT 侧不接受这种组件 Asmitem，原生「外观」面板
   的元件级指派只能走 OTK（wfcWSelection::SetVisibleAppearance）。
   ⇒ 本插件放弃元件级，统一写到零件模型（见下）。 */

/* 给一个零件模型写外观（或清除）。同一零件的多个实例只写一次。 */
static void HColorOnePart(ProMdl mdl, HColorCtx *ctx)
{
    ProAsmitem ai;
    int        idx;

    if (mdl == NULL || ctx == NULL)
        return;

    idx = HColorIndexOf(ctx, mdl);
    if (idx < 0 || idx >= H_COLOR_MAX_MDL)
        return;
    if (ctx->applied[idx])
        return;                       /* 同一零件的多个实例只设一次 */
    ctx->applied[idx] = 1;

    memset(&ai, 0, sizeof(ai));
    if (ProMdlToModelitem(mdl, &ai.item) != PRO_TK_NO_ERROR)
    {
        HLog("HColorOnePart: toModelitem failed\n");
        return;
    }
    ai.path.owner     = NULL;         /* 零件模型：path 为空 */
    ai.path.table_num = 0;

    if (HColorApply(&ai, ctx, idx) == PRO_TK_NO_ERROR)
        ctx->part_ok++;
}

/* 装配遍历：只对元件引用的【零件模型】上色；子装配递归。 */
static void HColorVisitAsm(ProSolid asm, HColorCtx *ctx);

static ProError HColorCompVisit(ProFeature *feat, ProError status,
                                ProAppData app)
{
    HColorCtx  *ctx = (HColorCtx *)app;
    ProAsmcomp  comp;
    ProMdl      sub = NULL;
    ProMdlType  mt = PRO_MDL_UNUSED;
    ProFeattype ftype = 0;

    if (status != PRO_TK_NO_ERROR || feat == NULL || ctx == NULL)
        return PRO_TK_NO_ERROR;
    ctx->seen_feat++;

    /* ★ feat->type 是【对象类型】PRO_FEATURE(3)，永远不是 PRO_FEAT_COMPONENT(1000)。
       判元件必须用 ProFeatureTypeGet 取【特征类型】。 */
    if (ProFeatureTypeGet(feat, &ftype) != PRO_TK_NO_ERROR
        || ftype != PRO_FEAT_COMPONENT)
        return PRO_TK_NO_ERROR;
    ctx->comp_ok++;

    comp = *(ProAsmcomp *)feat;            /* 同一种 pro_model_item 布局 */
    if (ProAsmcompMdlGet(&comp, &sub) != PRO_TK_NO_ERROR || sub == NULL)
    {
        HLog("HColorCompVisit: compMdlGet failed (fid=%d)\n", feat->id);
        return PRO_TK_NO_ERROR;
    }
    if (ProMdlTypeGet(sub, &mt) != PRO_TK_NO_ERROR)
        return PRO_TK_NO_ERROR;

    if (mt == PRO_MDL_ASSEMBLY)
    {
        HColorVisitAsm((ProSolid)sub, ctx); /* 子装配：递归进去 */
        return PRO_TK_NO_ERROR;
    }
    if (mt != PRO_MDL_PART)
        return PRO_TK_NO_ERROR;

    HColorOnePart(sub, ctx);
    return PRO_TK_NO_ERROR;
}

static void HColorVisitAsm(ProSolid asm, HColorCtx *ctx)
{
    if (asm == NULL || ctx == NULL)
        return;
    ProSolidFeatVisit(asm, HColorCompVisit, NULL, (ProAppData)ctx);
}

static void HColorOneMdl(ProMdl mdl, HColorCtx *ctx)
{
    ProMdlType t = PRO_MDL_UNUSED;

    if (mdl == NULL || ctx == NULL)
        return;
    if (ProMdlTypeGet(mdl, &t) != PRO_TK_NO_ERROR)
        return;

    if (t == PRO_MDL_ASSEMBLY)
    {
        HLog("HColorOneMdl: assembly, visiting components\n");
        HColorVisitAsm((ProSolid)mdl, ctx);
        return;
    }
    if (t == PRO_MDL_PART)
    {
        HColorOnePart(mdl, ctx);
        return;
    }
    HLog("HColorOneMdl: skip mdl type=%d\n", (int)t);
}

static int HColorRun(int mode, char *tag)
{
    ProMdl     mdl  = NULL;
    ProMdlType type = PRO_MDL_UNUSED;
    HColorCtx  ctx;

    if (HGetSolid3D(&mdl, &type) != PRO_TK_NO_ERROR)
    {
        HMsg("TD_NOT_3D");
        return 0;
    }

    memset(&ctx, 0, sizeof(ctx));
    ctx.mode = mode;
    HColorSeedOnce();                 /* 整个会话只 srand 一次，避免连点同色 */

    /* 色相起点随机：每次点「随机着色」，整套配色都换一个方向重排 */
    ctx.hue_base = (double)(rand() % 10000) / 10000.0;
    ctx.hue_idx  = 0;
    HLog("HColorRun: hue_base=%.4f step=%.6f\n", ctx.hue_base, H_COLOR_HUE_STEP);

    HColorOneMdl(mdl, &ctx);
    HLog("%s: mode=%d parts=%d fail=%d feats=%d comps=%d ok=%d\n",
         tag, mode, ctx.n_done, ctx.fail, ctx.seen_feat, ctx.comp_ok,
         ctx.part_ok);

    ProWindowRepaint(PRO_VALUE_UNUSED);
    ProWindowRefresh(PRO_VALUE_UNUSED);

    /* 成功判定只看实际写进零件的外观数 */
    if (ctx.part_ok <= 0)
        HMsg("TD_COLOR_FAIL");
    else if (mode == H_COLOR_CLEAR)
        HMsg("TD_COLOR_CLR_OK");
    else
        HMsg("TD_COLOR_OK");
    return 0;
}

/* 随机着色（装配 / 零件通用，一律写到零件外观） */
static int HColorRandomAct(uiCmdCmdId command, uiCmdValue *p_value,
                           void *p_push_command_data)
{
    (void)command;
    (void)p_value;
    (void)p_push_command_data;
    return HColorRun(H_COLOR_RANDOM, "HColorRandomAct");
}

static int HClearColorAct(uiCmdCmdId command, uiCmdValue *p_value,
                          void *p_push_command_data)
{
    (void)command;
    (void)p_value;
    (void)p_push_command_data;
    return HColorRun(H_COLOR_CLEAR, "HClearColorAct");
}

/*--------------------------- 6. 原位打开 -----------------------------*/
/* 2026-10-22 修：原实现 ProMdlDisplay(sub) 直接把【当前窗口】（装配）换成零件，
   装配窗口就没了；而且 ProViewMatrixSet 传的是 ProViewMatrixGet 原样矩阵，
   带 Creo 自己的缩放/位移 -> 返回 -2(PRO_TK_BAD_INPUTS)，视角根本没套上。
   现在：
   1) 先 ProObjectwindowMdlnameCreate 给零件分配它自己的窗口（装配窗口保留）；
   2) 视图矩阵 R = 元件放置旋转 T（member->顶层装配）× 装配当前视图 V，
      再按官方 UserMatrixNormalize 归一化（ProViewMatrixSet 拒收带缩放/位移的矩阵）；
   3) 切到零件窗口后 Set（NULL = 该窗口当前视图）。 */

/* 归一化 4x4 变换矩阵 —— 官方示例 protk_appls/.../UgGraphViewsSave.c 的
   UserMatrixNormalize：按第 0 行长度去缩放，第 3 行前 3 列清零去位移。 */
static void HMatrixNormalize(ProMatrix m)
{
    double scale = sqrt(m[0][0] * m[0][0] + m[0][1] * m[0][1]
                        + m[0][2] * m[0][2]);
    int    row, col;

    if (scale > 1e-12)
    {
        for (row = 0; row < 3; row++)
            for (col = 0; col < 3; col++)
                m[row][col] /= scale;
    }
    m[3][0] = 0.0;
    m[3][1] = 0.0;
    m[3][2] = 0.0;
}

static int HOpenInPlaceAct(uiCmdCmdId command, uiCmdValue *p_value,
                           void *p_push_command_data)
{
    ProMdl        asm_mdl = NULL;
    ProMdlType    type    = PRO_MDL_UNUSED;
    ProSelection *sels    = NULL;
    int           n = 0;
    ProModelitem  item;
    ProAsmcomppath path;
    ProMdl        sub = NULL;
    ProMdlName    mname;
    ProMdlType    stype = PRO_MDL_UNUSED;
    ProType       otype = PRO_PART;
    ProMatrix     vmtx, tmtx, rmtx;
    int           win_part = -1;
    int           i, j;
    ProError      status;

    (void)command;
    (void)p_value;
    (void)p_push_command_data;

    if (HGetSolid3D(&asm_mdl, &type) != PRO_TK_NO_ERROR)
    {
        HMsg("TD_NOT_3D");
        return 0;
    }
    if (type != PRO_MDL_ASSEMBLY)
    {
        HMsg("TD_ASSY_ONLY");
        return 0;
    }

    if (!HSelectItems(-1, "HOpenInPlaceAct", &sels, &n))
    {
        HMsg("TD_NOTHING_SEL");
        return 0;
    }

    /* ★ 取"选中的那个元件对应的模型"，唯一可靠的路子是 ProAsmcomppathMdlGet。
       ProSelectionAsmcomppathGet 给的 path 是【从顶层装配出发的完整元件 id 链】
       （ProAsmcomppath.h 里 ProAsmcomppathInit 写明 owner = 顶层装配），
       所以 path.comp_id_table[table_num-1] 只是"最深层父装配里的元件 id"。
       旧代码把它和 owner=顶层装配 拼成一个 ProAsmcomp 去 ProAsmcompMdlGet，
       等于在【顶层装配】里找一个同 id 的元件 —— 嵌套一深必然串到别的元件
       （多半是个子装配），这就是"点零件却打开装配图"的根因；
       顶层里没有那个 id 时就是日志里的 compMdlGet failed。 */
    memset(&path, 0, sizeof(path));
    status = PRO_TK_GENERAL_ERROR;
    if (ProSelectionAsmcomppathGet(sels[0], &path) == PRO_TK_NO_ERROR
        && path.table_num > 0 && path.owner != NULL)
    {
        HLog("HOpenInPlaceAct: path depth=%d last_id=%d\n",
             path.table_num, path.comp_id_table[path.table_num - 1]);
        status = ProAsmcomppathMdlGet(&path, &sub);
        HLog("HOpenInPlaceAct: pathMdlGet -> %d\n", (int)status);
    }
    /* 兜底：对"元件"的选择，ProModelitem 里的 owner 就是那个元件模型本身 */
    if ((status != PRO_TK_NO_ERROR || sub == NULL)
        && ProSelectionModelitemGet(sels[0], &item) == PRO_TK_NO_ERROR
        && item.owner != NULL)
    {
        sub    = item.owner;
        status = PRO_TK_NO_ERROR;
        HLog("HOpenInPlaceAct: fallback item.owner type=%d id=%d\n",
             (int)item.type, item.id);
    }
    HSelectFree(sels);
    sels = NULL;

    if (status != PRO_TK_NO_ERROR || sub == NULL)
    {
        HLog("HOpenInPlaceAct: model resolve failed\n");
        HMsg("TD_OPENINPLACE_FAIL");
        return 0;
    }
    {
        ProMdlType rt = PRO_MDL_UNUSED;
        if (ProMdlTypeGet(sub, &rt) == PRO_TK_NO_ERROR)
            HLog("HOpenInPlaceAct: target type=%d (asm=%d part=%d)\n",
                 (int)rt, (int)PRO_MDL_ASSEMBLY, (int)PRO_MDL_PART);
    }

    /* ---- 1. 装配当前视图矩阵 V（归一化后才能跨窗口使用） ---- */
    memset(vmtx, 0, sizeof(vmtx));
    status = ProViewMatrixGet(asm_mdl, NULL, vmtx);
    HLog("HOpenInPlaceAct: matrixGet -> %d\n", (int)status);
    if (status != PRO_TK_NO_ERROR)
    {
        HMsg("TD_OPENINPLACE_FAIL");
        return 0;
    }
    HMatrixNormalize(vmtx);

    /* ---- 2. 元件放置旋转 T（member -> 顶层装配）；取不到就退回单位阵 ---- */
    memset(tmtx, 0, sizeof(tmtx));
    tmtx[0][0] = tmtx[1][1] = tmtx[2][2] = tmtx[3][3] = 1.0;
    if (path.table_num > 0)
    {
        ProError tst = ProAsmcomppathTrfGet(&path, PRO_B_TRUE, tmtx);

        HLog("HOpenInPlaceAct: trfGet -> %d\n", (int)tst);
        if (tst != PRO_TK_NO_ERROR)
        {
            memset(tmtx, 0, sizeof(tmtx));
            tmtx[0][0] = tmtx[1][1] = tmtx[2][2] = tmtx[3][3] = 1.0;
        }
    }

    /* ---- 3. R = T × V。ProMatrix 是行向量约定（位移在第 3 行）：
       p_asm = p_comp·T，screen = p_asm·V ⇒ screen = p_comp·(T·V) ---- */
    memset(rmtx, 0, sizeof(rmtx));
    for (i = 0; i < 3; i++)
        for (j = 0; j < 3; j++)
            rmtx[i][j] = tmtx[i][0] * vmtx[0][j]
                       + tmtx[i][1] * vmtx[1][j]
                       + tmtx[i][2] * vmtx[2][j];
    rmtx[3][3] = 1.0;
    HMatrixNormalize(rmtx);
    HLog("HOpenInPlaceAct: R=(%.2f %.2f %.2f | %.2f %.2f %.2f | %.2f %.2f %.2f)\n",
         rmtx[0][0], rmtx[0][1], rmtx[0][2],
         rmtx[1][0], rmtx[1][1], rmtx[1][2],
         rmtx[2][0], rmtx[2][1], rmtx[2][2]);

    /* ---- 4. 给零件分配它自己的窗口；装配窗口保持不动 ---- */
    memset(mname, 0, sizeof(mname));
    status = ProMdlMdlnameGet(sub, mname);
    HLog("HOpenInPlaceAct: mdlnameGet -> %d\n", (int)status);
    if (status == PRO_TK_NO_ERROR)
    {
        otype = PRO_PART;
        if (ProMdlTypeGet(sub, &stype) == PRO_TK_NO_ERROR
            && stype == PRO_MDL_ASSEMBLY)
            otype = PRO_ASSEMBLY;
        status = ProObjectwindowMdlnameCreate(mname, otype, &win_part);
        HLog("HOpenInPlaceAct: objwindowCreate -> %d win=%d\n",
             (int)status, win_part);
    }
    /* 零件已经有自己的窗口了，这一下只会把它显示在那个窗口里，不会顶掉装配。
       ★ 窗口创建失败时【不要】再调 ProMdlDisplay——那会把装配窗口换成零件。 */
    if (win_part < 0)
    {
        HLog("HOpenInPlaceAct: no window for part, abort (assembly kept)\n");
        HMsg("TD_OPENINPLACE_FAIL");
        return 0;
    }
    ProMdlDisplay(sub);
    ProWindowCurrentSet(win_part);   /* 同步模式：切当前窗口 */
    ProWindowActivate(win_part);     /* 异步模式有效；同步下失败无害 */

    /* ---- 5. 套视角（矩阵已归一化，不会再拿 -2） ---- */
    status = ProViewMatrixSet(sub, NULL, rmtx);
    HLog("HOpenInPlaceAct: matrixSet -> %d\n", (int)status);
    ProWindowRepaint(PRO_VALUE_UNUSED);

    HMsg("TD_OPENINPLACE_OK");
    return 0;
}

/*----------------- 6a-2. 工程图：原位打开 / 打开 3D -----------------*/
/* 2026-10-23 新增。工程图页的两个命令：
     · 原位打开：选中一个图纸视图 -> 把该视图对应的 3D 模型放进【它自己的
       窗口】打开，并把 3D 视图的朝向设成与这张图纸视图完全一致。
     · 打开 3D ：同上，但【不动朝向】，只把模型调出来看。

   ★ 为什么不能直接 ProMdlDisplay：
     模型若还没有自己的窗口，ProMdlDisplay 会把它塞进【当前窗口】，也就是
     把工程图窗口顶掉（图纸一消失，之前摆好的视图位置全白调）。所以一律
     先拿到模型自己的窗口 id（已有就复用，没有才建），再 Display + Activate。
     这一套与 6a 的 HOpenInPlaceAct 完全一致。

   ★ 朝向矩阵的来源：ProViewMatrixGet(drawing, view, m)
     返回的是"模型坐标 -> 逻辑屏幕"的行向量矩阵（位移在第 3 行，比例在
     3x3 里）。归一化（去掉图纸比例、去掉图纸原点位移）之后把 3x3 旋转
     直接 ProViewMatrixSet 到 3D 模型上，得到的就是同一个观察方向。
     ProViewMatrixSet 要求"每行长度 1"，未归一化会直接返回 -2。 */

/* 只读选择缓冲区里的那一个视图，不弹选择（供「打开 3D」用）。
   缓冲区数组必须 ProSelectionarrayFree（它会连每个成员一起释放）；
   这里不保留任何 ProSelection，所以直接释放是安全的。 */
static int HOpenViewFromBuffer(ProView *p_view)
{
    ProSelection *sels = NULL;
    int           n = 0;

    *p_view = NULL;
    if (ProSelbufferSelectionsGet(&sels) != PRO_TK_NO_ERROR || sels == NULL)
        return 0;
    ProArraySizeGet(sels, &n);
    if (n == 1)
        ProSelectionViewGet(sels[0], p_view);
    ProSelectionarrayFree(sels);
    return (*p_view != NULL);
}

/* 把模型放进"它自己的窗口"并激活。成功返回窗口 id，失败 -1。
   已有窗口就直接复用（ProMdlWindowGet），不重复开窗。 */
static int HOpenMdlOwnWindow(ProMdl mdl)
{
    ProMdlName mname;
    ProMdlType mtype = PRO_MDL_UNUSED;
    ProType    otype = PRO_PART;
    int        win   = -1;
    ProError   status = PRO_TK_GENERAL_ERROR;

    if (ProMdlWindowGet(mdl, &win) == PRO_TK_NO_ERROR && win >= 0)
    {
        HLog("HOpenMdlOwnWindow: reuse win=%d\n", win);
    }
    else
    {
        win = -1;
        memset(mname, 0, sizeof(mname));
        if (ProMdlMdlnameGet(mdl, mname) != PRO_TK_NO_ERROR)
            return -1;
        if (ProMdlTypeGet(mdl, &mtype) == PRO_TK_NO_ERROR
            && mtype == PRO_MDL_ASSEMBLY)
            otype = PRO_ASSEMBLY;
        status = ProObjectwindowMdlnameCreate(mname, otype, &win);
        HLog("HOpenMdlOwnWindow: create -> %d win=%d\n", (int)status, win);
        if (win < 0)
            return -1;
    }

    ProMdlDisplay(mdl);
    ProWindowCurrentSet(win);   /* 同步模式：切当前窗口 */
    ProWindowActivate(win);     /* 异步模式有效；同步下失败无害 */
    return win;
}

/* 取一个图纸视图：先看选择缓冲区；缓冲区里没有视图（或选着别的东西），
   清掉缓冲区后直接让用户点一次。
   ★ 为什么不用 HSelectItemsF：它在"用户按 Esc 取消"时也返回 0，
     拿不到"是取消还是缓冲区里不是视图"，会被我这里的兜底再弹一次选择框。
     这里的语义很简单 —— 缓冲区有视图就用，否则就选一次。 */
static int HOpenPickView(char *tag, ProView *p_view)
{
    ProSelection *sels = NULL;
    int           n = 0;
    ProError      status;

    *p_view = NULL;

    if (HOpenViewFromBuffer(p_view))
    {
        HLog("%s: view from selection buffer\n", tag);
        return 1;
    }

    ProSelbufferClear();          /* 缓冲区里那个不是视图（例如一条尺寸），清掉 */
    n      = 0;
    status = ProSelect((char*)"dwg_view", 1, NULL, NULL, NULL, NULL, &sels, &n);
    HLog("%s: ProSelect(dwg_view) -> %d n=%d\n", tag, (int)status, n);
    if (status == PRO_TK_NO_ERROR && n >= 1 && sels != NULL)
        ProSelectionViewGet(sels[0], p_view);
    return (*p_view != NULL);
}

static uiCmdAccessState HOpenViewAccess(uiCmdAccessMode access_mode)
{
    (void)access_mode;
    return ACCESS_AVAILABLE;
}

static int HOpenViewInPlaceAct(uiCmdCmdId command, uiCmdValue *p_value,
                               void *p_push_command_data)
{
    ProMdl     dwg_mdl = NULL;
    ProView    view    = NULL;
    ProSolid   solid   = NULL;
    ProMatrix  mtx;
    double     scale;
    double     det;
    ProError   status;
    ProError   st2;
    int        win;
    int        vid;

    (void)command;
    (void)p_value;
    (void)p_push_command_data;

    if (HGetDrawing(&dwg_mdl) != PRO_TK_NO_ERROR)
    {
        HMsg("OPENVIEW_NO_DRAWING");
        return 0;
    }

    if (!HOpenPickView("HOpenViewInPlaceAct", &view))
    {
        HMsg("OPENVIEW_NO_VIEW");
        return 0;
    }

    /* 1. 视图对应的 3D 模型（简化视图也返回正常模型指针） */
    status = ProDrawingViewSolidGet((ProDrawing)dwg_mdl, view, &solid);
    HLog("HOpenViewInPlaceAct: viewSolidGet -> %d solid=%p\n",
         (int)status, (void*)solid);
    if (status != PRO_TK_NO_ERROR || solid == NULL)
    {
        HMsg("OPENVIEW_FAIL");
        return 0;
    }

    /* 2. 视图朝向矩阵，归一化成纯旋转
       ★★ 2026-10-23 第二次实测（h_load.log）：
            viewSolidGet -> 0   viewIdGet -> 0   viewInit -> 0
            matrixGet(init) -> -1    matrixGet(sel) -> -1
          ⇒ ProViewMatrixGet 取图纸视图这条路根本走不通（选择集句柄、Init 换出来的
            正式句柄都返回 -1），所以之前"打开 3D 但方向没变"。
          正确的 API 是 ProDrawingViewTransformGet()：官方示例
          UgDrawingDimensions.c / TestDrwView.c 用它把「模型里的 3D 点」换算成
          「图纸上的 2D 点」，可见它左上 3x3 就是【模型 → 图纸】的完整朝向。
          去掉比例（行归一化）和平移（末行清零）后，正是 ProViewMatrixSet
          要的那个旋转矩阵。旧的两条 ProViewMatrixGet 保留作兜底。 */
    memset(mtx, 0, sizeof(mtx));
    scale  = 0.0;
    status = ProDrawingViewTransformGet((ProDrawing)dwg_mdl, view,
                                        PRO_B_TRUE, mtx);
    HLog("HOpenViewInPlaceAct: transformGet(view->drawing) -> %d\n", (int)status);
    if (status == PRO_TK_NO_ERROR)
    {
        HLog("HOpenViewInPlaceAct: raw=(%.4f %.4f %.4f | %.4f %.4f %.4f"
             " | %.4f %.4f %.4f | %.4f %.4f %.4f)\n",
             mtx[0][0], mtx[0][1], mtx[0][2],
             mtx[1][0], mtx[1][1], mtx[1][2],
             mtx[2][0], mtx[2][1], mtx[2][2],
             mtx[3][0], mtx[3][1], mtx[3][2]);
        scale = sqrt(mtx[0][0] * mtx[0][0] + mtx[0][1] * mtx[0][1]
                   + mtx[0][2] * mtx[0][2]);
    }

    if (status != PRO_TK_NO_ERROR || scale < 1e-9)
    {
        /* 兜底：老路线（对普通 3D 视图有效，对图纸视图历来 -1） */
        memset(mtx, 0, sizeof(mtx));
        scale = 0.0;
        vid   = -1;
        st2   = ProDrawingViewIdGet((ProDrawing)dwg_mdl, view, &vid);
        HLog("HOpenViewInPlaceAct: viewIdGet -> %d id=%d\n", (int)st2, vid);
        if (st2 == PRO_TK_NO_ERROR)
        {
            ProView v2 = NULL;

            st2 = ProDrawingViewInit((ProDrawing)dwg_mdl, vid, &v2);
            HLog("HOpenViewInPlaceAct: viewInit -> %d view=%p\n",
                 (int)st2, (void*)v2);
            if (st2 == PRO_TK_NO_ERROR && v2 != NULL)
            {
                status = ProViewMatrixGet(dwg_mdl, v2, mtx);
                HLog("HOpenViewInPlaceAct: matrixGet(init) -> %d\n", (int)status);
            }
        }
        if (status != PRO_TK_NO_ERROR)
        {
            memset(mtx, 0, sizeof(mtx));
            status = ProViewMatrixGet(dwg_mdl, view, mtx);
            HLog("HOpenViewInPlaceAct: matrixGet(sel) -> %d\n", (int)status);
        }
        scale = sqrt(mtx[0][0] * mtx[0][0] + mtx[0][1] * mtx[0][1]
                   + mtx[0][2] * mtx[0][2]);
    }

    HLog("HOpenViewInPlaceAct: matrix -> %d scale=%.5f\n",
         (int)status, scale);
    if (status != PRO_TK_NO_ERROR || scale < 1e-9)
    {
        /* 方向读不出来（该视图类型不支持矩阵读取等）：
           仍然把 3D 开出来，只是不套方向，并明确告知。
           不能整条命令报失败 —— 用户要的"看到这个零件"已经办到了。 */
        HLog("HOpenViewInPlaceAct: no usable matrix, open without orientation\n");
        if (HOpenMdlOwnWindow((ProMdl)solid) < 0)
        {
            HMsg("OPENVIEW_FAIL");
            return 0;
        }
        HMsg("OPENVIEW_SETDIR_FAIL");
        return 0;
    }
    HMatrixNormalize(mtx);
    /* det < 0 => 该视图是镜像视图，3D 里没有等价朝向，套上去会左右反；
       日志里留一行，出问题时一眼能看出是"视图本身镜像"而不是代码算错。 */
    det = mtx[0][0] * (mtx[1][1] * mtx[2][2] - mtx[1][2] * mtx[2][1])
        - mtx[0][1] * (mtx[1][0] * mtx[2][2] - mtx[1][2] * mtx[2][0])
        + mtx[0][2] * (mtx[1][0] * mtx[2][1] - mtx[1][1] * mtx[2][0]);
    HLog("HOpenViewInPlaceAct: det=%.4f (负=镜像视图)\n", det);
    HLog("HOpenViewInPlaceAct: R=(%.3f %.3f %.3f | %.3f %.3f %.3f | %.3f %.3f %.3f)\n",
         mtx[0][0], mtx[0][1], mtx[0][2],
         mtx[1][0], mtx[1][1], mtx[1][2],
         mtx[2][0], mtx[2][1], mtx[2][2]);

    /* 3. 在它自己的窗口里打开 */
    win = HOpenMdlOwnWindow((ProMdl)solid);
    if (win < 0)
    {
        HMsg("OPENVIEW_FAIL");
        return 0;
    }

    /* 4. 套方向 */
    status = ProViewMatrixSet((ProMdl)solid, NULL, mtx);
    HLog("HOpenViewInPlaceAct: matrixSet -> %d\n", (int)status);
    {
        /* 回读一次：Creo 会加自己的缩放/平移，值不会和传进去的一样，
           但旋转部分应当一致。日志里对一下就知道到底吃没吃进去。 */
        ProMatrix chk;

        memset(chk, 0, sizeof(chk));
        if (ProViewMatrixGet((ProMdl)solid, NULL, chk) == PRO_TK_NO_ERROR)
            HLog("HOpenViewInPlaceAct: applied R=(%.3f %.3f %.3f | %.3f %.3f %.3f"
                 " | %.3f %.3f %.3f)\n",
                 chk[0][0], chk[0][1], chk[0][2],
                 chk[1][0], chk[1][1], chk[1][2],
                 chk[2][0], chk[2][1], chk[2][2]);
        else
            HLog("HOpenViewInPlaceAct: applied matrix readback failed\n");
    }
    ProWindowRepaint(PRO_VALUE_UNUSED);
    /* 模型已经开出来了，只是方向没套上（镜像视图等） —— 单独给一句提示，
       否则用户会以为整条命令失败了。 */
    HMsg(status == PRO_TK_NO_ERROR ? "OPENVIEW_OK" : "OPENVIEW_SETDIR_FAIL");
    return 0;
}

static int HOpen3DAct(uiCmdCmdId command, uiCmdValue *p_value,
                      void *p_push_command_data)
{
    ProMdl     dwg_mdl = NULL;
    ProView    view    = NULL;
    ProSolid   solid   = NULL;
    ProError   status;
    int        win;

    (void)command;
    (void)p_value;
    (void)p_push_command_data;

    if (HGetDrawing(&dwg_mdl) != PRO_TK_NO_ERROR)
    {
        HMsg("OPEN3D_NO_DRAWING");
        return 0;
    }

    /* 选着一个视图就打开该视图的模型（一张图纸带多个模型时常用）；
       没选就用图纸的"当前模型"。 */
    if (HOpenViewFromBuffer(&view) && view != NULL)
    {
        status = ProDrawingViewSolidGet((ProDrawing)dwg_mdl, view, &solid);
        HLog("HOpen3DAct: viewSolidGet -> %d\n", (int)status);
        if (status != PRO_TK_NO_ERROR)
            solid = NULL;
    }
    if (solid == NULL)
    {
        status = ProDrawingCurrentsolidGet((ProDrawing)dwg_mdl, &solid);
        HLog("HOpen3DAct: currentSolidGet -> %d solid=%p\n",
             (int)status, (void*)solid);
        if (status != PRO_TK_NO_ERROR || solid == NULL)
        {
            HMsg("OPEN3D_NOMDL");
            return 0;
        }
    }

    win = HOpenMdlOwnWindow((ProMdl)solid);
    if (win < 0)
    {
        HMsg("OPEN3D_FAIL");
        return 0;
    }
    ProWindowRepaint(PRO_VALUE_UNUSED);
    HMsg("OPEN3D_OK");
    return 0;
}

/*--------------------------- 6b. 视图正归位 --------------------------*/
/* 2026-10-22 新增，同日改版：**以坐标轴为基准**（不再要求先拾取基准面）。
   1) 方向 n 的来源：
      · 用户预先选了平面/基准面 → 垂直于该面（n = 面法向，朝观察者）；
      · 什么都没选 → 观察者在 -X 侧朝 +X 看，n = (-1,0,0)，参考轴 = (0,0,-1)
        ⇒ 屏幕右 -Z、屏幕上 -Y（2026-10-23 用户定稿「逆时针 90°」）。
   2) 视图矩阵 M[2] = n —— 屏幕 Z 轴（指向观察者）；
      M[0] = 参考世界轴在垂直于 n 的平面内的投影（画面不歪头）；
      M[1] = M[2] × M[0]（右手系）；
   3) 距离分两步：Set 之后【无条件】ProWindowRefit（Set 在矩阵未变时会跳过
      自带的适配，这是「第一次缩放完后面不再缩放」的根因），再交给
      HViewFitSolidOnly 做基准修正：k = 含基准包围盒 ÷ 实体包围盒 × HFIT_FILL，
      k≈1 就什么都不做。线性缩放写回 pan/zoom 矩阵，旋转一个元素都不动。
   矩阵语义见 ProView.h：ProViewMatrixSet 要求"每行长度 1、底行 0,0,0,1"，
   即行向量约定 v_screen = v_model · M，M[0]=屏幕X、M[1]=屏幕Y、M[2]=屏幕Z（朝观察者）。
   ★ 基准平面在内部就是一个 ProSurface（ProSurface.h 有 ProSurfaceIsDatumPlane
     为证），但 ProGeomitemdataGet 不收 PRO_DATUM_PLANE，
     所以基准面走 ProSurfaceInit(owner, id) 再取数据这条路。 */

static void HVecNormalize(ProVector v)
{
    double len = sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);

    if (len > 1e-12)
    {
        v[0] /= len;
        v[1] /= len;
        v[2] /= len;
    }
}

/* ★ 2026-10-23 用户实测：选中一个面点正归位，看到的是该面的【背面】。
   原因：曲面几何数据里的法向（平面 e3 轴 / 参数化法向）并不等于
   「朝观察者」那一侧 —— Creo 的曲面朝向是内部参数化约定，与视觉正反无关。
   这里统一取反，让选中的面正对观察者。若以后又反了，把宏改成 0。 */
#define HVIEWNORMAL_FACE_FLIP  1

static void HFlipFaceNormal(ProVector n)
{
    if (!HVIEWNORMAL_FACE_FLIP)
        return;
    n[0] = -n[0];
    n[1] = -n[1];
    n[2] = -n[2];
}

/* 曲面法向。uv 传 NULL 时取参数域中点（平面直接用局部系的 e3）。 */
static int HSurfNormalGet(ProSurface surf, ProUvParam uv, ProVector n)
{
    ProGeomitemdata *pdata = NULL;
    ProSurfacedata  *sd;
    ProUvParam       p;
    ProVector        xyz, d1[2], d2[3];
    ProError         st;

    n[0] = n[1] = n[2] = 0.0;
    if (surf == NULL)
        return 0;
    if (ProSurfaceDataGet(surf, &pdata) != PRO_TK_NO_ERROR || pdata == NULL)
    {
        HLog("HSurfNormalGet: surfDataGet failed\n");
        return 0;
    }
    sd = pdata->data.p_surface_data;
    if (sd == NULL)
    {
        ProGeomitemdataFree(&pdata);
        return 0;
    }
    HLog("HSurfNormalGet: surf type=%d\n", (int)sd->type);

    if (sd->type == PRO_SRF_PLANE)
    {
        n[0] = sd->srf_shape.plane.e3[0];
        n[1] = sd->srf_shape.plane.e3[1];
        n[2] = sd->srf_shape.plane.e3[2];
        ProGeomitemdataFree(&pdata);
        HVecNormalize(n);
        return (fabs(n[0]) + fabs(n[1]) + fabs(n[2]) > 1e-9) ? 1 : 0;
    }

    if (uv != NULL)
    {
        p[0] = uv[0];
        p[1] = uv[1];
    }
    else
    {
        p[0] = 0.5 * (sd->uv_min[0] + sd->uv_max[0]);
        p[1] = 0.5 * (sd->uv_min[1] + sd->uv_max[1]);
    }
    ProGeomitemdataFree(&pdata);

    st = ProSurfaceXyzdataEval(surf, p, xyz, d1, d2, n);
    HLog("HSurfNormalGet: xyzdataEval -> %d n=(%.3f %.3f %.3f)\n",
         (int)st, n[0], n[1], n[2]);
    if (st != PRO_TK_NO_ERROR)
        return 0;
    HVecNormalize(n);
    return 1;
}

/* 从一次拾取里拿平面法向（朝观察者侧）。成功返回 1。 */
static int HPlaneNormalFromSel(ProSelection sel, ProMdl mdl, ProVector n)
{
    ProModelitem     item;
    ProGeomitemdata *pdata = NULL;
    ProSurfacedata  *sd;
    ProSurface       surf = NULL;
    ProUvParam       uv;
    ProError         st;
    int              have_uv = 0;

    n[0] = n[1] = n[2] = 0.0;
    if (ProSelectionModelitemGet(sel, &item) != PRO_TK_NO_ERROR)
    {
        HLog("HPlaneNormalFromSel: modelitemGet failed\n");
        return 0;
    }
    HLog("HPlaneNormalFromSel: type=%d id=%d\n", (int)item.type, item.id);

    /* 拾取点 UV（只对曲面有意义，拿不到就算了） */
    memset(uv, 0, sizeof(uv));
    if (ProSelectionUvParamGet(sel, uv) == PRO_TK_NO_ERROR)
        have_uv = 1;

    /* ---- 路线 A：普通曲面的几何数据（基准面这里会失败，属正常） ---- */
    pdata = NULL;
    st = ProGeomitemdataGet((ProGeomitem *)&item, &pdata);
    if (st == PRO_TK_NO_ERROR && pdata != NULL)
    {
        sd = pdata->data.p_surface_data;
        if (sd != NULL && sd->type == PRO_SRF_PLANE)
        {
            n[0] = sd->srf_shape.plane.e3[0];
            n[1] = sd->srf_shape.plane.e3[1];
            n[2] = sd->srf_shape.plane.e3[2];
            ProGeomitemdataFree(&pdata);
            HVecNormalize(n);
            HFlipFaceNormal(n);
            HLog("HPlaneNormalFromSel: A-plane n=(%.3f %.3f %.3f) (flipped=%.0f)\n",
                 n[0], n[1], n[2], (double)HVIEWNORMAL_FACE_FLIP);
            if (fabs(n[0]) + fabs(n[1]) + fabs(n[2]) > 1e-9)
                return 1;
        }
        ProGeomitemdataFree(&pdata);
        pdata = NULL;
    }
    else
    {
        HLog("HPlaneNormalFromSel: geomitemdata -> %d\n", (int)st);
    }

    /* ---- 路线 B：ProSurfaceInit（基准平面靠这条） ---- */
    surf = NULL;
    st = ProSurfaceInit(item.owner, item.id, &surf);
    HLog("HPlaneNormalFromSel: surfInit(owner) -> %d\n", (int)st);
    if (st != PRO_TK_NO_ERROR || surf == NULL)
    {
        surf = NULL;
        st = ProSurfaceInit(mdl, item.id, &surf);
        HLog("HPlaneNormalFromSel: surfInit(mdl) -> %d\n", (int)st);
    }
    if (surf == NULL)
        return 0;

    if (HSurfNormalGet(surf, have_uv ? uv : NULL, n))
    {
        HFlipFaceNormal(n);
        HLog("HPlaneNormalFromSel: surf normal flipped -> (%.3f %.3f %.3f)\n",
             n[0], n[1], n[2]);
        return 1;
    }
    return 0;
}

/*------------------ 6b-0. 视角自检 + 只按实体适配距离 ------------------*/
/* 2026-10-23 起因（用户截图“出现了 3 个坐标”）：
   模型里离实体很远的基准坐标系会把 ProWindowRefit 的包围盒撑大 ——
   Creo 的坐标符号按【固定屏幕尺寸】绘制，不随缩放变小，于是实体被缩成
   一个点，屏幕上只剩三个坐标符号。这里改成：距离只按【实体几何】算，
   四类基准（平面/点/坐标系/轴）一律排除在包围盒之外。

   ★「角度怎么确认」—— 这是本段的核心：
   Creo 会把自身的缩放乘进视图矩阵的三个行向量（实测行长度 33.764），
   所以【必须先把每行除以自己的长度归一化】，再去和期望的旋转逐元素比，
   否则永远对不上、看起来像“角度又跑了”。另外校验 det(R) = +1，
   出现 -1 就是镜像（画面翻面）。这两项都由 HRotCheckLog 打进日志。 */

#define HFIT_USE_REFIT  0   /* 1 = 退回原来的 ProWindowRefit（连基准一起框） */

/* 2026-10-23 用户反馈「大小也不对，第一次缩放完后面不缩放了」。
   根因：ProViewMatrixSet 只在视图矩阵【发生变化】时才重算 shift+scaling，
   连点同一个矩阵它就直接跳过 ⇒ 用户中途滚轮缩放过之后，点正归位不恢复。
   修法：Set 之后【无条件】再调一次 ProWindowRefit(win)，强制重算距离；
   refit 只改平移/缩放、不改旋转，随后用 rotcheck 验证。
   HFIT_FILL 是最后的微调旋钮：1.00 = Creo 标准适配（留一点边距），
   > 1 放大、< 1 缩小；改成某个值后不管 k 多少都会应用一次。 */
#define HFIT_FILL       1.00

static void HMatIdentity(ProMatrix m)
{
    int i, j;

    for (i = 0; i < 4; i++)
        for (j = 0; j < 4; j++)
            m[i][j] = (i == j) ? 1.0 : 0.0;
}

/* 把轴对齐包围盒的 8 个角投影到 (u, v) 两根轴上，
   返回 {umin, umax, vmin, vmax}。 */
static void HBoxProject(double box[2][3], const ProVector u, const ProVector v,
                        double r[4])
{
    int    i, j;
    double p[3], du, dv;

    for (i = 0; i < 8; i++)
    {
        for (j = 0; j < 3; j++)
            p[j] = box[(i >> j) & 1][j];
        du = p[0] * u[0] + p[1] * u[1] + p[2] * u[2];
        dv = p[0] * v[0] + p[1] * v[1] + p[2] * v[2];
        if (i == 0)
        {
            r[0] = r[1] = du;
            r[2] = r[3] = dv;
        }
        else
        {
            if (du < r[0]) r[0] = du;
            if (du > r[1]) r[1] = du;
            if (dv < r[2]) r[2] = dv;
            if (dv > r[3]) r[3] = dv;
        }
    }
}

/* 诊断：把模型里所有基准坐标系（PRO_FEAT_CSYS）的原点与轴向打进日志。
   用来回答「屏幕上那几个 XYZ 三轴到底是什么、有几个、在哪」——
   Creo 画坐标符号是【固定屏幕尺寸】，不随缩放变小，所以它们很显眼。 */
typedef struct
{
    ProMdl mdl;
    int    count;
} HCsysScanCtx;

static ProError HCsysScanVisit(ProFeature *feat, ProError status, ProAppData app)
{
    HCsysScanCtx    *sc = (HCsysScanCtx *)app;
    ProFeattype      ftype = 0;
    ProCsys          csys  = NULL;
    ProGeomitemdata *pdata = NULL;
    ProCsysdata     *cd;

    if (status != PRO_TK_NO_ERROR || feat == NULL || sc == NULL)
        return PRO_TK_NO_ERROR;
    if (ProFeatureTypeGet(feat, &ftype) != PRO_TK_NO_ERROR
        || ftype != PRO_FEAT_CSYS)
        return PRO_TK_NO_ERROR;

    sc->count++;
    if (ProCsysInit((ProSolid)sc->mdl, feat->id, &csys) != PRO_TK_NO_ERROR
        || csys == NULL)
    {
        HLog("csys scan: #%d id=%d  csysInit failed\n", sc->count, feat->id);
        return PRO_TK_NO_ERROR;
    }
    if (ProCsysDataGet(csys, &pdata) != PRO_TK_NO_ERROR || pdata == NULL)
    {
        HLog("csys scan: #%d id=%d  dataGet failed\n", sc->count, feat->id);
        return PRO_TK_NO_ERROR;
    }
    cd = pdata->data.p_csys_data;
    if (cd != NULL)
        HLog("csys scan: #%d id=%d origin=(%.2f %.2f %.2f) "
             "x=(%.2f %.2f %.2f) y=(%.2f %.2f %.2f) z=(%.2f %.2f %.2f)\n",
             sc->count, feat->id,
             cd->origin[0], cd->origin[1], cd->origin[2],
             cd->x_vector[0], cd->x_vector[1], cd->x_vector[2],
             cd->y_vector[0], cd->y_vector[1], cd->y_vector[2],
             cd->z_vector[0], cd->z_vector[1], cd->z_vector[2]);
    ProGeomitemdataFree(&pdata);
    return PRO_TK_NO_ERROR;
}

static void HDatumCsysScan(ProMdl mdl)
{
    HCsysScanCtx sc;

    sc.mdl   = mdl;
    sc.count = 0;
    ProSolidFeatVisit((ProSolid)mdl, HCsysScanVisit, NULL, (ProAppData)&sc);
    HLog("csys scan: model has %d datum coordinate system(s)\n", sc.count);
}

/* 回读视图矩阵 → 归一化 → 与期望旋转逐元素比对 → 打印 maxdev 与 det。
   返回值 < 0 表示回读失败；否则返回最大元素偏差。 */
static double HRotCheckLog(ProMdl mdl, ProMatrix expect, const char *tag)
{
    ProMatrix c;
    double    s[3], dev = 0.0, det;
    int       i, j;

    memset(c, 0, sizeof(ProMatrix));
    if (ProViewMatrixGet(mdl, NULL, c) != PRO_TK_NO_ERROR)
    {
        HLog("rotcheck[%s]: matrixGet failed\n", tag);
        return -1.0;
    }

    for (i = 0; i < 3; i++)
    {
        s[i] = sqrt(c[i][0] * c[i][0] + c[i][1] * c[i][1] + c[i][2] * c[i][2]);
        if (s[i] < 1e-12)
        {
            HLog("rotcheck[%s]: row %d degenerate len=%.6f\n", tag, i, s[i]);
            return -1.0;
        }
        for (j = 0; j < 3; j++)
            c[i][j] /= s[i];
    }

    for (i = 0; i < 3; i++)
        for (j = 0; j < 3; j++)
            if (fabs(c[i][j] - expect[i][j]) > dev)
                dev = fabs(c[i][j] - expect[i][j]);

    det = c[0][0] * (c[1][1] * c[2][2] - c[1][2] * c[2][1])
        - c[0][1] * (c[1][0] * c[2][2] - c[1][2] * c[2][0])
        + c[0][2] * (c[1][0] * c[2][1] - c[1][1] * c[2][0]);

    HLog("rotcheck[%s]: scale=(%.4f %.4f %.4f) maxdev=%.5f det=%+.4f "
         "r0=(%.3f %.3f %.3f) r1=(%.3f %.3f %.3f) r2=(%.3f %.3f %.3f)%s\n",
         tag, s[0], s[1], s[2], dev, det,
         c[0][0], c[0][1], c[0][2],
         c[1][0], c[1][1], c[1][2],
         c[2][0], c[2][1], c[2][2],
         (dev > 1e-3 || det < 0.0) ? "  <<< ANGLE MISMATCH" : "  [angle ok]");
    return dev;
}

/* 只按实体几何适配距离（基准不参与）。失败就原样返回，不影响角度。 */
static void HViewFitSolidOnly(ProMdl mdl, ProMatrix rot, int win)
{
    ProMatrix idm, pz;
    double    bd[2][3], bs[2][3];
    double    pd[4], ps[4];
    double    du, dv, su, sv, ku, kv, k, s0, sc;
    double    cx, cy, cz, dev;
    int       i, j;
    ProError  st;

    HMatIdentity(idm);

    /* 1) 「含基准」的世界轴包围盒 —— 这就是 Creo refit 的口径 */
    memset(bd, 0, sizeof(bd));
    st = ProSolidDispoutlineGet((ProSolid)mdl, idm, bd);
    HLog("fit: dispoutline -> %d  (%.3f %.3f %.3f) - (%.3f %.3f %.3f)\n",
         (int)st, bd[0][0], bd[0][1], bd[0][2], bd[1][0], bd[1][1], bd[1][2]);
    if (st != PRO_TK_NO_ERROR)
        return;

    /* 2) 「纯几何」的世界轴包围盒（官方：不含基准、注释等非几何项） */
    memset(bs, 0, sizeof(bs));
    st = ProSolidOutlineGet((ProSolid)mdl, bs);
    HLog("fit: solidoutline -> %d  (%.3f %.3f %.3f) - (%.3f %.3f %.3f)\n",
         (int)st, bs[0][0], bs[0][1], bs[0][2], bs[1][0], bs[1][1], bs[1][2]);
    if (st != PRO_TK_NO_ERROR)
        return;

    /* 3) 两个包围盒各自投影到屏幕的 (r0, r1) 上，比出该放大多少倍 */
    HBoxProject(bd, rot[0], rot[1], pd);
    HBoxProject(bs, rot[0], rot[1], ps);
    du = pd[1] - pd[0];  dv = pd[3] - pd[2];
    su = ps[1] - ps[0];  sv = ps[3] - ps[2];
    HLog("fit: disp extent u=%.3f v=%.3f | solid extent u=%.3f v=%.3f\n",
         du, dv, su, sv);
    if (su < 1e-9 || sv < 1e-9 || du < 1e-9 || dv < 1e-9)
    {
        HLog("fit: degenerate extent, skip\n");
        return;
    }

    ku = du / su;
    kv = dv / sv;
    k  = ((ku > kv) ? ku : kv) * HFIT_FILL;
    if (k < 0.05)
        k = 0.05;
    if (k > 500.0)
        k = 500.0;
    HLog("fit: k=%.4f (ku=%.4f kv=%.4f fill=%.3f)\n", k, ku, kv, HFIT_FILL);
    if (k > 0.98 && k < 1.02)
    {
        HLog("fit: solid already fills the view, keep as is\n");
        return;
    }

    /* 4) 取当前 pan/zoom 矩阵，认格式：若不是 [s·R | t] 那一套就不动 */
    memset(pz, 0, sizeof(pz));
    st = ProWindowCurrentMatrixGet(pz);
    HLog("fit: panzoomGet -> %d\n", (int)st);
    HLog("fit: pz [0]=(%.4f %.4f %.4f %.4f) [1]=(%.4f %.4f %.4f %.4f)\n",
         pz[0][0], pz[0][1], pz[0][2], pz[0][3],
         pz[1][0], pz[1][1], pz[1][2], pz[1][3]);
    HLog("fit: pz [2]=(%.4f %.4f %.4f %.4f) [3]=(%.4f %.4f %.4f %.4f) org=(%.3f %.3f %.3f)\n",
         pz[2][0], pz[2][1], pz[2][2], pz[2][3],
         pz[3][0], pz[3][1], pz[3][2], pz[3][3],
         pz[3][0], pz[3][1], pz[3][2]);
    if (st != PRO_TK_NO_ERROR)
        return;

    s0 = sqrt(pz[0][0] * pz[0][0] + pz[0][1] * pz[0][1] + pz[0][2] * pz[0][2]);
    if (s0 < 1e-12)
    {
        HLog("fit: panzoom row0 degenerate, skip\n");
        return;
    }
    dev = 0.0;
    for (i = 0; i < 3; i++)
    {
        double si = sqrt(pz[i][0] * pz[i][0] + pz[i][1] * pz[i][1]
                       + pz[i][2] * pz[i][2]);
        if (si < 1e-12)
        {
            dev = 9.9;
            break;
        }
        for (j = 0; j < 3; j++)
            if (fabs(pz[i][j] / si - rot[i][j]) > dev)
                dev = fabs(pz[i][j] / si - rot[i][j]);
    }
    HLog("fit: panzoom vs rot maxdev=%.5f  (%s)\n", dev,
         (dev < 1e-2) ? "same layout, apply k" : "UNKNOWN layout, skip");
    if (dev >= 1e-2)
        return;

    /* 5) 应用：线性部分乘 k；平移【相对保持】——让实体包围盒中心在屏幕上
       的位置保持不变（refit 之后它本来就在窗口中心，放大后仍然居中）。
       行向量约定 p_screen_j = s·(p·r_j) + t_j ⇒ t' = t + (s0 - s')·(c·r_j)。 */
    sc = s0 * k;
    cx = 0.5 * (bs[0][0] + bs[1][0]);
    cy = 0.5 * (bs[0][1] + bs[1][1]);
    cz = 0.5 * (bs[0][2] + bs[1][2]);

    for (i = 0; i < 3; i++)
        for (j = 0; j < 3; j++)
            pz[i][j] *= k;
    for (j = 0; j < 3; j++)
        pz[3][j] += (s0 - sc) * (cx * rot[j][0] + cy * rot[j][1] + cz * rot[j][2]);

    st = ProWindowPanZoomMatrixSet(win, pz);
    HLog("fit: panzoomSet -> %d  (k=%.3f s=%.3f center=(%.2f %.2f %.2f))\n",
         (int)st, k, sc, cx, cy, cz);
    if (st != PRO_TK_NO_ERROR)
        return;

    /* 6) 应用完再查一次角度 —— 缩放不该改旋转，改了就是格式猜错 */
    dev = HRotCheckLog(mdl, rot, "after-fit");
    if (dev < 0.0 || dev > 1e-3)
        HLog("fit: WARNING rotation changed by fit, re-applying orientation\n");
}

static int HViewNormalAct(uiCmdCmdId command, uiCmdValue *p_value,
                          void *p_push_command_data)
{
    ProMdl        mdl  = NULL;
    ProMdlType    mt   = PRO_MDL_UNUSED;
    ProSelection *sels = NULL;
    int           n = 0;
    int           i, k, got = 0, have_ref = 0;
    int           win = -1;
    ProVector     nrm, ref;
    double        dot;
    ProMatrix     m;
    ProError      st;

    (void)command;
    (void)p_value;
    (void)p_push_command_data;

    if (HGetSolid3D(&mdl, &mt) != PRO_TK_NO_ERROR)
    {
        HMsg("TD_NOT_3D");
        return 0;
    }

    /* 2026-10-22 改版：以【坐标轴】为基准。
       先看用户有没有预先选对象（平面/基准面 → 垂直于该面）；
       什么都没选就按模型坐标轴正视，不再弹拾取框，一点就归位。 */
    if (HSelbufferOnly("HViewNormalAct", &sels, &n))
    {
        for (i = 0; i < n && !got; i++)
        {
            if (HPlaneNormalFromSel(sels[i], mdl, nrm))
                got = 1;
        }
        HSelectFree(sels);
        sels = NULL;
    }

    if (!got)
    {
        /* 2026-10-23 定稿（用户：顺时针反了 ⇒ 改逆时针 90°）：
           相机仍在 -X 侧朝 +X 看 ⇒ nrm（屏幕 Z，朝观察者）= -X；
           画面逆时针转 90° ⇒ 屏幕右 = -Z、屏幕上 = -Y。
           右手系校验：m[1] = nrm × m[0] = (-X) × (-Z) = (0,-1,0) = -Y（屏幕上）；
           m[0] × m[1] = (-Z) × (-Y) = (-1,0,0) = -X = nrm ✓
           ⇒ 屏幕右 = -Z、屏幕上 = -Y、朝观察者 = -X。 */
        nrm[0] = -1.0;
        nrm[1] = 0.0;
        nrm[2] = 0.0;
        ref[0] = 0.0;
        ref[1] = 0.0;
        ref[2] = -1.0;   /* 屏幕右 = -Z ⇒ 屏幕上 = -Y */
        have_ref = 1;
        HLog("HViewNormalAct: no pick -> look +X from -X, normal (-1,0,0) ref (0,0,-1)\n");
    }
    HLog("HViewNormalAct: normal=(%.3f %.3f %.3f)\n", nrm[0], nrm[1], nrm[2]);

    /* ---- 参考轴：没指定时取与法向最不平行的世界轴（优先 X，然后 Z，最后 Y） ---- */
    if (!have_ref)
    {
        ref[0] = ref[1] = ref[2] = 0.0;
        if (fabs(nrm[0]) < 0.9)
            ref[0] = 1.0;
        else if (fabs(nrm[2]) < 0.9)
            ref[2] = 1.0;
        else
            ref[1] = 1.0;
    }

    /* ---- 构造视图矩阵：r0 = ref 在面内的投影，r1 = r2 × r0，r2 = 法向 ---- */
    dot = ref[0] * nrm[0] + ref[1] * nrm[1] + ref[2] * nrm[2];
    for (k = 0; k < 3; k++)
        m[0][k] = ref[k] - dot * nrm[k];
    HVecNormalize(m[0]);
    if (fabs(m[0][0]) + fabs(m[0][1]) + fabs(m[0][2]) < 1e-9)
    {
        HLog("HViewNormalAct: degenerate screen X, abort\n");
        HMsg("TD_VIEWNORMAL_FAIL");
        return 0;
    }
    m[1][0] = nrm[1] * m[0][2] - nrm[2] * m[0][1];
    m[1][1] = nrm[2] * m[0][0] - nrm[0] * m[0][2];
    m[1][2] = nrm[0] * m[0][1] - nrm[1] * m[0][0];
    HVecNormalize(m[1]);

    m[2][0] = nrm[0];
    m[2][1] = nrm[1];
    m[2][2] = nrm[2];
    m[0][3] = m[1][3] = m[2][3] = 0.0;
    m[3][0] = m[3][1] = m[3][2] = 0.0;
    m[3][3] = 1.0;

    st = 0;

    win = -1;
    ProWindowCurrentGet(&win);
    HLog("HViewNormalAct: win=%d\n", win);

    /* 先把模型里的基准坐标系列一遍，确认屏幕上那几个三轴到底是什么 */
    HDatumCsysScan(mdl);

    /* ---- 1) 先定【角度】。ProView.h 明说 Set 会自带 shift + scaling
       （“to ensure that the model fits properly into the view”），所以它必须是
       最后一个改朝向的动作；距离放在它之后，用窗口的 pan/zoom 矩阵单独调。
       这样「角度」和「距离」彻底解耦 —— 缩放按定义改不了旋转。 ---- */
    st = ProViewMatrixSet(mdl, NULL, m);
    HLog("HViewNormalAct: matrixSet -> %d\n", (int)st);
    if (st != PRO_TK_NO_ERROR)
    {
        HMsg("TD_VIEWNORMAL_FAIL");
        return 0;
    }

    /* 角度确认：回读 → 每行除以自身长度归一化 → 逐元素比 maxdev + det(R)=+1 */
    HRotCheckLog(mdl, m, "after-set");

#if HFIT_USE_REFIT
    /* 老路子：ProWindowRefit 连远处的基准一起框 ⇒ 实体被缩成一个点 */
    st = ProWindowRefit(win);
    HLog("HViewNormalAct: windowRefit(win=%d) -> %d\n", win, (int)st);
    if (st != PRO_TK_NO_ERROR)
    {
        st = ProViewRefit(mdl, NULL);
        HLog("HViewNormalAct: viewRefit -> %d\n", (int)st);
    }
    HRotCheckLog(mdl, m, "after-refit");
#else
    /* 2) 【无条件】强制重算距离。Set 在矩阵和上次完全一样时会跳过它自带的
       shift+scaling —— 这就是「第一次缩放完、后面不缩放了」的根因。
       ProWindowRefit 只重算平移/缩放、不动旋转，随后用 rotcheck 验证。 */
    {
        double dev;

        st = ProWindowRefit(win);
        HLog("HViewNormalAct: windowRefit(win=%d) -> %d\n", win, (int)st);
        if (st != PRO_TK_NO_ERROR)
        {
            st = ProViewRefit(mdl, NULL);
            HLog("HViewNormalAct: viewRefit -> %d\n", (int)st);
            st = ProWindowRefit(win);
            HLog("HViewNormalAct: windowRefit retry -> %d\n", (int)st);
        }

        dev = HRotCheckLog(mdl, m, "after-refit");
        if (dev > 1e-3)
            HLog("HViewNormalAct: WARNING refit changed the rotation\n");
    }

    /* 3) 基准修正 + 填充微调：k = 含基准包围盒 ÷ 实体包围盒 × HFIT_FILL */
    HViewFitSolidOnly(mdl, m, win);
#endif

    ProWindowRepaint((win > 0) ? win : PRO_VALUE_UNUSED);
    ProWindowDeviceFlush(NULL);   /* 显示命令强制冲掉，避免残留帧 */

    HMsg("TD_VIEWNORMAL_OK");
    return 0;
}

/*--------------------------- 7. 批量备份 -----------------------------*/
static int HBatchBackupAct(uiCmdCmdId command, uiCmdValue *p_value,
                           void *p_push_command_data)
{
    ProMdl     mdl  = NULL;
    ProMdlType type = PRO_MDL_UNUSED;
    ProPath    dir;
    ProError   status;

    (void)command;
    (void)p_value;
    (void)p_push_command_data;

    if (HGetSolid3D(&mdl, &type) != PRO_TK_NO_ERROR)
    {
        HMsg("TD_NOT_3D");
        return 0;
    }

    dir[0] = L'\0';
    status = ProDirectoryChoose(L"选择备份目录 / Backup to", NULL, NULL, NULL, dir);
    HLog("HBatchBackupAct: choose -> %d\n", (int)status);
    if (status == PRO_TK_USER_ABORT)
        return 0;
    if (status != PRO_TK_NO_ERROR || dir[0] == L'\0')
    {
        HMsg("TD_CANCEL");
        return 0;
    }
    HLogW("HBatchBackupAct: dir = ", dir);

    status = ProMdlnameBackup(mdl, dir);
    HLog("HBatchBackupAct: backup -> %d\n", (int)status);
    HMsg(status == PRO_TK_NO_ERROR ? "TD_BACKUP_OK" : "TD_BACKUP_FAIL");
    return 0;
}

/*--------------------------- 8. 从方程建曲线 -----------------------------*/
/* 2026-10-23 新增。
   原理：Creo「来自方程的曲线」这个特征的 Element Tree 在 ProDtmCrv.h
   里有完整文档（"FROM EQUATION" 那一节），结构是固定的 8 个元素：

     PRO_E_FEATURE_TREE
       |-- PRO_E_FEATURE_TYPE         = PRO_FEAT_CURVE
       |-- PRO_E_CURVE_TYPE           = PRO_CURVE_TYPE_FROM_EQUATION (27)
       |-- PRO_E_STD_FEATURE_NAME     特征名（文档未标 Mandatory）
       |-- PRO_E_CRV_FR_EQ_REF_CSYS   坐标系   PRO_VALUE_TYPE_SELECTION
       |-- PRO_E_CRV_FR_EQ_CSYS_TYPE  坐标系类型 ProCrvFrEquatCsysTypes
       |-- PRO_E_CRV_FR_EQ_PARAM_MIN  参数下限 PRO_VALUE_TYPE_DOUBLE
       |-- PRO_E_CRV_FR_EQ_PARAM_MAX  参数上限 PRO_VALUE_TYPE_DOUBLE
       |-- PRO_E_CRV_ENTER_EQUATION   方程正文 PRO_VALUE_TYPE_WSTRING

   ★ 关键结论：**方程不归插件解析**。PRO_E_CRV_ENTER_EQUATION 只是一段
     多行文本，交给 Creo 自己的方程解析器 —— 所以 sin / cos / if / endif /
     evalgraph / 模型参数全都跟手动建曲线一模一样，插件不用写表达式引擎。

   交互：方程写在 D:\H\eq_curve.txt（UTF-8 带不带 BOM 都行，也认 ANSI）。
     · 文件不存在 → 自动写一份模板并用记事本打开，提示改完再点一次；
     · 文件存在   → 直接读、直接建特征。
   文件头是 # TYPE: / # CSYS:，其余不以 # 开头的非空行拼成方程正文。 */

#define H_EQFILE    "D:\\H\\eq_curve.txt"
#define H_EQ_MAX    16384
#define H_CSYS_MAX  128

static wchar_t g_eq_buf[H_EQ_MAX];
static wchar_t g_csys_name[H_CSYS_MAX];

/* 对话框的坐标系下拉用：先把模型里所有坐标系枚举一遍（名字 + 坐标系 id），
   用户选中哪个就按 id 直接用，不再靠名字匹配。
   ★★★ 2026-10-23 关键修：id 必须是【坐标系 datum id】，不是【特征 id】！
   原来用 ProSolidFeatVisit 收 PRO_FEAT_CSYS，拿到的是 feature id；拿它去
   ProModelitemInit(mdl, id, PRO_CSYS) 直接 -23(NOT_EXIST)，于是退路用
   PRO_FEATURE 建引用，喂给 PRO_E_CRV_FR_EQ_REF_CSYS 的就是一条"特征引用"，
   不是坐标系引用 —— 创建时 Creo 在参数/元素校验之外就 -1 掉，errs 还是 0。
   正确做法（官方 UtilCollect.c:ProUtilFindCsysByName 同款）：
     ProSolidCsysVisit -> ProCsysToGeomitem -> ProGeomitem{type=PRO_CSYS, id} */
#define H_CSYS_LIST_MAX  32

typedef struct
{
    wchar_t name[H_CSYS_MAX];
    int     id;                  /* 坐标系 datum id（不是特征 id） */
} HCsysItem;

static HCsysItem g_csys_list[H_CSYS_LIST_MAX];
static int       g_csys_cnt     = 0;   /* 可用项数 */
static int       g_csys_sel_id  = 0;   /* 对话框选中的坐标系 id；0 = 自动取第一个 */
static ProSolid  g_csys_solid   = NULL;/* ProCsysToGeomitem 需要 owner */

/* 整个文件读成宽字符串；UTF-8 BOM 优先，否则按系统码页(GBK) */
static wchar_t *HLoadTextW(const char *path)
{
    FILE    *fp;
    long     sz;
    char    *buf;
    wchar_t *w;
    int      n, skip = 0;
    UINT     cp = CP_ACP;

    fp = fopen(path, "rb");
    if (fp == NULL)
        return NULL;
    fseek(fp, 0, SEEK_END);
    sz = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (sz <= 0 || sz > (long)(1 << 20))
    {
        fclose(fp);
        return NULL;
    }
    buf = (char *)malloc((size_t)sz);
    if (buf == NULL)
    {
        fclose(fp);
        return NULL;
    }
    if (fread(buf, 1, (size_t)sz, fp) != (size_t)sz)
    {
        free(buf);
        fclose(fp);
        return NULL;
    }
    fclose(fp);

    if (sz >= 3 && (unsigned char)buf[0] == 0xEF
        && (unsigned char)buf[1] == 0xBB && (unsigned char)buf[2] == 0xBF)
    {
        skip = 3;
        cp   = CP_UTF8;
    }
    n = MultiByteToWideChar(cp, 0, buf + skip, (int)sz - skip, NULL, 0);
    if (n <= 0)
    {
        free(buf);
        return NULL;
    }
    w = (wchar_t *)malloc(sizeof(wchar_t) * ((size_t)n + 1));
    if (w == NULL)
    {
        free(buf);
        return NULL;
    }
    MultiByteToWideChar(cp, 0, buf + skip, (int)sz - skip, w, n);
    w[n] = L'\0';
    free(buf);
    return w;
}

/* 写模板（UTF-8 带 BOM：新版记事本、VS Code 都能认） */
static void HWriteEqTemplate(void)
{
    FILE *fp = fopen(H_EQFILE, "wb");

    if (fp == NULL)
        return;
    fputs("\xEF\xBB\xBF", fp);
    fputs(
"# TYPE: cartesian\n"
"# CSYS:\n"
"# ------------------------------------------------------------------\n"
"# \xe4\xb8\x8a\xe9\x9d\xa2\xe4\xb8\xa4\xe8\xa1\x8c\xe4\xb8\x8d\xe8\xa6\x81\xe5\x88\xa0\xe3\x80\x82\n"
"#   TYPE   cartesian(\xe7\xac\x9b\xe5\x8d\xa1\xe5\xb0\x94) / cylindrical(\xe5\x9c\x86\xe6\x9f\xb1) / spherical(\xe7\x90\x83)\n"
"#   CSYS   \xe7\x95\x99\xe7\xa9\xba = \xe8\x87\xaa\xe5\x8a\xa8\xe7\x94\xa8\xe6\xa8\xa1\xe5\x9e\x8b\xe9\x87\x8c\xe7\x9a\x84\xe7\xac\xac\xe4\xb8\x80\xe4\xb8\xaa\xe5\x9d\x90\xe6\xa0\x87\xe7\xb3\xbb\n"
"#          \xe4\xb9\x9f\xe5\x8f\xaf\xe4\xbb\xa5\xe5\xa1\xab\xe5\x9d\x90\xe6\xa0\x87\xe7\xb3\xbb\xe5\x90\x8d\xe7\xa7\xb0\n"
"#   t \xe5\x9b\xba\xe5\xae\x9a\xe4\xbb\x8e 0 \xe5\x88\xb0 1\xef\xbc\x8c\xe4\xb8\x89\xe8\xa7\x92\xe5\x87\xbd\xe6\x95\xb0\xe7\x94\xa8\xe3\x80\x8c\xe5\xba\xa6\xe3\x80\x8d (sin(30)=0.5)\n"
"# ------------------------------------------------------------------\n"
"\n"
"x = 30*t\n"
"y = 15*sin(360*t)\n"
"z = 0\n", fp);
    fclose(fp);
}

/* 忽略大小写比较前缀（只用于 "# TYPE:" 这类固定头） */
static int HWPrefixNoCase(const wchar_t *line, const wchar_t *key)
{
    size_t i;

    for (i = 0; key[i] != L'\0'; i++)
    {
        wchar_t a = line[i], b = key[i];

        if (a >= L'a' && a <= L'z')
            a = (wchar_t)(a - 32);
        if (b >= L'a' && b <= L'z')
            b = (wchar_t)(b - 32);
        if (a == L'\0' || a != b)
            return 0;
    }
    return 1;
}

/* 解析：抽 # TYPE: / # CSYS:，其余非 # 开头的非空行拼成方程正文 */
static void HEqParse(wchar_t *txt, int *p_type)
{
    wchar_t *line = txt;
    int      eqlen = 0;

    *p_type     = PRO_CRV_FR_EQ_CSYS_TYPE_CARTESIAN;
    g_csys_name[0] = L'\0';
    g_csys_sel_id  = 0;          /* 文件模式按名字找，不用下拉框的 id */
    g_eq_buf[0]    = L'\0';

    while (line != NULL && *line != L'\0')
    {
        wchar_t *next = wcschr(line, L'\n');
        wchar_t *q;
        size_t   L;

        if (next != NULL)
        {
            *next = L'\0';
            next++;
        }
        L = wcslen(line);
        while (L > 0 && (line[L - 1] == L'\r' || line[L - 1] == L' '
                         || line[L - 1] == L'\t'))
            line[--L] = L'\0';
        q = line;
        while (*q == L' ' || *q == L'\t')
            q++;

        if (*q == L'#')
        {
            if (HWPrefixNoCase(q, L"# TYPE:"))
            {
                const wchar_t *v = q + 7;

                while (*v == L' ' || *v == L'\t')
                    v++;
                if (HWPrefixNoCase(v, L"cylind") || *v == L'1')
                    *p_type = PRO_CRV_FR_EQ_CSYS_TYPE_CYLINDRICAL;
                else if (HWPrefixNoCase(v, L"spher") || *v == L'2')
                    *p_type = PRO_CRV_FR_EQ_CSYS_TYPE_SPHERICAL;
                else
                    *p_type = PRO_CRV_FR_EQ_CSYS_TYPE_CARTESIAN;
            }
            else if (HWPrefixNoCase(q, L"# CSYS:"))
            {
                const wchar_t *v = q + 7;
                size_t         n;

                while (*v == L' ' || *v == L'\t')
                    v++;
                n = wcslen(v);
                if (n > 0 && n < H_CSYS_MAX)
                    wcscpy(g_csys_name, v);
            }
        }
        else if (*q != L'\0')
        {
            L = wcslen(q);
            if (eqlen + (int)L + 2 < H_EQ_MAX)
            {
                wcscpy(g_eq_buf + eqlen, q);
                eqlen += (int)L;
                g_eq_buf[eqlen++] = L'\n';
                g_eq_buf[eqlen]   = L'\0';
            }
        }
        line = next;
    }
}

/* 找坐标系：名字非空按名字比，否则取模型里第一个坐标系。
   走 ProSolidCsysVisit（坐标系对象），id 才是能喂给 ProModelitemInit(PRO_CSYS) 的那个。 */
typedef struct h_csys_find
{
    int found;
    int id;
} HCsysFind;

/* ProCsys 是不透明句柄，必须经 ProCsysToGeomitem 换成 ProGeomitem 才拿得到 id */
static int HCsysToItem(ProCsys csys, ProGeomitem *p_gi)
{
    ProError st;

    if (csys == NULL || p_gi == NULL || g_csys_solid == NULL)
        return 0;
    memset(p_gi, 0, sizeof(*p_gi));
    st = ProCsysToGeomitem(g_csys_solid, csys, p_gi);
    if (st != PRO_TK_NO_ERROR)
    {
        HLog("HCsysToItem: csysToGeomitem -> %d\n", (int)st);
        return 0;
    }
    return 1;
}

static ProError HCsysFindVisit(ProCsys csys, ProError status, ProAppData app)
{
    HCsysFind  *f = (HCsysFind *)app;
    ProGeomitem gi;
    ProName     nm;

    if (status != PRO_TK_NO_ERROR || csys == NULL || f == NULL || f->found > 0)
        return PRO_TK_NO_ERROR;
    if (!HCsysToItem(csys, &gi))
        return PRO_TK_NO_ERROR;

    nm[0] = L'\0';
    ProModelitemNameGet(&gi, nm);
    if (g_csys_name[0] != L'\0' && wcscmp(nm, g_csys_name) != 0)
    {
        HLog("HEqCurveAct: skip csys id=%d name=", gi.id);
        HLogW("", nm);
        return PRO_TK_NO_ERROR;              /* 名字不符，继续找 */
    }
    HLog("HEqCurveAct: csys(datum) id=%d name=", gi.id);
    HLogW("", nm);
    f->id    = gi.id;
    f->found = 1;
    return PRO_TK_NO_ERROR;
}

/* 枚举模型里全部坐标系，给下拉框用 */
static ProError HCsysListVisit(ProCsys csys, ProError status, ProAppData app)
{
    int        *p_n = (int *)app;
    ProGeomitem gi;

    if (status != PRO_TK_NO_ERROR || csys == NULL || p_n == NULL)
        return PRO_TK_NO_ERROR;
    if (*p_n >= H_CSYS_LIST_MAX)
        return PRO_TK_NO_ERROR;
    if (!HCsysToItem(csys, &gi))
        return PRO_TK_NO_ERROR;

    memset(&g_csys_list[*p_n], 0, sizeof(g_csys_list[*p_n]));
    ProModelitemNameGet(&gi, g_csys_list[*p_n].name);
    g_csys_list[*p_n].name[H_CSYS_MAX - 1] = L'\0';
    g_csys_list[*p_n].id                   = gi.id;
    HLog("HCsysListVisit: [%d] id=%d name=", *p_n, gi.id);
    HLogW("", g_csys_list[*p_n].name);
    (*p_n)++;
    return PRO_TK_NO_ERROR;
}

static void HEqCsysCollect(ProMdl mdl)
{
    int      n = 0;
    ProError st;

    g_csys_cnt   = 0;
    g_csys_solid = NULL;
    if (mdl == NULL)
        return;
    g_csys_solid = (ProSolid)mdl;
    st = ProSolidCsysVisit((ProSolid)mdl, HCsysListVisit, NULL, (ProAppData)&n);
    g_csys_cnt = n;
    HLog("HEqCsysCollect: %d csys found (st=%d)\n", g_csys_cnt, (int)st);
}

/* 往树里加一个 int / double / wstring 元素
   ★ 2026-10-23 修：原来这三个函数不管中间哪一步失败都无条件 return 1，
   日志里 "add xxx -> 1" 是假的，把真正的错误全盖掉了。现在逐段返回真实
   状态码，任何一个 0 以外的值都能在 h_load.log 里看出是哪一步挂的。 */
static ProError HEqAddInt(ProElement tree, ProElemId id, int val)
{
    ProElement e  = NULL;
    ProError   st = PRO_TK_GENERAL_ERROR;

    st = ProElementAlloc(id, &e);
    if (st != PRO_TK_NO_ERROR || e == NULL)
        return (st != PRO_TK_NO_ERROR) ? st : PRO_TK_BAD_INPUTS;
    st = ProElementIntegerSet(e, val);
    if (st == PRO_TK_NO_ERROR)
        st = ProElemtreeElementAdd(tree, NULL, e);
    if (st != PRO_TK_NO_ERROR)
        ProElementFree(&e);
    return st;
}

static ProError HEqAddDouble(ProElement tree, ProElemId id, double val)
{
    ProElement e  = NULL;
    ProError   st = PRO_TK_GENERAL_ERROR;

    st = ProElementAlloc(id, &e);
    if (st != PRO_TK_NO_ERROR || e == NULL)
        return (st != PRO_TK_NO_ERROR) ? st : PRO_TK_BAD_INPUTS;
    st = ProElementDoubleSet(e, val);
    if (st == PRO_TK_NO_ERROR)
        st = ProElemtreeElementAdd(tree, NULL, e);
    if (st != PRO_TK_NO_ERROR)
        ProElementFree(&e);
    return st;
}

static ProError HEqAddWstring(ProElement tree, ProElemId id, wchar_t *val)
{
    ProElement e  = NULL;
    ProError   st = PRO_TK_GENERAL_ERROR;

    st = ProElementAlloc(id, &e);
    if (st != PRO_TK_NO_ERROR || e == NULL)
        return (st != PRO_TK_NO_ERROR) ? st : PRO_TK_BAD_INPUTS;
    st = ProElementWstringSet(e, val);
    if (st == PRO_TK_NO_ERROR)
        st = ProElemtreeElementAdd(tree, NULL, e);
    if (st != PRO_TK_NO_ERROR)
        ProElementFree(&e);
    return st;
}

/*=================== 8b. 内置方程模板库 ==============================
   2026-10-23 用户要求：「把有用的方程都提前写好，点按钮选方程、改几个参数、
   自动生成」。所以这里预置一批常用曲线，参数由对话框输入。

   每个模板只存「方程正文」，参数赋值行由 HAppend 自动拼在正文前面，
   所以正文里直接引用变量名（r0 / h / b1 ...）即可，改参数就是改那一行。

   ★ Creo 关系式的三条铁律（模板里都遵守）：
     · t 固定 0→1；
     · 三角函数用【度】，sin(30)=0.5，所以 180*u、360*u 才是半圈/一圈；
     · 没有 pi 常量，弧度换算写 0.0174532925199433（=pi/180），
       2*pi 写 6.28318530717959。
   ==================================================================== */

typedef struct
{
    const wchar_t *var;      /* 方程里用的变量名 */
    const wchar_t *label;    /* 对话框里显示的中文说明 */
    double         def;      /* 默认值 */
} HEqPrm;

typedef struct
{
    const wchar_t *title;    /* 列表里显示的名字 */
    const wchar_t *desc;     /* 右侧说明（支持 \n 换行） */
    int            ctype;    /* 0=笛卡尔 1=圆柱 2=球；-1 = 读 eq_curve.txt */
    int            nprm;
    HEqPrm         prm[6];
    const wchar_t *eq;       /* 单曲线模板的方程正文；分段模板为 NULL */
    /* 分段模板（凸轮）：每段一条曲线，各自一个不含条件的表达式。
       2026-10-23：方程求值器不接受 if/else/endif（9 个自检用例证实），
       所以凸轮按运动段拆开建，靠 t 映射到本段的角度区间。 */
    const wchar_t *segname[8];
    const wchar_t *segeq[8];
    int            nseg;     /* 0 = 单曲线，用 eq */
} HEqTpl;

static const HEqTpl g_eqtpl[] =
{
    /* 0 ---------------------------------------------------------------- */
    {
        L"盘形凸轮 - 简谐运动",
        L"柱坐标。最常用的运动规律，中低速首选，加速度连续无刚性冲击。\n"
        L"r0=基圆半径，h=行程，b1/b2/b3=推程/远休/回程角（度和不要超 360）。\n"
        L"按运动段拆成 4 条曲线（推程/远休/回程/近休），首尾严格连续；\n"
        L"拉伸时进草图用「参考-使用边」把它们选成一圈即为完整轮廓。\n"
        L"（得到的是理论轮廓；滚子从动件要在 Creo 里按滚子半径向内偏移）",
        1, 5,
        {
            { L"r0", L"基圆半径 r0 (mm)",  40.0 },
            { L"h",  L"行程 h (mm)",       20.0 },
            { L"b1", L"推程角 b1 (度)",   120.0 },
            { L"b2", L"远休止角 b2 (度)",  60.0 },
            { L"b3", L"回程角 b3 (度)",   120.0 },
        },
        NULL,
        { L"CAM_RISE", L"CAM_DWELL_F", L"CAM_RETURN", L"CAM_DWELL_N" },
        {
            L"theta = b1*t\n"
            L"r = r0+h*(1-cos(180*t))/2\n"
            L"z = 0",

            L"theta = b1+b2*t\n"
            L"r = r0+h\n"
            L"z = 0",

            L"theta = b1+b2+b3*t\n"
            L"r = r0+h*(1+cos(180*t))/2\n"
            L"z = 0",

            L"theta = b1+b2+b3+rest*t\n"
            L"r = r0\n"
            L"z = 0"
        },
        4
    },
    /* 1 ---------------------------------------------------------------- */
    {
        L"盘形凸轮 - 摆线运动",
        L"柱坐标。推程回程都无任何冲击（速度、加速度全连续），高速凸轮首选。\n"
        L"参数含义同「简谐运动」；同样按 4 段拆成 4 条曲线，首尾连续。",
        1, 5,
        {
            { L"r0", L"基圆半径 r0 (mm)",  40.0 },
            { L"h",  L"行程 h (mm)",       20.0 },
            { L"b1", L"推程角 b1 (度)",   120.0 },
            { L"b2", L"远休止角 b2 (度)",  60.0 },
            { L"b3", L"回程角 b3 (度)",   120.0 },
        },
        NULL,
        { L"CAM_RISE", L"CAM_DWELL_F", L"CAM_RETURN", L"CAM_DWELL_N" },
        {
            L"theta = b1*t\n"
            L"r = r0+h*(t-sin(360*t)/6.28318530717959)\n"
            L"z = 0",

            L"theta = b1+b2*t\n"
            L"r = r0+h\n"
            L"z = 0",

            L"theta = b1+b2+b3*t\n"
            L"r = r0+h*(1-t+sin(360*t)/6.28318530717959)\n"
            L"z = 0",

            L"theta = b1+b2+b3+rest*t\n"
            L"r = r0\n"
            L"z = 0"
        },
        4
    },
    /* 2 ---------------------------------------------------------------- */
    {
        L"盘形凸轮 - 等加速等减速",
        L"柱坐标。加速度分段恒定，只有柔性冲击，中速场合常用。\n"
        L"推程/回程各有一个 u=0.5 的拐点，所以拆成 6 条曲线\n"
        L"（推程前后段/远休/回程前后段/近休），首尾连续。",
        1, 5,
        {
            { L"r0", L"基圆半径 r0 (mm)",  40.0 },
            { L"h",  L"行程 h (mm)",       20.0 },
            { L"b1", L"推程角 b1 (度)",   120.0 },
            { L"b2", L"远休止角 b2 (度)",  60.0 },
            { L"b3", L"回程角 b3 (度)",   120.0 },
        },
        NULL,
        { L"CAM_RISE1", L"CAM_RISE2", L"CAM_DWELL_F",
          L"CAM_RET1",  L"CAM_RET2",  L"CAM_DWELL_N" },
        {
            L"theta = b1*0.5*t\n"
            L"r = r0+h*0.5*t*t\n"
            L"z = 0",

            L"theta = b1*(0.5+0.5*t)\n"
            L"r = r0+h-h*0.5*(1-t)*(1-t)\n"
            L"z = 0",

            L"theta = b1+b2*t\n"
            L"r = r0+h\n"
            L"z = 0",

            L"theta = b1+b2+b3*0.5*t\n"
            L"r = r0+h-h*0.5*t*t\n"
            L"z = 0",

            L"theta = b1+b2+b3*(0.5+0.5*t)\n"
            L"r = r0+h*0.5*(1-t)*(1-t)\n"
            L"z = 0",

            L"theta = b1+b2+b3+rest*t\n"
            L"r = r0\n"
            L"z = 0"
        },
        6
    },
    /* 3 ---------------------------------------------------------------- */
    {
        L"盘形凸轮 - 等速运动",
        L"柱坐标。位移线性，但首尾速度突变有刚性冲击，只适合低速、轻载、单向送料。\n"
        L"参数含义同「简谐运动」；按 4 段拆成 4 条曲线。",
        1, 5,
        {
            { L"r0", L"基圆半径 r0 (mm)",  40.0 },
            { L"h",  L"行程 h (mm)",       20.0 },
            { L"b1", L"推程角 b1 (度)",   120.0 },
            { L"b2", L"远休止角 b2 (度)",  60.0 },
            { L"b3", L"回程角 b3 (度)",   120.0 },
        },
        NULL,
        { L"CAM_RISE", L"CAM_DWELL_F", L"CAM_RETURN", L"CAM_DWELL_N" },
        {
            L"theta = b1*t\n"
            L"r = r0+h*t\n"
            L"z = 0",

            L"theta = b1+b2*t\n"
            L"r = r0+h\n"
            L"z = 0",

            L"theta = b1+b2+b3*t\n"
            L"r = r0+h*(1-t)\n"
            L"z = 0",

            L"theta = b1+b2+b3+rest*t\n"
            L"r = r0\n"
            L"z = 0"
        },
        4
    },
    /* 4 ---------------------------------------------------------------- */
    {
        L"渐开线（齿轮齿廓）",
        L"笛卡尔坐标。rb=基圆半径，a0/a1=起始/终止展开角（度），"
        L"半径 rb/cos(a1) 处结束。\n"
        L"做完整齿轮：再用「阵列 / 镜像」绕中心按 360/z 复制，齿根用圆角过渡。",
        0, 3,
        {
            { L"rb", L"基圆半径 rb (mm)",      40.0 },
            { L"a0", L"起始展开角 (度)",   0.0 },
            { L"a1", L"终止展开角 (度)",  60.0 },
        },
        L"u = a0+(a1-a0)*t\n"
        L"ar = u*0.0174532925199433\n"
        L"x = rb*(cos(u)+ar*sin(u))\n"
        L"y = rb*(sin(u)-ar*cos(u))\n"
        L"z = 0"
    },
    /* 5 ---------------------------------------------------------------- */
    {
        L"圆柱螺旋线（弹簧）",
        L"柱坐标。R=中径半径，n=圈数，p=螺距，z0=起点高度。\n"
        L"得到中心线；再用「扫描」配圆形截面，就是弹簧实体。",
        1, 4,
        {
            { L"R",  L"中径半径 R (mm)",      20.0 },
            { L"n",  L"圈数 n (圈)",           8.0 },
            { L"p",  L"螺距 p (mm)",           6.0 },
            { L"z0", L"起点高度 z0 (mm)",      0.0 },
        },
        L"theta = t*360*n\n"
        L"r = R\n"
        L"z = z0+p*n*t"
    },
    /* 6 ---------------------------------------------------------------- */
    {
        L"圆锥螺旋线（锥形弹簧）",
        L"柱坐标。R1/R2=小端/大端半径，n=圈数，p=螺距。\n"
        L"半径随高度线性变化，用于塔形簧、变径簧。",
        1, 4,
        {
            { L"R1", L"小端半径 R1 (mm)",     15.0 },
            { L"R2", L"大端半径 R2 (mm)",     25.0 },
            { L"n",  L"圈数 n (圈)",           8.0 },
            { L"p",  L"螺距 p (mm)",           6.0 },
        },
        L"theta = t*360*n\n"
        L"r = R1+(R2-R1)*t\n"
        L"z = p*n*t"
    },
    /* 7 ---------------------------------------------------------------- */
    {
        L"变螺距弹簧",
        L"柱坐标。a/b=起始/终止螺距，螺距沿高度线性变化（两端密、中间疏之类）。\n"
        L"R=中径半径，n=圈数。",
        1, 4,
        {
            { L"R", L"中径半径 R (mm)",      20.0 },
            { L"n", L"圈数 n (圈)",           8.0 },
            { L"a", L"起始螺距 a (mm)",       3.0 },
            { L"b", L"终止螺距 b (mm)",      10.0 },
        },
        /* 2026-10-28 实测：被 Creo 12 拒的是变量名 p1/p2，不是语法。
           同一次跑的 11 条语法探针里，只有把 p1/p2 换成 a/b 的那条（P10）
           拿到 create 0，其余全 -1 —— 括号后接乘、嵌套括号、减号、t*t 都没问题。
           所以这里就用跟「阿基米德螺线」「椭圆」同一套短名写法（a/b），
           方程也逐字取自 P10，别再自创长名。
           数学：n*(a*t + (b-a)*t²/2) 就是螺距从 a 线性变到 b 的螺旋高度。 */
        L"theta = t*360*n\n"
        L"r = R\n"
        L"z = n*(a*t+(b-a)*t*t/2)"
    },
    /* 8 ---------------------------------------------------------------- */
    {
        L"阿基米德螺线",
        L"柱坐标。a=起始半径，b=每弧度半径增量，n=圈数。\n"
        L"用于盘形凸轮近似轮廓、涡卷、蚊香形流道。",
        1, 3,
        {
            { L"a", L"起始半径 a (mm)",        2.0 },
            { L"b", L"每弧度增量 b (mm/rad)",      3.0 },
            { L"n", L"圈数 n (圈)",            3.0 },
        },
        L"theta = t*360*n\n"
        L"r = a+b*theta*0.0174532925199433\n"
        L"z = 0"
    },
    /* 9 ---------------------------------------------------------------- */
    {
        L"对数螺线（等压力角凸轮）",
        L"柱坐标。a=起始半径，k=压力角系数（越大张得越快），n=圈数。\n"
        L"与径向线的夹角处处相等，是做等压力角凸轮的理论轮廓。",
        1, 3,
        {
            { L"a", L"起始半径 a (mm)",       10.0 },
            { L"k", L"压力角系数 k (无量纲)",     0.15 },
            { L"n", L"圈数 n (圈)",            3.0 },
        },
        L"theta = t*360*n\n"
        L"r = a*exp(k*theta*0.0174532925199433)\n"
        L"z = 0"
    },
    /* 10 --------------------------------------------------------------- */
    {
        L"摆线（摆线针轮齿廓）",
        L"笛卡尔坐标。R=滚动圆半径，T=总转角（度，360=一整圈）。\n"
        L"摆线针轮、齿根过渡曲线都用它；改 T 只取一段。",
        0, 2,
        {
            { L"R", L"滚动圆半径 R (mm)",     30.0 },
            { L"T", L"总转角 T (度)",   360.0 },
        },
        L"u = T*t\n"
        L"ur = u*0.0174532925199433\n"
        L"x = R*(ur-sin(u))\n"
        L"y = R*(1-cos(u))\n"
        L"z = 0"
    },
    /* 11 --------------------------------------------------------------- */
    {
        L"椭圆",
        L"笛卡尔坐标。a/b=长/短半轴，一笔画满一整圈。\n"
        L"做成实体：拉伸 / 或用扫描配截面。",
        0, 2,
        {
            { L"a", L"长半轴 a (mm)",         40.0 },
            { L"b", L"短半轴 b (mm)",         25.0 },
        },
        L"x = a*cos(360*t)\n"
        L"y = b*sin(360*t)\n"
        L"z = 0"
    },
    /* 12 --------------------------------------------------------------- */
    {
        L"正弦曲线",
        L"笛卡尔坐标。A=幅值，L=总长，n=周期数。\n"
        L"做波纹板截面、蛇形管中心线、拖链走向。",
        0, 3,
        {
            { L"A", L"幅值 A (mm)",           15.0 },
            { L"L", L"总长 L (mm)",          100.0 },
            { L"n", L"周期数 n (个)",          3.0 },
        },
        L"x = L*t\n"
        L"y = A*sin(360*n*t)\n"
        L"z = 0"
    },
    /* 13 --------------------------------------------------------------- */
    {
        L"正弦波纹环（波形垫圈）",
        L"柱坐标。R=中径半径，A=波高，n=波数。\n"
        L"在 XY 平面走一整圈，高度按正弦起伏；扫描小截面即为波形垫圈。",
        1, 3,
        {
            { L"R", L"中径半径 R (mm)",       25.0 },
            { L"A", L"波高 A (mm)",            3.0 },
            { L"n", L"波数 n (个)",            6.0 },
        },
        L"theta = t*360\n"
        L"r = R\n"
        L"z = A*sin(n*theta)"
    },
    /* 15 ---------------------------------------------------------------
       以下 5 条是给精密转轴/传动件补的（渐开线齿廓、蜗杆、变截面母线、
       轴肩过渡、波形弹簧）。凡是「绕轴回转成形」的，都是一条母线绕 X 轴
       转 360° 或沿母线扫描，方程只负责这条母线/中心线。 */
    {
        L"渐开线齿廓（按模数齿数压力角）",
        L"笛卡尔坐标。m=模数，z=齿数，alpha=压力角(度)，ua=画多长(展开角,度)。\n"
        L"基圆半径 rb=m*z*cos(alpha)/2 自动算，改 m/z 齿形立刻跟着变，不用手算 rb。\n"
        L"ua 要盖到齿顶圆：ua≈acos(rb/ra)，20齿m2(ra=22,rb=18.79)约 32°。\n"
        L"用途：齿轮齿廓、渐开线花键(DIN 5480 / ANSI B92.1)齿侧、涡旋盘型线。\n"
        L"用法：镜像出对称齿侧 → 按 360/z 阵列 → 齿根加过渡圆角。",
        0, 4,
        {
            { L"m",     L"模数 m (mm)",              2.0 },
            { L"z",     L"齿数 z (个)",             20.0 },
            { L"alpha", L"压力角 alpha (度)",        20.0 },
            { L"ua",    L"展开角上限 ua (度)",       32.0 },
        },
        L"rb = m*z*cos(alpha)/2\n"
        L"u = ua*t\n"
        L"ar = u*0.0174532925199433\n"
        L"x = rb*(cos(u)+ar*sin(u))\n"
        L"y = rb*(sin(u)-ar*cos(u))\n"
        L"z = 0"
    },
    /* 16 --------------------------------------------------------------- */
    {
        L"蜗杆螺旋线（按模数头数）",
        L"柱坐标。m=轴向模数，z1=头数，q=直径系数，n=圈数。\n"
        L"分度圆半径 r1=m*q/2、导程 s=pi*m*z1 全部自动算，不用手推导程。\n"
        L"用途：阿基米德蜗杆(ZA)/法向直廓蜗杆(ZN)的中心线——\n"
        L"      配一条轴向直线齿廓做「螺旋扫描」即得蜗杆；也是丝杠、螺纹中心线。",
        1, 4,
        {
            { L"m",  L"轴向模数 m (mm)",        2.0 },
            { L"z1", L"头数 z1 (个)",           1.0 },
            { L"q",  L"直径系数 q (无量纲)",   11.0 },
            { L"n",  L"圈数 n (圈)",            3.0 },
        },
        L"r1 = m*q/2\n"
        L"s = 3.14159265358979*m*z1\n"
        L"theta = t*360*n\n"
        L"r = r1\n"
        L"z = s*n*t"
    },
    /* 17 --------------------------------------------------------------- */
    {
        L"抛物线母线（变截面轴）",
        L"笛卡尔坐标。d1/d2=两端直径，L=过渡段长度，k=曲率指数。\n"
        L"k=2 是标准抛物线；k 取 1.5~3 可调过渡缓急（越小越早变粗）。\n"
        L"注意：只有小端(t=0)相切，大端(t=1)有折角。\n"
        L"用途：锥形轴/造型母线、变截面梁；两端都要相切请用下面「S 曲线过渡」。",
        0, 4,
        {
            { L"d1", L"小端直径 d1 (mm)",      20.0 },
            { L"d2", L"大端直径 d2 (mm)",      40.0 },
            { L"L",  L"过渡长度 L (mm)",       60.0 },
            { L"k",  L"曲率指数 k (无量纲)",    2.0 },
        },
        L"x = L*t\n"
        L"y = d1/2+(d2/2-d1/2)*t^k\n"
        L"z = 0"
    },
    /* 18 --------------------------------------------------------------- */
    {
        L"椭圆弧过渡（轴肩圆角）",
        L"笛卡尔坐标。r1/r2=小端/大端半径，L=轴向过渡长度。\n"
        L"两端都与相邻面相切：t=0 处竖切（贴轴肩端面），t=1 处水平（贴大圆柱面）。\n"
        L"用途：精密转轴台阶根部、轴承定位轴肩、磨削越程处——\n"
        L"      比直角+小圆角的应力集中低得多，且长度可精确控制。",
        0, 3,
        {
            { L"r1", L"小端半径 r1 (mm)",      10.0 },
            { L"r2", L"大端半径 r2 (mm)",      20.0 },
            { L"L",  L"轴向过渡长 L (mm)",      8.0 },
        },
        L"x = L*(1-cos(90*t))\n"
        L"y = r1+(r2-r1)*sin(90*t)\n"
        L"z = 0"
    },
    /* 19 --------------------------------------------------------------- */
    {
        L"波形弹簧（螺旋开口）",
        L"柱坐标。R=中径半径，A=波高，n=每圈波数，p=轴向螺距，nz=圈数。\n"
        L"开口波形簧：边转边升边起伏，扫描小圆截面即为实体。\n"
        L"用途：精密转轴/轴承的轻载预紧、空间受限处的轴向压紧。\n"
        L"（闭合的波形垫圈用上面那条「正弦波纹环」）",
        1, 5,
        {
            { L"R",  L"中径半径 R (mm)",       20.0 },
            { L"A",  L"波高 A (mm)",            1.5 },
            { L"n",  L"每圈波数 n (个)",        3.0 },
            { L"p",  L"轴向螺距 p (mm)",        2.0 },
            { L"nz", L"圈数 nz (圈)",           2.0 },
        },
        L"theta = t*360*nz\n"
        L"r = R\n"
        L"z = p*nz*t+A*sin(n*theta)"
    },
    /* 21 ---------------------------------------------------------------
       转轴变径段的三条互补曲线：抛物线(单端相切,造型) / 椭圆弧(短圆角) /
       S 曲线(双端相切,长过渡)。按过渡长度和是否允许折角选。 */
    {
        L"S 曲线过渡（两端相切变截面轴）",
        L"笛卡尔坐标。d1/d2=两端直径，L=过渡长度。\n"
        L"y=d1/2+(d2/2-d1/2)*(3*t^2-2*t^3)，两端斜率同时为 0，\n"
        L"即小端贴小圆柱面、大端贴大圆柱面，两头都没有折角。\n"
        L"用途：精密转轴长变径段、花键轴到轴颈的过渡、磨削让位过渡——\n"
        L"      折角为零 ⇒ 没有二次应力集中，是台阶轴最省心的过渡形式。",
        0, 3,
        {
            { L"d1", L"小端直径 d1 (mm)",      20.0 },
            { L"d2", L"大端直径 d2 (mm)",      40.0 },
            { L"L",  L"过渡长度 L (mm)",       40.0 },
        },
        L"x = L*t\n"
        L"y = d1/2+(d2/2-d1/2)*(3*t^2-2*t^3)\n"
        L"z = 0"
    },
    /* 23 --------------------------------------------------------------- */
    {
        L"圆",
        L"笛卡尔坐标。XY 平面上的整圆，圆心在坐标原点、半径 R。\n"
        L"用途：基准曲线、端面轮廓、回转体参考边、拉伸/旋转的辅助线、缠绕展开的基线。",
        0, 1,
        {
            { L"R", L"半径 R (mm)", 10.0 },
        },
        L"x = R*cos(360*t)\n"
        L"y = R*sin(360*t)\n"
        L"z = 0"
    },
    /* 24 --------------------------------------------------------------- */
    {
        L"哥特式沟道（滚珠丝杠/轴承沟道）",
        L"笛卡尔坐标，XY 平面上的一段沟道截面：两段等半径圆弧（偏心），共 2 条曲线。\n"
        L"rb=钢球半径，f=吻合度（R/rb，滚珠丝杠常用 1.04，轴承 1.02~1.08），\n"
        L"alpha=接触角（度，常用 45），amax=圆弧张到多少度（度，常用 55~60，决定沟槽宽度）。\n"
        L"右弧 GOTHIC_R + 左弧 GOTHIC_L，在 x=0 的沟底相交（交点即哥特尖），\n"
        L"两弧在 ±alpha 处与钢球相切；沟底比钢球最低点低一点，留出油膜间隙。\n"
        L"用法：两条弧「参考-使用边」进草图 → 沿螺旋线扫描切除 = 滚珠丝杠沟道。",
        0, 4,
        {
            { L"rb",    L"钢球半径 rb (mm)",        3.0 },
            { L"f",     L"吻合度 f (R/rb)",         1.04 },
            { L"alpha", L"接触角 alpha (度)",       45.0 },
            { L"amax",  L"圆弧张角 amax (度)",      60.0 },
        },
        NULL,
        { L"GOTHIC_R", L"GOTHIC_L" },
        {
            L"RR = f*rb\n"
            L"dd = RR-rb\n"
            L"cx = dd*sin(alpha)\n"
            L"cy = dd*cos(alpha)\n"
            L"y0 = cy-sqrt(RR*RR-cx*cx)\n"
            L"y1 = cy-RR*cos(amax)\n"
            L"y = y0+(y1-y0)*t\n"
            L"x = -cx+sqrt(RR*RR-(y-cy)*(y-cy))\n"
            L"z = 0",

            L"RR = f*rb\n"
            L"dd = RR-rb\n"
            L"cx = dd*sin(alpha)\n"
            L"cy = dd*cos(alpha)\n"
            L"y0 = cy-sqrt(RR*RR-cx*cx)\n"
            L"y1 = cy-RR*cos(amax)\n"
            L"y = y0+(y1-y0)*t\n"
            L"x = cx-sqrt(RR*RR-(y-cy)*(y-cy))\n"
            L"z = 0"
        },
        2
    },
    /* 22 --------------------------------------------------------------- */
    {
        L"【自定义】读取 D:\\H\\eq_curve.txt",
        L"不用模板，直接读 D:\\H\\eq_curve.txt 里的方程（UTF-8 或 ANSI 都行）。\n"
        L"文件头的 # TYPE: / # CSYS: 照旧有效；文件不存在会自动生成一份模板。\n"
        L"适合自己写复杂方程、或者反复调试同一个公式。",
        -1, 0,
        { { NULL, NULL, 0.0 } },
        NULL
    },
};

#define H_EQ_TPL_N  ((int)(sizeof(g_eqtpl) / sizeof(g_eqtpl[0])))

/* 追加格式化文本，返回新的写入位置；失败返回 -1 */
static int HAppend(wchar_t *dst, int cap, int pos, const wchar_t *fmt, ...)
{
    va_list ap;
    int     n;

    if (pos < 0 || pos >= cap)
        return -1;
    va_start(ap, fmt);
    n = vswprintf(dst + pos, (size_t)(cap - pos), fmt, ap);
    va_end(ap);
    if (n < 0)
        return -1;
    return pos + n;
}

/*=================== 8c. 模板选择对话框 ==============================
   纯 Win32 自建窗口，**不用 .res 资源文件**。
   理由：TOOLKIT 的 ProUIDialog 必须配 .res，不同 Creo 版本对 res 的
   接受度不一样；自己 CreateWindow 没有这个依赖，而且失败时能静默退回
   文件模式（不会让按钮彻底不可用）。
   ==================================================================== */

#define HED_MAX_PRM 6
#define HED_LIST    3001
#define HED_INFO    3002
#define HED_DESC    3003
#define HED_CSYSL   3004
#define HED_CSYS    3005
#define HED_TIP     3006
#define HED_OK      3007
#define HED_CAN     3008
#define HED_PLAB(i) (3100 + (i))
#define HED_PEDT(i) (3120 + (i))

typedef struct
{
    HWND    hwnd;
    int     tpl;                 /* 当前选中的模板下标 */
    int     ok;                  /* 1 = 点了「生成曲线」 */
    int     done;                /* 1 = 对话框该关了 */
    int     created;             /* 1 = 窗口真建起来过（用来区分“取消”和“建不起来”） */
    HFONT   font;
    HWND    hlist, hinfo, hdesc, htip, hcsysl, hcsys, hok, hcan;
    HWND    hplab[HED_MAX_PRM], hpedt[HED_MAX_PRM];
    wchar_t csysname[H_CSYS_MAX];
    int     csyspick;            /* 下拉框选中项：0 = 默认（第一个），n = g_csys_list[n-1] */
    int     sc;                  /* DPI 缩放百分比：100 = 96dpi，175 = 175% */
    double  val[HED_MAX_PRM];
} HEqDlg;

/* 逻辑像素 → 物理像素。v 一律按 96dpi 下的大小写，这里乘 d->sc。
   2026-10-23：本机 2880x1920 @175%，原来写死 880x560 + 15px 字体，
   在 DPI 感知进程里就是 15 物理像素（≈8.6 逻辑像素 ≈6.5pt），看不清。 */
#define HSL(d, v)  MulDiv((v), ((d)->sc > 0 ? (d)->sc : 100), 100)

/* WM_GETMINMAXINFO 在 WM_CREATE 之前就到了，那时 d 还是 NULL，
   所以这里另存一份给最小尺寸用。 */
static int g_heq_sc = 100;

/* 当前模板的参数名/默认值刷到控件上（切模板时调用） */
static void HEqDlgSync(HEqDlg *d)
{
    const HEqTpl *t;
    int           i;

    if (d == NULL || d->tpl < 0 || d->tpl >= H_EQ_TPL_N)
        return;
    t = &g_eqtpl[d->tpl];
    {
        wchar_t buf[128];

        /* 单位统一按 mm / 度，直接标在标题后面，省得用户猜 */
        swprintf(buf, 128, L"%ls    [单位 mm / 度]", t->title);
        SetWindowTextW(d->hinfo, buf);
    }
    SetWindowTextW(d->hdesc, t->desc);
    for (i = 0; i < HED_MAX_PRM; i++)
    {
        if (i < t->nprm && t->ctype >= 0)
        {
            wchar_t buf[64];

            SetWindowTextW(d->hplab[i], t->prm[i].label);
            swprintf(buf, 64, L"%g", t->prm[i].def);
            SetWindowTextW(d->hpedt[i], buf);
            ShowWindow(d->hplab[i], SW_SHOW);
            ShowWindow(d->hpedt[i], SW_SHOW);
            EnableWindow(d->hpedt[i], TRUE);
        }
        else
        {
            SetWindowTextW(d->hplab[i], L"");
            SetWindowTextW(d->hpedt[i], L"");
            ShowWindow(d->hplab[i], SW_HIDE);
            ShowWindow(d->hpedt[i], SW_HIDE);
            EnableWindow(d->hpedt[i], FALSE);
        }
    }
    /* 2026-10-23 用户要求：「给方程曲线备注非闭合曲线，记得投影」。
       这里把两条提示拼在一起常驻在说明区：
       ① 用法（改参数→生成、坐标系的作用）；
       ② ★ 方程曲线是开放样条 —— 首尾两点坐标重合，但 Creo 存的是
          带两个端点的开放曲线，拓扑上没有闭合（方程输出类型固定，
          没有「闭合」开关）。直接拿它去修剪 / 扫描切除，接缝处必然
          留缝或切不全，所以必须先投影到目标曲面（穿透求交），或者
          用这条曲线扫一条 0.01mm 窄面组当刀。 */
    {
        wchar_t tip[512];

        if (t->ctype < 0)
            wcscpy(tip, L"选这一项时直接用 eq_curve.txt 里的方程，上面的参数框不生效。\n");
        else
            wcscpy(tip, L"改完参数点「生成曲线」；坐标系决定原点与 X 轴朝向。\n");
        wcscat(tip, L"※ 方程曲线是开放样条（首尾两点重合、但拓扑不闭合）："
                    L"直接修剪 / 扫描切除会在接缝处留缝或切不全。\n"
                    L"  记得先【投影】到这个曲面，再用投影曲线（或 0.01mm 窄面组）去修剪。");
        SetWindowTextW(d->htip, tip);
    }
}

/* 读回参数。返回 1 成功；0 表示某个框里不是数字（已弹框提示并选中该框） */
static int HEqDlgCollect(HEqDlg *d)
{
    const HEqTpl *t;
    int           i;

    if (d == NULL || d->tpl < 0 || d->tpl >= H_EQ_TPL_N)
        return 0;
    t = &g_eqtpl[d->tpl];

    /* 坐标系改成下拉框：0 = 默认（第一个），1..N = g_csys_list[ci-1] */
    {
        int ci = (int)SendMessageW(d->hcsys, CB_GETCURSEL, 0, 0);

        d->csyspick    = 0;
        d->csysname[0] = L'\0';
        if (ci >= 1 && ci <= g_csys_cnt)
        {
            d->csyspick = ci;
            wcsncpy(d->csysname, g_csys_list[ci - 1].name, H_CSYS_MAX - 1);
            d->csysname[H_CSYS_MAX - 1] = L'\0';
        }
    }
    if (t->ctype < 0)
        return 1;                        /* 自定义：不校验参数 */

    for (i = 0; i < t->nprm; i++)
    {
        wchar_t  buf[64];
        wchar_t *end = NULL;
        double   v;

        GetWindowTextW(d->hpedt[i], buf, 64);
        v = wcstod(buf, &end);
        while (end != NULL && (*end == L' ' || *end == L'\t'))
            end++;
        if (end == buf || end == NULL || *end != L'\0')
        {
            wchar_t msg[256];

            swprintf(msg, 256, L"参数「%ls」不是有效数字：%ls",
                     t->prm[i].label, buf);
            MessageBoxW(d->hwnd, msg, L"参数有误", MB_OK | MB_ICONWARNING);
            SetFocus(d->hpedt[i]);
            SendMessageW(d->hpedt[i], EM_SETSEL, 0, (LPARAM)(-1));
            return 0;
        }
        d->val[i] = v;
    }
    return 1;
}

/* 参数赋值行 + 指定的方程正文 → g_eq_buf。
   分段模板（凸轮）额外追加 rest = 360-b1-b2-b3（近休止角），
   这样近休段的方程可以写成 theta = b1+b2+b3+rest*t，
   无论用户把三种角加成多少都不会退化成零长度曲线（下限钳到 0.5°）。 */
static int HEqBuildBody(HEqDlg *d, const HEqTpl *t, const wchar_t *body)
{
    int i, pos;

    if (d == NULL || t == NULL || body == NULL)
        return 0;

    g_eq_buf[0] = L'\0';
    pos = 0;
    for (i = 0; i < t->nprm; i++)
    {
        pos = HAppend(g_eq_buf, H_EQ_MAX, pos, L"%ls = %g\n",
                      t->prm[i].var, d->val[i]);
        if (pos < 0)
            return 0;
    }
    if (t->nseg > 0 && t->nprm >= 5)
    {
        double rest = 360.0 - d->val[2] - d->val[3] - d->val[4];

        if (rest < 0.5)
            rest = 0.5;
        HLog("HEqBuildBody: rest=%g (b1=%g b2=%g b3=%g)\n",
             rest, d->val[2], d->val[3], d->val[4]);
        pos = HAppend(g_eq_buf, H_EQ_MAX, pos, L"rest = %g\n", rest);
        if (pos < 0)
            return 0;
    }
    pos = HAppend(g_eq_buf, H_EQ_MAX, pos, L"\n%ls\n", body);
    return (pos < 0) ? 0 : 1;
}

/* 参数赋值行 + 方程正文 → g_eq_buf，坐标系名 → g_csys_name */
static int HEqBuild(HEqDlg *d, int *p_type)
{
    const HEqTpl *t;

    if (d == NULL || p_type == NULL)
        return 0;
    if (d->tpl < 0 || d->tpl >= H_EQ_TPL_N)
        return 0;
    t = &g_eqtpl[d->tpl];
    if (t->ctype < 0 || (t->nseg <= 0 && t->eq == NULL) || t->nseg > 8)
        return 0;

    *p_type = t->ctype;

    wcsncpy(g_csys_name, d->csysname, H_CSYS_MAX - 1);
    g_csys_name[H_CSYS_MAX - 1] = L'\0';
    g_csys_sel_id = (d->csyspick > 0 && d->csyspick <= g_csys_cnt)
                    ? g_csys_list[d->csyspick - 1].id : 0;
    HLog("HEqBuild: csyspick=%d csys_id=%d\n", d->csyspick, g_csys_sel_id);

    if (!HEqBuildBody(d, t, (t->nseg > 0) ? t->segeq[0] : t->eq))
        return 0;

    HLog("HEqBuild: tpl=%d nprm=%d type=%d nseg=%d eq_len=%d\n",
         d->tpl, t->nprm, t->ctype, t->nseg, (int)wcslen(g_eq_buf));
    return 1;
}

static void HEqDlgLayout(HWND hwnd, HEqDlg *d)
{
    RECT rc;
    int  cw, ch, i, y, pad, lw, rx, ew, bh, row, np, top;

    GetClientRect(hwnd, &rc);
    cw = rc.right;
    ch = rc.bottom;
    if (d == NULL)
        return;

    pad = HSL(d, 12);
    lw  = HSL(d, 246);                 /* 左侧列表宽 */
    rx  = pad + lw + HSL(d, 14);       /* 右侧列起点 */
    ew  = HSL(d, 118);                 /* 参数输入框宽 */
    bh  = HSL(d, 28);                  /* 按钮高 */
    row = HSL(d, 32);                  /* 参数行高 */

    /* 当前模板真正有几个参数：只按这个数让位，否则 3 参数的模板
       下面会空出一大块（原来固定按 6 行算）。 */
    np = 0;
    if (d->tpl >= 0 && d->tpl < H_EQ_TPL_N)
        np = g_eqtpl[d->tpl].nprm;
    if (np < 0)
        np = 0;
    if (np > HED_MAX_PRM)
        np = HED_MAX_PRM;

    if (cw < rx + HSL(d, 230) || ch < HSL(d, 400))
        return;

    top = pad + HSL(d, 142);           /* 参数区起点：标题 24 + 说明 104 之后 */

    MoveWindow(d->hlist, pad, pad, lw, ch - pad - HSL(d, 52), TRUE);
    MoveWindow(d->hinfo, rx, pad, cw - rx - pad, HSL(d, 24), TRUE);
    MoveWindow(d->hdesc, rx, pad + HSL(d, 30), cw - rx - pad, HSL(d, 104), TRUE);
    for (i = 0; i < HED_MAX_PRM; i++)
    {
        y = top + i * row;
        MoveWindow(d->hplab[i], rx, y + HSL(d, 5), HSL(d, 150), HSL(d, 20), TRUE);
        MoveWindow(d->hpedt[i], rx + HSL(d, 158), y, ew, HSL(d, 24), TRUE);
    }
    y = top + np * row;                /* 坐标系行紧跟最后一个真实参数 */
    MoveWindow(d->hcsysl, rx, y + HSL(d, 5), HSL(d, 150), HSL(d, 20), TRUE);
    MoveWindow(d->hcsys, rx + HSL(d, 158), y, HSL(d, 226), HSL(d, 220), TRUE);

    y = y + row + HSL(d, 4);           /* 提示文字吃掉剩下的高度，不留空白 */
    {
        int th = ch - y - HSL(d, 50);

        /* 提示区现在常驻 3~4 行（用法 + 非闭合/投影提醒），
           上限从 104 提到 150，否则 5~6 个参数的模板会把提醒切掉。 */
        if (th > HSL(d, 150))
            th = HSL(d, 150);
        if (th < HSL(d, 40))
            th = HSL(d, 40);
        MoveWindow(d->htip, rx, y, cw - rx - pad, th, TRUE);
    }
    MoveWindow(d->hok, cw - HSL(d, 200), ch - HSL(d, 40), HSL(d, 90), bh, TRUE);
    MoveWindow(d->hcan, cw - HSL(d, 102), ch - HSL(d, 40), HSL(d, 90), bh, TRUE);
}

static HWND HEdCtl(HWND par, const wchar_t *cls, const wchar_t *txt,
                   DWORD style, int x, int y, int w, int h, int id, HFONT f)
{
    HWND c = CreateWindowExW(0, cls, txt, style, x, y, w, h, par,
                             (HMENU)(INT_PTR)id,
                             (HINSTANCE)GetWindowLongPtrW(par, GWLP_HINSTANCE),
                             NULL);

    if (c != NULL && f != NULL)
        SendMessageW(c, WM_SETFONT, (WPARAM)f, TRUE);
    return c;
}

static LRESULT CALLBACK HEqDlgProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    HEqDlg *d = (HEqDlg *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);

    switch (msg)
    {
    case WM_CREATE:
    {
        CREATESTRUCTW *cs = (CREATESTRUCTW *)lp;
        int            i;

        d = (HEqDlg *)cs->lpCreateParams;
        if (d == NULL)
            return -1;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)d);
        d->hwnd = hwnd;

        d->font = CreateFontW(-HSL(d, 16), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                              DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                              CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                              DEFAULT_PITCH | FF_SWISS, L"Microsoft YaHei UI");
        if (d->font == NULL)
            d->font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);

        d->hlist = HEdCtl(hwnd, L"LISTBOX", L"",
                          WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_BORDER
                          | LBS_NOTIFY, 12, 12, 232, 380, HED_LIST, d->font);
        d->hinfo = HEdCtl(hwnd, L"STATIC", L"",
                          WS_CHILD | WS_VISIBLE | SS_LEFT,
                          256, 10, 380, 24, HED_INFO, d->font);
        d->hdesc = HEdCtl(hwnd, L"STATIC", L"",
                          WS_CHILD | WS_VISIBLE | SS_LEFT,
                          256, 36, 380, 56, HED_DESC, d->font);
        for (i = 0; i < HED_MAX_PRM; i++)
        {
            d->hplab[i] = HEdCtl(hwnd, L"STATIC", L"",
                                 WS_CHILD | WS_VISIBLE | SS_LEFT,
                                 256, 0, 148, 20, HED_PLAB(i), d->font);
            d->hpedt[i] = HEdCtl(hwnd, L"EDIT", L"",
                                 WS_CHILD | WS_VISIBLE | WS_BORDER
                                 | ES_AUTOHSCROLL, 408, 0, 120, 24,
                                 HED_PEDT(i), d->font);
        }
        d->hcsysl = HEdCtl(hwnd, L"STATIC", L"坐标系",
                           WS_CHILD | WS_VISIBLE | SS_LEFT,
                           328, 0, 160, 22, HED_CSYSL, d->font);
        d->hcsys  = HEdCtl(hwnd, L"COMBOBOX", L"",
                           WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_BORDER
                           | CBS_DROPDOWNLIST, 496, 0, 300, 260,
                           HED_CSYS, d->font);
        d->htip   = HEdCtl(hwnd, L"STATIC", L"",
                           WS_CHILD | WS_VISIBLE | SS_LEFT,
                           256, 0, 380, 44, HED_TIP, d->font);
        d->hok    = HEdCtl(hwnd, L"BUTTON", L"生成曲线",
                           WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
                           0, 0, 96, 30, HED_OK, d->font);
        d->hcan   = HEdCtl(hwnd, L"BUTTON", L"取消",
                           WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                           0, 0, 96, 30, HED_CAN, d->font);

        if (d->hlist == NULL || d->hok == NULL || d->hpedt[0] == NULL)
        {
            HLog("HEqDlg: create controls failed\n");
            return -1;
        }

        for (i = 0; i < H_EQ_TPL_N; i++)
            SendMessageW(d->hlist, LB_ADDSTRING, 0, (LPARAM)g_eqtpl[i].title);

        /* 坐标系下拉：第 0 项 = 默认（模型里第一个），后面是模型里全部坐标系 */
        SendMessageW(d->hcsys, CB_ADDSTRING, 0,
                     (LPARAM)L"（默认）模型里第一个坐标系");
        for (i = 0; i < g_csys_cnt; i++)
        {
            wchar_t txt[H_CSYS_MAX + 32];

            if (g_csys_list[i].name[0] != L'\0')
                swprintf(txt, H_CSYS_MAX + 32, L"%ls", g_csys_list[i].name);
            else
                swprintf(txt, H_CSYS_MAX + 32, L"坐标系 #%d", g_csys_list[i].id);
            SendMessageW(d->hcsys, CB_ADDSTRING, 0, (LPARAM)txt);
        }
        SendMessageW(d->hcsys, CB_SETCURSEL, 0, 0);

        d->tpl = 0;
        SendMessageW(d->hlist, LB_SETCURSEL, 0, 0);
        HEqDlgSync(d);
        HEqDlgLayout(hwnd, d);
        return 0;
    }

    case WM_SIZE:
        HEqDlgLayout(hwnd, d);
        return 0;

    case WM_GETMINMAXINFO:
    {
        MINMAXINFO *mm = (MINMAXINFO *)lp;

        if (mm != NULL)
        {
            int k = (d != NULL && d->sc > 0) ? d->sc : g_heq_sc;

            mm->ptMinTrackSize.x = MulDiv(620, k, 100);
            mm->ptMinTrackSize.y = MulDiv(420, k, 100);
        }
        return 0;
    }

    case WM_COMMAND:
    {
        int id   = LOWORD(wp);
        int code = HIWORD(wp);

        if (d == NULL)
            break;
        if (id == HED_LIST && code == LBN_SELCHANGE)
        {
            int sel = (int)SendMessageW(d->hlist, LB_GETCURSEL, 0, 0);

            if (sel >= 0 && sel < H_EQ_TPL_N)
            {
                d->tpl = sel;
                HEqDlgSync(d);
                HEqDlgLayout(hwnd, d);   /* 参数个数变了，坐标系行要跟着上移 */
            }
            return 0;
        }
        if (id == HED_LIST && code == LBN_DBLCLK)
        {
            SendMessageW(hwnd, WM_COMMAND,
                         MAKEWPARAM(HED_OK, BN_CLICKED), 0);
            return 0;
        }
        if (id == HED_OK)
        {
            if (HEqDlgCollect(d))
            {
                d->ok   = 1;
                d->done = 1;
            }
            return 0;
        }
        if (id == HED_CAN)
        {
            d->done = 1;
            return 0;
        }
        break;
    }

    case WM_CLOSE:
        if (d != NULL)
            d->done = 1;
        return 0;                    /* 自己收尾，不走 DefWindowProc 销毁 */

    case WM_DESTROY:
        return 0;                    /* 千万别 PostQuitMessage，会杀掉 Creo 的消息循环 */
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/* 返回值：1 = 点了生成；0 = 取消或建不起来（看 d->created 区分） */
static int HEqDlgShow(HEqDlg *d)
{
    static int  registered = 0;
    HINSTANCE   hi;
    HWND        owner;
    WNDCLASSEXW wc;
    RECT        rc;
    MSG         m;
    int         w, h, x, y;

    memset(d, 0, sizeof(*d));
    hi = GetModuleHandleW(NULL);

    if (!registered)
    {
        memset(&wc, 0, sizeof(wc));
        wc.cbSize        = sizeof(wc);
        wc.style         = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc   = HEqDlgProc;
        wc.hInstance     = hi;
        wc.hCursor       = LoadCursorW(NULL, MAKEINTRESOURCEW(32512)); /* IDC_ARROW */
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.lpszClassName = L"H_EqCurveDlg";
        if (RegisterClassExW(&wc) == 0
            && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        {
            HLog("HEqDlgShow: RegisterClass failed %lu\n",
                 (unsigned long)GetLastError());
            return 0;
        }
        registered = 1;
    }

    /* ★ 整套 UI 按系统 DPI 放大。原来写死 880x560 + 15px 字体，
       在 175% 缩放屏上相当于 6.5pt，根本看不清。 */
    {
        HDC hdc = GetDC(NULL);
        int dpi = (hdc != NULL) ? GetDeviceCaps(hdc, LOGPIXELSY) : 96;

        if (hdc != NULL)
            ReleaseDC(NULL, hdc);
        if (dpi <= 0)
            dpi = 96;
        d->sc = MulDiv(dpi, 100, 96);
        if (d->sc < 100)                 /* 低 DPI 不缩小，只放大 */
            d->sc = 100;
        if (d->sc > 300)
            d->sc = 300;
        HLog("HEqDlgShow: dpi=%d scale=%d%%\n", dpi, d->sc);
    }
    g_heq_sc = d->sc;

    rc.left   = 0;
    rc.top    = 0;
    rc.right  = HSL(d, 780);             /* 基础客户区 780x540 (96dpi 口径)
                                            2026-10-23：470 → 540，给下面常驻的
                                            「非闭合 / 先投影」提醒腾出 2 行高度 */
    rc.bottom = HSL(d, 540);
    AdjustWindowRectEx(&rc, WS_POPUP | WS_CAPTION | WS_SYSMENU
                       | WS_THICKFRAME | WS_MAXIMIZEBOX, FALSE,
                       WS_EX_DLGMODALFRAME);
    w = rc.right - rc.left;
    h = rc.bottom - rc.top;
    {
        RECT wa;                          /* 放不下就收到工作区内，别出屏 */
        int  aw, ah;

        if (SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0))
        {
            aw = wa.right - wa.left;
            ah = wa.bottom - wa.top;
            /* 上限：宽不超过工作区 62%，高不超过 76%，避免 DPI 一放大就霸屏 */
            if (w > aw * 62 / 100)
                w = aw * 62 / 100;
            if (h > ah * 76 / 100)
                h = ah * 76 / 100;
        }
    }

    owner = GetActiveWindow();
    {
        RECT orc;

        if (owner != NULL && GetWindowRect(owner, &orc)
            && (orc.right - orc.left) > 200)
        {
            x = orc.left + ((orc.right - orc.left) - w) / 2;
            y = orc.top + ((orc.bottom - orc.top) - h) / 2;
        }
        else
        {
            x = (GetSystemMetrics(SM_CXSCREEN) - w) / 2;
            y = (GetSystemMetrics(SM_CYSCREEN) - h) / 2;
        }
    }

    d->hwnd = CreateWindowExW(WS_EX_DLGMODALFRAME, L"H_EqCurveDlg",
                              L"方程曲线 — 选模板 / 改参数 / 生成",
                              WS_POPUP | WS_CAPTION | WS_SYSMENU
                              | WS_THICKFRAME | WS_MAXIMIZEBOX,
                              x, y, w, h, owner, NULL, hi, d);
    if (d->hwnd == NULL)
    {
        HLog("HEqDlgShow: CreateWindow failed %lu\n",
             (unsigned long)GetLastError());
        return 0;
    }
    d->created = 1;

    if (owner != NULL)
        EnableWindow(owner, FALSE);
    ShowWindow(d->hwnd, SW_SHOW);
    UpdateWindow(d->hwnd);
    SetForegroundWindow(d->hwnd);

    while (d->done == 0 && GetMessageW(&m, NULL, 0, 0) > 0)
    {
        if (!IsDialogMessageW(d->hwnd, &m))
        {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
    }

    if (owner != NULL)
    {
        EnableWindow(owner, TRUE);
        SetActiveWindow(owner);
    }
    if (IsWindow(d->hwnd))
        DestroyWindow(d->hwnd);
    d->hwnd = NULL;
    if (d->font != NULL && d->font != (HFONT)GetStockObject(DEFAULT_GUI_FONT))
        DeleteObject(d->font);
    d->font = NULL;

    HLog("HEqDlgShow: ok=%d tpl=%d created=%d\n", d->ok, d->tpl, d->created);
    return d->ok;
}

/* 创建失败时把整棵树走一遍，逐元素收集诊断信息
   （ProElementDiagnosticsCollect 能给到"这个元素缺什么/值不合法"，
   比 ProErrorlist 有用得多——实测 errs.error_number 经常是 0）。 */
static ProError HEqDiagVisit(ProElement elem_tree, ProElement elem,
                             ProElempath elem_path, ProAppData appdata)
{
    ProElemdiagnostic  *diags = NULL;
    ProError            st;
    ProElemId           id   = (ProElemId)0;
    ProDiagnosticSeverity sev;
    ProComment          msg;
    int                 n = 0, i;

    (void)elem_tree;
    (void)elem_path;
    (void)appdata;

    ProElementIdGet(elem, &id);
    st = ProElementDiagnosticsCollect(elem, &diags);
    if (st != PRO_TK_NO_ERROR || diags == NULL)
        return PRO_TK_NO_ERROR;

    ProArraySizeGet((ProArray)diags, &n);
    for (i = 0; i < n; i++)
    {
        memset(msg, 0, sizeof(msg));
        sev = PRO_ELEM_INFO;
        ProElemdiagnosticSeverityGet(diags[i], &sev);
        ProElemdiagnosticMessageGet(diags[i], msg);
        HLog("HEqCurveAct:   DIAG elem=%d sev=%d msg=", (int)id, (int)sev);
        HLogW("", msg);
    }
    ProElemdiagnosticProarrayFree(diags);
    return PRO_TK_NO_ERROR;
}

static void HEqDiagDump(ProElement tree)
{
    if (tree == NULL)
        return;
    ProElemtreeElementVisit(tree, NULL, NULL, HEqDiagVisit, NULL);
}

/* 引用本身的诊断：ProReferenceSet 成功只说明"存进去了"，
   不代表这条引用在模型里解析得通（比如 id 指向的对象类型不对）。
   ProReferenceDiagnosticsCollect 会把这类问题以 warning/error 报出来。 */
static void HEqRefDiag(ProReference ref)
{
    ProElemdiagnostic    *diags = NULL;
    ProError              st;
    ProDiagnosticSeverity sev;
    ProComment            msg;
    int                   n = 0, i;

    if (ref == NULL)
        return;
    st = ProReferenceDiagnosticsCollect(ref, &diags);
    if (st != PRO_TK_NO_ERROR || diags == NULL)
    {
        HLog("HEqRefDiag: collect -> %d (no diag)\n", (int)st);
        return;
    }
    ProArraySizeGet((ProArray)diags, &n);
    HLog("HEqRefDiag: %d diag\n", n);
    for (i = 0; i < n; i++)
    {
        memset(msg, 0, sizeof(msg));
        sev = PRO_ELEM_INFO;
        ProElemdiagnosticSeverityGet(diags[i], &sev);
        ProElemdiagnosticMessageGet(diags[i], msg);
        HLog("HEqRefDiag:   sev=%d msg=", (int)sev);
        HLogW("", msg);
    }
    ProElemdiagnosticProarrayFree(diags);
}

/* 回读：把 Creo 真正存下来的方程文本读出来。
   动机（2026-10-23 截图）：在 Creo 的方程编辑器里，开头 5 行参数定义 + 那个空行
   全都不见了，只剩从 theta = t*360 开始的部分。要确认这是"显示问题"还是
   "存进去就被改写" —— 只有回读能证明。 */
typedef struct
{
    int found;
    int len;
} HEqReadCtx;

static ProError HEqReadVisit(ProElement elem_tree, ProElement elem,
                             ProElempath elem_path, ProAppData appdata)
{
    ProElemId   id = (ProElemId)0;
    HEqReadCtx *c  = (HEqReadCtx *)appdata;
    wchar_t    *v  = NULL;
    ProError    st;

    (void)elem_tree;
    (void)elem_path;
    if (c == NULL || c->found)
        return PRO_TK_NO_ERROR;
    ProElementIdGet(elem, &id);
    if (id != PRO_E_CRV_ENTER_EQUATION)
        return PRO_TK_NO_ERROR;

    st = ProElementWstringGet(elem, NULL, &v);
    if (st != PRO_TK_NO_ERROR || v == NULL)
    {
        HLog("HEqReadBack: wstringGet -> %d\n", (int)st);
        return PRO_TK_NO_ERROR;
    }
    c->found = 1;
    c->len   = (int)wcslen(v);
    HLog("HEqReadBack: stored len=%d\n", c->len);
    HLogW("HEqReadBack: stored text =\n", v);
    ProWstringFree(v);
    return PRO_TK_NO_ERROR;
}

static void HEqReadBackEq(ProFeature *p_feat)
{
    ProElement tree = NULL;
    HEqReadCtx c;
    ProError   st;

    if (p_feat == NULL)
        return;
    memset(&c, 0, sizeof(c));
    st = ProFeatureElemtreeExtract(p_feat, NULL, PRO_FEAT_EXTRACT_NO_OPTS, &tree);
    if (st != PRO_TK_NO_ERROR || tree == NULL)
    {
        HLog("HEqReadBack: extract -> %d\n", (int)st);
        return;
    }
    ProElemtreeElementVisit(tree, NULL, NULL, HEqReadVisit, (ProAppData)&c);
    ProElementFree(&tree);
    HLog("HEqReadBack: done found=%d len=%d\n", c.found, c.len);
}

/*=========================================================================
   9) 方程曲线的自检工具（只在失败路径上跑，正常创建不碰）

   背景（2026-10-23）：options 和坐标系 id 都修好后，`create -> 0` 成功了，
   但 Creo 报「重新生成失败：EQ_CURVE」。也就是说元素树和引用都对，是
   **方程文本求值** 这一步过不去，而 ProFeatureWithoptionsCreate 对此只返回 0
   （特征建出来了，但处于失败状态），errs 依旧是空。

   所以要自己判断"建成了但失败"：ProSolidFailedfeaturesList 拿失败特征 id 表。
   再用一组内置方程逐个试，哪个构造过不去就一目了然（试完立刻删掉，不留垃圾）。
   ======================================================================== */

/* 某特征是否在失败列表里 */
static int HEqFeatureFailed(ProSolid solid, int feat_id)
{
    int     *ids = NULL;
    int      n = 0, i, hit = 0;
    ProError st;

    if (solid == NULL || feat_id <= 0)
        return 0;
    st = ProSolidFailedfeaturesList(solid, &ids, NULL, NULL);
    if (st == PRO_TK_NO_ERROR && ids != NULL)
    {
        ProArraySizeGet((ProArray)ids, &n);
        for (i = 0; i < n; i++)
        {
            if (ids[i] == feat_id)
                hit = 1;
        }
        ProArrayFree((ProArray *)&ids);
    }
    HLog("HEqFeatureFailed: id=%d hit=%d n=%d st=%d\n",
         feat_id, hit, n, (int)st);
    return hit;
}

/* 把送进 Creo 的方程原文按行打进日志。
   2026-10-28 加：create 失败时 Creo 只回 -1、errs.error_number=0，
   屏幕上那句「具体出错元素见 h_load.log」以前在日志里根本没有任何东西可看。 */
static void HEqDumpEq(const wchar_t *tag, const wchar_t *eq)
{
    wchar_t line[512];
    int     len, i, k = 0, n = 0;

    if (eq == NULL)
        return;
    len = (int)wcslen(eq);
    HLog("HEqDumpEq[%ls]: begin len=%d\n", tag, len);
    for (i = 0; i <= len; i++)
    {
        if (i == len || eq[i] == L'\n')
        {
            line[k] = L'\0';
            HLog("HEqDumpEq:  | %ls\n", line);
            k = 0;
            if (++n > 200)
                break;
        }
        else if (k < 500)
        {
            line[k++] = eq[i];
        }
    }
    HLog("HEqDumpEq: end lines=%d\n", n);
}

/* 建一条方程曲线（探针/自检共用）。p_feat 回传特征，后续判失败/删除用。 */
static ProError HEqCreateCurveEx(ProSelection model_sel, ProModelitem *p_csys,
                                 int csys_type, const wchar_t *name,
                                 const wchar_t *eq, ProFeature *p_feat)
{
    ProElement              tree = NULL, e = NULL;
    ProFeatureCreateOptions *opts = NULL;
    ProReference            pref = NULL;
    ProFeature              feat;
    ProErrorlist            errs;
    ProError                st;

    if (model_sel == NULL || p_csys == NULL || eq == NULL)
        return PRO_TK_BAD_INPUTS;

    st = ProElementAlloc(PRO_E_FEATURE_TREE, &tree);
    if (st != PRO_TK_NO_ERROR || tree == NULL)
        return PRO_TK_GENERAL_ERROR;

    HEqAddInt(tree, PRO_E_FEATURE_TYPE, PRO_FEAT_CURVE);
    HEqAddInt(tree, PRO_E_CURVE_TYPE, PRO_CURVE_TYPE_FROM_EQUATION);
    HEqAddWstring(tree, PRO_E_STD_FEATURE_NAME, (wchar_t *)name);

    st = ProElementAlloc(PRO_E_CRV_FR_EQ_REF_CSYS, &e);
    if (st == PRO_TK_NO_ERROR && e != NULL)
    {
        st = ProReferenceAlloc(&pref);
        if (st == PRO_TK_NO_ERROR && pref != NULL)
            st = ProReferenceSet(pref, NULL, p_csys);
        if (st == PRO_TK_NO_ERROR && pref != NULL)
            st = ProElementReferenceSet(e, pref);
        if (st == PRO_TK_NO_ERROR)
            st = ProElemtreeElementAdd(tree, NULL, e);
        if (st != PRO_TK_NO_ERROR)
            ProElementFree(&e);
    }
    HEqAddInt(tree, PRO_E_CRV_FR_EQ_CSYS_TYPE, csys_type);
    HEqAddDouble(tree, PRO_E_CRV_FR_EQ_PARAM_MIN, 0.0);
    HEqAddDouble(tree, PRO_E_CRV_FR_EQ_PARAM_MAX, 1.0);
    HEqAddWstring(tree, PRO_E_CRV_ENTER_EQUATION, (wchar_t *)eq);

    st = ProArrayAlloc(1, sizeof(ProFeatureCreateOptions), 1, (ProArray *)&opts);
    if (st == PRO_TK_NO_ERROR && opts != NULL)
    {
        opts[0] = PRO_FEAT_CR_NO_OPTS;
        memset(&errs, 0, sizeof(errs));
        memset(&feat, 0, sizeof(feat));
        st = ProFeatureWithoptionsCreate(model_sel, tree, opts,
                                         PRO_REGEN_NO_FLAGS, &feat, &errs);
        if (p_feat != NULL && st == PRO_TK_NO_ERROR)
            *p_feat = feat;
        HLog("HEqCreateCurveEx[%ls]: create -> %d (errs=%d) feat=%d\n",
             name, (int)st, errs.error_number, feat.id);
        if (st != PRO_TK_NO_ERROR)
        {
            int ei;

            HEqDumpEq(name, eq);
            if (errs.error_number > 0 && errs.error_list != NULL)
            {
                for (ei = 0; ei < errs.error_number; ei++)
                    HLog("HEqCreateCurveEx: err[%d] elem=%d type=%d code=%d\n",
                         ei, errs.error_list[ei].err_item_id,
                         (int)errs.error_list[ei].err_item_type,
                         (int)errs.error_list[ei].error);
            }
            else
            {
                HLog("HEqCreateCurveEx: Creo 没给任何元素级错误（errs=0）\n");
            }
        }
        ProArrayFree((ProArray *)&opts);
    }
    else
    {
        st = PRO_TK_GENERAL_ERROR;
    }

    ProElementFree(&tree);
    if (pref != NULL)
        ProReferenceFree(pref);
    return st;
}

/* 删掉一条自检曲线，别在模型树里留垃圾 */
static void HEqDeleteFeat(ProSolid solid, int feat_id)
{
    ProError st;

    if (solid == NULL || feat_id <= 0)
        return;
    st = ProFeatureDelete(solid, &feat_id, 1, NULL, 0);
    HLog("HEqDeleteFeat: id=%d -> %d\n", feat_id, (int)st);
}

/* 内置方程自检：逐个建、看是否失败、立刻删。
   结果形如  HEqSelfTest: [T1-cyl-basic] create=0 failed=0  —— failed=0 就是这种写法能过。 */
typedef struct
{
    const wchar_t *tag;
    int            ctype;    /* 0=笛卡尔 1=圆柱 2=球 */
    const wchar_t *eq;
} HEqTest;

static const HEqTest g_eqtests[] =
{
    { L"T0-cart-3line", 0,
      L"x = t*10\n"
      L"y = 5*sin(t*360)\n"
      L"z = 0" },

    { L"T1-cyl-basic", 1,
      L"r = 40\n"
      L"theta = t*360\n"
      L"z = 0" },

    { L"T2-cyl-sin", 1,
      L"theta = t*360\n"
      L"r = 20+20*sin(theta)\n"
      L"z = 0" },

    { L"T3-cyl-param", 1,
      L"r0 = 40\n"
      L"h = 20\n"
      L"theta = t*360\n"
      L"r = r0+h*sin(theta)\n"
      L"z = 0" },

    { L"T4-if-noelse", 1,
      L"theta = t*360\n"
      L"r = 40\n"
      L"if theta <= 180\n"
      L"r = 60\n"
      L"endif\n"
      L"z = 0" },

    { L"T5-if-else", 1,
      L"theta = t*360\n"
      L"if theta <= 180\n"
      L"r = 60\n"
      L"else\n"
      L"r = 40\n"
      L"endif\n"
      L"z = 0" },

    { L"T6-if-nested", 1,
      L"theta = t*360\n"
      L"if theta <= 90\n"
      L"  r = 60\n"
      L"else\n"
      L"  if theta <= 180\n"
      L"    r = 50\n"
      L"  else\n"
      L"    r = 40\n"
      L"  endif\n"
      L"endif\n"
      L"z = 0" },

    /* 大写关键字：Creo 关系里 IF/ELSE/ENDIF 的大小写是否敏感，用这条验 */
    { L"T7-if-upper", 1,
      L"theta = t*360\n"
      L"r = 40\n"
      L"IF theta <= 180\n"
      L"  r = 60\n"
      L"ENDIF\n"
      L"z = 0" },

    /* 只有空行、没有 if：验"空行会不会让前面整段失效" */
    { L"T8-blankline", 1,
      L"r0 = 40\n"
      L"h = 20\n"
      L"\n"
      L"r = r0+h\n"
      L"theta = t*360\n"
      L"z = 0" },

    /* 哥特式沟道模板用到 sqrt；下面两条专门验函数支持面，别等模板挂了才查 */
    { L"T9-sqrt", 0,
      L"a = 5\n"
      L"b = sqrt(a*a-9)\n"
      L"x = b*t\n"
      L"y = 0\n"
      L"z = 0" },

    { L"T10-asin", 0,
      L"a = 0.5\n"
      L"b = asin(a)\n"
      L"x = b*t\n"
      L"y = 0\n"
      L"z = 0" },
};

static void HEqSelfTest(ProSelection model_sel, ProModelitem *p_csys,
                        ProSolid solid)
{
    int       i;
    int       n = (int)(sizeof(g_eqtests) / sizeof(g_eqtests[0]));
    ProFeature feat;
    ProError  st;
    int       failed;

    HLog("HEqSelfTest: start (%d cases)\n", n);
    for (i = 0; i < n; i++)
    {
        memset(&feat, 0, sizeof(feat));
        failed = 0;
        st = HEqCreateCurveEx(model_sel, p_csys, g_eqtests[i].ctype,
                              g_eqtests[i].tag, g_eqtests[i].eq, &feat);
        if (st == PRO_TK_NO_ERROR)
        {
            HEqReadBackEq(&feat);                 /* Creo 到底存了什么 */
            failed = HEqFeatureFailed(solid, feat.id);
        }
        HLog("HEqSelfTest: [%ls] create=%d failed=%d\n",
             g_eqtests[i].tag, (int)st, failed);
        if (st == PRO_TK_NO_ERROR && feat.id > 0)
            HEqDeleteFeat(solid, feat.id);
    }
    HLog("HEqSelfTest: end\n");
}

/* ---------------------------------------------------------------
   方程被 Creo 直接拒绝（create -> -1、errs=0）时的语法探针
   2026-10-28 加。起因：「变螺距弹簧」的方程
       z = n*(p1*t+(p2-p1)*t*t/2)
   在 Creo 12 上连特征都建不出来，而同一模型里「圆柱螺旋线」的
       z = z0+p*n*t
   能建。Creo 不告诉你坏在哪个字符，所以用同一份参数头、只换 z 行的写法
   逐个试，把「哪一类构造过不去」直接钉在日志里。
   每建成一条立刻删掉，不留垃圾。P00/P07 是已知能过的对照，
   如果连它们都过不去，说明问题不在方程而在环境。
   --------------------------------------------------------------- */
#define HEQ_PROBE_HDR \
    L"R = 20\nn = 8\np1 = 3\np2 = 10\n\ntheta = t*360*n\nr = R\n"

static const HEqTest g_eqprobes[] =
{
    /* 对照组：tpl5 圆柱螺旋线的写法，实测能建 */
    { L"P00-control-helix", 1,
      L"R = 20\nn = 8\np = 6\nz0 = 0\n\ntheta = t*360*n\nr = R\nz = z0+p*n*t" },

    /* 原式 */
    { L"P01-orig", 1,
      HEQ_PROBE_HDR L"z = n*(p1*t+(p2-p1)*t*t/2)" },

    /* 括起来的东西直接乘：h*(...) 在凸轮模板里是能过的 */
    { L"P02-paren-times", 1,
      HEQ_PROBE_HDR L"z = n*(1-cos(180*t))/2" },

    /* 变量减变量，无括号 */
    { L"P03-var-minus", 1,
      HEQ_PROBE_HDR L"z = p2-p1" },

    /* 括号里只有减号（无外层） */
    { L"P04-paren-minus", 1,
      HEQ_PROBE_HDR L"z = p1+(p2-p1)*t" },

    /* 单层括号，内部无嵌套 */
    { L"P05-flat-paren", 1,
      HEQ_PROBE_HDR L"z = n*(p1*t+p2*t*t/2)" },

    /* 单层括号，内部只有一项 */
    { L"P06-simple-paren", 1,
      HEQ_PROBE_HDR L"z = n*(p1*t)" },

    /* t 自乘 */
    { L"P07-t-square", 1,
      HEQ_PROBE_HDR L"z = n*p1*t*t" },

    /* 拆成两行：先算螺距差，再展开成无括号式（这就是新模板的写法） */
    { L"P08-flat-two-lines", 1,
      HEQ_PROBE_HDR L"dp = p2-p1\nz = n*p1*t+n*dp*t*t/2" },

    /* 全展开成一行 */
    { L"P09-flat-one-line", 1,
      HEQ_PROBE_HDR L"z = n*p1*t+n*p2*t*t/2-n*p1*t*t/2" },

    /* 换名字：排除 p1/p2 这两个变量名本身的问题 */
    { L"P10-rename-ab", 1,
      L"R = 20\nn = 8\na = 3\nb = 10\n\ntheta = t*360*n\nr = R\n"
      L"z = n*(a*t+(b-a)*t*t/2)" },
};

static void HEqProbeBisect(ProSelection model_sel, ProModelitem *p_csys,
                           ProSolid solid)
{
    ProFeature feat;
    ProError   st;
    int        i, n = (int)(sizeof(g_eqprobes) / sizeof(g_eqprobes[0]));

    if (model_sel == NULL || p_csys == NULL)
        return;

    HLog("HEqProbeBisect: start (%d cases)\n", n);
    for (i = 0; i < n; i++)
    {
        int failed = -1;

        memset(&feat, 0, sizeof(feat));
        st = HEqCreateCurveEx(model_sel, p_csys, g_eqprobes[i].ctype,
                              g_eqprobes[i].tag, g_eqprobes[i].eq, &feat);
        if (st == PRO_TK_NO_ERROR)
            failed = HEqFeatureFailed(solid, feat.id);
        HLog("HEqProbeBisect: [%ls] create=%d failed=%d\n",
             g_eqprobes[i].tag, (int)st, failed);
        if (st == PRO_TK_NO_ERROR && feat.id > 0)
            HEqDeleteFeat(solid, feat.id);
    }
    HLog("HEqProbeBisect: end\n");
}

/* 建特征失败（连特征都没建出来）时的单发探针 */
static ProError HEqProbeCreate(ProSelection model_sel, ProModelitem *p_csys)
{
    ProFeature feat;
    ProError   st;

    memset(&feat, 0, sizeof(feat));
    st = HEqCreateCurveEx(model_sel, p_csys, 0, L"EQ_PROBE",
                          L"x = t*10\ny = 0\nz = 0", &feat);
    HLog("HEqProbe: create -> %d feat=%d\n", (int)st, feat.id);
    return st;
}

static int HEqCurveAct(uiCmdCmdId command, uiCmdValue *p_value,
                       void *p_push_command_data)
{
    ProMdl        mdl  = NULL;
    ProMdlType    mtype = PRO_MDL_UNUSED;
    ProModelitem  item;
    ProSelection  model_sel = NULL;
    ProReference  ref = NULL;
    ProFeature    feat;
    ProError      st;
    ProModelitem  csys_item;     /* 坐标系 datum（探针重建引用时还要用） */
    wchar_t      *txt = NULL;
    HCsysFind     find;
    HEqDlg        dlg;
    int           csys_type = 0, i;
    int           ans, use_file = 0;

    (void)command;
    (void)p_value;
    (void)p_push_command_data;

    if (HGetSolid3D(&mdl, &mtype) != PRO_TK_NO_ERROR)
    {
        HMsg("TD_NOT_3D");
        return 0;
    }

    /* ---- 1) 弹模板选择框：选方程、改参数 ----
       三条出路：
         · 点了「生成曲线」且选的是模板  → HEqBuild 拼方程；
         · 点了「生成曲线」但选的是自定义 → use_file，走下面的文件模式；
         · 取消                        → 直接返回（不回退，否则用户点了取消
                                         却凭空建出一条曲线）；
         · 窗口压根建不起来             → use_file，退回文件模式（防按钮失效）。 */
    HEqCsysCollect(mdl);          /* 下拉框要列出模型里的坐标系，先枚举一遍 */
    ans = HEqDlgShow(&dlg);
    if (ans == 1)
    {
        if (dlg.tpl < 0 || dlg.tpl >= H_EQ_TPL_N)
        {
            HMsg("TD_EQ_FAIL");
            return 0;
        }
        if (g_eqtpl[dlg.tpl].ctype < 0)
        {
            use_file = 1;
        }
        else if (!HEqBuild(&dlg, &csys_type))
        {
            HMsg("TD_EQ_FAIL");
            return 0;
        }
    }
    else if (dlg.created)
    {
        HLog("HEqCurveAct: dialog cancelled by user\n");
        return 0;
    }
    else
    {
        use_file = 1;
        HLog("HEqCurveAct: dialog unavailable, fall back to %s\n", H_EQFILE);
    }

    /* ---- 2) 自定义模式：读 eq_curve.txt（文件模式 / 回退路径） ---- */
    if (use_file)
    {
        txt = HLoadTextW(H_EQFILE);
        if (txt == NULL)
        {
            HWriteEqTemplate();
            HLog("HEqCurveAct: no template, created %s\n", H_EQFILE);
            HMsg("TD_EQ_NOTEMPLATE");
            ShellExecuteA(NULL, "open", "notepad.exe", H_EQFILE, NULL,
                          SW_SHOWNORMAL);
            return 0;
        }
        HEqParse(txt, &csys_type);
        free(txt);
        txt = NULL;
    }

    HLog("HEqCurveAct: type=%d csys=", csys_type);
    HLogW("", g_csys_name);
    HLog("HEqCurveAct: eq_len=%d file=%d\n", (int)wcslen(g_eq_buf), use_file);
    if (g_eq_buf[0] == L'\0')
    {
        HMsg("TD_EQ_EMPTY");
        return 0;
    }

    /* ---- 3) 找坐标系，建 reference ----
       ★ 2026-10-23 修：原来走 ProSelectionAlloc -> ProSelectionToReference，
       实测就在这几步里静默失败（日志打到 csys found 就没了，只弹"生成曲线失败"）。
       现在主路改 ProReferenceAlloc + ProReferenceSet（完全不经过 Selection），
       失败才退回 Selection 路线，而且每一步都把状态码记进日志。 */
    memset(&find, 0, sizeof(find));
    if (g_csys_sel_id > 0)
    {
        /* 下拉框里选定了具体坐标系：直接用 datum id，不再按名字匹配 */
        find.found = 1;
        find.id    = g_csys_sel_id;
        HLog("HEqCurveAct: csys from dialog id=%d\n", find.id);
    }
    else
    {
        ProSolidCsysVisit((ProSolid)mdl, HCsysFindVisit, NULL,
                          (ProAppData)&find);
        HLog("HEqCurveAct: csys found=%d id=%d\n", find.found, find.id);
    }
    if (find.found <= 0)
    {
        HMsg("TD_EQ_NOCSYS");
        return 0;
    }

    /* ★ 只用 PRO_CSYS 初始化；失败就报错退出，不再退回 PRO_FEATURE
       —— 那条退路只会造出一条"特征引用"，对 REF_CSYS 元素是无效的。 */
    memset(&item, 0, sizeof(item));
    st = ProModelitemInit(mdl, find.id, PRO_CSYS, &item);
    HLog("HEqCurveAct: itemInit(PRO_CSYS, id=%d) -> %d type=%d id=%d\n",
         find.id, (int)st, (int)item.type, item.id);
    if (st != PRO_TK_NO_ERROR)
    {
        HMsg("TD_EQ_NOCSYS");
        return 0;
    }
    csys_item = item;            /* 存一份：下面 item 会被 modelitem 覆盖 */

    st = ProReferenceAlloc(&ref);
    HLog("HEqCurveAct: refAlloc -> %d\n", (int)st);
    if (st == PRO_TK_NO_ERROR && ref != NULL)
    {
        st = ProReferenceSet(ref, NULL, &item);
        HLog("HEqCurveAct: refSet(local) -> %d\n", (int)st);
        if (st != PRO_TK_NO_ERROR)
        {
            ProReferenceFree(ref);
            ref = NULL;
        }
    }
    if (ref == NULL)
    {
        /* 退路：Selection 转 Reference */
        ProSelection tmp_sel = NULL;

        st = ProSelectionAlloc(NULL, &item, &tmp_sel);
        HLog("HEqCurveAct: selAlloc(csys) -> %d\n", (int)st);
        if (st == PRO_TK_NO_ERROR && tmp_sel != NULL)
        {
            st = ProSelectionToReference(tmp_sel, &ref);
            HLog("HEqCurveAct: selToRef -> %d\n", (int)st);
            ProSelectionFree(&tmp_sel);
        }
    }
    if (ref == NULL)
    {
        HMsg("TD_EQ_FAIL");
        return 0;
    }
    HEqRefDiag(ref);             /* 引用建好先自检一遍 */

    /* ---- 3b) 建特征用的 model selection ---- */
    st = ProMdlToModelitem(mdl, &item);
    HLog("HEqCurveAct: mdlToModelitem -> %d type=%d id=%d\n",
         (int)st, (int)item.type, item.id);
    if (st == PRO_TK_NO_ERROR)
        st = ProSelectionAlloc(NULL, &item, &model_sel);
    HLog("HEqCurveAct: modelSelAlloc -> %d ok=%d\n", (int)st,
         (model_sel != NULL) ? 1 : 0);
    if (st != PRO_TK_NO_ERROR || model_sel == NULL)
    {
        ProReferenceFree(ref);
        HMsg("TD_EQ_FAIL");
        return 0;
    }

    /* ---- 4) 创建 ----
       单曲线模板建 1 条；凸轮模板按运动段建多条（每段一个纯 sin/cos 表达式）。
       ★ 2026-10-23：方程求值器不接受 if/else/endif —— 大小写、缩进、嵌套、
         有无 else、单层嵌套全试过，一律"特征建成但重新生成失败"。所以凸轮
         改成"拆段"：参数 t 映射到本段的角度区间，段与段首尾严格连续。 */
    {
        const HEqTpl *tpl  = NULL;
        int           nseg = 1;
        int           use_seg = 0;
        int           nok = 0, nfail = 0, nbad = 0;

        if (!use_file && dlg.tpl >= 0 && dlg.tpl < H_EQ_TPL_N)
        {
            tpl     = &g_eqtpl[dlg.tpl];
            use_seg = 1;
            nseg    = (tpl->nseg > 0) ? tpl->nseg : 1;
        }

        for (i = 0; i < nseg; i++)
        {
            const wchar_t *nm   = L"EQ_CURVE";
            const wchar_t *body = NULL;

            if (use_seg && tpl != NULL && tpl->nseg > 0)
            {
                nm   = tpl->segname[i];
                body = tpl->segeq[i];
                if (nm == NULL || body == NULL
                    || !HEqBuildBody(&dlg, tpl, body))
                {
                    HLog("HEqCurveAct: seg[%d] build FAILED\n", i);
                    nbad++;
                    continue;
                }
            }
            /* 单曲线模板 / 文件模式：g_eq_buf 已由 HEqBuild / HEqParse 拼好 */

            HLog("HEqCurveAct: seg[%d/%d] name=", i + 1, nseg);
            HLogW("", nm);
            HLog("HEqCurveAct: seg[%d] eq_len=%d\n", i, (int)wcslen(g_eq_buf));

            memset(&feat, 0, sizeof(feat));
            st = HEqCreateCurveEx(model_sel, &csys_item, csys_type,
                                  nm, g_eq_buf, &feat);
            HLog("HEqCurveAct: seg[%d] create -> %d feat=%d\n",
                 i, (int)st, feat.id);
            if (st != PRO_TK_NO_ERROR)
            {
                nbad++;
                continue;
            }
            if (feat.id > 0 && HEqFeatureFailed((ProSolid)mdl, feat.id))
            {
                nfail++;
                HEqReadBackEq(&feat);      /* Creo 到底怎么存的 */
            }
            else
            {
                nok++;
            }
        }

        HLog("HEqCurveAct: done seg=%d ok=%d failed=%d bad=%d\n",
             nseg, nok, nfail, nbad);

        if (nfail > 0)
        {
            /* 建出来了但求值失败：跑内置方程自检，把是哪种构造过不去打进日志 */
            HEqSelfTest(model_sel, &csys_item, (ProSolid)mdl);
            ProReferenceFree(ref);
            ProSelectionFree(&model_sel);
            HMsg("TD_EQ_SELFTEST");
            return 0;
        }
        if (nok == 0)
        {
            /* 一条都没建成：跑语法探针，把坏的写法钉进日志 */
            if (nbad > 0)
                HEqProbeBisect(model_sel, &csys_item, (ProSolid)mdl);
            ProReferenceFree(ref);
            ProSelectionFree(&model_sel);
            HMsg("TD_EQ_FAIL");
            return 0;
        }
        st = PRO_TK_NO_ERROR;
    }

    ProReferenceFree(ref);
    ProSelectionFree(&model_sel);

    ProWindowRepaint(PRO_VALUE_UNUSED);
    HMsg("TD_EQ_OK");
    return 0;
}

/*==============================================================*\
  9. 标准件库（2026-10-28 新增）

  做什么：
      把 <插件目录>\stdlib 下的族表零件做成一个能点的库 ——
      左列类别 → 中列零件 → 右列规格 → 插进当前装配。

  库从哪来：
      HelloCreo V1.80 的 stdlib（第三方商用零件，只本机自用）。
      .gitignore 已排除 stdlib/、pack.py 白名单也不含它 ——
      **不能进公开仓库，也不能进 release 包**。

  目录约定（跟 HelloCreo 一样，随时可以把自己的零件丢进去）：
      stdlib\<类别>\index.idx        一行一个零件名，不含版本号
      stdlib\<类别>\<零件>.prt.1     族表基件（.asm 也行）

  规格列不读它的 .idx，而是现场问 Creo 要族表实例：
      ProFamtableInit + ProFamtableInstanceVisit
  第三方维护的 idx 可能过期，族表是唯一的真相。

  插入链路（官方 TestCavityLayout.c 的写法）：
      ProFamtableInit(基件) → ProFaminstanceInit(实例名)
      → ProFaminstanceRetrieve → ProAsmcompAssemble(当前装配, 实例, 单位阵)
  注意 ProFaminstanceMdlGet 只查【已在会话里】的实例，取不到要先用
  ProFaminstanceRetrieve 把它从磁盘生成出来。
\*==============================================================*/
#define HSTD_SUBDIR   L"stdlib"
#define HSTD_MAX_CAT  40
#define HSTD_MAX_PART 300
#define HSTD_MAX_INST 1500
#define HSTD_NAME     96

#define HSD_CAT   4101
#define HSD_PART  4102
#define HSD_INST  4103
#define HSD_INFO  4104
#define HSD_OK    4105
#define HSD_CAN   4106
#define HSD_L1    4107
#define HSD_L2    4108
#define HSD_L3    4109
#define HSD_TIP   4110
#define HSD_PREV  4111
#define HSD_PNOTE 4112
#define HSD_S(d, v) MulDiv((v), ((d)->sc > 0 ? (d)->sc : 100), 100)

typedef struct
{
    wchar_t name[HSTD_NAME];        /* 不含扩展名 */
    wchar_t path[PRO_PATH_SIZE];    /* 类别=目录；零件=全路径（不带扩展名） */
} HStdEnt;

typedef struct
{
    wchar_t  root[PRO_PATH_SIZE];
    HStdEnt  cat[HSTD_MAX_CAT];     int ncat;
    HStdEnt  part[HSTD_MAX_PART];   int npart;
    wchar_t  inst[HSTD_MAX_INST][HSTD_NAME]; int ninst;
    int      nofam;                 /* 1 = 这个零件没有族表，插基件 */
    int      ftbad;                 /* 1 = 有族表但读不出来（不是"没有"） */
    wchar_t  verinfo[HSTD_NAME];    /* ftbad 时的线索：目录里那个新版文件 */
    int      cat_sel, part_sel, inst_sel;
    wchar_t  loaded[HSTD_NAME];     /* 已经问过族表的零件名，做缓存 */

    HWND     hwnd, hcat, hpart, hinst, hinfo, htip, hok, hcan, hl1, hl2, hl3;
    HWND     hprev;                 /* 预览图（stdlb 里那对同名 png） */
    HWND     hnote;                 /* 预览框下方的小字：无图/加载失败时提示 */
    HBITMAP  prevbm;                /* 当前预览位图，换图/关窗要删 */
    HFONT    font;
    int      sc, done, ok, inserted;
    int      req_insert;            /* 用户点了插入：关窗后由 HStdLibAct 执行 */
} HStdDlg;

/* 一次只开一个库窗口：这个结构体带 1500 个实例名，放栈上会爆 */
static HStdDlg g_hstd;

/* ---- 族表实例名单缓存 ----------------------------------------------------
   实测（Creo 12 + 这套 Creo 7 写的库）：同一个模型第二次 ProFamtableInit
   会拿到空表 —— 日志里 famtableCheck 第一次 0（有表）、第二次 -4（找不到），
   visit 从 85 条变成 0 条。表现就是「点一次零件，规格列表就只剩基件」。

   所以每个零件只问一次族表，结果留在这里；之后无论怎么切换都走缓存。
   顺带把 ProFamtableErase 也去掉了 —— 它很可能就是让第二次 Init 拿到空表
   的原因（句柄释放后 Creo 侧的族表缓存没跟着复位）。                  */
#define HSTD_CACHE_MAX 64
#define HSTD_COL_MAX   24       /* 族表最多几个驱动项（实测 7~11） */
#define HSTD_NOVAL     (-1.0e300)

typedef struct
{
    wchar_t  key[PRO_PATH_SIZE];
    wchar_t *names;                 /* n 个 HSTD_NAME 宽的连续块 */
    int      n;
    int      nofam;

    /* 族表列（驱动项）+ 每个实例的整行值。
       ★ 只有「第一次 ProFamtableInit」那次读得到，第二次起 Creo 12 返回空表，
         所以必须在那一刻抄下来，之后一律用缓存，不再碰族表接口。 */
    int      ncol;
    int      colid[HSTD_COL_MAX];
    int      coltype[HSTD_COL_MAX];
    double  *vals;                  /* n × HSTD_COL_MAX，HSTD_NOVAL = 取不到 */
} HStdCache;

static HStdCache g_hstd_cache[HSTD_CACHE_MAX];
static int       g_hstd_ncache;

/* 当前正在浏览的零件路径（插入时用它找回缓存） */
static wchar_t   g_hstd_curpath[PRO_PATH_SIZE];

/* 读族表那一趟用的暂存区 */
static int             g_ft_ncol;
static ProFamtableItem g_ft_item[HSTD_COL_MAX];
static int             g_ft_colid[HSTD_COL_MAX];
static int             g_ft_coltype[HSTD_COL_MAX];
static wchar_t         g_ft_colname[HSTD_COL_MAX][PRO_NAME_SIZE];
static double          g_ft_vals[HSTD_MAX_INST][HSTD_COL_MAX];

static int HStdDirExistsW(const wchar_t *p)
{
    DWORD a = GetFileAttributesW(p);

    return (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY)) ? 1 : 0;
}

/* 拼 <dir>\<base>（不带扩展名，retrieve 用） */
static int HStdJoinW(wchar_t *out, int cap, const wchar_t *dir, const wchar_t *base)
{
    size_t dl  = wcslen(dir);
    int    sep = (dl > 0 && (dir[dl - 1] == L'\\' || dir[dl - 1] == L'/')) ? 0 : 1;

    if ((int)(dl + (size_t)sep + wcslen(base) + 1) > cap)
        return -1;
    swprintf(out, (size_t)cap, L"%ls%ls%ls", dir, sep ? L"\\" : L"", base);
    return 0;
}

/* 去掉末尾的 .prt / .asm（大小写都认，index.idx 里混着写） */
static void HStdStripExt(wchar_t *s)
{
    size_t n = wcslen(s);

    while (n > 0 && (s[n - 1] == L' ' || s[n - 1] == L'\t'))
        s[--n] = L'\0';
    if (n > 4 && _wcsicmp(s + n - 4, L".prt") == 0)
        s[n - 4] = L'\0';
    else if (n > 4 && _wcsicmp(s + n - 4, L".asm") == 0)
        s[n - 4] = L'\0';
}

/* 文件其实是 name.prt.1，直接查 name.prt 是查不到的，必须用通配符 */
static int HStdMdlExistsW(const wchar_t *dir, const wchar_t *base)
{
    static const wchar_t *exts[] = { L".prt*", L".asm*" };
    wchar_t        pat[PRO_PATH_SIZE];
    WIN32_FIND_DATAW fd;
    HANDLE         h;
    int            i;

    for (i = 0; i < 2; i++)
    {
        swprintf_s(pat, PRO_PATH_SIZE, L"%ls\\%ls%ls", dir, base, exts[i]);
        h = FindFirstFileW(pat, &fd);
        if (h != INVALID_HANDLE_VALUE)
        {
            FindClose(h);
            return 1;
        }
    }
    return 0;
}

/* 扫目录里 <base>.prt.N / .asm.N，返回最大版本号 N，并把文件名写进 out。
   ★ 为什么需要它：Creo 永远优先加载版本号最大的文件。目录里只要躺着一个
     被保存过的 .prt.2，Creo 就再也看不到那个 .prt.1 —— 而 .prt.2 是残的
     时候，族表就读不出来（famtableCheck -4），列表表现为空白。 */
static int HStdNewestVerW(const wchar_t *path_noext, wchar_t *out, int cap)
{
    static const wchar_t *exts[] = { L".prt.*", L".asm.*" };
    WIN32_FIND_DATAW fd;
    HANDLE           h;
    wchar_t          pat[PRO_PATH_SIZE];
    int              i, best = 0;

    if (out != NULL && cap > 0)
        out[0] = L'\0';

    for (i = 0; i < 2; i++)
    {
        swprintf_s(pat, PRO_PATH_SIZE, L"%ls%ls", path_noext, exts[i]);
        h = FindFirstFileW(pat, &fd);
        if (h == INVALID_HANDLE_VALUE)
            continue;
        do
        {
            const wchar_t *dot = wcsrchr(fd.cFileName, L'.');
            int            v   = (dot != NULL) ? _wtoi(dot + 1) : 0;

            if (v > best)
            {
                best = v;
                if (out != NULL && cap > 0)
                    swprintf_s(out, (size_t)cap, L"%ls (%lu KB)", fd.cFileName,
                               (unsigned long)(fd.nFileSizeLow / 1024u));
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    return best;
}

/* 库根目录：① 环境变量 H_STDLIB ② <插件目录>\stdlib ③ HelloCreo 原目录 */
static int HStdRootW(wchar_t *out, int cap)
{
    wchar_t env[PRO_PATH_SIZE];
    ProPath base;
    DWORD   n;

    out[0] = L'\0';

    n = GetEnvironmentVariableW(L"H_STDLIB", env, PRO_PATH_SIZE);
    if (n > 0 && n < PRO_PATH_SIZE && HStdDirExistsW(env))
    {
        wcsncpy_s(out, (size_t)cap, env, _TRUNCATE);
        HLogW("HStdRootW: env -> ", out);
        return 1;
    }

    HPluginDirW(base);
    if (base[0] != L'\0')
    {
        swprintf_s(out, (size_t)cap, L"%ls\\%ls", base, HSTD_SUBDIR);
        if (HStdDirExistsW(out))
        {
            HLogW("HStdRootW: plugin -> ", out);
            return 1;
        }
    }

    if (HStdDirExistsW(L"D:\\HelloCreoV180\\HelloCreo\\stdlib"))
    {
        wcsncpy_s(out, (size_t)cap, L"D:\\HelloCreoV180\\HelloCreo\\stdlib", _TRUNCATE);
        HLogW("HStdRootW: hellocreo -> ", out);
        return 1;
    }

    out[0] = L'\0';
    HLog("HStdRootW: not found\n");
    return 0;
}

static int HStdScanCats(HStdDlg *d)
{
    wchar_t        pat[PRO_PATH_SIZE];
    WIN32_FIND_DATAW fd;
    HANDLE         h;

    d->ncat = 0;
    swprintf_s(pat, PRO_PATH_SIZE, L"%ls\\*", d->root);
    h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE)
    {
        HLogW("HStdScanCats: empty dir ", d->root);
        return 0;
    }
    do
    {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
            continue;
        if (fd.cFileName[0] == L'.')
            continue;
        if (d->ncat >= HSTD_MAX_CAT)
            break;
        wcsncpy_s(d->cat[d->ncat].name, HSTD_NAME, fd.cFileName, _TRUNCATE);
        HStdJoinW(d->cat[d->ncat].path, PRO_PATH_SIZE, d->root, fd.cFileName);
        d->ncat++;
    } while (FindNextFileW(h, &fd));
    FindClose(h);

    HLog("HStdScanCats: %d categories\n", d->ncat);
    return d->ncat;
}

/* 读 <类别>\index.idx；没有就退化成扫目录里的 *.prt.* / *.asm.* */
static int HStdScanParts(HStdDlg *d)
{
    static ProLine lines[HSTD_MAX_PART + 20];
    wchar_t idx[PRO_PATH_SIZE];
    int     n, i;

    d->npart    = 0;
    d->part_sel = -1;
    d->inst_sel = -1;
    d->ninst    = 0;
    d->nofam    = 0;
    d->loaded[0] = L'\0';

    if (d->cat_sel < 0 || d->cat_sel >= d->ncat)
        return 0;

    if (HPathJoinW(idx, PRO_PATH_SIZE, d->cat[d->cat_sel].path,
                   L"index", L".idx") == 0 && HFileExistsW(idx))
    {
        n = HReadTplLinesW(idx, lines, HSTD_MAX_PART + 20);
        for (i = 0; i < n; i++)
        {
            wchar_t nm[HSTD_NAME];

            if (lines[i][0] == L'\0' || lines[i][0] == L' ')
                continue;
            wcsncpy_s(nm, HSTD_NAME, lines[i], _TRUNCATE);
            HStdStripExt(nm);
            if (nm[0] == L'\0')
                continue;
            if (!HStdMdlExistsW(d->cat[d->cat_sel].path, nm))
            {
                HLogW("HStdScanParts: missing ", nm);
                continue;
            }
            if (d->npart >= HSTD_MAX_PART)
                break;
            wcscpy_s(d->part[d->npart].name, HSTD_NAME, nm);
            HStdJoinW(d->part[d->npart].path, PRO_PATH_SIZE,
                      d->cat[d->cat_sel].path, nm);
            d->npart++;
        }
    }

    if (d->npart == 0)                     /* 没有 index.idx，直接扫 */
    {
        wchar_t          pat[PRO_PATH_SIZE];
        WIN32_FIND_DATAW fd;
        HANDLE           h;

        swprintf_s(pat, PRO_PATH_SIZE, L"%ls\\*.prt*", d->cat[d->cat_sel].path);
        h = FindFirstFileW(pat, &fd);
        if (h != INVALID_HANDLE_VALUE)
        {
            do
            {
                wchar_t nm[HSTD_NAME];
                wchar_t *dot;

                wcsncpy_s(nm, HSTD_NAME, fd.cFileName, _TRUNCATE);
                dot = wcsrchr(nm, L'.');           /* 去掉版本号 .1 */
                if (dot != NULL && dot != nm)
                    *dot = L'\0';
                HStdStripExt(nm);                  /* 去掉 .prt */
                if (nm[0] == L'\0' || d->npart >= HSTD_MAX_PART)
                    continue;
                wcscpy_s(d->part[d->npart].name, HSTD_NAME, nm);
                HStdJoinW(d->part[d->npart].path, PRO_PATH_SIZE,
                          d->cat[d->cat_sel].path, nm);
                d->npart++;
            } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
    }

    if (d->npart > 0)
        d->part_sel = 0;
    HLog("HStdScanParts: %d parts in cat %d\n", d->npart, d->cat_sel);
    return d->npart;
}

/* 加载基件：先在会话里找，再按全路径 retrieve，最后切工作目录按名字取
   （ProMdlnameRetrieve 只在当前工作目录里找，这是官方文档明写的） */
static ProError HStdLoadMdl(const wchar_t *path_noext, ProMdl *out)
{
    ProMdl      m = NULL;
    ProMdlName  full;
    ProName     base;
    ProPath     old, dir;
    const wchar_t *b;
    wchar_t    *slash;
    ProError    st;

    *out = NULL;
    b = wcsrchr(path_noext, L'\\');
    b = (b != NULL) ? b + 1 : path_noext;
    wcsncpy_s(base, PRO_NAME_SIZE, b, _TRUNCATE);
    wcsncpy_s(full, PRO_MDLNAME_SIZE, path_noext, _TRUNCATE);

    /* 1) 全路径 retrieve（不带扩展名）—— 必须放第一位。
       ★★ 2026-10-28 实测教训：原来把 ProMdlnameInit 放在最前面当"已在会话里"，
          它会返回 NO_ERROR + 非空句柄，但那个句柄上的族表是空的 ——
          h_load.log 里凡是走这条分支的零件全部 `visit -> 0, 0 instances`
          （04-垫片、05-螺母），而走 pathRetrieve 的全对（85 / 104 / 173 / 19 / 145）。
          同一个 05-螺母 换条路径就能拿到 19 个实例，说明零件本身没问题，
          是句柄不对。所以先按全路径 retrieve，再考虑会话里的。 */
    st = ProMdlnameRetrieve(full, PRO_MDLFILE_PART, &m);
    if (st != PRO_TK_NO_ERROR)
        st = ProMdlnameRetrieve(full, PRO_MDLFILE_ASSEMBLY, &m);
    HLog("HStdLoadMdl: pathRetrieve -> %d\n", (int)st);
    if (st == PRO_TK_NO_ERROR && m != NULL)
    {
        *out = m;
        return PRO_TK_NO_ERROR;
    }

    /* 2) 已经在会话里（浏览时加载过） */
    st = ProMdlnameInit(base, PRO_MDL_PART, &m);
    if (st != PRO_TK_NO_ERROR || m == NULL)
        st = ProMdlnameInit(base, PRO_MDLFILE_ASSEMBLY, &m);
    if (st == PRO_TK_NO_ERROR && m != NULL)
    {
        ProMdlName  got;
        ProMdlType  mt = PRO_MDL_UNUSED;

        got[0] = L'\0';
        ProMdlMdlnameGet(m, got);
        ProMdlTypeGet(m, &mt);
        HLogW("HStdLoadMdl: in session ", got);
        HLog("HStdLoadMdl: in session type=%d\n", (int)mt);
        *out = m;
        return PRO_TK_NO_ERROR;
    }

    /* 3) 切到库目录再按名字取，取完立刻切回 */
    wcsncpy_s(dir, PRO_PATH_SIZE, path_noext, _TRUNCATE);
    slash = wcsrchr(dir, L'\\');
    if (slash != NULL)
        *slash = L'\0';
    if (ProDirectoryCurrentGet(old) == PRO_TK_NO_ERROR
        && ProDirectoryChange(dir) == PRO_TK_NO_ERROR)
    {
        st = ProMdlnameRetrieve(base, PRO_MDLFILE_PART, &m);
        if (st != PRO_TK_NO_ERROR)
            st = ProMdlnameRetrieve(base, PRO_MDLFILE_ASSEMBLY, &m);
        ProDirectoryChange(old);
        HLog("HStdLoadMdl: chdirRetrieve -> %d\n", (int)st);
        if (st == PRO_TK_NO_ERROR && m != NULL)
        {
            *out = m;
            return PRO_TK_NO_ERROR;
        }
    }

    HLogW("HStdLoadMdl: failed ", path_noext);
    return (st != PRO_TK_NO_ERROR) ? st : PRO_TK_GENERAL_ERROR;
}

/* 族表列（驱动项）遍历：把每个列的 id/类型/名字连同 ProFamtableItem 本体
   一起抄下来。ProFamtableItem 是 {type, 列名字符串, owner} 的值结构，不是
   不透明句柄，整块 memcpy 之后仍然可以拿去问值。 */
static ProError HStdColVisit(ProFamtableItem *item, ProError status, ProAppData app)
{
    ProModelitem mi;
    int          i = g_ft_ncol;

    (void)status;
    (void)app;
    if (i >= HSTD_COL_MAX)
        return PRO_TK_NO_ERROR;

    g_ft_item[i]       = *item;
    g_ft_colid[i]      = 0;
    g_ft_coltype[i]    = (int)PRO_DIMENSION;
    g_ft_colname[i][0] = L'\0';

    if (ProFamtableItemToModelitem(item, &mi) == PRO_TK_NO_ERROR)
    {
        g_ft_colid[i]   = mi.id;
        g_ft_coltype[i] = (int)mi.type;
        ProModelitemNameGet(&mi, g_ft_colname[i]);
    }
    HLog("HStdColVisit: #%d id=%d type=%d name=%ls\n",
         i, g_ft_colid[i], g_ft_coltype[i], g_ft_colname[i]);
    g_ft_ncol++;
    return PRO_TK_NO_ERROR;
}

/* 实例遍历：记名字，同时把这一行的**全部驱动值**读出来。
   这一步必须在第一次 Init 的那个句柄上做，否则全拿不到。 */
static ProError HStdInstVisit(ProFaminstance *inst, ProError status, ProAppData app)
{
    HStdDlg *d = (HStdDlg *)app;
    int      k, n;

    if (status != PRO_TK_NO_ERROR || d == NULL || inst == NULL)
        return PRO_TK_NO_ERROR;
    if (d->ninst >= HSTD_MAX_INST)
        return PRO_TK_GENERAL_ERROR;        /* 满了就停 */

    n = d->ninst;
    wcsncpy_s(d->inst[n], HSTD_NAME, inst->name, _TRUNCATE);

    for (k = 0; k < HSTD_COL_MAX; k++)
        g_ft_vals[n][k] = HSTD_NOVAL;

    for (k = 0; k < g_ft_ncol; k++)
    {
        ProParamvalue v;

        memset(&v, 0, sizeof(v));
        if (ProFaminstanceValueGet(inst, &g_ft_item[k], &v) != PRO_TK_NO_ERROR)
            continue;
        if (v.type == PRO_PARAM_DOUBLE)
            g_ft_vals[n][k] = v.value.d_val;
        else if (v.type == PRO_PARAM_INTEGER)
            g_ft_vals[n][k] = (double)v.value.i_val;
    }

    if (n < 3)                              /* 头三条留个样本给日志 */
    {
        HLog("HStdInstVisit[%d]: ", n);
        for (k = 0; k < g_ft_ncol && k < 12; k++)
            HLog("%ls=%.4f ", g_ft_colname[k], g_ft_vals[n][k]);
        HLog("\n");
    }
    d->ninst++;
    return PRO_TK_NO_ERROR;
}

/* 兜底：从 <类别>\<类名>.idx 里取这个零件的实例名。
   行格式（HelloCreo）：  2   GB6170_六角螺母M10      GB6170_六角螺母
   第 2 列 = 实例名，第 3 列 = 基件名（大小写常和文件名不一致，按 _wcsicmp 比）。
   Creo 的族表遍历偶尔给 0（原因见 HStdLoadInsts 里的注释），这时用它顶上。 */
static int HStdIdxInsts(HStdDlg *d, const wchar_t *partname)
{
    static ProLine   lines[700];
    wchar_t          pat[PRO_PATH_SIZE];
    WIN32_FIND_DATAW fd;
    HANDLE           h;
    int              got = 0;

    if (d->cat_sel < 0 || d->cat_sel >= d->ncat)
        return 0;

    swprintf_s(pat, PRO_PATH_SIZE, L"%ls\\*.idx", d->cat[d->cat_sel].path);
    h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE)
        return 0;

    do
    {
        wchar_t  f[PRO_PATH_SIZE];
        int      n, i;

        if (_wcsicmp(fd.cFileName, L"index.idx") == 0)
            continue;
        if (HStdJoinW(f, PRO_PATH_SIZE, d->cat[d->cat_sel].path, fd.cFileName) != 0)
            continue;
        n = HReadTplLinesW(f, lines, 700);
        if (n <= 0)
            continue;

        for (i = 0; i < n; i++)
        {
            wchar_t *p = lines[i];
            wchar_t *inst, *gen, *end;

            while (*p == L' ' || *p == L'\t')
                p++;
            if (*p < L'0' || *p > L'9')
                continue;                       /* MB_INST_IDX 那行 */
            while (*p >= L'0' && *p <= L'9')
                p++;                            /* 序号 */
            while (*p == L' ' || *p == L'\t')
                p++;
            if (*p == L'\0')
                continue;
            inst = p;
            while (*p != L'\0' && *p != L' ' && *p != L'\t')
                p++;
            if (*p != L'\0')
                *p++ = L'\0';
            while (*p == L' ' || *p == L'\t')
                p++;
            gen = p;
            end = p;
            while (*end != L'\0')
                end++;
            while (end > gen && (end[-1] == L' ' || end[-1] == L'\t'))
                *--end = L'\0';
            if (gen[0] == L'\0' || _wcsicmp(gen, partname) != 0)
                continue;
            if (got >= HSTD_MAX_INST)
                break;
            wcsncpy_s(d->inst[got], HSTD_NAME, inst, _TRUNCATE);
            got++;
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);

    HLog("HStdIdxInsts: %d instances for this part (from idx)\n", got);
    return got;
}

/* 问 Creo 要这个零件的族表实例（缓存：同一个零件只问一次） */
/* 把当前 d 里的实例名单存进缓存（按零件全路径作 key） */
static void HStdCachePut(const wchar_t *key, HStdDlg *d)
{
    HStdCache *c;

    if (g_hstd_ncache >= HSTD_CACHE_MAX || key == NULL || key[0] == L'\0')
        return;
    if (d->ninst <= 0)
        return;
    /* 只缓存「真的读到族表」的零件。nofam 不缓存：那可能是 Creo 这次
       没读出来（-4），下次切换还应该再试一次。 */
    if (d->nofam)
        return;

    c = &g_hstd_cache[g_hstd_ncache];
    c->names = (wchar_t *)malloc((size_t)d->ninst * sizeof(d->inst[0]));
    if (c->names == NULL)
        return;

    memcpy(c->names, d->inst, (size_t)d->ninst * sizeof(d->inst[0]));
    c->n     = d->ninst;
    c->nofam = d->nofam;
    wcsncpy_s(c->key, PRO_PATH_SIZE, key, _TRUNCATE);

    /* 连同族表列与整行值一起存（大小要和 d->inst 一样多行） */
    c->ncol = (g_ft_ncol > HSTD_COL_MAX) ? HSTD_COL_MAX : g_ft_ncol;
    memset(c->colid, 0, sizeof(c->colid));
    memset(c->coltype, 0, sizeof(c->coltype));
    c->vals = NULL;
    if (c->ncol > 0 && d->ninst <= HSTD_MAX_INST)
    {
        int k;

        for (k = 0; k < c->ncol; k++)
        {
            c->colid[k]   = g_ft_colid[k];
            c->coltype[k] = g_ft_coltype[k];
        }
        c->vals = (double *)malloc((size_t)d->ninst * HSTD_COL_MAX * sizeof(double));
        if (c->vals != NULL)
            memcpy(c->vals, g_ft_vals, (size_t)d->ninst * HSTD_COL_MAX * sizeof(double));
    }

    g_hstd_ncache++;
    HLog("HStdCachePut: %d instances, %d cols (cache %d/%d)\n",
         c->n, c->ncol, g_hstd_ncache, HSTD_CACHE_MAX);
}

/* 缓存命中就把名单铺回 d，返回 1 */
static int HStdCacheGet(const wchar_t *key, HStdDlg *d)
{
    int i;

    if (key == NULL)
        return 0;
    for (i = 0; i < g_hstd_ncache; i++)
    {
        HStdCache *c = &g_hstd_cache[i];

        if (wcscmp(c->key, key) != 0)
            continue;

        d->ninst = (c->n > HSTD_MAX_INST) ? HSTD_MAX_INST : c->n;
        d->nofam = c->nofam;
        d->ftbad = 0;               /* 能进缓存的都是真读到表的零件 */
        d->verinfo[0] = L'\0';
        if (d->ninst > 0 && c->names != NULL)
            memcpy(d->inst, c->names, (size_t)d->ninst * sizeof(d->inst[0]));
        d->inst_sel = 0;
        HLog("HStdCacheGet: hit, %d instances\n", d->ninst);
        return 1;
    }
    return 0;
}

static HStdCache *HStdCacheFind(const wchar_t *key)
{
    int i;

    if (key == NULL || key[0] == L'\0')
        return NULL;
    for (i = 0; i < g_hstd_ncache; i++)
        if (_wcsicmp(g_hstd_cache[i].key, key) == 0)
            return &g_hstd_cache[i];
    return NULL;
}

static int HStdLoadInsts(HStdDlg *d)
{
    ProMdl      gen = NULL;
    ProFamtable famtab;
    ProError    st;
    HStdEnt    *p;
    int         famtab_bad = 0;

    if (d->part_sel < 0 || d->part_sel >= d->npart)
    {
        d->ninst    = 0;
        d->nofam    = 0;
        d->inst_sel = -1;
        return 0;
    }
    p = &d->part[d->part_sel];

    /* 当前零件路径必须**先**记下：命中缓存时后面那个赋值不会执行，
       否则插入时会去上一个零件的族表缓存里找值。 */
    wcsncpy_s(g_hstd_curpath, PRO_PATH_SIZE, p->path, _TRUNCATE);

    /* ① 只上过一次族表就已经读过的零件，直接给名单。
          Creo 12 第二次问同一个模型会返回空表，这里必须挡住。 */
    if (HStdCacheGet(p->path, d))
        return d->ninst;

    d->ninst      = 0;
    d->nofam      = 0;
    d->ftbad      = 0;
    d->verinfo[0] = L'\0';
    d->inst_sel   = -1;
    wcsncpy_s(d->loaded, HSTD_NAME, p->name, _TRUNCATE);

    st = HStdLoadMdl(p->path, &gen);
    if (st != PRO_TK_NO_ERROR || gen == NULL)
    {
        HMsg("TD_STD_NOLOAD");
        return 0;
    }

    st = ProFamtableInit(gen, &famtab);
    HLog("HStdLoadInsts: famtableInit -> %d\n", (int)st);
    if (st == PRO_TK_NO_ERROR)
    {
        ProError ck = ProFamtableCheck(&famtab);

        HLog("HStdLoadInsts: famtableCheck -> %d (EMPTY=%d)\n",
             (int)ck, (int)PRO_TK_EMPTY);
        /* 表能打开、但不是「空」而是别的码（实测 -4）＝ 表在，只是读不出来。
           这跟「零件本来就没有族表」是两回事，不能混着报。 */
        if (ck != PRO_TK_NO_ERROR && ck != PRO_TK_EMPTY)
            famtab_bad = 1;
        /* ★ 这里是唯一能读到族的时刻：先读列，再带着列读每个实例的整行值 */
        g_ft_ncol = 0;
        memset(g_ft_item, 0, sizeof(g_ft_item));
        st = ProFamtableItemVisit(&famtab, HStdColVisit, NULL, NULL);
        HLog("HStdLoadInsts: itemVisit -> %d cols=%d\n", (int)st, g_ft_ncol);

        st = ProFamtableInstanceVisit(&famtab, HStdInstVisit, NULL, d);
        HLog("HStdLoadInsts: visit -> %d, %d instances\n", (int)st, d->ninst);
        /* 刻意不调 ProFamtableErase：实测 Erase 之后同一个模型再 Init
           会拿到空表（famtableCheck 由 0 变 -4）。这一点泄漏可以接受。 */
    }

    if (d->ninst == 0)                      /* Creo 给 0 → 用厂商 idx 兜底 */
    {
        int n = HStdIdxInsts(d, p->name);

        if (n > 0)
        {
            d->ninst = n;
            HLog("HStdLoadInsts: fallback to idx, %d instances\n", n);
        }
    }

    if (d->ninst == 0 && famtab_bad)
    {
        wchar_t newest[HSTD_NAME];
        int     v = HStdNewestVerW(p->path, newest, HSTD_NAME);

        d->ftbad = 1;
        HLog("HStdLoadInsts: 族表读取失败，目录里最新文件 v%d %ls\n", v, newest);
        if (v > 1)
        {
            HLog("HStdLoadInsts: !!! 存在被保存过的 v%d，Creo 会优先读它\n", v);
            swprintf_s(d->verinfo, HSTD_NAME, L"目录里有 %ls", newest);
        }
        else
            wcsncpy_s(d->verinfo, HSTD_NAME, L"请查 h_load.log", _TRUNCATE);
    }

    if (d->ninst == 0)                      /* 真没有族表 → 直接插基件 */
    {
        d->nofam = 1;
        wcsncpy_s(d->inst[0], HSTD_NAME, p->name, _TRUNCATE);
        d->ninst = 1;
        HLog("HStdLoadInsts: no family table, use generic\n");
    }
    d->inst_sel = 0;

    /* ② 存起来，以后同一个零件不再问 Creo */
    HStdCachePut(p->path, d);
    return d->ninst;
}

/* ---- 族表实例模型：Creo 12 上怎么把它造出来 ------------------------------
   Creo 12 把 ProFaminstanceCreate 从 SDK 里整个删掉了（Creo 6 头文件里它还挂着
   DEPRECATED since Creo 3，但运行时直接回 -34 = PRO_TK_OBSOLETE_FUNC）。
   剩下两条路都要求模型**已经存在**：
       ProFaminstanceMdlGet  —— 会话里已有
       ProFaminstanceRetrieve —— 磁盘上有 <实例名>.prt
   HelloCreo 这套库的 1615 个规格只写在族表定义里、从没落成 .prt 文件，
   所以两条都不通，这就是「这个零件没有可插入的族表实例」的真因。

   于是自己做：读族表里这一行的值 → 复制一份基件 → 把值灌进副本 → 再生。
   得到的副本几何等于 Creo 自己「验证实例」的结果，而且副本是独立模型，
   不会污染库里的基件（基件在会话里始终是原样）。

   值的来源：族表项在 Creo 12 上遍历不出来（ItemVisit 给 0 条、
   ModelitemToFamtableItem 全失败），但 ProFamtableItem 是个可自建的
   结构 {type, 列名, owner}，于是拿尺寸符号拼出列名（$d2）直接问
   ProFaminstanceValueGet。拿到的值按 id 灌回副本（复制不改 id）。
   ------------------------------------------------------------------------ */
#define HSTD_SPEC_MAX 8

/* ★ 结论（2026-10-28 实测 + 二进制核对）：这套库的族表在 Creo 12 上
   「只有实例名，没有可用的列」——
     ProFamtableItemVisit        -> 0 条
     ProModelitemToFamtableItem  -> 每个尺寸都失败
     自建 ProFamtableItem 问值    -> 全部 -2 (BAD_INPUTS)
     ProFaminstanceCreate        -> -34 (OBSOLETE_FUNC，12 已删)
     ProFaminstanceRetrieve      -> -24 (磁盘上没有实例 prt)
   文件里的 info_items 是 27 条同一类型(0x15)的遗留项，Creo 自己弹的
   「在族表中未找到项」就是读不出它们。所以 1615 个规格**拿不到驱动值**，
   任何 API 都变不出对应几何。

   退而求其次：这套库的尺寸被作者改成了中文语义名（螺纹规格 / 螺纹长度 /
   螺丝头外径 …），实例名本身又带规格（M1_6X3、M10X100、D10）。
   于是按名字解析出「螺纹规格 / 长度」，直接改基件副本上的两个主参数再再生，
   其余尺寸靠模型内部关系联动。这是唯一还能自动出图的路径。            */

typedef struct
{
    /* 从实例名解析出来的规格 */
    double       thr, len;
    int          has_thr, has_len;

    /* 在第副本上找到的目标尺寸 */
    ProDimension dthr, dlen, ddia;
    int          has_dthr, has_dlen, has_ddia;

    int          ndim, nset, nfail, nlog;
} HStdSpecCtx;

static int HStdHasW(const wchar_t *s, const wchar_t *sub)
{
    return wcsstr(s, sub) != NULL;
}

/* 副本名字统一加 HSTD_ 前缀：既不和族表实例名撞，又好认。
   同规格再插会命中会话里已有的这份，直接复用。 */
static void HStdCopyName(wchar_t *out, size_t cap, const wchar_t *iname)
{
    swprintf_s(out, cap, L"HSTD_%s", iname);
}

/* M1_6 -> 1.6（下划线当小数点） */
static double HStdNumW(const wchar_t *s, const wchar_t **end)
{
    double v = 0.0, frac = 0.1;
    int    infrac = 0;

    while (*s)
    {
        if (*s >= L'0' && *s <= L'9')
        {
            if (!infrac)
                v = v * 10.0 + (double)(*s - L'0');
            else
            {
                v += (double)(*s - L'0') * frac;
                frac /= 10.0;
            }
            s++;
        }
        else if (*s == L'_' && !infrac)
        {
            infrac = 1;
            s++;
        }
        else
            break;
    }
    *end = s;
    return v;
}

/* 从实例名里挖 M<x>[X<y>] / D<x> */
static void HStdParseSpec(const wchar_t *iname, HStdSpecCtx *c)
{
    const wchar_t *p;

    for (p = iname; *p; p++)
    {
        const wchar_t *q;

        if (p == iname)
            continue;
        if ((*p == L'M' || *p == L'm' || *p == L'D' || *p == L'd')
            && p[1] >= L'0' && p[1] <= L'9')
        {
            c->thr     = HStdNumW(p + 1, &q);
            c->has_thr = 1;
            if ((*q == L'X' || *q == L'x') && q[1] >= L'0' && q[1] <= L'9')
            {
                c->len     = HStdNumW(q + 1, &q);
                c->has_len = 1;
            }
            break;
        }
    }
}

static ProError HStdFindCb(ProDimension *dim, ProError status, ProAppData app)
{
    HStdSpecCtx *c = (HStdSpecCtx *)app;
    ProName      nm;

    (void)status;
    c->ndim++;
    nm[0] = L'\0';
    if (ProDimensionSymbolGet(dim, nm) != PRO_TK_NO_ERROR || nm[0] == L'\0')
        return PRO_TK_NO_ERROR;

    if (!c->has_dthr && (HStdHasW(nm, L"规格") || HStdHasW(nm, L"公称")))
    {
        c->dthr     = *dim;
        c->has_dthr = 1;
        return PRO_TK_NO_ERROR;
    }
    if (!c->has_dlen && HStdHasW(nm, L"长度"))
    {
        c->dlen     = *dim;
        c->has_dlen = 1;
        return PRO_TK_NO_ERROR;
    }
    if (!c->has_ddia && (HStdHasW(nm, L"内径") || HStdHasW(nm, L"直径")))
    {
        c->ddia     = *dim;
        c->has_ddia = 1;
    }
    return PRO_TK_NO_ERROR;
}

/* 再生之后把尺寸值打出来，用来核对关系有没有跟着动 */
static ProError HStdDumpCb(ProDimension *dim, ProError status, ProAppData app)
{
    HStdSpecCtx *c = (HStdSpecCtx *)app;
    ProName      nm;
    double       v;

    (void)status;
    if (c->nlog >= 10)
        return PRO_TK_NO_ERROR;
    if (ProDimensionSymbolGet(dim, nm) != PRO_TK_NO_ERROR || nm[0] == L'\0')
        return PRO_TK_NO_ERROR;
    if (ProDimensionValueGet(dim, &v) != PRO_TK_NO_ERROR)
        return PRO_TK_NO_ERROR;
    c->nlog++;
    HLogW("HStdBuildInst:   dim ", nm);
    HLog("HStdBuildInst:      = %.4f\n", v);
    return PRO_TK_NO_ERROR;
}

static void HStdSetDim(ProDimension *d, double v, HStdSpecCtx *c, const char *tag)
{
    ProError st = ProDimensionValueSet(d, v);

    if (st == PRO_TK_NO_ERROR)
        c->nset++;
    else
    {
        c->nfail++;
        HLog("%s -> %d (v=%.4f)\n", tag, (int)st, v);
    }
}

/* 复制基件 → 把族表这一行的**全部**驱动值灌进去 → 再生。
   这才是等价于 Creo 自己算出来的那个实例；只改两个尺寸会造出
   「M30 的杆 + M1.6 的头」。失败返回 NULL（绝不返回半成品）。 */
static ProMdl HStdBuildInstFull(ProMdl gen, const wchar_t *iname,
                                HStdCache *c, int idx)
{
    ProMdl     cp = NULL;
    ProMdlName newname;
    ProError   st;
    int        k, nset = 0, nfail = 0, nskip = 0;

    HStdCopyName(newname, PRO_MDLNAME_SIZE, iname);
    st = ProMdlnameCopy(gen, newname, &cp);
    HLog("HStdBuildInstFull: copy -> %d mdl=%p cols=%d\n",
         (int)st, (void *)cp, c->ncol);
    if (st != PRO_TK_NO_ERROR || cp == NULL)
        return NULL;

    for (k = 0; k < c->ncol; k++)
    {
        double       v = c->vals[(size_t)idx * HSTD_COL_MAX + k];
        ProModelitem mi;

        if (v == HSTD_NOVAL)
        {
            nskip++;
            continue;
        }
        if (c->coltype[k] != (int)PRO_DIMENSION)
        {
            HLog("HStdBuildInstFull: col %d type=%d 不是尺寸，跳过\n",
                 k, c->coltype[k]);
            nskip++;
            continue;
        }
        if (c->colid[k] == 0 ||
            ProModelitemInit(cp, c->colid[k], PRO_DIMENSION, &mi) != PRO_TK_NO_ERROR)
        {
            HLog("HStdBuildInstFull: col %d id=%d 在副本上找不到\n",
                 k, c->colid[k]);
            nfail++;
            continue;
        }
        if (ProDimensionValueSet((ProDimension *)&mi, v) != PRO_TK_NO_ERROR)
        {
            HLog("HStdBuildInstFull: col %d id=%d = %.4f 设值失败\n",
                 k, c->colid[k], v);
            nfail++;
        }
        else
        {
            nset++;
            HLog("HStdBuildInstFull: set id=%-3d = %.4f\n", c->colid[k], v);
        }
    }

    st = ProSolidRegenerate((ProSolid)cp, PRO_REGEN_NO_FLAGS);
    HLog("HStdBuildInstFull: regen -> %d set=%d fail=%d skip=%d\n",
         (int)st, nset, nfail, nskip);
    if (st != PRO_TK_NO_ERROR || nset == 0)
    {
        ProMdlErase(cp);
        HLog("HStdBuildInstFull: 再生失败或一个值都没灌进，丢弃\n");
        return NULL;
    }
    return cp;
}

/* 解析名字 → 复制基件 → 改主参数 → 再生。失败返回 NULL */
static ProMdl HStdBuildInst(ProMdl gen, const wchar_t *iname)
{
    HStdSpecCtx ctx;
    ProMdl      cp = NULL;
    ProMdlName  newname;
    ProError    st;

    /* ★ 首选：用对话框加载时抄下来的整行族表值 */
    {
        HStdCache *c = HStdCacheFind(g_hstd_curpath);
        int        idx = -1, i;

        if (c != NULL && c->ncol > 0 && c->vals != NULL)
        {
            for (i = 0; i < c->n && i < HSTD_MAX_INST; i++)
                if (_wcsicmp(c->names + (size_t)i * HSTD_NAME, iname) == 0)
                {
                    idx = i;
                    break;
                }
        }
        if (idx >= 0)
        {
            HLog("HStdBuildInst: 用族表整行值 (inst #%d, %d 列)\n", idx, c->ncol);
            return HStdBuildInstFull(gen, iname, c, idx);
        }
        HLog("HStdBuildInst: 没有可用族表值 (cache=%s ncol=%d)，退回按名字\n",
             c ? "有" : "无", c ? c->ncol : -1);
    }

    memset(&ctx, 0, sizeof(ctx));
    HStdParseSpec(iname, &ctx);
    HLog("HStdBuildInst: spec thr=%.4f(%d) len=%.4f(%d)\n",
         ctx.thr, ctx.has_thr, ctx.len, ctx.has_len);
    if (!ctx.has_thr && !ctx.has_len)
        return NULL;

    HStdCopyName(newname, PRO_MDLNAME_SIZE, iname);
    st = ProMdlnameCopy(gen, newname, &cp);
    HLog("HStdBuildInst: copy -> %d mdl=%p\n", (int)st, (void *)cp);
    if (st != PRO_TK_NO_ERROR || cp == NULL)
        return NULL;

    st = ProSolidDimensionVisit((ProSolid)cp, PRO_B_FALSE, HStdFindCb, NULL, &ctx);
    HLog("HStdBuildInst: find -> %d dims=%d thr=%d len=%d dia=%d\n",
         (int)st, ctx.ndim, ctx.has_dthr, ctx.has_dlen, ctx.has_ddia);

    if (ctx.has_thr)
    {
        if (ctx.has_dthr)
            HStdSetDim(&ctx.dthr, ctx.thr, &ctx, "HStdBuildInst: set 规格");
        else if (ctx.has_ddia)
            HStdSetDim(&ctx.ddia, ctx.thr, &ctx, "HStdBuildInst: set 直径");
    }
    if (ctx.has_len && ctx.has_dlen)
        HStdSetDim(&ctx.dlen, ctx.len, &ctx, "HStdBuildInst: set 长度");

    if (ctx.nset == 0)
    {
        HLog("HStdBuildInst: nothing settable, abort\n");
        return NULL;                     /* 别插一个尺寸根本没变的假实例 */
    }

    st = ProSolidRegenerate((ProSolid)cp, PRO_REGEN_NO_FLAGS);
    HLog("HStdBuildInst: regen -> %d set=%d fail=%d\n",
         (int)st, ctx.nset, ctx.nfail);

    if (st != PRO_TK_NO_ERROR)
    {
        /* 再生失败 = 这个副本的几何是坏的（实测表现为草绘截面崩掉），
           绝不能插进装配。卸掉它，让调用方按「没能生成」处理。 */
        HLog("HStdBuildInst: regen failed, drop this copy\n");
        ProMdlErase(cp);
        return NULL;
    }

    ProSolidDimensionVisit((ProSolid)cp, PRO_B_FALSE, HStdDumpCb, NULL, &ctx);
    return cp;
}

/* 拿到「这个规格的模型」：
   ① 本插件自己之前造过的那份副本（会话里同名，名字带 HSTD_ 前缀）
   ② 会话里真有这个实例模型（个别库会自带实例文件）
   ③ 按名字里的规格改基件副本（HStdBuildInst）
   注：不再调 ProMdlnameRetrieve / ProFaminstanceRetrieve —— 它们对这套库
   必然失败，而且每次都在界面弹「在族表中未找到项 / 无法检索」，纯噪音。 */
static ProMdl HStdInstMdl(ProMdl gen, const wchar_t *iname)
{
    ProMdl         inst = NULL;
    ProFamtable    ft;
    ProFaminstance fi;
    ProError       st;
    ProMdlName     cmname;

    HStdCopyName(cmname, PRO_MDLNAME_SIZE, iname);
    st = ProMdlnameInit(cmname, PRO_MDL_PART, &inst);
    if (st == PRO_TK_NO_ERROR && inst != NULL)
    {
        HLog("HStdInstMdl: 1 copy already in session\n");
        return inst;
    }
    inst = NULL;

    /* ★ 不再在这里第二次 ProFamtableInit。
       实测 Creo 12 对同一个模型第二次 Init 会拿到空表（-4），
       之前所有「族表列读不出来」的结论都是在那个坏句柄上测的。
       值和列已经在 HStdLoadInsts 那一次里存进缓存了，这里直接用。 */
    (void)ft;
    (void)fi;
    HLog("HStdInstMdl: 3 build from family table values\n");
    return HStdBuildInst(gen, iname);
}



/* 把选中的规格插进当前装配。返回 0 = 成功 */
static int HStdInsertOne(HStdDlg *d)
{
    ProMdl        asm_mdl = NULL;
    ProMdlType    at      = PRO_MDL_UNUSED;
    ProMdl        gen = NULL, inst_mdl = NULL;
    ProAsmcomp    comp;
    ProMatrix     mtx;
    ProName       iname;
    ProError      st;
    HStdEnt      *p;
    int           i, j;

    if (d->part_sel < 0 || d->part_sel >= d->npart)
        return -1;
    p = &d->part[d->part_sel];

    if (ProMdlCurrentGet(&asm_mdl) != PRO_TK_NO_ERROR || asm_mdl == NULL)
    {
        HMsg("TD_ASSY_ONLY");
        return -1;
    }
    ProMdlTypeGet(asm_mdl, &at);
    if (at != PRO_MDL_ASSEMBLY)
    {
        HMsg("TD_ASSY_ONLY");
        return -1;
    }

    st = HStdLoadMdl(p->path, &gen);
    if (st != PRO_TK_NO_ERROR || gen == NULL)
    {
        HMsg("TD_STD_NOLOAD");
        return -1;
    }

    inst_mdl = gen;                 /* 没有族表时的保底：直接插基件 */
    if (!d->nofam && d->inst_sel >= 0 && d->inst_sel < d->ninst)
    {
        /* 走族表：必须拿到真正对应这个规格的模型。拿不到就报错，
           绝不能退回基件 —— 否则插进去的和选的规格不是一回事。 */
        wcsncpy_s(iname, PRO_NAME_SIZE, d->inst[d->inst_sel], _TRUNCATE);
        HLogW("HStdInsertOne: instName ", iname);
        inst_mdl = HStdInstMdl(gen, iname);
        if (inst_mdl == NULL)
        {
            HLog("HStdInsertOne: no instance model\n");
            HMsg("TD_STD_NOINST");
            return -1;
        }
    }

    for (i = 0; i < 4; i++)
        for (j = 0; j < 4; j++)
            mtx[i][j] = (i == j) ? 1.0 : 0.0;

    st = ProAsmcompAssemble((ProAssembly)asm_mdl, (ProSolid)inst_mdl, mtx, &comp);
    HLog("HStdInsertOne: assemble -> %d\n", (int)st);
    if (st != PRO_TK_NO_ERROR)
    {
        HMsg("TD_STD_FAIL");
        return -1;
    }

    ProWindowRepaint(PRO_VALUE_UNUSED);
    HMsg("TD_STD_OK");
    return 0;
}

/* ---- 缩略图预览 ----------------------------------------------------------
   库目录里每个零件旁边都有一张同名 png（HelloCreo 给的是 198x250 的 RGB），
   直接拿来当预览图。

   为什么用 WIC 而不是 GDI+：gdiplus.h 开头就是 `namespace Gdiplus`，只能 C++
   编译；本项目是 .c、cl 按 C 编，实测 include 直接语法错。WIC 的 wincodec.h
   是 C 兼容的，SDK 自带、不需要额外运行时。

   流程：WIC 解码成 32bpp BGR 缓冲 → 白底 32 位 DIB → StretchDIBits 等比缩放
   居中。任何一步失败就静默放弃（保留空白），绝不因为一张图弹错误。
   -------------------------------------------------------------------------- */
static HBITMAP HWicLoadPng(const wchar_t *path, int dw, int dh)
{
    IWICImagingFactory    *fac = NULL;
    IWICBitmapDecoder     *dec = NULL;
    IWICBitmapFrameDecode *frm = NULL;
    IWICFormatConverter   *con = NULL;
    BITMAPINFO  bmi, sbi;
    BYTE       *buf = NULL;
    HBITMAP     hbm = NULL;
    HDC         dc = NULL, mdc = NULL;
    HGDIOBJ     old = NULL;
    UINT        sw = 0, sh = 0;
    int         x, y, w2, h2, good = 0;
    double      s, s1, s2;

    if (dw < 8 || dh < 8)
        return NULL;

    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);   /* 已初始化过也无所谓 */

    if (CoCreateInstance(&CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER,
                         &IID_IWICImagingFactory, (void **)&fac) != S_OK
        || fac == NULL)
    {
        HLog("HWicLoadPng: WIC factory failed\n");
        return NULL;
    }
    if (fac->lpVtbl->CreateDecoderFromFilename(fac, path, NULL, GENERIC_READ,
            WICDecodeMetadataCacheOnLoad, &dec) != S_OK || dec == NULL)
    {
        HLogW("HWicLoadPng: decode open failed ", path);
        goto done;
    }
    if (dec->lpVtbl->GetFrame(dec, 0, &frm) != S_OK || frm == NULL)
        goto done;
    if (frm->lpVtbl->GetSize(frm, &sw, &sh) != S_OK || sw == 0 || sh == 0)
        goto done;
    if (fac->lpVtbl->CreateFormatConverter(fac, &con) != S_OK || con == NULL)
        goto done;
    if (con->lpVtbl->Initialize(con, (IWICBitmapSource *)frm,
            &GUID_WICPixelFormat32bppBGR, WICBitmapDitherTypeNone, NULL, 0.0,
            WICBitmapPaletteTypeCustom) != S_OK)
        goto done;

    buf = (BYTE *)malloc((size_t)sw * (size_t)sh * 4);
    if (buf == NULL)
        goto done;
    if (con->lpVtbl->CopyPixels(con, NULL, sw * 4,
            (UINT)((size_t)sw * sh * 4), buf) != S_OK)
        goto done;

    memset(&bmi, 0, sizeof(bmi));
    bmi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth       = dw;
    bmi.bmiHeader.biHeight      = -dh;          /* 负数 = 自上而下 */
    bmi.bmiHeader.biPlanes      = 1;
    bmi.bmiHeader.biBitCount    = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    dc  = GetDC(NULL);
    hbm = CreateDIBSection(dc, &bmi, DIB_RGB_COLORS, NULL, NULL, 0);
    if (hbm == NULL)
        goto done;
    mdc = CreateCompatibleDC(dc);
    if (mdc == NULL)
        goto done;
    old = SelectObject(mdc, hbm);
    {
        RECT   rc;
        HBRUSH br = (HBRUSH)GetStockObject(WHITE_BRUSH);

        rc.left = 0; rc.top = 0; rc.right = dw; rc.bottom = dh;
        FillRect(mdc, &rc, br);
    }

    /* 等比缩放，最多放大到 1:1（小图不硬拉，免得糊） */
    s1 = (double)(dw - 8) / (double)sw;
    s2 = (double)(dh - 8) / (double)sh;
    s  = (s1 < s2) ? s1 : s2;
    if (s > 1.0)
        s = 1.0;
    w2 = (int)((double)sw * s);
    h2 = (int)((double)sh * s);
    if (w2 < 1) w2 = 1;
    if (h2 < 1) h2 = 1;
    x = (dw - w2) / 2;
    y = (dh - h2) / 2;

    memset(&sbi, 0, sizeof(sbi));
    sbi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    sbi.bmiHeader.biWidth       = (LONG)sw;
    sbi.bmiHeader.biHeight      = -(LONG)sh;
    sbi.bmiHeader.biPlanes      = 1;
    sbi.bmiHeader.biBitCount    = 32;
    sbi.bmiHeader.biCompression = BI_RGB;

    SetStretchBltMode(mdc, HALFTONE);
    SetBrushOrgEx(mdc, 0, 0, NULL);
    StretchDIBits(mdc, x, y, w2, h2, 0, 0, (int)sw, (int)sh, buf, &sbi,
                  DIB_RGB_COLORS, SRCCOPY);
    good = 1;

done:
    if (mdc != NULL)
    {
        SelectObject(mdc, old);
        DeleteDC(mdc);
    }
    if (dc != NULL)
        ReleaseDC(NULL, dc);
    if (buf != NULL)
        free(buf);
    if (con != NULL) con->lpVtbl->Release(con);
    if (frm != NULL) frm->lpVtbl->Release(frm);
    if (dec != NULL) dec->lpVtbl->Release(dec);
    if (fac != NULL) fac->lpVtbl->Release(fac);
    if (!good && hbm != NULL)
    {
        DeleteObject(hbm);
        hbm = NULL;
    }
    return hbm;
}

/* 换预览：<类别>\<零件>.png，没有就留空 */
static void HStdShowPreview(HStdDlg *d)
{
    wchar_t png[PRO_PATH_SIZE];
    RECT    rc;
    HBITMAP bm;

    if (d == NULL || d->hprev == NULL)
        return;

    if (d->prevbm != NULL)                  /* 先撤掉旧的，再删 */
    {
        SendMessageW(d->hprev, STM_SETIMAGE, IMAGE_BITMAP, 0);
        DeleteObject(d->prevbm);
        d->prevbm = NULL;
    }
    if (d->hnote != NULL)
        SetWindowTextW(d->hnote, L"");

    if (d->part_sel < 0 || d->part_sel >= d->npart)
        return;

    swprintf_s(png, PRO_PATH_SIZE, L"%ls.png", d->part[d->part_sel].path);
    if (!HFileExistsW(png))
    {
        HLogW("HStdShowPreview: no png ", png);
        if (d->hnote != NULL)
            SetWindowTextW(d->hnote, L"此零件无预览图");
        return;
    }

    GetClientRect(d->hprev, &rc);
    bm = HWicLoadPng(png, rc.right, rc.bottom);
    if (bm == NULL)
    {
        if (d->hnote != NULL)
            SetWindowTextW(d->hnote, L"预览图加载失败");
        return;
    }
    d->prevbm = bm;
    SendMessageW(d->hprev, STM_SETIMAGE, IMAGE_BITMAP, (LPARAM)bm);
}

static void HStdFillCats(HStdDlg *d)
{
    int i;

    SendMessageW(d->hcat, LB_RESETCONTENT, 0, 0);
    for (i = 0; i < d->ncat; i++)
        SendMessageW(d->hcat, LB_ADDSTRING, 0, (LPARAM)d->cat[i].name);
    if (d->cat_sel >= 0 && d->cat_sel < d->ncat)
        SendMessageW(d->hcat, LB_SETCURSEL, (WPARAM)d->cat_sel, 0);
}

static void HStdFillParts(HStdDlg *d)
{
    int i;

    SendMessageW(d->hpart, LB_RESETCONTENT, 0, 0);
    for (i = 0; i < d->npart; i++)
        SendMessageW(d->hpart, LB_ADDSTRING, 0, (LPARAM)d->part[i].name);
    if (d->part_sel >= 0 && d->part_sel < d->npart)
        SendMessageW(d->hpart, LB_SETCURSEL, (WPARAM)d->part_sel, 0);
}

static void HStdFillInsts(HStdDlg *d)
{
    int i;

    SendMessageW(d->hinst, LB_RESETCONTENT, 0, 0);
    for (i = 0; i < d->ninst; i++)
        SendMessageW(d->hinst, LB_ADDSTRING, 0, (LPARAM)d->inst[i]);
    if (d->inst_sel >= 0 && d->inst_sel < d->ninst)
        SendMessageW(d->hinst, LB_SETCURSEL, (WPARAM)d->inst_sel, 0);
}

static void HStdInfoUpdate(HStdDlg *d)
{
    wchar_t buf[512];

    buf[0] = L'\0';
    if (d->part_sel >= 0 && d->part_sel < d->npart)
    {
        if (d->ftbad)
            swprintf_s(buf, 512, L"%ls ｜ 族表读取失败（不是没有族表）｜ %ls",
                       d->part[d->part_sel].name, d->verinfo);
        else if (d->nofam)
            swprintf_s(buf, 512, L"%ls ｜ 无族表，插入基件",
                       d->part[d->part_sel].name);
        else
            swprintf_s(buf, 512, L"%ls ｜ %d 个规格%ls",
                       d->part[d->part_sel].name, d->ninst,
                       d->inserted ? L" ｜ 已插入，可继续选" : L"");
    }
    else
        wcscpy_s(buf, 512, L"选一个零件");

    SetWindowTextW(d->hinfo, buf);
}

static LRESULT CALLBACK HStdDlgProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    HStdDlg *d = (HStdDlg *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);

    switch (msg)
    {
    case WM_CREATE:
    {
        CREATESTRUCTW *cs = (CREATESTRUCTW *)lp;
        int pad, cw, ch, bh, lw, mw, rw, y, lh;

        d = (HStdDlg *)cs->lpCreateParams;
        if (d == NULL)
            return -1;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)d);
        d->hwnd = hwnd;

        d->font = CreateFontW(-HSD_S(d, 16), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                              DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                              CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                              DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
        if (d->font == NULL)
            d->font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);

        pad = HSD_S(d, 12);
        bh  = HSD_S(d, 30);
        lh  = HSD_S(d, 20);
        lw  = HSD_S(d, 190);          /* 类别 */
        mw  = HSD_S(d, 250);          /* 零件 */
        rw  = HSD_S(d, 300);          /* 规格 */
        {
            RECT rc;
            GetClientRect(hwnd, &rc);
            cw = rc.right;
            ch = rc.bottom;
        }

        d->hl1 = HEdCtl(hwnd, L"STATIC", L"类别", WS_CHILD | WS_VISIBLE,
                        pad, pad, lw, lh, HSD_L1, d->font);
        d->hl2 = HEdCtl(hwnd, L"STATIC", L"零件", WS_CHILD | WS_VISIBLE,
                        pad + lw + pad, pad, mw, lh, HSD_L2, d->font);
        d->hl3 = HEdCtl(hwnd, L"STATIC", L"规格", WS_CHILD | WS_VISIBLE,
                        pad + lw + mw + pad * 2, pad, rw, lh, HSD_L3, d->font);

        y = pad + lh + HSD_S(d, 4);
        d->hcat = HEdCtl(hwnd, L"LISTBOX", L"",
                         WS_CHILD | WS_VISIBLE | WS_BORDER | WS_VSCROLL |
                         LBS_NOTIFY | LBS_NOINTEGRALHEIGHT,
                         pad, y, lw, ch - y - HSD_S(d, 96), HSD_CAT, d->font);
        d->hpart = HEdCtl(hwnd, L"LISTBOX", L"",
                          WS_CHILD | WS_VISIBLE | WS_BORDER | WS_VSCROLL |
                          LBS_NOTIFY | LBS_NOINTEGRALHEIGHT,
                          pad + lw + pad, y, mw, ch - y - HSD_S(d, 96), HSD_PART, d->font);
        d->hinst = HEdCtl(hwnd, L"LISTBOX", L"",
                          WS_CHILD | WS_VISIBLE | WS_BORDER | WS_VSCROLL |
                          LBS_NOTIFY | LBS_NOINTEGRALHEIGHT,
                          pad + lw + mw + pad * 2, y, rw,
                          ch - y - HSD_S(d, 96), HSD_INST, d->font);
        d->hprev = HEdCtl(hwnd, L"STATIC", L"",
                          WS_CHILD | WS_VISIBLE | WS_BORDER | SS_BITMAP |
                          SS_CENTERIMAGE,
                          pad + lw + mw + rw + pad * 3, y,
                          cw - (pad + lw + mw + rw + pad * 3) - pad,
                          ch - y - HSD_S(d, 122), HSD_PREV, d->font);
        /* 预览框正下方一行小字：库里那几个没有同名 png 的零件不至于看起来像"没加载" */
        d->hnote = HEdCtl(hwnd, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_CENTER,
                          pad + lw + mw + rw + pad * 3,
                          ch - HSD_S(d, 120),
                          cw - (pad + lw + mw + rw + pad * 3) - pad,
                          lh, HSD_PNOTE, d->font);

        d->hinfo = HEdCtl(hwnd, L"STATIC", L"", WS_CHILD | WS_VISIBLE,
                          pad, ch - HSD_S(d, 88), cw - pad * 2, lh,
                          HSD_INFO, d->font);
        d->htip = HEdCtl(hwnd, L"STATIC",
                         L"双击规格直接插入；插入后模型以未约束状态出现在原点，"
                         L"再用「元件放置」约束即可。",
                         WS_CHILD | WS_VISIBLE,
                         pad, ch - HSD_S(d, 64), cw - pad * 2, lh,
                         HSD_TIP, d->font);

        d->hok = HEdCtl(hwnd, L"BUTTON", L"插入",
                        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
                        cw - pad - HSD_S(d, 180), ch - HSD_S(d, 40),
                        HSD_S(d, 84), bh, HSD_OK, d->font);
        d->hcan = HEdCtl(hwnd, L"BUTTON", L"关闭",
                         WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                         cw - pad - HSD_S(d, 88), ch - HSD_S(d, 40),
                         HSD_S(d, 84), bh, HSD_CAN, d->font);

        HStdFillCats(d);
        HStdFillParts(d);
        if (d->part_sel >= 0)
            HStdLoadInsts(d);
        HStdFillInsts(d);
        HStdShowPreview(d);
        HStdInfoUpdate(d);
        return 0;
    }

    case WM_COMMAND:
    {
        int id   = LOWORD(wp);
        int code = HIWORD(wp);

        if (d == NULL)
            break;

        if (id == HSD_CAT && code == LBN_SELCHANGE)
        {
            int sel = (int)SendMessageW(d->hcat, LB_GETCURSEL, 0, 0);

            if (sel >= 0 && sel < d->ncat)
            {
                d->cat_sel  = sel;
                d->inserted = 0;
                HStdScanParts(d);
                HStdFillParts(d);
                if (d->part_sel >= 0)
                    HStdLoadInsts(d);
                HStdFillInsts(d);
                HStdShowPreview(d);
                HStdInfoUpdate(d);
            }
            return 0;
        }
        if (id == HSD_PART && (code == LBN_SELCHANGE || code == LBN_DBLCLK))
        {
            int sel = (int)SendMessageW(d->hpart, LB_GETCURSEL, 0, 0);

            if (sel >= 0 && sel < d->npart)
            {
                d->part_sel = sel;
                d->inserted = 0;
                HStdLoadInsts(d);
                HStdFillInsts(d);
                HStdShowPreview(d);
                HStdInfoUpdate(d);
            }
            return 0;
        }
        if (id == HSD_INST && code == LBN_SELCHANGE)
        {
            int sel = (int)SendMessageW(d->hinst, LB_GETCURSEL, 0, 0);

            if (sel >= 0 && sel < d->ninst)
            {
                d->inst_sel = sel;
                HStdInfoUpdate(d);
            }
            return 0;
        }
        if (id == HSD_INST && code == LBN_DBLCLK)
        {
            if (d->part_sel >= 0 && d->part_sel < d->npart)
            {
                d->req_insert = 1;      /* 关窗后执行，见 HStdLibAct 里的说明 */
                d->done       = 1;
            }
            return 0;
        }
        if (id == HSD_OK)
        {
            if (d->part_sel >= 0 && d->part_sel < d->npart)
            {
                d->req_insert = 1;
                d->done       = 1;
            }
            return 0;
        }
        if (id == HSD_CAN)
        {
            d->done = 1;
            return 0;
        }
        break;
    }

    case WM_CLOSE:
        if (d != NULL)
            d->done = 1;
        return 0;                 /* 自己收尾，不走 DefWindowProc 销毁 */

    case WM_DESTROY:
        return 0;                 /* 千万别 PostQuitMessage，会杀掉 Creo 的消息循环 */
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static int HStdDlgShow(HStdDlg *d)
{
    static int  registered = 0;
    HINSTANCE   hi;
    HWND        owner;
    WNDCLASSEXW wc;
    RECT        rc;
    MSG         m;
    int         w, h, x, y;

    hi = GetModuleHandleW(NULL);

    if (!registered)
    {
        memset(&wc, 0, sizeof(wc));
        wc.cbSize        = sizeof(wc);
        wc.style         = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc   = HStdDlgProc;
        wc.hInstance     = hi;
        wc.hCursor       = LoadCursorW(NULL, MAKEINTRESOURCEW(32512));
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.lpszClassName = L"H_StdLibDlg";
        if (RegisterClassExW(&wc) == 0
            && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        {
            HLog("HStdDlgShow: RegisterClass failed %lu\n",
                 (unsigned long)GetLastError());
            return 0;
        }
        registered = 1;
    }

    {
        HDC hdc = GetDC(NULL);
        int dpi = (hdc != NULL) ? GetDeviceCaps(hdc, LOGPIXELSY) : 96;

        if (hdc != NULL)
            ReleaseDC(NULL, hdc);
        if (dpi <= 0)
            dpi = 96;
        d->sc = MulDiv(dpi, 100, 96);
        if (d->sc < 100)
            d->sc = 100;
        if (d->sc > 300)
            d->sc = 300;
    }

    rc.left   = 0;
    rc.top    = 0;
    rc.right  = HSD_S(d, 1010);        /* 三列 + 预览列 */
    rc.bottom = HSD_S(d, 560);
    AdjustWindowRectEx(&rc, WS_POPUP | WS_CAPTION | WS_SYSMENU, FALSE,
                       WS_EX_DLGMODALFRAME);
    w = rc.right - rc.left;
    h = rc.bottom - rc.top;

    owner = GetActiveWindow();
    if (owner != NULL && GetWindowRect(owner, &rc) && (rc.right - rc.left) > 200)
    {
        x = rc.left + ((rc.right - rc.left) - w) / 2;
        y = rc.top + ((rc.bottom - rc.top) - h) / 2;
    }
    else
    {
        x = (GetSystemMetrics(SM_CXSCREEN) - w) / 2;
        y = (GetSystemMetrics(SM_CYSCREEN) - h) / 2;
    }

    d->hwnd = CreateWindowExW(WS_EX_DLGMODALFRAME, L"H_StdLibDlg",
                              L"标准件库 — 选规格 / 插入装配",
                              WS_POPUP | WS_CAPTION | WS_SYSMENU,
                              x, y, w, h, owner, NULL, hi, d);
    if (d->hwnd == NULL)
    {
        HLog("HStdDlgShow: CreateWindow failed %lu\n",
             (unsigned long)GetLastError());
        return 0;
    }

    if (owner != NULL)
        EnableWindow(owner, FALSE);
    ShowWindow(d->hwnd, SW_SHOW);
    UpdateWindow(d->hwnd);
    SetForegroundWindow(d->hwnd);

    while (d->done == 0 && GetMessageW(&m, NULL, 0, 0) > 0)
    {
        if (!IsDialogMessageW(d->hwnd, &m))
        {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
    }

    if (owner != NULL)
    {
        EnableWindow(owner, TRUE);
        SetActiveWindow(owner);
    }
    if (IsWindow(d->hwnd))
        DestroyWindow(d->hwnd);
    d->hwnd = NULL;
    if (d->prevbm != NULL)              /* 位图挂在 STATIC 上，窗口已销毁，这里删掉 */
    {
        DeleteObject(d->prevbm);
        d->prevbm = NULL;
    }
    d->hprev = NULL;
    d->hnote = NULL;
    if (d->font != NULL && d->font != (HFONT)GetStockObject(DEFAULT_GUI_FONT))
        DeleteObject(d->font);
    d->font = NULL;

    HLog("HStdDlgShow: ok=%d inserted=%d\n", d->ok, d->inserted);
    return d->ok;
}

static uiCmdAccessState HStdAccess(uiCmdAccessMode access_mode)
{
    return ACCESS_AVAILABLE;
}

static int HStdLibAct(uiCmdCmdId command, uiCmdValue *p_value,
                      void *p_push_command_data)
{
    ProMdl     mdl = NULL;
    ProMdlType t   = PRO_MDL_UNUSED;
    HStdDlg  *d    = &g_hstd;

    (void)command;
    (void)p_value;
    (void)p_push_command_data;

    if (ProMdlCurrentGet(&mdl) != PRO_TK_NO_ERROR || mdl == NULL)
    {
        HMsg("TD_ASSY_ONLY");
        return 0;
    }
    ProMdlTypeGet(mdl, &t);
    if (t != PRO_MDL_ASSEMBLY)
    {
        HMsg("TD_ASSY_ONLY");
        return 0;
    }

    memset(d, 0, sizeof(*d));
    d->cat_sel  = -1;
    d->part_sel = -1;
    d->inst_sel = -1;

    if (!HStdRootW(d->root, PRO_PATH_SIZE))
    {
        HMsg("TD_STD_NODIR");
        return 0;
    }
    if (HStdScanCats(d) == 0)
    {
        HMsg("TD_STD_EMPTY");
        return 0;
    }
    d->cat_sel = 0;
    HStdScanParts(d);
    if (d->part_sel >= 0)
        HStdLoadInsts(d);

    HStdDlgShow(d);

    /* 插入动作放在窗口关掉之后做。
       为什么：在自建的模态 Win32 消息循环里调 Pro/TOOLKIT，接口内部一旦要
       弹提示（比如族表实例未验证时的确认框）就拿不到消息，实测
       ProFaminstanceCheck 直接回 -25 (PRO_TK_ABORT)。挪到 Creo 自己的
       上下文里执行，这条路才通。 */
    if (d->req_insert)
    {
        d->req_insert = 0;
        HStdInsertOne(d);
    }
    return 0;
}

/* 原生命令自检：rbn 里这些按钮的 mName 直接写的是 Creo 系统命令名，
   名字必须真实存在，否则按钮不显示。启动时用 ProCmdCmdIdFind 逐个确认，
   结果写 h_load.log。2026-10-23 起除绘图注释外，还含「基准」组的
   修剪 / 投影 / 扫描 三个实体命令。 */
static const char *g_native_cmds[] =
{
    "ProCmdDwgShowAllAnnots",
    "ProCmdDwgCrStdNewRefDim",
    "ProCmdDwgCrGtolSpecTol",
    "ProCmdDwgCrOrdDim",
    "ProCmdDtlInsFreeNote",
    "ProCmdDtlInsOffsetNote",
    "ProCmdDtlInsOnItemNote",
    "ProCmdDtlInsLeaderNote",
    "ProCmdDwgCreateSurfFin",
    "ProCmdDwgCrGtolSetDtm",
    "ProCmdDwgCrGtolMakeTarg",
    "ProCmdDwgCreateDrftDatum",
    "ProCmdDwgCreateDraftAxis",

    /* 2026-10-23 新增：「基准」组里的三个原生按钮。
       名字不是猜的，是从本机 Creo 的 trail 里反查出来的 ——
       trail 记录形如  ~ Command `ProCmdFtXxx`
       紧跟着一行  Feature title : 扫描 / 投影 / 修剪
         ProCmdFtTrim    -> 修剪
         ProCmdFtProject -> 投影
         ProCmdFtLoft    -> 扫描
           注意：Creo 4.0 起「扫描」和「混合」合并成一个工具，
           内部命令名仍沿用 Loft，不是 Sweep。写 ProCmdFtSweep 会找不到。
       ProCmdSurfSweep 是老版本「曲面扫描」的名字，留一个当备用核对。 */
    "ProCmdFtTrim",
    "ProCmdFtProject",
    "ProCmdFtLoft",
    "ProCmdSurfSweep",
};

static void HProbeNativeCmds(void)
{
    int       i;
    int       n = (int)(sizeof(g_native_cmds) / sizeof(g_native_cmds[0]));
    uiCmdCmdId id;

    for (i = 0; i < n; i++)
    {
        id = 0;
        ProCmdCmdIdFind((char *)g_native_cmds[i], &id);
        HLog("native cmd %-28s -> %p %s\n",
             g_native_cmds[i], (void *)id, id ? "[FOUND]" : "[MISSING]");
    }
}

int user_initialize(
    int    argc,
    char  *argv[],
    char  *proe_vsn,
    char  *build,
    wchar_t errbuf[80])
{
    ProError    status;
    uiCmdCmdId  cmd_add, cmd_insert, cmd_renum, cmd_delete, cmd_export, cmd_dynaview;
    uiCmdCmdId  cmd_tolbrush;
    uiCmdCmdId  cmd_techreq;
    uiCmdCmdId  cmd_keydim;
    uiCmdCmdId  cmd_keydimc;
    uiCmdCmdId  cmd_openworkdir, cmd_setworkdir, cmd_openmdldir, cmd_mdltodir;
    uiCmdCmdId  cmd_purgeold, cmd_eraseidle, cmd_cleanjunk;
    uiCmdCmdId  cmd_outline, cmd_copyname, cmd_hide, cmd_unhide, cmd_unhideall;
    uiCmdCmdId  cmd_togglehide, cmd_hideall;
    uiCmdCmdId  cmd_rename3d, cmd_colorasm, cmd_clearcolor;
    uiCmdCmdId  cmd_openinplace, cmd_batchbackup;
    uiCmdCmdId  cmd_openviewinplace, cmd_open3d;
    uiCmdCmdId  cmd_viewnormal, cmd_eqcrv;
    uiCmdCmdId  cmd_stdlib;
    ProFileName msgfile;
    int         i;
    char        helpkey[PRO_LINE_SIZE];

    g_symdef_valid = PRO_B_FALSE;

    HInitPluginDir();
    HLog("---- user_initialize: H plugin ----\n");
    HProbeNativeCmds();
    HLog("plugin_dir=%s\n", g_plugin_dir);
    HLog("symbols_dir=%s\n", g_symbols_dir);
    HLog("creo version=%s build=%s argc=%d\n",
         proe_vsn ? proe_vsn : "?", build ? build : "?", argc);

    ProUITranslationFilesEnable();

    /* 消息文件名转宽字符串（ProFileName = wchar_t[]），两个 API 共用 */
    ProStringToWstring(msgfile, MSGFILE_NAME);
    ProStringToWstring(g_msg_file, MSGFILE_NAME);

    /* 1. 注册动作（action）
       优先级用 uiProe2ndImmediate，不要用 uiCmdPrioDefault ——
       后者会“把其它所有动作都 dismiss 掉”，让依赖当前会话上下文的
       ProSelect 等函数出现不可预期行为（状态栏过滤器被留在空白）。
       官方示例注册 ribbon 动作都用 uiProe2ndImmediate。 */
    status = ProCmdActionAdd(CMD_ADD, (uiCmdCmdActFn)HAddAct,
        uiProe2ndImmediate, HAddAccess, PRO_B_TRUE, PRO_B_FALSE, &cmd_add);
    HLog("ProCmdActionAdd(Add)      -> %d\n", (int)status);
    if (status != PRO_TK_NO_ERROR)
    {
        ProStringToWstring(errbuf, "H: fail to add command");
        return -1;
    }
    status = ProCmdActionAdd(CMD_INSERT, (uiCmdCmdActFn)HInsertAct,
        uiProe2ndImmediate, HAddAccess, PRO_B_TRUE, PRO_B_FALSE, &cmd_insert);
    HLog("ProCmdActionAdd(Insert)   -> %d\n", (int)status);
    if (status != PRO_TK_NO_ERROR)
    {
        ProStringToWstring(errbuf, "H: fail to add command");
        return -1;
    }
    status = ProCmdActionAdd(CMD_RENUM, (uiCmdCmdActFn)HRenumberAct,
        uiProe2ndImmediate, HAddAccess, PRO_B_TRUE, PRO_B_FALSE, &cmd_renum);
    HLog("ProCmdActionAdd(Renumber) -> %d\n", (int)status);
    if (status != PRO_TK_NO_ERROR)
    {
        ProStringToWstring(errbuf, "H: fail to add command");
        return -1;
    }
    status = ProCmdActionAdd(CMD_DELETE, (uiCmdCmdActFn)HDeleteAct,
        uiProe2ndImmediate, HAddAccess, PRO_B_TRUE, PRO_B_FALSE, &cmd_delete);
    HLog("ProCmdActionAdd(Delete)   -> %d\n", (int)status);
    if (status != PRO_TK_NO_ERROR)
    {
        ProStringToWstring(errbuf, "H: fail to add command");
        return -1;
    }
    status = ProCmdActionAdd(CMD_EXPORT, (uiCmdCmdActFn)HExportAct,
        uiProe2ndImmediate, HExportAccess, PRO_B_TRUE, PRO_B_FALSE, &cmd_export);
    HLog("ProCmdActionAdd(Export)   -> %d\n", (int)status);
    if (status != PRO_TK_NO_ERROR)
    {
        ProStringToWstring(errbuf, "H: fail to add command");
        return -1;
    }
    status = ProCmdActionAdd(CMD_DYNAVIEW, (uiCmdCmdActFn)HDynaViewAct,
        uiProe2ndImmediate, HDynaViewAccess, PRO_B_TRUE, PRO_B_FALSE, &cmd_dynaview);
    HLog("ProCmdActionAdd(DynaView) -> %d\n", (int)status);
    if (status != PRO_TK_NO_ERROR)
    {
        ProStringToWstring(errbuf, "H: fail to add command");
        return -1;
    }
    status = ProCmdActionAdd(CMD_TOLBRUSH, (uiCmdCmdActFn)HTolBrushAct,
        uiProe2ndImmediate, HTolBrushAccess, PRO_B_TRUE, PRO_B_FALSE, &cmd_tolbrush);
    HLog("ProCmdActionAdd(TolBrush) -> %d\n", (int)status);
    if (status != PRO_TK_NO_ERROR)
    {
        ProStringToWstring(errbuf, "H: fail to add command");
        return -1;
    }
    status = ProCmdActionAdd(CMD_TECHREQ, (uiCmdCmdActFn)HTechReqAct,
        uiProe2ndImmediate, HExportAccess, PRO_B_TRUE, PRO_B_FALSE, &cmd_techreq);
    HLog("ProCmdActionAdd(TechReq)  -> %d\n", (int)status);
    if (status != PRO_TK_NO_ERROR)
    {
        ProStringToWstring(errbuf, "H: fail to add command");
        return -1;
    }
    status = ProCmdActionAdd(CMD_KEYDIM, (uiCmdCmdActFn)HKeyDimAct,
        uiProe2ndImmediate, HExportAccess, PRO_B_TRUE, PRO_B_FALSE, &cmd_keydim);
    HLog("ProCmdActionAdd(KeyDim)   -> %d\n", (int)status);
    if (status != PRO_TK_NO_ERROR)
    {
        ProStringToWstring(errbuf, "H: fail to add command");
        return -1;
    }
    status = ProCmdActionAdd(CMD_KEYDIMC, (uiCmdCmdActFn)HKeyDimClearAct,
        uiProe2ndImmediate, HExportAccess, PRO_B_TRUE, PRO_B_FALSE, &cmd_keydimc);
    HLog("ProCmdActionAdd(KeyDimC)  -> %d\n", (int)status);
    if (status != PRO_TK_NO_ERROR)
    {
        ProStringToWstring(errbuf, "H: fail to add command");
        return -1;
    }
    status = ProCmdActionAdd(CMD_OUTLINE, (uiCmdCmdActFn)HOutlineAct,
        uiProe2ndImmediate, HExportAccess, PRO_B_TRUE, PRO_B_FALSE, &cmd_outline);
    HLog("ProCmdActionAdd(Outline    ) -> %d\n", (int)status);
    if (status != PRO_TK_NO_ERROR)
    {
        ProStringToWstring(errbuf, "H: fail to add command");
        return -1;
    }
    status = ProCmdActionAdd(CMD_COPYNAME, (uiCmdCmdActFn)HCopyNameAct,
        uiProe2ndImmediate, HExportAccess, PRO_B_TRUE, PRO_B_FALSE, &cmd_copyname);
    HLog("ProCmdActionAdd(CopyName   ) -> %d\n", (int)status);
    if (status != PRO_TK_NO_ERROR)
    {
        ProStringToWstring(errbuf, "H: fail to add command");
        return -1;
    }
    status = ProCmdActionAdd(CMD_HIDE, (uiCmdCmdActFn)HHideAct,
        uiProe2ndImmediate, HExportAccess, PRO_B_TRUE, PRO_B_FALSE, &cmd_hide);
    HLog("ProCmdActionAdd(Hide       ) -> %d\n", (int)status);
    if (status != PRO_TK_NO_ERROR)
    {
        ProStringToWstring(errbuf, "H: fail to add command");
        return -1;
    }
    status = ProCmdActionAdd(CMD_UNHIDE, (uiCmdCmdActFn)HUnhideAct,
        uiProe2ndImmediate, HExportAccess, PRO_B_TRUE, PRO_B_FALSE, &cmd_unhide);
    HLog("ProCmdActionAdd(Unhide     ) -> %d\n", (int)status);
    if (status != PRO_TK_NO_ERROR)
    {
        ProStringToWstring(errbuf, "H: fail to add command");
        return -1;
    }
    status = ProCmdActionAdd(CMD_UNHIDEALL, (uiCmdCmdActFn)HUnhideAllAct,
        uiProe2ndImmediate, HExportAccess, PRO_B_TRUE, PRO_B_FALSE, &cmd_unhideall);
    HLog("ProCmdActionAdd(UnhideAll  ) -> %d\n", (int)status);
    if (status != PRO_TK_NO_ERROR)
    {
        ProStringToWstring(errbuf, "H: fail to add command");
        return -1;
    }
    status = ProCmdActionAdd(CMD_TOGGLEHIDE, (uiCmdCmdActFn)HToggleHideAct,
        uiProe2ndImmediate, HExportAccess, PRO_B_TRUE, PRO_B_FALSE, &cmd_togglehide);
    HLog("ProCmdActionAdd(ToggleHide ) -> %d\n", (int)status);
    if (status != PRO_TK_NO_ERROR)
    {
        ProStringToWstring(errbuf, "H: fail to add command");
        return -1;
    }
    status = ProCmdActionAdd(CMD_HIDEALL, (uiCmdCmdActFn)HHideAllAct,
        uiProe2ndImmediate, HExportAccess, PRO_B_TRUE, PRO_B_FALSE, &cmd_hideall);
    HLog("ProCmdActionAdd(HideAll    ) -> %d\n", (int)status);
    if (status != PRO_TK_NO_ERROR)
    {
        ProStringToWstring(errbuf, "H: fail to add command");
        return -1;
    }
    status = ProCmdActionAdd(CMD_RENAME3D, (uiCmdCmdActFn)HRename3DAct,
        uiProe2ndImmediate, HExportAccess, PRO_B_TRUE, PRO_B_FALSE, &cmd_rename3d);
    HLog("ProCmdActionAdd(Rename3D   ) -> %d\n", (int)status);
    if (status != PRO_TK_NO_ERROR)
    {
        ProStringToWstring(errbuf, "H: fail to add command");
        return -1;
    }
    status = ProCmdActionAdd(CMD_COLORASM, (uiCmdCmdActFn)HColorRandomAct,
        uiProe2ndImmediate, HExportAccess, PRO_B_TRUE, PRO_B_FALSE, &cmd_colorasm);
    HLog("ProCmdActionAdd(ColorAsm   ) -> %d\n", (int)status);
    if (status != PRO_TK_NO_ERROR)
    {
        ProStringToWstring(errbuf, "H: fail to add command");
        return -1;
    }
    status = ProCmdActionAdd(CMD_CLEARCOLOR, (uiCmdCmdActFn)HClearColorAct,
        uiProe2ndImmediate, HExportAccess, PRO_B_TRUE, PRO_B_FALSE, &cmd_clearcolor);
    HLog("ProCmdActionAdd(ClearColor ) -> %d\n", (int)status);
    if (status != PRO_TK_NO_ERROR)
    {
        ProStringToWstring(errbuf, "H: fail to add command");
        return -1;
    }
    status = ProCmdActionAdd(CMD_OPENINPLACE, (uiCmdCmdActFn)HOpenInPlaceAct,
        uiProe2ndImmediate, HExportAccess, PRO_B_TRUE, PRO_B_FALSE, &cmd_openinplace);
    HLog("ProCmdActionAdd(OpenInPlace) -> %d\n", (int)status);
    if (status != PRO_TK_NO_ERROR)
    {
        ProStringToWstring(errbuf, "H: fail to add command");
        return -1;
    }
    status = ProCmdActionAdd(CMD_OPENVIEWINPLACE, (uiCmdCmdActFn)HOpenViewInPlaceAct,
        uiProe2ndImmediate, HOpenViewAccess, PRO_B_TRUE, PRO_B_FALSE, &cmd_openviewinplace);
    HLog("ProCmdActionAdd(OpenViewInPlace) -> %d\n", (int)status);
    if (status != PRO_TK_NO_ERROR)
    {
        ProStringToWstring(errbuf, "H: fail to add command");
        return -1;
    }
    status = ProCmdActionAdd(CMD_OPEN3D, (uiCmdCmdActFn)HOpen3DAct,
        uiProe2ndImmediate, HOpenViewAccess, PRO_B_TRUE, PRO_B_FALSE, &cmd_open3d);
    HLog("ProCmdActionAdd(Open3D    ) -> %d\n", (int)status);
    if (status != PRO_TK_NO_ERROR)
    {
        ProStringToWstring(errbuf, "H: fail to add command");
        return -1;
    }
    status = ProCmdActionAdd(CMD_VIEWNORMAL, (uiCmdCmdActFn)HViewNormalAct,
        uiProe2ndImmediate, HExportAccess, PRO_B_TRUE, PRO_B_FALSE, &cmd_viewnormal);
    HLog("ProCmdActionAdd(ViewNormal ) -> %d\n", (int)status);
    if (status != PRO_TK_NO_ERROR)
    {
        ProStringToWstring(errbuf, "H: fail to add command");
        return -1;
    }
    status = ProCmdActionAdd(CMD_BATCHBACKUP, (uiCmdCmdActFn)HBatchBackupAct,
        uiProe2ndImmediate, HExportAccess, PRO_B_TRUE, PRO_B_FALSE, &cmd_batchbackup);
    HLog("ProCmdActionAdd(BatchBackup) -> %d\n", (int)status);
    if (status != PRO_TK_NO_ERROR)
    {
        ProStringToWstring(errbuf, "H: fail to add command");
        return -1;
    }
    status = ProCmdActionAdd(CMD_EQCRV, (uiCmdCmdActFn)HEqCurveAct,
        uiProe2ndImmediate, HExportAccess, PRO_B_TRUE, PRO_B_FALSE, &cmd_eqcrv);
    HLog("ProCmdActionAdd(EqCurve    ) -> %d\n", (int)status);
    if (status != PRO_TK_NO_ERROR)
    {
        ProStringToWstring(errbuf, "H: fail to add command");
        return -1;
    }
    status = ProCmdActionAdd(CMD_STDLIB, (uiCmdCmdActFn)HStdLibAct,
        uiProe2ndImmediate, HStdAccess, PRO_B_TRUE, PRO_B_FALSE, &cmd_stdlib);
    HLog("ProCmdActionAdd(StdLib    ) -> %d\n", (int)status);
    if (status != PRO_TK_NO_ERROR)
    {
        ProStringToWstring(errbuf, "H: fail to add command");
        return -1;
    }
    status = ProCmdActionAdd(CMD_OPENWORKDIR, (uiCmdCmdActFn)HOpenWorkDirAct,
        uiProe2ndImmediate, HExportAccess, PRO_B_TRUE, PRO_B_FALSE, &cmd_openworkdir);
    HLog("ProCmdActionAdd(OpenWorkDir) -> %d\n", (int)status);
    if (status != PRO_TK_NO_ERROR)
    {
        ProStringToWstring(errbuf, "H: fail to add command");
        return -1;
    }
    status = ProCmdActionAdd(CMD_SETWORKDIR, (uiCmdCmdActFn)HSetWorkDirAct,
        uiProe2ndImmediate, HExportAccess, PRO_B_TRUE, PRO_B_FALSE, &cmd_setworkdir);
    HLog("ProCmdActionAdd(SetWorkDir ) -> %d\n", (int)status);
    if (status != PRO_TK_NO_ERROR)
    {
        ProStringToWstring(errbuf, "H: fail to add command");
        return -1;
    }
    status = ProCmdActionAdd(CMD_OPENMDLDIR, (uiCmdCmdActFn)HOpenMdlDirAct,
        uiProe2ndImmediate, HExportAccess, PRO_B_TRUE, PRO_B_FALSE, &cmd_openmdldir);
    HLog("ProCmdActionAdd(OpenMdlDir ) -> %d\n", (int)status);
    if (status != PRO_TK_NO_ERROR)
    {
        ProStringToWstring(errbuf, "H: fail to add command");
        return -1;
    }
    status = ProCmdActionAdd(CMD_MDLTODIR, (uiCmdCmdActFn)HMdlToDirAct,
        uiProe2ndImmediate, HExportAccess, PRO_B_TRUE, PRO_B_FALSE, &cmd_mdltodir);
    HLog("ProCmdActionAdd(MdlToDir   ) -> %d\n", (int)status);
    if (status != PRO_TK_NO_ERROR)
    {
        ProStringToWstring(errbuf, "H: fail to add command");
        return -1;
    }
    status = ProCmdActionAdd(CMD_PURGEOLD, (uiCmdCmdActFn)HPurgeOldAct,
        uiProe2ndImmediate, HExportAccess, PRO_B_TRUE, PRO_B_FALSE, &cmd_purgeold);
    HLog("ProCmdActionAdd(PurgeOld   ) -> %d\n", (int)status);
    if (status != PRO_TK_NO_ERROR)
    {
        ProStringToWstring(errbuf, "H: fail to add command");
        return -1;
    }
    status = ProCmdActionAdd(CMD_ERASEIDLE, (uiCmdCmdActFn)HEraseIdleAct,
        uiProe2ndImmediate, HExportAccess, PRO_B_TRUE, PRO_B_FALSE, &cmd_eraseidle);
    HLog("ProCmdActionAdd(EraseIdle  ) -> %d\n", (int)status);
    if (status != PRO_TK_NO_ERROR)
    {
        ProStringToWstring(errbuf, "H: fail to add command");
        return -1;
    }
    status = ProCmdActionAdd(CMD_CLEANJUNK, (uiCmdCmdActFn)HCleanJunkAct,
        uiProe2ndImmediate, HExportAccess, PRO_B_TRUE, PRO_B_FALSE, &cmd_cleanjunk);
    HLog("ProCmdActionAdd(CleanJunk  ) -> %d\n", (int)status);
    if (status != PRO_TK_NO_ERROR)
    {
        ProStringToWstring(errbuf, "H: fail to add command");
        return -1;
    }


    /* 绘图比例固定档位：共用 HScaleAct，靠 action 收到的 command id 区分 */
    for (i = 0; i < H_SCALE_COUNT; i++)
    {
        status = ProCmdActionAdd((char *)g_scale_items[i].name,
            (uiCmdCmdActFn)HScaleAct, uiProe2ndImmediate, HScaleAccess,
            PRO_B_TRUE, PRO_B_FALSE, &g_scale_cmd[i]);
        HLog("ProCmdActionAdd(%s) -> %d\n", g_scale_items[i].name, (int)status);
        if (status != PRO_TK_NO_ERROR)
        {
            ProStringToWstring(errbuf, "H: fail to add command");
            return -1;
        }
    }

    /* 自定义比例 */
    status = ProCmdActionAdd(CMD_SCALE_CUSTOM, (uiCmdCmdActFn)HScaleCustomAct,
        uiProe2ndImmediate, HScaleAccess, PRO_B_TRUE, PRO_B_FALSE,
        &g_scale_custom_cmd);
    HLog("ProCmdActionAdd(%s) -> %d\n", CMD_SCALE_CUSTOM, (int)status);
    if (status != PRO_TK_NO_ERROR)
    {
        ProStringToWstring(errbuf, "H: fail to add command");
        return -1;
    }

    /* 1c. 图标
       ★ 只能走 ProCmdIconSet：rbn 里没有给自定义命令写图标的语法
         （Creo 自家 .rbn 的按钮图标走内部资源号，外部插件用不了）。
       ★ 它按【文件名】在三个目录里找：
             <Creo loadpoint>/text/resource
             <text_dir>/resource                       ← 我们能用的是这个
             <text_dir>/<语言>/resource
         所以图标必须放在 D:\H\text\resource\，且只传文件名不传路径。
       ★ 尺寸约定（官方头文件原文）：小图 16x16 或 20x20，大图 32x32，
         文件名不带尺寸后缀；大图叫 <name>_large.png。
         这些 png 是从 Creo 安装目录 Common Files/text/resource 里挑的、
         语义最接近的官方图标，由 tools/make_icons.py 统一转成 20/32 两种
         尺寸后放进 text/resource，可随时重跑重新生成。
       ★ 图标失败不影响命令本身（返回 -3 = E_NOT_FOUND 就是没找到文件），
         所以这里只记日志不 return。 */
    {
        /* 不能加 static：表里装的是下面这些自动变量（cmd_*）的地址，
           静态存储期初始化器必须是常量表达式，加了会 C2099。 */
        const struct { uiCmdCmdId *p; const char *f; } ictab[] = {
            /* 会话 / 文件 */
            { &cmd_openworkdir,    "h_openworkdir.png"    },
            { &cmd_setworkdir,     "h_setworkdir.png"     },
            { &cmd_openmdldir,     "h_openmdldir.png"     },
            { &cmd_mdltodir,       "h_mdltodir.png"       },
            { &cmd_batchbackup,    "h_batchbackup.png"    },
            { &cmd_purgeold,       "h_purgeold.png"       },
            { &cmd_eraseidle,      "h_eraseidle.png"      },
            { &cmd_cleanjunk,      "h_cleanjunk.png"      },
            { &cmd_export,         "h_export.png"         },
            /* 模型信息 */
            { &cmd_rename3d,       "h_rename3d.png"       },
            { &cmd_copyname,       "h_copyname.png"       },
            { &cmd_outline,        "h_outline.png"        },
            /* 显示 / 颜色 */
            { &cmd_colorasm,       "h_colorasm.png"       },
            { &cmd_clearcolor,     "h_clearcolor.png"     },
            { &cmd_viewnormal,     "h_viewnormal.png"     },
            { &cmd_togglehide,     "h_togglehide.png"     },
            { &cmd_hide,           "h_hide.png"           },
            { &cmd_unhide,         "h_unhide.png"         },
            { &cmd_hideall,        "h_hideall.png"        },
            { &cmd_unhideall,      "h_unhideall.png"      },
            /* 装配 / 基准 */
            { &cmd_openinplace,    "h_openinplace.png"    },
            { &cmd_eqcrv,          "h_eqcrv.png"          },
            { &cmd_stdlib,         "h_stdlib.png"         },
            /* 工程图 */
            { &cmd_openviewinplace,"h_openviewinplace.png"},
            { &cmd_open3d,         "h_open3d.png"         },
            { &cmd_tolbrush,       "h_tolbrush.png"       },
            { &cmd_techreq,        "h_techreq.png"        },
            { &cmd_dynaview,       "h_dynaview.png"       },
            { &cmd_keydim,         "h_keydim.png"         },
            { &cmd_keydimc,        "h_keydimc.png"        },
            { &cmd_add,            "h_add.png"            },
            { &cmd_insert,         "h_insert.png"         },
            { &cmd_renum,          "h_renum.png"          },
            { &cmd_delete,         "h_delete.png"         },
            /* 比例：24 个档位 + 自定义，见下面单独循环 */
        };
        int k;

        for (k = 0; k < (int)(sizeof(ictab) / sizeof(ictab[0])); k++)
        {
            ProError ist = ProCmdIconSet(*ictab[k].p, (char *)ictab[k].f);

            if (ist != PRO_TK_NO_ERROR)
                HLog("ProCmdIconSet(%s) -> %d\n", ictab[k].f, (int)ist);
        }

        /* 24 个固定档位 + 自定义比例共用一套比例图标 */
        for (k = 0; k < H_SCALE_COUNT; k++)
        {
            status = ProCmdIconSet(g_scale_cmd[k], (char *)"h_scale.png");
            if (status != PRO_TK_NO_ERROR)
                HLog("ProCmdIconSet(h_scale.png) #%d -> %d\n", k, (int)status);
        }
        status = ProCmdIconSet(g_scale_custom_cmd, (char *)"h_scalecustom.png");
        if (status != PRO_TK_NO_ERROR)
            HLog("ProCmdIconSet(h_scalecustom.png) -> %d\n", (int)status);
        HLog("H: icons applied (%d + %d scale)\n",
             (int)(sizeof(ictab) / sizeof(ictab[0])), H_SCALE_COUNT + 1);
    }

    /* 2. 指定命令的标签/帮助来源（按钮文字从 h_msg.txt 读）
       ★ 四个字符串参数全都是【消息文件里的键名】，Creo 会逐个去
         msgfile 里查；只要有一个查不到，就返回 -28 = PRO_TK_MSG_NOT_FOUND，
         而且【整条登记作废】—— 直接后果是 Ribbon 按钮只剩图标没有文字
         （所以 Small 样式下看到光秃秃的图标就是这个原因）。
         之前第 4 个参数传的是空串 ""，被当成一个键去查，必然查不到 →
         日志里 51 个 -28。现在改成真实存在的 H_CmdDesc。 */
    {
        static char cmd_desc[] = "H_CmdDesc";   /* 见 text/<语言>/h_msg.txt */
        status = ProCmdDesignate(cmd_add, CMD_ADD, CMD_ADD ".Help", cmd_desc, msgfile);
        HLog("ProCmdDesignate(Add)      -> %d\n", (int)status);
        status = ProCmdDesignate(cmd_insert, CMD_INSERT, CMD_INSERT ".Help", cmd_desc, msgfile);
        HLog("ProCmdDesignate(Insert)   -> %d\n", (int)status);
        status = ProCmdDesignate(cmd_renum, CMD_RENUM, CMD_RENUM ".Help", cmd_desc, msgfile);
        HLog("ProCmdDesignate(Renumber) -> %d\n", (int)status);
        status = ProCmdDesignate(cmd_delete, CMD_DELETE, CMD_DELETE ".Help", cmd_desc, msgfile);
        HLog("ProCmdDesignate(Delete)   -> %d\n", (int)status);
        status = ProCmdDesignate(cmd_export, CMD_EXPORT, CMD_EXPORT ".Help", cmd_desc, msgfile);
        HLog("ProCmdDesignate(Export)   -> %d\n", (int)status);
        status = ProCmdDesignate(cmd_dynaview, CMD_DYNAVIEW, CMD_DYNAVIEW ".Help", cmd_desc, msgfile);
        HLog("ProCmdDesignate(DynaView) -> %d\n", (int)status);
        status = ProCmdDesignate(cmd_tolbrush, CMD_TOLBRUSH, CMD_TOLBRUSH ".Help", cmd_desc, msgfile);
        HLog("ProCmdDesignate(TolBrush) -> %d\n", (int)status);
        status = ProCmdDesignate(cmd_techreq, CMD_TECHREQ, CMD_TECHREQ ".Help", cmd_desc, msgfile);
        HLog("ProCmdDesignate(TechReq)  -> %d\n", (int)status);
        status = ProCmdDesignate(cmd_keydim, CMD_KEYDIM, CMD_KEYDIM ".Help", cmd_desc, msgfile);
        HLog("ProCmdDesignate(KeyDim)   -> %d\n", (int)status);
        status = ProCmdDesignate(cmd_keydimc, CMD_KEYDIMC, CMD_KEYDIMC ".Help", cmd_desc, msgfile);
        HLog("ProCmdDesignate(KeyDimC)  -> %d\n", (int)status);
        status = ProCmdDesignate(cmd_openworkdir, CMD_OPENWORKDIR, CMD_OPENWORKDIR ".Help", cmd_desc, msgfile);
        HLog("ProCmdDesignate(OpenWorkDir) -> %d\n", (int)status);
        status = ProCmdDesignate(cmd_setworkdir, CMD_SETWORKDIR, CMD_SETWORKDIR ".Help", cmd_desc, msgfile);
        HLog("ProCmdDesignate(SetWorkDir ) -> %d\n", (int)status);
        status = ProCmdDesignate(cmd_openmdldir, CMD_OPENMDLDIR, CMD_OPENMDLDIR ".Help", cmd_desc, msgfile);
        HLog("ProCmdDesignate(OpenMdlDir ) -> %d\n", (int)status);
        status = ProCmdDesignate(cmd_mdltodir, CMD_MDLTODIR, CMD_MDLTODIR ".Help", cmd_desc, msgfile);
        HLog("ProCmdDesignate(MdlToDir   ) -> %d\n", (int)status);
        status = ProCmdDesignate(cmd_purgeold, CMD_PURGEOLD, CMD_PURGEOLD ".Help", cmd_desc, msgfile);
        HLog("ProCmdDesignate(PurgeOld   ) -> %d\n", (int)status);
        status = ProCmdDesignate(cmd_eraseidle, CMD_ERASEIDLE, CMD_ERASEIDLE ".Help", cmd_desc, msgfile);
        HLog("ProCmdDesignate(EraseIdle  ) -> %d\n", (int)status);
        status = ProCmdDesignate(cmd_cleanjunk, CMD_CLEANJUNK, CMD_CLEANJUNK ".Help", cmd_desc, msgfile);
        HLog("ProCmdDesignate(CleanJunk  ) -> %d\n", (int)status);
        status = ProCmdDesignate(cmd_outline, CMD_OUTLINE, CMD_OUTLINE ".Help", cmd_desc, msgfile);
        HLog("ProCmdDesignate(Outline    ) -> %d\n", (int)status);
        status = ProCmdDesignate(cmd_copyname, CMD_COPYNAME, CMD_COPYNAME ".Help", cmd_desc, msgfile);
        HLog("ProCmdDesignate(CopyName   ) -> %d\n", (int)status);
        status = ProCmdDesignate(cmd_hide, CMD_HIDE, CMD_HIDE ".Help", cmd_desc, msgfile);
        HLog("ProCmdDesignate(Hide       ) -> %d\n", (int)status);
        status = ProCmdDesignate(cmd_unhide, CMD_UNHIDE, CMD_UNHIDE ".Help", cmd_desc, msgfile);
        HLog("ProCmdDesignate(Unhide     ) -> %d\n", (int)status);
        status = ProCmdDesignate(cmd_unhideall, CMD_UNHIDEALL, CMD_UNHIDEALL ".Help", cmd_desc, msgfile);
        status = ProCmdDesignate(cmd_togglehide, CMD_TOGGLEHIDE, CMD_TOGGLEHIDE ".Help", cmd_desc, msgfile);
        HLog("ProCmdDesignate(ToggleHide ) -> %d\n", (int)status);
        status = ProCmdDesignate(cmd_hideall, CMD_HIDEALL, CMD_HIDEALL ".Help", cmd_desc, msgfile);
        HLog("ProCmdDesignate(HideAll    ) -> %d\n", (int)status);
        HLog("ProCmdDesignate(UnhideAll  ) -> %d\n", (int)status);
        status = ProCmdDesignate(cmd_rename3d, CMD_RENAME3D, CMD_RENAME3D ".Help", cmd_desc, msgfile);
        HLog("ProCmdDesignate(Rename3D   ) -> %d\n", (int)status);
        status = ProCmdDesignate(cmd_colorasm, CMD_COLORASM, CMD_COLORASM ".Help", cmd_desc, msgfile);
        HLog("ProCmdDesignate(ColorAsm   ) -> %d\n", (int)status);
        status = ProCmdDesignate(cmd_clearcolor, CMD_CLEARCOLOR, CMD_CLEARCOLOR ".Help", cmd_desc, msgfile);
        HLog("ProCmdDesignate(ClearColor ) -> %d\n", (int)status);
        status = ProCmdDesignate(cmd_openinplace, CMD_OPENINPLACE, CMD_OPENINPLACE ".Help", cmd_desc, msgfile);
        HLog("ProCmdDesignate(OpenInPlace) -> %d\n", (int)status);
        status = ProCmdDesignate(cmd_openviewinplace, CMD_OPENVIEWINPLACE, CMD_OPENVIEWINPLACE ".Help", cmd_desc, msgfile);
        HLog("ProCmdDesignate(OpenViewInPlace) -> %d\n", (int)status);
        status = ProCmdDesignate(cmd_open3d, CMD_OPEN3D, CMD_OPEN3D ".Help", cmd_desc, msgfile);
        HLog("ProCmdDesignate(Open3D     ) -> %d\n", (int)status);
        status = ProCmdDesignate(cmd_viewnormal, CMD_VIEWNORMAL, CMD_VIEWNORMAL ".Help", cmd_desc, msgfile);
        HLog("ProCmdDesignate(ViewNormal ) -> %d\n", (int)status);
        status = ProCmdDesignate(cmd_eqcrv, CMD_EQCRV, CMD_EQCRV ".Help", cmd_desc, msgfile);
        HLog("ProCmdDesignate(EqCurve    ) -> %d\n", (int)status);
        status = ProCmdDesignate(cmd_stdlib, CMD_STDLIB, CMD_STDLIB ".Help", cmd_desc, msgfile);
        HLog("ProCmdDesignate(StdLib    ) -> %d\n", (int)status);
        status = ProCmdDesignate(cmd_batchbackup, CMD_BATCHBACKUP, CMD_BATCHBACKUP ".Help", cmd_desc, msgfile);
        HLog("ProCmdDesignate(BatchBackup) -> %d\n", (int)status);
        for (i = 0; i < H_SCALE_COUNT; i++)
        {
            sprintf(helpkey, "%s.Help", g_scale_items[i].name);
            status = ProCmdDesignate(g_scale_cmd[i], (char *)g_scale_items[i].name,
                                     helpkey, cmd_desc, msgfile);
            HLog("ProCmdDesignate(%s) -> %d\n", g_scale_items[i].name, (int)status);
        }
        sprintf(helpkey, "%s.Help", CMD_SCALE_CUSTOM);
        status = ProCmdDesignate(g_scale_custom_cmd, CMD_SCALE_CUSTOM,
                                 helpkey, cmd_desc, msgfile);
        HLog("ProCmdDesignate(%s) -> %d\n", CMD_SCALE_CUSTOM, (int)status);
    }

    /* 3. 功能区（Ribbon）——主动调用官方 API 加载 rbn。
          ProRibbonDefinitionfileLoad 的搜索路径（官方头文件 ProRibbon.h）：
             当前目录 / <text_dir>\ribbon / <text_dir>\<语言>\ribbon
          注意：rbn 放在 text 根目录【不在】搜索路径里，必须放 ribbon 子目录。
          返回码含义（ProToolkitErrors.h）：
              0 = 成功
             -4 = PRO_TK_E_NOT_FOUND    文件没找到
            -16 = PRO_TK_INVALID_FILE   找到了但格式不合法
            -39 = PRO_TK_E_IN_USE       已被自动加载过（也说明加载成功）
          保险起见：先用文件名走官方搜索路径，若没找到再用 DLL 目录下的
          绝对路径重试一次。 */
    {
        wchar_t rbnfile[PRO_PATH_SIZE];
        char    rbnpath[PRO_PATH_SIZE];

        ProStringToWstring(rbnfile, RBNFILE_NAME);
        status = ProRibbonDefinitionfileLoad(rbnfile);
        HLog("ProRibbonDefinitionfileLoad(%s) -> %d%s\n", RBNFILE_NAME, (int)status,
             status == PRO_TK_NO_ERROR    ? "  [OK]" :
             status == PRO_TK_E_NOT_FOUND ? "  [not on search path]" :
             status == PRO_TK_INVALID_FILE? "  [INVALID RBN FORMAT]" :
             status == PRO_TK_E_IN_USE    ? "  [already loaded - OK]" : "");

        if (status == PRO_TK_E_NOT_FOUND)
        {
            sprintf(rbnpath, "%s\\text\\ribbon\\%s", g_plugin_dir, RBNFILE_NAME);
            ProStringToWstring(rbnfile, rbnpath);
            status = ProRibbonDefinitionfileLoad(rbnfile);
            HLog("ProRibbonDefinitionfileLoad(%s) -> %d%s\n", rbnpath, (int)status,
                 status == PRO_TK_NO_ERROR    ? "  [OK]" :
                 status == PRO_TK_INVALID_FILE? "  [INVALID RBN FORMAT]" :
                 status == PRO_TK_E_IN_USE    ? "  [already loaded - OK]" : "");
        }
    }

    HLog("user_initialize done.\n");
    return 0;
}

void user_terminate(void)
{
    ProTKPrintf("H plugin terminated.\n");
}
