/*
 * config.h - 批改設定 (比對方式、滿分、評分規則、時間限制、編譯失敗處理...) 與「評分模板」。
 *
 * 設定存成文字檔 grader.cfg (放在題目資料夾)，老師可以直接用記事本修改，
 * GUI 與 CLI 讀寫的是同一個檔案。格式範例：
 *
 *   full_score = 100
 *   compare = strict              # strict / ignore_whitespace / token
 *   ignore_trailing_space = 1     # 1 = 開啟
 *   ignore_lines_with = 請輸入    # 含這些字的行不比對，多個用 | 分隔
 *   score_mode = range            # per_char / per_n_char / range
 *   range = 0 0 0                 # 最小CHAR 最大CHAR 扣分
 *   range = 21 MAX ALL            # ALL = 扣光該題分數
 *   ai_fix = 1                    # 編譯失敗時呼叫本機 AI 做最小修正後繼續批改
 *
 * 註解：# 只有在行首、或前面有空白時才算註解，所以 ignore_lines_with = C#|請輸入 不會被切掉。
 */
#ifndef CONFIG_H
#define CONFIG_H

#include <stddef.h>

#include "comparator.h"
#include "scorer.h"

typedef struct {
    double full_score;
    CompareOptions compare;
    ScoreRules rules;

    int timeout_ms;
    long long output_limit;   /* bytes */
    long long memory_limit;   /* bytes，0 = 不限制 */
    int runtime_error_zero;   /* 1 = Runtime Error 的 testcase 直接 0 分；0 = 照樣比對已輸出的內容 */

    char gcc_path[512];
    char compile_flags[512];
    int keep_temp;            /* 1 = 批改完保留暫存資料夾方便除錯 */

    /*
     * 編譯失敗 (CE) 自動修正：把學生原始碼 + gcc 錯誤訊息交給本機 AI 做「最小修改」，
     * 修正字元數 (CHAR) x ai_fix_penalty_per_char = 修正扣分；修正後的程式照常執行測資再算輸出扣分。
     */
    int ai_fix;                     /* 1 = 開啟；0 = CE 直接 0 分 */
    char ai_fix_tool[256];          /* auto / claude / codex / gemini / 自訂命令列 */
    double ai_fix_penalty_per_char; /* 每修正 1 CHAR 扣幾分 */
    double ai_fix_penalty_max;      /* 修正扣分上限；0 = 不另設上限 (最多仍只扣到滿分) */
    int ai_fix_attempts;            /* 修正後仍無法編譯時最多試幾次 (含第一次) */
    int ai_fix_timeout_ms;          /* 每次呼叫 AI 的時間限制 */
} GradeConfig;

void config_default(GradeConfig *cfg);
/* 讀取設定檔，沒寫到的項目維持原值。失敗回傳 0 並寫入 err */
int config_load(const char *path, GradeConfig *cfg, char *err, size_t err_size);
int config_save(const char *path, const GradeConfig *cfg);

/* 修正 chars 個字元要扣幾分 (套用 ai_fix_penalty_per_char 與 ai_fix_penalty_max) */
double config_fix_penalty(const GradeConfig *cfg, long chars);

/*
 * 評分模板：一次設定好「比對方式 + 比對選項 + 評分規則」。
 * 滿分、時間限制、編譯設定、CE 自動修正不會被模板改變。
 */
int config_template_count(void);
const char *config_template_name(int index);
const char *config_template_description(int index);
void config_apply_template(GradeConfig *cfg, int index);
/* 目前設定符合哪個模板 (只比較比對方式與評分方式)；都不符合回傳 -1 (= 自訂) */
int config_match_template(const GradeConfig *cfg);

/* 扣分顯示：SCORE_PENALTY_ALL 顯示成 "ALL"，其他用 %g */
void format_penalty(char *out, size_t size, double penalty);
/* 解析扣分文字："ALL" / "扣光" -> SCORE_PENALTY_ALL；失敗回傳 0 */
int parse_penalty(const char *text, double *penalty);

#endif
