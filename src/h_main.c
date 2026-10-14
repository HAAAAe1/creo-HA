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
#include <ProView.h>
#include <ProPDF.h>
#include <windows.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <wchar.h>
#include <stdlib.h>

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
int user_initialize(
    int    argc,
    char  *argv[],
    char  *proe_vsn,
    char  *build,
    wchar_t errbuf[80])
{
    ProError    status;
    uiCmdCmdId  cmd_add, cmd_insert, cmd_renum, cmd_delete, cmd_export;
    ProFileName msgfile;

    g_symdef_valid = PRO_B_FALSE;

    HInitPluginDir();
    HLog("---- user_initialize: H plugin ----\n");
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
