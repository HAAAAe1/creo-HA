/*===========================================================================*\
  FILE    : h_main.c
  PRODUCT : H  —— Creo 工程图圆圈序号工具（TOOLKIT 插件）
            - 不依赖 BOM 表，独立的符号实例
            - 点一下放一个圆圈球标，编号自动 +1（1、2、3…）
            - 删除任意球标后自动重排（删 1，原 2 变 1）
            - 符号载体：symbols\111.sym（圆形球标，可变文本 = 数字）

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
#include <ProDtlitem.h>
#include <ProDtlattach.h>
#include <ProDtlnote.h>
#include <ProMessage.h>
#include <ProWindows.h>
#include <ProGraphic.h>
#include <ProView.h>
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
   这是 Creo 自己的规矩，所以这里必须与 symbols\3hao.sym 完全对应）。

   ★ 为什么是 3hao 而不是 111：
   Creo 的文件只向上兼容 —— 新版能读旧版，旧版读不了新版。
   原来的 111.sym 是 Creo 12.4 存出来的（头部 "#Creo TM 12 ... 12.4.0.0"，
   格式级别 4300，正文还带 symbol_font / p_sym_param 等 Creo 8/9 才有的字段），
   在 Creo 6 上 ProDrawingDtlsymdefRetrieve 必然失败。
   3hao.sym 是用 Creo 6.0.2.0 存出来的（格式级别 3700），
   没有任何新版本字段 —— Creo 6/7/8/9/10/11/12 全部可读。
   ⚠ 以后千万不要用新版 Creo 打开它再另存，那会把它升级成新版格式，
     同事的 Creo 6 就又读不了了。 */
#define SYMNAME     "3hao"

/* 命令 ID —— 统一加 H_ 前缀，避免与其它插件的命令重名。
   这三个名字必须与 text\toolkitribbonui.rbn 里的 mName 以及
   h_msg.txt 里的标签键完全一致。 */
#define CMD_ADD      "H_AddBalloon"
#define CMD_INSERT   "H_InsertBalloon"
#define CMD_DELETE   "H_DeleteBalloon"
#define CMD_RENUM    "H_RenumberBalloon"

/*---------------------------------------------------------------------*\
  全局
\*---------------------------------------------------------------------*/
static ProDtlsymdef g_symdef;                       /* 符号定义（一次加载，全程复用） */
static ProBoolean  g_symdef_valid = PRO_B_FALSE;
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
        return err;

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

/* 在 point 处创建一个编号为 number 的球标（「添加」与「插入」共用） */
static ProError HCreateBalloon(ProMdl drawing, ProView over_view,
                               ProVector point, int number)
{
    ProDtlsyminstdata data = NULL;
    ProDtlattach      attach = NULL;
    ProDtlsyminst     syminst;

    if (ProDtlsyminstdataAlloc(drawing, &data) != PRO_TK_NO_ERROR)
        return PRO_TK_GENERAL_ERROR;
    ProDtlsyminstdataDefSet(data, &g_symdef);
    ProDtlsyminstdataAttachtypeSet(data, PROSYMDEFATTACHTYPE_FREE);
    ProDtlsyminstdataDisplayedSet(data, PRO_B_TRUE);

    if (HSetVarTexts(drawing, data, number) != PRO_TK_NO_ERROR)
    {
        ProDtlsyminstdataFree(data);
        return PRO_TK_GENERAL_ERROR;
    }
    if (ProDtlattachAlloc(PRO_DTLATTACHTYPE_FREE, over_view, point, NULL, &attach)
            != PRO_TK_NO_ERROR)
    {
        ProDtlsyminstdataFree(data);
        return PRO_TK_GENERAL_ERROR;
    }
    ProDtlsyminstdataAttachmentSet(data, attach);
    ProDtlattachFree(attach);           /* attach 已被复制，立即释放 */

    if (ProDtlsyminstCreate(drawing, data, &syminst) != PRO_TK_NO_ERROR)
    {
        ProDtlsyminstdataFree(data);
        return PRO_TK_GENERAL_ERROR;
    }
    ProDtlsyminstShow(&syminst);
    HLog("HCreateBalloon: number=%d OK\n", number);
    ProDtlsyminstdataFree(data);
    return PRO_TK_NO_ERROR;
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
    ProError     status;
    ProMdl       drawing;
    ProVector    point;
    HScan        scan;
    int          new_number;
    int          sheet;
    ProView      over_view;
    ProMouseButton btn;
    int          placed;

    status = HGetDrawing(&drawing);
    if (status != PRO_TK_NO_ERROR)
    {
        HMsg("BALLOON_NO_DRAWING");
        return 0;
    }

    if (!g_symdef_valid)
    {
        if (HLoadSymbol(drawing) != PRO_TK_NO_ERROR)
        {
            HMsg("BALLOON_NO_SYMBOL");
            return 0;
        }
    }

    /* 交互选放置点：ProMousePickGet 直接取鼠标点击位置，不改变 Creo 的
       选择过滤器，可在图纸任意位置（含空白处）点选。

       ★ 连续放置：整个命令期间就留在这个循环里，每按一次左键放一个球标，
       不用再回去点功能区按钮。

       为什么 expected_button 传 PRO_ANY_BUTTON 而不是 PRO_LEFT_BUTTON：
       官方头文件 ProGraphic.h 写明，传 PRO_ANY_BUTTON 时用户可用任意键
       拾取，并且 button_pressed 会回传实际按下的键（左键 1 / 右键 2 /
       中键 4）。只有拿到这个回传值，才能区分「继续放」和「结束」；
       若传 PRO_LEFT_BUTTON，中键、右键根本不会让函数返回，就没法收尾。

       退出条件（三条都有官方依据）：
         1) 左键         -> 放一个，继续循环
         2) 中键 / 右键  -> 正常结束（与 Creo「中键=完成」的习惯一致）
         3) 返回码非 0   -> 用户点了别的命令或按 Esc 取消。ProGraphic.h 原话：
            "You must check the return value to make sure the user did not
             select a command." —— 这正是安全退出的依据。
       另外 HCreateBalloon 出错也立刻跳出，不留半截状态。 */
    ProDrawingCurrentSheetGet((ProDrawing)drawing, &sheet);
    ProDrawingBackgroundViewGet((ProDrawing)drawing, sheet, &over_view);

    placed = 0;
    for (;;)
    {
        HMsg(placed == 0 ? "BALLOON_PICK" : "BALLOON_PICK_MORE");
        ProWindowRepaint(PRO_VALUE_UNUSED);   /* 先刷出提示，用户才知道要点 */

        btn = PRO_NO_BUTTON;
        point[0] = point[1] = point[2] = 0.0;
        status = ProMousePickGet(PRO_ANY_BUTTON, &btn, point);
        ProMessageClear();

        HLog("HAddAct: MousePick status=%d btn=%d point=(%.1f,%.1f) placed=%d\n",
             (int)status, (int)btn, point[0], point[1], placed);

        if (status != PRO_TK_NO_ERROR)
            break;                      /* 用户点了别的命令 / 取消 */
        if (btn != PRO_LEFT_BUTTON)
            break;                      /* 中键或右键：结束连续放置 */

        /* 扫描现有编号，取最大值 +1 —— 每轮都重新扫，所以编号始终连号 */
        scan.count = 0; scan.numbers = NULL; scan.max = 0;
        ProDrawingDtlsyminstVisit((ProDrawing)drawing, PRO_VALUE_UNUSED,
                                  HScanAction, NULL, (ProAppData)&scan);
        new_number = scan.max + 1;
        HScanFree(&scan);
        HLog("HAddAct: new_number=%d\n", new_number);

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
    if (placed > 1)
        HMsg("BALLOON_ADD_MULTI_OK");   /* 连续放了多个，收尾给个明确反馈 */
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
    ProMouseButton btn;
    ProSelection  *sel = NULL;
    int            n_sels = 0, sheet, insert_num;
    ProDtlsyminst  ref_inst;

    status = HGetDrawing(&drawing);
    if (status != PRO_TK_NO_ERROR)
    {
        HMsg("BALLOON_NO_DRAWING");
        return 0;
    }
    if (!g_symdef_valid && HLoadSymbol(drawing) != PRO_TK_NO_ERROR)
    {
        HMsg("BALLOON_NO_SYMBOL");
        return 0;
    }

    ProDrawingCurrentSheetGet((ProDrawing)drawing, &sheet);
    ProDrawingBackgroundViewGet((ProDrawing)drawing, sheet, &over_view);

    /* ① 放置位置 */
    HMsg("BALLOON_INSERT_PICK");
    ProWindowRepaint(PRO_VALUE_UNUSED);
    btn = PRO_NO_BUTTON;
    point[0] = point[1] = point[2] = 0.0;
    status = ProMousePickGet(PRO_LEFT_BUTTON, &btn, point);
    ProMessageClear();
    if (status != PRO_TK_NO_ERROR || btn != PRO_LEFT_BUTTON)
        return 0;

    /* ② 参照球标（新球标插在它前面） */
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
    uiCmdCmdId  cmd_add, cmd_insert, cmd_renum, cmd_delete;
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
