/*
 * gui.h - 視覺化介面 (Win32 API)。
 *
 * GUI 只負責畫面、按鈕、表格、輸入與顯示結果。
 * 編譯、執行、比對、評分全部交給 grader / scorer / config / exporter 模組。
 *
 *   gui.c         主視窗 (資料夾、比對模式、評分方式、CE 自動修正、批改結果表格)
 *   gui_rules.c   「編輯評分規則」視窗
 *   gui_detail.c  學生詳細結果視窗 (每個 testcase 的 Expected / Actual)
 *   gui_tests.c   「編輯測資」總表視窗
 *   gui_common.c  共用：字型、DPI 縮放、建立元件、文字轉換、模態視窗、工具提示、深色主題
 *
 * 深色科技風主題 (gui_common.c 的 gui_theme_*)：
 *   - 視窗底色 / 文字 / 強調色由 g_theme 統一定義
 *   - 按鈕、核取方塊、選項按鈕、群組框用 NM_CUSTOMDRAW 自己畫 (保留 Windows 的自動勾選邏輯)
 *   - 表格、表頭、進度條、狀態列、下拉選單、輸入框套用深色
 *   每個視窗程序只要在 WM_CTLCOLOR* / WM_NOTIFY / WM_DRAWITEM 先呼叫 gui_theme_* 即可。
 */
#ifndef GUI_H
#define GUI_H

#include <windows.h>

#include "config.h"
#include "grader.h"

int gui_run(HINSTANCE instance, int show);

/* ---------- gui_common.c ---------- */
extern HINSTANCE g_inst;
extern HFONT g_font;
extern HFONT g_bold_font;
extern HFONT g_mono_font;
extern HFONT g_title_font; /* 群組標題用，稍大一點的粗體 */

typedef struct {
    COLORREF bg;          /* 視窗底色 */
    COLORREF field;       /* 輸入框、表格底色 */
    COLORREF field_hot;   /* 按鈕滑鼠移過 */
    COLORREF border;      /* 框線 */
    COLORREF header;      /* 表頭 */
    COLORREF text;
    COLORREF muted;       /* 次要文字 */
    COLORREF accent;      /* 強調色 (青) */
    COLORREF accent_hot;
    COLORREF accent_dark;
    COLORREF on_accent;   /* 強調色上的文字 */
    COLORREF success, warn, danger, info; /* 狀態色 */
    COLORREF row_ok, row_bad, row_warn, row_fixed, row_alt; /* 表格列底色 */
} GuiTheme;
extern const GuiTheme g_theme;

void gui_common_init(HINSTANCE instance);
int dpi(int px);  /* 以 96 DPI 設計的尺寸 -> 實際螢幕像素 */

/* WM_CTLCOLORSTATIC / BTN / EDIT / LISTBOX / DLG：回傳要用的筆刷並設定文字顏色 */
LRESULT gui_theme_ctlcolor(UINT msg, HDC dc, HWND control);
LRESULT gui_control_color(HDC dc); /* 舊名稱，同 gui_theme_ctlcolor(WM_CTLCOLORSTATIC) */
/* WM_NOTIFY：按鈕類的 NM_CUSTOMDRAW 自己畫；有處理回傳 1 並填 *result */
int gui_theme_notify(NMHDR *hdr, LRESULT *result);
/* WM_DRAWITEM：狀態列文字；有處理回傳 1 */
int gui_theme_drawitem(DRAWITEMSTRUCT *dis);
/* 視窗與所有子元件建立完後呼叫一次：深色標題列、表格/表頭/進度條/下拉選單/輸入框套色 */
void gui_theme_window(HWND wnd);
/* 標記：主要按鈕 (強調色底)、次要文字 (灰) */
void gui_mark_primary(HWND button);
void gui_mark_muted(HWND label);
/* 狀態列文字 (自繪，深色) */
void gui_status_text(HWND status, const wchar_t *text);
/* 面板：圓角框線 + 強調色標題 (取代群組框；在父視窗的 WM_PAINT 呼叫，rc 為像素) */
void gui_draw_panel(HDC dc, const RECT *rc, const wchar_t *title);

HWND make_control(HWND parent, const wchar_t *cls, const wchar_t *text, DWORD style, DWORD ex_style,
                  int x, int y, int w, int h, int id);
void move_control(HWND control, int x, int y, int w, int h); /* 參數以 96 DPI 為單位 */

HWND create_dialog_window(const wchar_t *cls, const wchar_t *title, HWND owner, int client_w, int client_h,
                          int resizable);
void register_window_class(const wchar_t *name, WNDPROC proc);
/* 模態視窗：停用主視窗，直到 *done 變成 1 才返回 (返回時會關閉 dialog) */
void run_modal(HWND dialog, HWND owner, volatile int *done);

void set_text_utf8(HWND control, const char *text);
/* UTF-8 -> 可以放進 Edit 的文字：\n 轉 \r\n；show_ws = 1 時把空白/Tab/換行顯示成符號 */
wchar_t *make_display_text(const char *text, size_t len, int show_ws);
int read_number(HWND edit, double *out);  /* 讀取 Edit 的數字，格式錯誤回傳 0 */

/* 表格外觀：深色、整列選取、雙緩衝、自繪表頭 */
void gui_style_listview(HWND list);
/* 表頭排序箭頭：column = -1 表示全部清掉 */
void gui_set_sort_arrow(HWND list, int column, int ascending);
/* 工具提示：建立一個 tooltip 視窗，然後為每個元件加說明文字 */
HWND gui_create_tooltip(HWND parent);
void gui_add_tooltip(HWND tooltip, HWND control, const wchar_t *text);

/* 顯示一段唯讀文字 (編譯錯誤、驗證結果) */
void show_text_window(HWND owner, const wchar_t *title, const char *text);

/* 讓老師貼上一大段文字；按取消回傳 NULL，否則回傳 malloc 的 UTF-8 (\n 換行) */
char *input_text_dialog(HWND owner, const wchar_t *title, const wchar_t *prompt);

/* 開一個看得到的主控台視窗執行命令 (AI 腳本)，等待期間畫面照常重繪；回傳結束碼，無法啟動回傳 -1 */
int run_visible_command(HWND owner, const char *command_line);

/* 批改工具資料夾\tools\<name> 的完整路徑；不存在回傳 0 */
int tool_script_path(const char *name, char *out, size_t size);

/* ---------- gui_rules.c ---------- */
/* 回傳 1 = 老師按了「儲存規則」，rules 已更新 */
int rules_dialog(HWND owner, ScoreRules *rules);

/* ---------- gui_tests.c ---------- */
/* 測資總表編輯：reference = 參考答案 .c；data_dir = 測資資料夾；cfg 用於預覽輸出 (編譯參數、時間限制) */
void tests_dialog(HWND owner, const char *reference, const char *data_dir, const GradeConfig *cfg);

/* ---------- gui_detail.c ---------- */
/* report_path：這位學生的 HTML 報告 (可為空字串) */
void detail_dialog(HWND owner, const Grader *g, const StudentResult *r, const char *report_path);

#endif
