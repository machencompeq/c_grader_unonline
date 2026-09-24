/*
 * feedback.h - 給學生看的白話說明：「哪裡錯了」。
 *
 * 不呼叫 AI，全部由比對結果推導 (同一份輸出永遠得到同一段說明)：
 *   feedback_output          標準答案 vs 學生輸出：常見錯誤類型的提示 + 逐條列出不同的行
 *   feedback_compile_errors  gcc 錯誤訊息翻成白話 (少了分號、名稱拼錯、全形字元…)
 *
 * 輸出是純文字，每條一行 (以 '\n' 分隔)，由呼叫者決定怎麼顯示 (報告會做 HTML 跳脫)。
 */
#ifndef FEEDBACK_H
#define FEEDBACK_H

#include <stddef.h>

#include "util.h"

#define FEEDBACK_MAX_ITEMS 5 /* 最多列幾條不同的行 / 幾個編譯錯誤 */

/*
 * hidden = 1：隱藏測資，只給錯誤類型 (例如「數字不同」)，不透露任何一行的內容。
 * 兩段完全相同時不輸出任何東西。
 */
void feedback_output(const char *expected, size_t expected_len, const char *actual, size_t actual_len, int hidden,
                     StrBuf *out);

void feedback_compile_errors(const char *compile_log, StrBuf *out);

#endif
