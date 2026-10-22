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
#include <ProMenuBar.h>
#include <ProRibbon.h>
#include <ProUtil.h>
#include <ProObjects.h>
#include <ProArray.h>
#include <ProWstring.h>
#include <ProDtlsyminst.h>
#include <ProDtlsymdef.h>

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
static int HCleanDirW(const wchar_t *dir, int do_delete)
{
    static const wchar_t *ok_ext[] = { L"err", L"out", L"inf", L"idx", L"xpr",
                                       L"bak", L"dwl", L"m_p", L"lst", L"tst" };
    wchar_t         pat[PRO_PATH_SIZE], full[PRO_PATH_SIZE], ext[32];
    WIN32_FIND_DATAW fd;
    HANDLE          h;
    int             n = 0;

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
        if (HExtOfW(fd.cFileName, ext, 32) != 0)
            continue;
        if (!HInListW(ext, ok_ext, (int)(sizeof(ok_ext) / sizeof(ok_ext[0]))))
            continue;

        n++;
        if (!do_delete)
            continue;
        if (HPathJoinW(full, PRO_PATH_SIZE, dir, fd.cFileName, L"") != 0)
            continue;
        if (DeleteFileW(full))
            HLogW("HCleanDirW: removed ", full);
        else
            HLog("HCleanDirW: delete FAILED err=%lu\n",
                 (unsigned long)GetLastError());
    } while (FindNextFileW(h, &fd));

    FindClose(h);
    HLog("HCleanDirW: %s %d file(s)\n", do_delete ? "removed" : "found", n);
    return n;
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
    ProMdldata data;
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

    memset(&data, 0, sizeof(data));
    status = ProMdlDataGet(mdl, &data);
    HLog("HOpenMdlDirAct: dataGet -> %d\n", (int)status);
    if (status != PRO_TK_NO_ERROR)
    {
        HMsg("SESSION_NO_MDL");
        return 0;
    }

    wcsncpy(path, data.path, (size_t)PRO_PATH_SIZE - 1);
    path[PRO_PATH_SIZE - 1] = L'\0';
    HLogW("HOpenMdlDirAct: model path = ", path);

    HDirOfW(path);
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
    HLogW("HCleanJunkAct: dir = ", dir);

    n = HCleanDirW(dir, 0);
    HLog("HCleanJunkAct: scan n=%d\n", n);
    if (n <= 0)
    {
        HMsg("SESSION_CLEAN_NONE");
        return 0;
    }

    if (!HConfirmYes("SESSION_CLEAN_ASK"))
    {
        HMsg("SESSION_CANCEL");
        return 0;
    }

    n = HCleanDirW(dir, 1);
    HLog("HCleanJunkAct: removed n=%d\n", n);
    HMsg("SESSION_CLEAN_OK");
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



/* 原生绘图注释命令自检：rbn 里这 11 个按钮的 mName 直接写的是
   Creo 系统命令名，名字必须真实存在，否则按钮不显示。
   启动时用 ProCmdCmdIdFind 逐个确认，结果写 h_load.log。 */
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
    "ProCmdDwgCrSymInstance",
    "ProCmdDwgCrGtolSetDtm",
    "ProCmdDwgCrGtolMakeTarg",
    "ProCmdDwgCreateDrftDatum",
    "ProCmdDwgCreateDraftAxis",
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

    /* 2. 指定命令的标签/帮助来源（按钮文字从 h_msg.txt 读）
       注意第 4 个参数 description（自定义功能区里的命令描述）：
       必须传【空字符串】而不是 NULL —— 传 NULL 时 Creo 会去消息
       文件里查一个空键，查不到就返回 -28（虽然 label/help 其实
       已经登记成功，按钮也能用，但日志里永远挂着 3 个 -28）。 */
    {
        static char empty_desc[PRO_LINE_SIZE] = "";
        status = ProCmdDesignate(cmd_add, CMD_ADD, CMD_ADD ".Help", empty_desc, msgfile);
        HLog("ProCmdDesignate(Add)      -> %d\n", (int)status);
        status = ProCmdDesignate(cmd_insert, CMD_INSERT, CMD_INSERT ".Help", empty_desc, msgfile);
        HLog("ProCmdDesignate(Insert)   -> %d\n", (int)status);
        status = ProCmdDesignate(cmd_renum, CMD_RENUM, CMD_RENUM ".Help", empty_desc, msgfile);
        HLog("ProCmdDesignate(Renumber) -> %d\n", (int)status);
        status = ProCmdDesignate(cmd_delete, CMD_DELETE, CMD_DELETE ".Help", empty_desc, msgfile);
        HLog("ProCmdDesignate(Delete)   -> %d\n", (int)status);
        status = ProCmdDesignate(cmd_export, CMD_EXPORT, CMD_EXPORT ".Help", empty_desc, msgfile);
        HLog("ProCmdDesignate(Export)   -> %d\n", (int)status);
        status = ProCmdDesignate(cmd_dynaview, CMD_DYNAVIEW, CMD_DYNAVIEW ".Help", empty_desc, msgfile);
        HLog("ProCmdDesignate(DynaView) -> %d\n", (int)status);
        status = ProCmdDesignate(cmd_tolbrush, CMD_TOLBRUSH, CMD_TOLBRUSH ".Help", empty_desc, msgfile);
        HLog("ProCmdDesignate(TolBrush) -> %d\n", (int)status);
        status = ProCmdDesignate(cmd_techreq, CMD_TECHREQ, CMD_TECHREQ ".Help", empty_desc, msgfile);
        HLog("ProCmdDesignate(TechReq)  -> %d\n", (int)status);
        status = ProCmdDesignate(cmd_keydim, CMD_KEYDIM, CMD_KEYDIM ".Help", empty_desc, msgfile);
        HLog("ProCmdDesignate(KeyDim)   -> %d\n", (int)status);
        status = ProCmdDesignate(cmd_keydimc, CMD_KEYDIMC, CMD_KEYDIMC ".Help", empty_desc, msgfile);
        HLog("ProCmdDesignate(KeyDimC)  -> %d\n", (int)status);
        status = ProCmdDesignate(cmd_openworkdir, CMD_OPENWORKDIR, CMD_OPENWORKDIR ".Help", empty_desc, msgfile);
        HLog("ProCmdDesignate(OpenWorkDir) -> %d\n", (int)status);
        status = ProCmdDesignate(cmd_setworkdir, CMD_SETWORKDIR, CMD_SETWORKDIR ".Help", empty_desc, msgfile);
        HLog("ProCmdDesignate(SetWorkDir ) -> %d\n", (int)status);
        status = ProCmdDesignate(cmd_openmdldir, CMD_OPENMDLDIR, CMD_OPENMDLDIR ".Help", empty_desc, msgfile);
        HLog("ProCmdDesignate(OpenMdlDir ) -> %d\n", (int)status);
        status = ProCmdDesignate(cmd_mdltodir, CMD_MDLTODIR, CMD_MDLTODIR ".Help", empty_desc, msgfile);
        HLog("ProCmdDesignate(MdlToDir   ) -> %d\n", (int)status);
        status = ProCmdDesignate(cmd_purgeold, CMD_PURGEOLD, CMD_PURGEOLD ".Help", empty_desc, msgfile);
        HLog("ProCmdDesignate(PurgeOld   ) -> %d\n", (int)status);
        status = ProCmdDesignate(cmd_eraseidle, CMD_ERASEIDLE, CMD_ERASEIDLE ".Help", empty_desc, msgfile);
        HLog("ProCmdDesignate(EraseIdle  ) -> %d\n", (int)status);
        status = ProCmdDesignate(cmd_cleanjunk, CMD_CLEANJUNK, CMD_CLEANJUNK ".Help", empty_desc, msgfile);
        HLog("ProCmdDesignate(CleanJunk  ) -> %d\n", (int)status);
        for (i = 0; i < H_SCALE_COUNT; i++)
        {
            sprintf(helpkey, "%s.Help", g_scale_items[i].name);
            status = ProCmdDesignate(g_scale_cmd[i], (char *)g_scale_items[i].name,
                                     helpkey, empty_desc, msgfile);
            HLog("ProCmdDesignate(%s) -> %d\n", g_scale_items[i].name, (int)status);
        }
        sprintf(helpkey, "%s.Help", CMD_SCALE_CUSTOM);
        status = ProCmdDesignate(g_scale_custom_cmd, CMD_SCALE_CUSTOM,
                                 helpkey, empty_desc, msgfile);
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
