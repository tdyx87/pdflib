# 与 libharu 的一致性分析与兼容层

## 1. 结论

**当前 `pdflib.dll` 的原生导出函数与 libharu 并不一致，用法也不一致。**

- `src/pdflib.def` 导出 **87** 个 `pdf_*` / `pdflib_*` 函数；
- libharu 2.4.4 的公开 `hpdf.h` 声明了 **281** 个 `HPDF_*` 函数；
- 两边的名字**没有任何一个重合**（`pdf_doc_create` vs `HPDF_New`，`pdf_page_add` vs `HPDF_AddPage`，……）；
- 调用模型也不同：libharu 用 `NULL` 返回 + `HPDF_GetError()`，pdflib 用 `pdf_status` 返回码；libharu 有 `HPDF_Page_BeginText()/ShowText()/EndText()` 文本对象，pdflib 只有一次性的 `pdf_page_draw_text()`。

因此，**不能把现有 `pdflib.dll` 直接当作 libharu 的替代品**。

为了满足“用法尽量与 libharu 一致，同时多一层 PoDoFo 支持”的目标，本次新增了
`compat/` 兼容层：

- 头文件：`compat/include/hpdf.h`（libharu 2.4.4 公开 API 的源码兼容版本）；
- 实现：`compat/hpdf_compat.c`（`HPDF_*` → `pdf_*`）；
- 产物：`hpdf.lib`（静态库，可直接替换 libharu 的 `hpdf.lib`）和
  `pdflib_hpdf.dll`（动态库，cdecl 导出）；
- 后端：`HPDF_SetBackend()` 或环境变量 `PDFLIB_BACKEND` 选择
  `libharu` / `podofo` / `dummy` / `auto`。

兼容层覆盖了 **281 个公开入口中的全部**：**219 个是真实实现**，
**65 个是返回 `HPDF_UNSUPPORTED_FUNC` 的冷门入口**（3D、ICC、shading、
page label、attachment、raw image 等）。常用的文档、页面、字体、文本、
图形、图片、注释、书签、加密、压缩、内存流都已可用。

---

## 2. 原生 API 与 libharu 的用法差异

| 场景 | libharu | pdflib 原生 API |
|---|---|---|
| 创建文档 | `HPDF_Doc pdf = HPDF_New(err, ud);` | `pdf_doc doc; pdf_doc_create(&doc);` |
| 页面尺寸 | `HPDF_AddPage()` 后 `HPDF_Page_SetSize(page, HPDF_PAGE_SIZE_A4, HPDF_PAGE_PORTRAIT)` | `pdf_page_add(doc, PDF_PAGE_A4, PDF_ORIENTATION_PORTRAIT, &page)` |
| 字体 | `HPDF_GetFont(pdf, "Helvetica", NULL)` | `pdf_font_get_std(doc, PDF_STD_FONT_HELVETICA, &font)` |
| 文本 | `BeginText` + `SetFontAndSize` + `TextOut/ShowText` + `EndText` | `pdf_page_draw_text(page, font, text, x, y, size)` |
| 文本换行 | `HPDF_Page_TextRect(...)` | `pdf_page_draw_text_wrapped(...)` |
| 颜色 | `SetRGBFill/SetRGBStroke/...` | `pdf_set_fill_color/pdf_set_stroke_color` + `pdf_color` 结构 |
| 路径 | `MoveTo/LineTo/CurveTo/... + Stroke/Fill` | `pdf_path_move_to/... + pdf_path_paint` |
| 图片 | `HPDF_LoadPngImageFromFile` + `HPDF_Page_DrawImage` | `pdf_image_load_file` + `pdf_page_draw_image` |
| 注释 | `HPDF_Page_CreateTextAnnot/...` | `pdf_annot_create` + setter |
| 书签 | `HPDF_CreateOutline` + `HPDF_Destination` | `pdf_outline_add` + `pdf_page` |
| 保存 | `HPDF_SaveToFile` / `HPDF_SaveToStream` | `pdf_doc_save_to_file` / `_memory` / `_stream` |
| 错误 | `NULL` + `HPDF_GetError(pdf)` | `pdf_status` 返回码 + `pdflib_last_error(doc)` |

结论：**风格相近，但函数名、错误模型、文本对象模型都不同**，现有 libharu
源码不能直接编译链接到原生 `pdflib.dll`。

---

## 3. 本次新增的兼容层

### 3.1 头文件

`compat/include/hpdf.h` 来自 libharu 2.4.4 的公开头文件，保留：

- `HPDF_Doc`、`HPDF_Page`、`HPDF_Font`、`HPDF_Image`、`HPDF_Annotation`、
  `HPDF_Outline`、`HPDF_Destination`、`HPDF_Encoder`、`HPDF_ExtGState` 等句柄；
- `HPDF_PageSizes`、`HPDF_PageDirection`、`HPDF_TextAlignment`、
  `HPDF_TextRenderingMode`、`HPDF_BlendMode`、`HPDF_AnnotIcon` 等枚举；
- 全部 281 个公开函数声明和错误码（`hpdf_errors.h`）。

差异：强制使用 libharu 的 `HPDF_SHARED` 句柄形态（`void *`），因此不需要
libharu 的内部头文件（`hpdf_doc.h`、`hpdf_pdfa.h` 等）。正常应用代码只传递
句柄，所以源码兼容。

### 3.2 实现映射

| libharu | pdflib |
|---|---|
| `HPDF_New/Free/NewDoc/FreeDoc` | `pdf_doc_create/free` |
| `HPDF_AddPage` | `pdf_page_add(A4)` |
| `HPDF_Page_SetWidth/Height/SetSize` | `pdf_page_set_size`（本次新增） |
| `HPDF_GetFont` | `pdf_font_get_std` / `pdf_font_find_system` |
| `HPDF_LoadTTFontFromFile` | `pdf_font_load_ttf` |
| `HPDF_Page_BeginText/ShowText/TextOut` | 兼容层跟踪文本矩阵，调用 `pdf_page_draw_text` |
| `HPDF_Page_MoveTo/LineTo/CurveTo/...` | `pdf_path_*` |
| `HPDF_Page_Clip/Eoclip` | `pdf_path_clip`（本次新增） |
| `HPDF_Page_SetRGB/CMYK/Gray Fill/Stroke` | `pdf_set_fill_color/pdf_set_stroke_color` |
| `HPDF_CreateExtGState` + `SetExtGState` | `pdf_set_opacity/pdf_set_blend_mode` |
| `HPDF_LoadPng/JpegImageFrom*` | `pdf_image_load_*` |
| `HPDF_Page_DrawImage` | `pdf_page_draw_image` |
| `HPDF_Page_CreateTextAnnot/URILink/Link/...` | `pdf_annot_create` |
| `HPDF_CreateOutline` + `Destination` | `pdf_outline_add` + `pdf_outline_set_destination`（本次新增） |
| `HPDF_SetInfoAttr/GetInfoAttr` | `pdf_doc_set_info/get_info` |
| `HPDF_SetCompressionMode` | `pdf_doc_set_compression` |
| `HPDF_SetPassword/Permission/EncryptionMode` | 延迟到保存前调用 `pdf_doc_set_encryption` |
| `HPDF_SaveToFile/Stream/GetContents` | `pdf_doc_save_to_file/memory` |

### 3.3 后端选择（“多一层 PoDoFo 支持”）

```c
#include "hpdf.h"

/* 默认 AUTO：优先 PoDoFo，其次 libharu。 */
HPDF_SetBackend(PDFLIB_HPDF_BACKEND_PODOFO);
/* 或 PDFLIB_HPDF_BACKEND_LIBHARU / DUMMY / AUTO */
```

也可以在进程启动前设置环境变量：

```bat
set PDFLIB_BACKEND=podofo
```

同一个 libharu 源码，不重新编译，只切换后端，即可分别由 libharu 或 PoDoFo
渲染。

### 3.4 多后端核心 + libharu 门面

需要强调：**pdflib 不是“libharu 的封装”，而是多后端核心，libharu 只是
当前编译进来的一个后端。** 当前后端是 libharu 和 PoDoFo，后续可以继续增加
其他 PDF 库。

```
libharu 兼容层 (HPDF_*)
          |
        pdf_*  (稳定 C API)
          |
   pdf_backend_vtable  (SPI)
     /          \
 libharu       PoDoFo      ... 以后新增的后端
```

- `HPDF_*` 只调用 `pdf_*`，**不知道当前是哪个引擎**；新增后端不需要改
  `compat/hpdf_compat.c`。
- 新增后端只需实现 `pdf_backend_vtable`、注册、加入 `pdf_init.c` 和 CMake；
  未实现的槽位保持 `NULL`，核心统一返回 `PDF_ERR_BACKEND_UNSUPPORTED`
  （`HPDF_*` 侧映射为 `HPDF_UNSUPPORTED_FUNC`）。
- 当前 `pdf_backend` 是固定枚举，`PDF_BACKEND_MAX = 8`。新增后端时追加
  枚举值即可；`HPDF_SetBackend()` 接受任意**已注册**的 id，不需要改兼容层。
  `HPDF_GetBackendName()` 只有一个小的名字 switch，未知 id 返回
  `"unknown"`。
- 更好的长期方案是**按名字选择后端**（例如
  `pdflib_set_backend_by_name("podofo")` / `HPDF_SetBackendByName(...)`），
  这样新增后端不需要暴露新的 ABI 枚举值；这是建议的下一步。

### 3.5 集成方式

**静态方式（推荐，最接近 libharu 的用法）：**

```bat
cl /I compat\include app.c hpdf.lib pdflib.lib
:: 运行目录需要 pdflib.dll
```

**DLL 方式：**

```bat
cl /I compat\include app.c pdflib_hpdf.lib
:: 运行目录需要 pdflib_hpdf.dll + pdflib.dll
```

**CMake：**

```cmake
add_subdirectory(pdflib)
target_link_libraries(app PRIVATE hpdf)   # hpdf 已 PUBLIC 链接 pdflib
```

安装后头文件在 `include/hpdf.h`，库在 `lib/hpdf.lib`（及
`pdflib_hpdf.dll`）。

---

## 4. 语义差异与已知限制

兼容层是“源码/链接级”兼容，不是 libharu 的二进制 ABI 替换：

1. **调用约定**：导出为 cdecl（与 libharu 静态库默认一致），不是
   `HPDF_DLL` 的 stdcall。若你原来链接的是 libharu 的 stdcall DLL，
   需要重新编译，不能直接替换 `hpdf.dll`。
2. **文本对象**：兼容层用 `pdf_page_draw_text` 模拟 `BT/ET`。字间距、
   词间距、水平缩放会影响后续文本的推进量，但不会改变每个字形的轮廓
   （pdflib 没有逐字形的文本状态 API）。文本矩阵的平移/旋转/缩放通过
   `q cm ... Q` 近似实现。
3. **`HPDF_Page_TextOut`**：在非单位文本矩阵下按绝对用户坐标放置，
   而不是 libharu 的“相对当前文本矩阵”换算。常见用法（矩阵为单位阵）
   完全一致。
4. **不支持项**：3D 注释/测量/视图、ICC、shading、page label、
   attachment、raw image、XObject、slide show、PDF/A、Type1 字体、
   `HPDF_GetEncoder` 的编码转换（返回稳定哑句柄，pdflib 内部自行处理
   UTF-8/WinAnsi）。这些函数返回 `HPDF_UNSUPPORTED_FUNC`，不会静默成功。
5. **字体**：标准 14 字体由后端处理；TTF 从文件加载后，用
   `HPDF_GetFont(pdf, 返回的名字, ...)` 取回。libharu 后端不支持
   `embed=0`，PoDoFo 支持。
6. **流式文档**：libharu 兼容层总是创建普通（内存）文档；`HPDF_SaveToStream`
   是内存流，不是 pdflib 的 `pdf_doc_create_streamed` 原生流式。需要
   O(1) 页内存时请使用原生 `pdf_*` API。
7. **加密**：libharu 兼容层只映射 RC4-40/RC4-128（`HPDF_ENCRYPT_R2/R3`）；
   是否真正加密取决于后端（PoDoFo 当前构建无 OpenSSL，会返回
   `HPDF_UNSUPPORTED_FUNC`；libharu 后端可用）。
8. **`HPDF_GetVersion`** 返回 `"2.4.4 (pdflib compat)"`，不是 libharu
   的真实版本字符串。

---

## 5. 本次同时修复的代码问题

| 问题 | 位置 | 处理 |
|---|---|---|
| Windows 下 `pdf_ensure_backends()` 竞争失败线程立即返回，可能看到半注册表 | `pdflib/pdf_init.c` | 失败线程自旋等待状态 2 |
| `pdf_page_draw_text/_aligned` 传 NULL 字体句柄会解引用崩溃 | `pdflib/pdf_core.c` | 增加 `!font` 校验 |
| 流式文档上 `save_to_*` 返回 `PDF_ERR_BACKEND_FAILED`，与文档/README 的 `PDF_ERR_INVALID_STATE` 不符 | `pdflib/pdf_core.c`, `pdf_api.h` | 新增 `PDF_ERR_INVALID_STATE=9` 并统一返回 |
| `pdf_doc_get_info` 在 `buf==NULL` 且 `*len!=0` 时误报参数错误 | `pdflib/pdf_core.c` | 只要求 `inout_len` 非空 |
| `pdf_page_draw_text_wrapped` 的 512 字节行缓冲会截断长段落 | `pdflib/pdf_core.c` | 按输入长度动态分配 |
| `pdf_path_discard` 在 PoDoFo 后端只 `Close()`，路径仍残留，下次绘制会重画 | `pdf_backend_podofo.cpp` | 改用 `EndPath()`（PDF `n`） |
| PoDoFo 文本宽度按 UTF-8 单字节测量，CJK 换行/对齐漂移 | `pdf_backend_podofo.cpp` | 解码 UTF-8 后用 `UnicodeCharWidth` |
| libharu 图片加载失败后错误锁存，后续调用静默 no-op | `pdf_backend_libharu.c` | 失败路径 `HPDF_ResetError` |
| libharu 标准字体使用 StandardEncoding，与文档/ PoDoFo 的 WinAnsi 不一致 | `pdf_backend_libharu.c` | 改为 `WinAnsiEncoding` |
| `pdf_annot_create` 对 NULL rect/out 返回 INVALID_HANDLE | `pdflib/pdf_core.c` | 改为 INVALID_PARAM |
| `pdf_annot_link_to_page` / `pdf_outline_add` 可能把 NULL 页指针传给后端 | `pdflib/pdf_core.c` | 在核心层拒绝 |
| 缺少“页面改尺寸 / 路径裁剪 / 书签改目标”能力，libharu 兼容层无法映射 | `pdf_api.h`, vtable, 两个后端 | 新增 `pdf_page_set_size`、`pdf_path_clip`、`pdf_outline_set_destination` |
| podofo 初始创建只捕获 `std::exception`，非 std 异常会穿出 C ABI | `pdf_backend_podofo.cpp` | 补 `catch (...)` |
| `pdflib-build-env.cmd` 在受限 shell 中 `for /f` 无法启动子 cmd，SDK 探测失败 | `pdflib-build-env.cmd` | 改为直接探测已知 SDK 版本目录 |

### 仍未解决/需要权衡的点

- **per-color alpha**：`pdf_color.alpha` 在 `pdf_set_fill_color/pdf_set_stroke_color`
  中未被两个后端完整应用；请使用 `pdf_set_opacity`。这是原生 API 的已知
  语义缺口，兼容层通过 ExtGState 走 `pdf_set_opacity`。
- **标准字体的 UTF-8**：libharu 后端把 UTF-8 字节直接交给 WinAnsi 标准字体，
  非 ASCII 字符不会自动转码；TTF（UTF-8 编码）正常。PoDoFo 后端对标准字体
  也会按 UTF-8 处理。需要完整 Unicode 标准字体支持时，请加载 TTF。
- **`pdf_set_line_dash` 最多 8 个 pattern**，超出被静默截断；libharu 支持
  100 个。兼容层同样截断。
- **PoDoFo 加密**：当前 conan 构建 `with_openssl=False`，AES/RC4 均不可用；
  需要加密时用 libharu 后端。
- **原生 DLL 导出表仍是 87 个 `pdf_*`**；`HPDF_*` 在独立的 `hpdf.lib` /
  `pdflib_hpdf.dll` 中，避免与静态链接进 `pdflib.dll` 的 libharu 符号冲突。

---

## 6. 验证

本次改动在 x86（MSVC 2017，/MT，静态 libharu + PoDoFo）上验证：

- `pdflib_apitest all`：**947 项检查，0 失败**（两个后端，含新增的
  `page_set_size`、`path_clip`、`outline_set_destination`）；
- `tools/check_api_coverage.py`：**87 个导出全部被 apitest 覆盖**；
- `compat/tests/hpdf_smoke.c`：在 libharu 和 PoDoFo 两个后端上全部通过；
- **未经修改的 libharu demo** `text_demo.c`、`line_demo.c`、`arc_demo.c`、
  `link_annotation.c` 用 `compat/include/hpdf.h` 编译、链接 `hpdf.lib` 后
  正常运行并生成有效 PDF（页面、文本、路径、裁剪、链接注释均可读回）；
- `hpdf.lib` 静态库可直接用 `lib.exe` 生成并被 demo 链接。

> 说明：本机受限 shell 中 CMake 的编译器探测会被沙箱拦截，因此上述结果
> 通过等价的 `cl.exe`/`link.exe`/`lib.exe` 命令复核；CMake 目标已写入
> `standalone/CMakeLists.txt` 和顶层 `CMakeLists.txt`，正常环境可直接构建。

---

## 7. 建议的后续路线

1. 用原生 `pdf_*` API 扩展文本状态（`Tc/Tw/Tz/TL/Ts`）和逐字形裁剪，
   让兼容层的 `ShowText` 与 libharu 的字节级输出更接近。
2. 扩展 `pdf_*` 的 page layout / viewer preference / page label，减少
   `HPDF_*` 的 no-op 入口。
3. 为兼容层增加 `HPDF_LoadPngImageFromFile2` 等边缘入口的真实实现，
   并补齐 PDF/A、attachment 等能力（需要后端支持）。
4. 增加 x64 与 libharu XOR PoDoFo 的组合 CI，尤其是 `pdflib_hpdf.dll`
   的导出表检查（可在 `.def` 中固定 HPDF_* 名单）。
