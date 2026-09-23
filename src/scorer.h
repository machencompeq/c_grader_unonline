/*
 * scorer.h - 評分規則：Difference Count -> 扣分 (Penalty)。
 *
 * 三種模式，全部由設定決定，程式碼裡不寫死任何扣分數字：
 *   A. SCORE_PER_CHAR   每 1 CHAR 扣 X 分               penalty = diff * X
 *   B. SCORE_PER_N_CHAR 每 N CHAR 扣 X 分               penalty = floor(diff / N) * X
 *   C. SCORE_RANGE      區間扣分，例如 1~2 扣 1、3~5 扣 2 ...
 *
 * 扣分是「每個 testcase 各自計算」：
 *   testcase 得分 = max(0, testcase 滿分 - penalty)
 */
#ifndef SCORER_H
#define SCORER_H

#include <stddef.h>

#define MAX_RANGE_RULES 64
#define RANGE_MAX (-1)            /* 區間上限寫 MAX 時用這個值表示無限大 */
#define SCORE_PENALTY_ALL 1.0e9   /* 差異數不在任何區間內：扣光該 testcase 的分數 */

typedef enum {
    SCORE_PER_CHAR,
    SCORE_PER_N_CHAR,
    SCORE_RANGE
} ScoreMode;

typedef struct {
    long min;       /* 最小 CHAR (含) */
    long max;       /* 最大 CHAR (含)，RANGE_MAX = 無上限 */
    double penalty; /* 扣幾分 */
} RangeRule;

typedef struct {
    ScoreMode mode;

    double per_char_penalty;  /* 模式 A */

    long n_chars;             /* 模式 B */
    double per_n_penalty;

    RangeRule ranges[MAX_RANGE_RULES]; /* 模式 C */
    int range_count;
    int require_contiguous;   /* 1 = 區間之間不能有空缺 */
} ScoreRules;

typedef enum {
    RULES_OK = 0,
    RULES_WARNING = 1, /* 可以使用，但有需要注意的地方 (例如區間有空缺) */
    RULES_ERROR = 2    /* 不能使用 (例如區間重疊) */
} RulesCheck;

double score_penalty(const ScoreRules *rules, long diff);
double score_apply(double full, double penalty);  /* max(0, full - penalty) */

/* 檢查規則是否合理，問題描述會寫進 message (UTF-8，每個問題一行) */
RulesCheck score_validate(const ScoreRules *rules, char *message, size_t size);

const char *score_mode_name(ScoreMode mode);  /* "per_char" ... */
int score_mode_from_name(const char *name, ScoreMode *mode);

#endif
