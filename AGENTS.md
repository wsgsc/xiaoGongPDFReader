# AGENTS.md - XiaoGongPDF Reader

This file provides guidance for AI agents working with the XiaoGongPDF codebase.

## Project Overview

小龚PDF阅读器 (XiaoGongPDF) is a lightweight PDF reader built with MFC (Microsoft Foundation Classes) and MuPDF. Designed for Chinese users with a focus on privacy — no network code, no data collection, all operations are local-only.

**Key Technologies:**
- MFC (Windows desktop framework, `CDialogEx`-based main window)
- MuPDF library for PDF rendering (`fz_*` API)
- Visual Studio 2022 with v143 platform toolset
- C++17, Unicode character set (`_T()` / `CString`)
- Windows API for UI and system integration

## Build Commands

> **AI agents must NOT run build commands.** The developer compiles manually. Do not invoke `msbuild`, `devenv`, or any compiler tool.

### Building Manually (developer only)
Open `XiaoGongPDF.sln` in Visual Studio and build, or from a Visual Studio Developer Command Prompt:
```
msbuild XiaoGongPDF.sln /p:Configuration=Debug /p:Platform=x64
msbuild XiaoGongPDF.sln /p:Configuration=Release /p:Platform=x64
msbuild XiaoGongPDF.sln /p:Configuration=Debug /p:Platform=Win32
msbuild XiaoGongPDF.sln /p:Configuration=Release /p:Platform=Win32
```

### Output Locations
| Configuration | Output |
|---|---|
| Debug\|x64 | `x64\Debug\XiaoGongPDF.exe` |
| Release\|x64 | `x64\Release\XiaoGongPDF.exe` |
| Debug\|Win32 | `Debug\XiaoGongPDF.exe` |
| Release\|Win32 | `Release\XiaoGongPDF.exe` |

### MuPDF Library Configuration
Expected at `D:\workspace\mupdf\`. Required libs: `libmupdf.lib`, `libresources.lib`, `libthirdparty.lib`.
If path differs, update `<AdditionalIncludeDirectories>` and `<AdditionalLibraryDirectories>` in `XiaoGongPDF.vcxproj`.

### Tests
No automated tests exist. Manual test checklist:
1. Open/close multiple PDFs simultaneously (multi-tab)
2. All zoom modes: fit-page, fit-width, custom (Ctrl+scroll)
3. Rotation (0°/90°/180°/270°) — verify cache key correctness
4. Continuous scroll and single-page modes
5. Fullscreen enter/exit — verify no zoom/offset change on exit
6. Search across documents
7. Large PDFs (>100 pages) — verify LRU cache eviction
8. Print with various page ranges
9. Drag-and-drop file open

## Source File Layout

| File | Purpose |
|---|---|
| `XiaoGongPDFDlg.h/.cpp` | Main dialog — rendering, layout, all event handling |
| `PDFDocument.h/.cpp` | Per-document state (pages, cache, zoom, thumbnails) |
| `CustomButton.h/.cpp` | Owner-drawn button with hover/press effects |
| `ModernTabCtrl.h/.cpp` | Custom tab bar for multi-document |
| `PDFEditDialog.h/.cpp` | Page-level edit dialog (reorder/delete pages) |
| `ProgressDlg.h/.cpp` | Background load progress dialog |
| `ShortcutsDialog.h/.cpp` | Keyboard shortcuts help dialog |
| `SearchTypes.h` | `SearchMatch` struct (shared, avoids circular deps) |
| `pch.h / pch.cpp` | Precompiled header |
| `XiaoGongPDF.rc / resource.h` | Resources, dialog/control IDs, string tables |

## Code Style Guidelines

### Naming Conventions
- **Member variables**: `m_` prefix — e.g., `m_currentPage`, `m_isFullscreen`
- **Pointer variables**: `p` prefix — e.g., `pDoc`, `pDC`
- **MFC classes**: `C` prefix — e.g., `CXiaoGongPDFDlg`, `CPDFDocument`, `CCustomButton`
- **Public methods**: PascalCase — e.g., `RenderPage()`, `GoToPage()`
- **Private/helper methods**: camelCase — e.g., `calculateScale()`, `updateLayout()`
- **Constants**: `UPPER_CASE` with underscores — e.g., `CACHE_LIMIT`, `PAGE_SPACING`
- **Enums**: PascalCase values — e.g., `ZOOM_CUSTOM`, `ZOOM_FIT_WIDTH`, `ZOOM_FIT_PAGE`

### Include Order
```cpp
#include "pch.h"          // 1. Precompiled header — always first
#include "framework.h"    // 2. MFC framework
#include <commctrl.h>     // 3. Windows / system headers
#include <mupdf/fitz.h>   // 4. Third-party (MuPDF)
#include "XiaoGongPDF.h"  // 5. Project headers
#include "XiaoGongPDFDlg.h"
#include "PDFEditDialog.h"
```

### Formatting
- **Indentation**: tabs (Visual Studio default — do not convert to spaces)
- **Braces**: Allman style — opening brace on its own line
- **Line length**: 80–120 characters preferred
- **Spacing**: space after commas, around binary operators
- **Comments**: Chinese for user-facing logic; English for technical/algorithm details

### Type Usage
- **Strings**: `CString` for all UI and file-path strings; use `_T("...")` literals
- **Encoding conversion**: `CStringA` / `CStringW` for MuPDF UTF-8 paths
- **Raw pointers**: MFC objects (`CWnd*`, `CDC*`) and MuPDF objects (`fz_page*`)
- **Owned heap objects**: `std::unique_ptr` where possible; explicit `delete` otherwise
- **Collections**: `std::vector`, `std::map`, `std::list` from STL
- **Integer types**: `int` for page numbers/coordinates; `DWORD`/`UINT` for Windows API; `size_t` for sizes
- **Booleans**: `bool` for C++ logic; `BOOL` only when required by MFC/Win32 API

### Error Handling
- Wrap all MuPDF calls with `fz_try(m_ctx) { ... } fz_catch(m_ctx) { ... }`
- Always drop MuPDF resources (`fz_drop_page`, `fz_drop_pixmap`, etc.) in `fz_catch` as well
- Use `TRACE(_T("..."))` for debug logging — **only inside `#ifdef _DEBUG` blocks**
- Check `GetSafeHwnd()` before calling methods on MFC controls that may not be initialized

### Memory Management
- **HBITMAP**: always `DeleteObject(hBmp)` before reassigning or on cleanup; never leak
- **MuPDF resources**: release in reverse order — page → document → context
- **LRU caches**: page cache limit = `CACHE_LIMIT` (50); thumbnail cache = `THUMBNAIL_CACHE_LIMIT` (100)
- **Document owns bitmaps**: UI holds references only — do not delete bitmaps owned by `CPDFDocument`

## Architecture Patterns

### Multi-Document Model
- `CXiaoGongPDFDlg` owns `std::vector<CPDFDocument*> m_documents`
- Each tab ↔ one `CPDFDocument` instance
- `m_activeDocIndex` tracks the active tab; call `GetActiveDocument()` to retrieve it
- All per-document state (page, zoom, rotation, search results, cache) lives in `CPDFDocument`

### Rendering Pipeline (Continuous Scroll Mode)
1. `CalculatePagePositions()` — computes `m_uniformScale` from current view width and `m_customZoom`, fills `m_pageYPositions[]` and `m_pageHeights[]`
2. `UpdateScrollBar()` — sets scroll range; returns immediately if `m_isFullscreen`
3. `RenderVisiblePages()` — iterates visible pages, checks LRU cache, calls MuPDF to render misses, composites into a single bitmap, passes to `CPDFViewCtrl`

**Always call `CalculatePagePositions()` before `RenderVisiblePages()`** when the view size changes (e.g., in `OnSize`), otherwise `m_uniformScale` will be stale.

### Zoom Modes
| Mode | Behaviour |
|---|---|
| `ZOOM_FIT_PAGE` | `scale = min(viewW/pageW, viewH/pageH) * 0.95` |
| `ZOOM_FIT_WIDTH` | `scale = viewW/pageW * 0.95` |
| `ZOOM_CUSTOM` | `scale = m_customZoom` directly |

In continuous scroll mode, `m_uniformScale = baseScale * m_customZoom` where `baseScale` is always fit-width relative to the widest page.

### Cache Key Rule
`PageCacheKey` includes `{pageNumber, width, height, rotation}`. **Always include rotation** — omitting it causes stale bitmaps after page rotation.

### Fullscreen State
Saved before entering fullscreen: `m_savedZoomMode`, `m_savedCustomZoom`, `m_savedPanOffset`, `m_savedScrollPosition`.
On exit: restore all four directly (do not use `SetZoom()` for restoration — it has scroll-ratio side effects); then call `CalculatePagePositions()` + `RenderVisiblePages()`.

### Flicker Prevention
- `CPDFViewCtrl::StaticWndProc` intercepts `WM_ERASEBKGND` → returns `TRUE` (suppresses erase)
- `OnSize` wraps layout changes in `SetRedraw(FALSE)` / `SetRedraw(TRUE)` + `RedrawWindow(...)`
- Use `Invalidate(FALSE)` (not `TRUE`) when triggering repaints

## Common Pitfalls

1. **Stale `m_uniformScale`**: call `CalculatePagePositions()` whenever view width changes before rendering
2. **Cache corruption after rotation**: `PageCacheKey` must include `rotation`
3. **HBITMAP leak**: every `CreateCompatibleBitmap` / `CreateDIBSection` needs a matching `DeleteObject`
4. **Status bar shown in fullscreen**: `UpdateStatusBar()` must check `m_isFullscreen` before `ShowWindow(SW_SHOW)`
5. **Scrollbar shown in fullscreen**: `UpdateScrollBar()` must return early when `m_isFullscreen`
6. **MuPDF context sharing**: `m_ctx` is owned by the main dialog; `CPDFDocument` holds a non-owning pointer
7. **UI thread safety**: never call MFC UI methods from the background load thread (`PDFLoadThreadProc`); post a message instead
8. **`SetZoom` skip optimisation**: `SetZoom` returns early if mode and value are identical — force a different value first if a re-render is required

## Version Management
- Version number in `XiaoGongPDF.rc` (`FILEVERSION` / `PRODUCTVERSION`), pattern `Major.Minor.Patch.0`
- Update on every user-visible release
