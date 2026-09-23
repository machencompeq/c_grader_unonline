/*
 * util.h - 共用小工具：路徑、檔案、UTF-8 <-> Windows 寬字元、字串緩衝。
 *
 * 整個程式內部一律使用 UTF-8 字串 (char *)。
 * 只有在呼叫 Windows API 的那一刻才轉成寬字元 (wchar_t *)，
 * 這樣中文資料夾名稱 / 中文學號也能正常處理。
 */
#ifndef UTIL_H
#define UTIL_H

#include <stdio.h>
#include <stddef.h>
#include <wchar.h>

#define GRADER_PATH_MAX 1024

/* ---------- 字串編碼轉換 (回傳 malloc 的記憶體，呼叫者要 free) ---------- */
wchar_t *utf8_to_wide(const char *s);
char *wide_to_utf8(const wchar_t *w);

/* ---------- 檔案 ---------- */
FILE *fopen_utf8(const char *path, const char *mode);
/* 讀整個檔案，回傳 malloc 的內容 (尾端補 '\0')，失敗回傳 NULL */
char *read_file(const char *path, size_t *out_len);
/* 只讀前 limit bytes (0 = 不限制)；*truncated = 1 表示檔案比 limit 長 (超大輸出不必整個讀進記憶體) */
char *read_file_limited(const char *path, size_t limit, size_t *out_len, int *truncated);
/* 寫入整個檔案 (覆蓋)，成功回傳 1 */
int write_file(const char *path, const char *data, size_t len);
int delete_file(const char *path); /* 檔案不存在也算成功 */
int file_exists(const char *path);
int dir_exists(const char *path);
int make_dir(const char *path);             /* 會一併建立上層資料夾 */
int remove_dir_recursive(const char *path); /* 刪除整個資料夾 */
void path_join(char *out, size_t size, const char *a, const char *b);
/* 在系統暫存資料夾建立這次批改專用的資料夾：%TEMP%\c-grader\run-<pid>-<time> */
int make_run_temp_dir(char *out, size_t size);
/* 內容是否為合法 UTF-8 (用來判斷學生原始碼是不是 Big5) */
int is_valid_utf8(const char *s, size_t len);
/* 不是 UTF-8 就當 Big5 轉成 UTF-8 (會 free 原本的 text，回傳新字串，*len 更新) */
char *text_to_utf8(char *text, size_t *len);
/* 批改工具資料夾 (Grader.exe 所在處；命令列版在 build\ 裡時取上一層) */
void app_folder(char *out, size_t size);
/* 相對路徑 -> 絕對路徑 (編譯/執行時工作目錄會切到 temp/，所以一律用絕對路徑) */
int absolute_path(const char *path, char *out, size_t size);

/* ---------- 字串清單 ---------- */
typedef struct {
    char **items;
    int count;
} StringList;

void string_list_add(StringList *list, const char *s);
void string_list_free(StringList *list);

/*
 * 列出 dir 底下符合 pattern (例如 "*.c") 的名稱 (只有名稱，不含路徑)，依名稱排序。
 * want_dirs = 1 只列資料夾，0 只列檔案。
 */
int list_dir(const char *dir, const char *pattern, int want_dirs, StringList *out);

/* ---------- 可自動變長的字串 ---------- */
typedef struct {
    char *data;
    size_t len;
    size_t cap;
} StrBuf;

void sb_append(StrBuf *sb, const char *s);
void sb_append_len(StrBuf *sb, const char *s, size_t len); /* 加入前 len 個 byte (可含 '\0' 以外的任何字元) */
void sb_appendf(StrBuf *sb, const char *fmt, ...);
void sb_free(StrBuf *sb);

/* 分數顯示：整數就不印小數，否則印到小數第 2 位 */
void format_score(char *out, size_t size, double value);

#endif
