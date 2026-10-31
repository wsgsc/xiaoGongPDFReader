# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

小龚PDF阅读器 (XiaoGongPDF) is a lightweight PDF reader built with MFC (Microsoft Foundation Classes) and MuPDF. It's designed for Chinese users with a focus on privacy — no network code, no data collection, all operations are local-only.

**Key Technologies:**
- MFC (Windows desktop framework)
- MuPDF library for PDF rendering
- Visual Studio 2019+ with v143 platform toolset
- C++, Unicode character set
- GDI+ for supplementary graphics

## Build Commands

Open the solution in Visual Studio or use MSBuild:

```
msbuild XiaoGongPDF.sln /p:Configuration=Release /p:Platform=x64
```

Build configurations: Debug|x64 (development), Release|x64 (distribution), Debug|Win32, Release|Win32.

The project expects MuPDF at `D:\workspace\mupdf\`. If your installation is elsewhere, update `<AdditionalIncludeDirectories>`, `<AdditionalLibraryDirectories>`, and `<AdditionalDependencies>` in `XiaoGongPDF.vcxproj` (Debug|x64 around line 96-101, Release|x64 around line 171-178). Required libs: `libmupdf.lib`, `libresources.lib`, `libthirdparty.lib`.

Output:
- Debug: `x64\Debug\XiaoGongPDF.exe`
- Release: `x64\Release\XiaoGongPDF.exe`

## Architecture

### Core Components

**Application Layer (`XiaoGongPDF.cpp/h`)**
- `CXiaoGongPDFApp`: Main application class, single-instance mutex, GDI+ initialization.

**Main Dialog (`XiaoGongPDFDlg.cpp/h`)**
- `CXiaoGongPDFDlg`: Primary UI window managing multi-document tabs, zoom/pan/rotate/search, keyboard shortcuts, recent files (max 10, stored in Windows Registry), drag-and-drop file open, and IPC via `WM_COPYDATA`.
- Contains a cached `fz_context*` pointer borrowed from the active `CPDFDocument` — does NOT own a context.
- Coordinates between PDF rendering and UI display across two modes: single-page and continuous scroll.

**PDF Document Model (`PDFDocument.cpp/h`)**
- `CPDFDocument`: Encapsulates a single PDF document. Each instance owns its own `fz_context` (created in constructor with 256MB store limit), ensuring thread-safe isolation between documents.
- Manages per-document state: current page, zoom, rotation angles, page bounds cache, scroll position, search results.
- Page cache with LRU eviction (50 pages max), thumbnail cache (100 thumbnails max), per-page zoom states.

**PDF Edit Dialog (`PDFEditDialog.cpp/h`)**
- Page management: reorder, delete, rotate pages. Import/merge PDFs or images. Drag-and-drop reordering with auto-scroll and easing animations.

**Custom Controls**
- `CPDFViewCtrl`: Subclassed static control for PDF rendering. Blocks `WM_ERASEBKGND` to prevent flicker, forwards scroll messages to parent, custom `OnPaint()` for scroll-aware rendering.
- `ModernTabCtrl`: Tab control with close buttons, hover effects, custom drawing.
- `CustomButton`: Custom-drawn buttons.

**Supporting Dialogs**
- `ShortcutsDialog`, `ProgressDlg`, `CCustomPrintDlg` (printer enumeration, page ranges, duplex).

### Two Rendering Modes

The app supports two rendering modes that share the dialog's scroll infrastructure:

**Single-Page Mode:** Only the current page is rendered and displayed. Navigation jumps between pages. Zoom/pan apply to the current page.

**Continuous Scroll Mode:** All pages are rendered in a vertical column with configurable spacing (`PAGE_SPACING = 10px`). A single virtual scroll area spans the total height of all pages. `CalculatePagePositions()` precomputes per-page Y positions and heights based on a uniform scale factor (`m_uniformScale`). `RenderVisiblePages()` renders only pages intersecting the current viewport. The mode is toggled via the UI and controls which scroll handler is active.

### Background PDF Loading

Opening a PDF spawns a worker thread (`PDFLoadThreadProc`) that loads the document via MuPDF in the background. A progress dialog is shown during loading. On completion, a custom message (`WM_PDF_LOAD_COMPLETE`) is posted back to the main thread with the result. This keeps the UI responsive for large files.

The main dialog tracks the thread via `m_pLoadThread` and load parameters via `m_pLoadParams`. `CleanupPendingLoad()` safely cancels an in-progress load (e.g., when the user closes the dialog).

### Context & Thread Safety

- Each `CPDFDocument` creates and owns its own `fz_context` (256MB store limit). No context is shared between documents.
- The main dialog's `m_ctx` is a convenience pointer set to the active document's context — it is never created or destroyed by the dialog. Always go through `GetActiveDocument()` to get the authoritative context for the current tab.
- MuPDF contexts are NOT thread-safe. All rendering for a given document must happen on the thread that owns its context. The background load thread creates/uses the document's context briefly during `OpenDocument()`, then all subsequent rendering happens on the main thread.

### Caching Strategy

- **Page Cache** (per-document, in `CPDFDocument`): LRU cache keyed by `PageCacheKey` (page number + dimensions + rotation). Limit 50. Rotation in the key prevents stale cached bitmaps after rotation changes.
- **Thumbnail Cache** (per-document): LRU cache for the thumbnail panel. Limit 100.
- **Page Bounds Cache** (per-document): Pre-computed original page boundaries stored on document open. Rotation-aware — bounds stored as raw coordinates, rotation applied via transform at query time.
- **Continuous Scroll Cache** (in main dialog): `m_continuousPageBitmaps` holds rendered pages visible in the current scroll viewport. Uses the dialog's own `PageCacheKey`/`PageCacheItem` types (duplicated from `PDFDocument.h`).

### Key Invariants

- **Rotation**: Each page can have independent rotation (0°/90°/180°/270°) stored in `m_pageRotations`. Both `PageCacheKey` and search highlight transforms account for rotation.
- **Flicker prevention**: `CPDFViewCtrl` blocks `WM_ERASEBKGND`. All drawing is done with complete content in `OnPaint()`. Critical on high-refresh-rate displays.
- **Resize debounce**: `OnSize` uses a 60ms debounce timer (`TIMER_ID_RESIZE_DEBOUNCE`) plus a 33ms throttle during drag resize to avoid high-frequency full re-renders.
- **State save/restore**: Per-page zoom state is saved when navigating away and restored when returning. On tab switch, `SaveDialogStateToDocument()` / `LoadDialogStateFromDocument()` transfer UI state (including bitmap ownership) to/from the document object.

## Code Style

- Hungarian notation: `m_` for members, `p` for pointers.
- Chinese comments throughout (project targets Chinese users).
- MFC message maps, DDX/DDV, AFX macros.
- Unicode: `CString` throughout (Unicode mode enabled).
- `TRACE()` macros for debug-only logging.
- Resource IDs in `resource.h`, dialog/string resources in `XiaoGongPDF.rc`.
