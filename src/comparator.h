/*
 * comparator.h - 比較「標準輸出」與「學生輸出」，算出 Difference Count (CHAR 差異數)。
 *
 * CHAR 的定義：一個 Unicode 字元。中文字 "系" 算 1 CHAR (不是 UTF-8 的 3 bytes)。
 *
 * 差異數 = 編輯距離 (Levenshtein distance)：
 *   把學生輸出改成標準輸出，最少需要「新增 / 刪除 / 替換」幾個字元。
 *   ABCDEFG vs ABCXEFG -> 1 (替換 1 個)
 *   ABCDEFG vs ABCXYFG -> 2 (替換 2 個)
 *   ABCDEFG vs ABC     -> 4 (少了 DEFG)
 *
 * 比對前的整理順序 (標準答案與學生輸出做完全一樣的處理，確保公平)：
 *   1. 去掉 UTF-8 BOM、"\r\n" 視為 "\n"            (一定做)
 *   2. 全形轉半形、忽略大小寫                        (選項)
 *   3. 逐行：去行尾空白、刪空白行、刪含關鍵字的行    (選項)
 *   4. 比對模式：Strict / Ignore Whitespace / Token
 *
 * 計算方式：
 *   - 先去掉相同的開頭與結尾。
 *   - 剩餘 n x m <= 5000 萬格：完整動態規劃 (精確)。
 *   - 更長：帶狀 (Ukkonen) 演算法，帶寬 64、128、256… 加倍；距離 <= 帶寬就是精確值。
 *     工作量超過預算 (4 億格) 才退回「逐位置比較」近似值並標記 approximate。
 */
#ifndef COMPARATOR_H
#define COMPARATOR_H

#include <stddef.h>
#include <stdint.h>

typedef enum {
    COMPARE_STRICT,            /* 逐字比較 (預設) */
    COMPARE_IGNORE_WHITESPACE, /* 刪除所有空白、Tab、換行後再比較 */
    COMPARE_TOKEN              /* 連續空白/換行都當成一個空白，並去掉頭尾空白後再比較 */
} CompareMode;

#define IGNORE_LINES_MAX 256

typedef struct {
    CompareMode mode;
    int ignore_trailing_space;   /* 忽略每行行尾空白，以及輸出最後多或少的換行 */
    int fullwidth_as_halfwidth;  /* 全形英數符號視為半形，例如 "：" = ":"、"１" = "1"、全形空白 = 空白 */
    int ignore_case;             /* 英文大小寫視為相同 */
    int ignore_blank_lines;      /* 忽略空白行 */
    char ignore_lines_with[IGNORE_LINES_MAX]; /* 含這些文字的行整行不比對，多個用 | 分隔，例如 "請輸入|請依序" */
} CompareOptions;

void compare_options_default(CompareOptions *options); /* Strict，其他選項全關 */

/*
 * 回傳差異數。
 * *approximate 會被設成 1 表示輸出太長、改用「逐位置比較」的近似值 (結果仍然穩定)。
 */
long compare_outputs(const char *expected, size_t expected_len,
                     const char *actual, size_t actual_len,
                     const CompareOptions *options, int *approximate);

/*
 * 逐字找第一個不同的位置 (給畫面提示用，換行 \r\n 視為 \n)。
 * 回傳 0 = 完全相同；1 = 有不同，*line / *column 從 1 開始 (column 以字元計)。
 */
int compare_first_difference(const char *expected, size_t expected_len,
                             const char *actual, size_t actual_len, long *line, long *column);

/*
 * UTF-8 bytes -> Unicode 字元陣列 (報告的差異標示也用這個，確保與差異數的算法一致)。
 * 去掉 BOM、"\r\n" 變成 "\n"；不合法的 byte 各自當成一個字元 COMPARE_INVALID_BYTE + byte。
 * out 至少要有 n 格。回傳字元數。
 */
#define COMPARE_INVALID_BYTE 0xDC00u
size_t compare_decode_utf8(const unsigned char *s, size_t n, uint32_t *out);

const char *compare_mode_name(CompareMode mode);       /* "strict" ... */
int compare_mode_from_name(const char *name, CompareMode *mode);

#endif
