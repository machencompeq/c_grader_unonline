# C 語言作業批改工具 (c_grader_unonline)

| 資料夾 | 內容 |
|---|---|
| [c_grader_v2/](c_grader_v2/) | **新版 (2026-09-23)**：完整原始碼 (`src/`)、測試 (`tests/`)、工具腳本 (`tools/`)、文件 (`docs/`)、可直接執行的 `Grader.exe`。新增本機 AI 修正編譯錯誤、Myers 位元平行比對、diff-match-patch 風格的差異報告、深色科技風介面。 |
| [c_grader_v1_stable/](c_grader_v1_stable/) | **舊版穩定執行檔** (只有 `Grader.exe`，說明見該資料夾 README)。 |

新版怎麼用、怎麼重新編譯：見 [c_grader_v2/README.md](c_grader_v2/README.md)；
程式架構與比對演算法：[c_grader_v2/docs/ARCHITECTURE.md](c_grader_v2/docs/ARCHITECTURE.md)、[c_grader_v2/docs/COMPARATOR.md](c_grader_v2/docs/COMPARATOR.md)；
審查報告：[c_grader_v2/docs/REVIEW-2026-09-23.md](c_grader_v2/docs/REVIEW-2026-09-23.md)。

重新編譯需要 MinGW-w64 (gcc、windres、mingw32-make)：

```
cd c_grader_v2
mingw32-make all test        # gcc 不在 PATH 時：mingw32-make CC=<gcc.exe> WINDRES=<windres.exe> all test
```
