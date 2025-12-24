// PDFDocument.h: PDF文档数据封装类
// 用于多文档标签页支持，封装单个PDF文档的所有数据和状态

#pragma once
#include <mupdf/fitz.h>
#include <afxwin.h>
#include <map>
#include <vector>
#include <list>
#include "SearchTypes.h"  // 包含搜索相关类型定义

// 缩略图信息结构
struct ThumbnailInfo {
	HBITMAP hBitmap;
	int pageNumber;
	bool isCurrentPage;
};

// 缩放模式枚举
enum ZoomMode { ZOOM_CUSTOM, ZOOM_FIT_WIDTH, ZOOM_FIT_PAGE };

// 页面缩放状态
struct PageZoomState {
	ZoomMode zoomMode;   // 缩放模式
	float customZoom;    // 自定义缩放比例
	CPoint panOffset;    // 平移偏移量（拖拽位置）

	PageZoomState() : zoomMode(ZOOM_FIT_PAGE), customZoom(1.0f), panOffset(0, 0) {}
	PageZoomState(ZoomMode mode, float zoom, CPoint offset)
		: zoomMode(mode), customZoom(zoom), panOffset(offset) {}
};

// 页面缓存键
struct PageCacheKey {
	int pageNumber;
	int width;
	int height;
	int rotation;  // ★★★ 添加旋转角度，解决旋转后缓存混乱问题

	bool operator<(const PageCacheKey& other) const noexcept {
		if (pageNumber != other.pageNumber) return pageNumber < other.pageNumber;
		if (width != other.width) return width < other.width;
		if (height != other.height) return height < other.height;
		return rotation < other.rotation;  // ★★★ 包含旋转角度比较
	}

	bool operator==(const PageCacheKey& other) const noexcept {
		return pageNumber == other.pageNumber &&
			width == other.width &&
			height == other.height &&
			rotation == other.rotation;  // ★★★ 包含旋转角度比较
	}
};

// 页面缓存项
struct PageCacheItem {
	HBITMAP hBitmap;
};

// PDF文档类 - 封装单个PDF文档的所有数据
class CPDFDocument
{
public:
	CPDFDocument(fz_context* ctx);
	~CPDFDocument();

	// 文档操作
	bool OpenDocument(const char* filename);
	void CloseDocument();
	bool IsOpen() const { return m_doc != nullptr; }

	// 文件信息
	CString GetFilePath() const { return m_filePath; }
	CString GetFileName() const;
	int GetTotalPages() const { return m_totalPages; }

	// 页面操作
	int GetCurrentPage() const { return m_currentPage; }
	void SetCurrentPage(int page);
	fz_page* GetCurrentPageObj() const { return m_currentPageObj; }
	void LoadPageObject(int pageNumber);
	void CleanupCurrentPage();

	// 缩放操作
	ZoomMode GetZoomMode() const { return m_zoomMode; }
	float GetZoom() const { return m_zoom; }
	float GetCustomZoom() const { return m_customZoom; }
	void SetZoom(float zoom, ZoomMode mode = ZOOM_CUSTOM);

	// 旋转操作
	int GetPageRotation(int pageNumber);
	void SetPageRotation(int pageNumber, int rotation);
	std::map<int, int>& GetPageRotations() { return m_pageRotations; }

	// 页面状态管理
	void SaveCurrentPageZoomState();
	void RestorePageZoomState(int pageNumber);
	PageZoomState GetPageZoomState(int pageNumber);

	// 拖拽平移
	CPoint GetPanOffset() const { return m_panOffset; }
	void SetPanOffset(CPoint offset) { m_panOffset = offset; }
	void ResetPanOffset() { m_panOffset = CPoint(0, 0); }
	bool GetCanDrag() const { return m_canDrag; }
	void SetCanDrag(bool canDrag) { m_canDrag = canDrag; }

	// 滚动位置
	int GetScrollPosition() const { return m_scrollPosition; }
	void SetScrollPosition(int pos) { m_scrollPosition = pos; }


	// 缩略图管理
	std::map<int, ThumbnailInfo>& GetThumbnailCache() { return m_thumbnailCache; }
	void CleanupThumbnails();


	// 页面缓存管理
	std::map<PageCacheKey, PageCacheItem>& GetPageCache() { return m_pageCache; }
	std::list<PageCacheKey>& GetCacheOrder() { return m_cacheOrder; }
	void ClearPageCache();

	// 位图管理
	HBITMAP GetCurrentBitmap() const { return m_hCurrentBitmap; }
	void SetCurrentBitmap(HBITMAP hBitmap);
	void CleanupBitmap();
	HBITMAP TransferCurrentBitmap();  // 转移位图所有权（不删除）
	HBITMAP GetPanPageBitmap() const { return m_hPanPageBitmap; }
	void SetPanPageBitmap(HBITMAP hBitmap);
	void CleanupPanPageBitmap();
	HBITMAP TransferPanPageBitmap();  // 转移拖拽位图所有权（不删除）

	// MuPDF对象访问
	fz_document* GetDocument() const { return m_doc; }

	// 缩略图尺寸
	int GetThumbnailPicWidth() const { return m_thumbnailPicWidth; }
	void SetThumbnailPicWidth(int width) { m_thumbnailPicWidth = width; }
	int GetThumbnailPicHeight() const { return m_thumbnailPicHeight; }
	void SetThumbnailPicHeight(int height) { m_thumbnailPicHeight = height; }

	// 搜索信息管理
	std::vector<SearchMatch>& GetSearchMatches() { return m_searchMatches; }
	void SetSearchMatches(const std::vector<SearchMatch>& matches) { m_searchMatches = matches; }
	int GetCurrentMatchIndex() const { return m_currentMatchIndex; }
	void SetCurrentMatchIndex(int index) { m_currentMatchIndex = index; }
	CString GetSearchKeyword() const { return m_searchKeyword; }
	void SetSearchKeyword(const CString& keyword) { m_searchKeyword = keyword; }

private:
	// MuPDF对象（不拥有ctx，由外部管理）
	fz_context* m_ctx;
	fz_document* m_doc;
	fz_page* m_currentPageObj;

	// 文件信息
	CString m_filePath;
	int m_totalPages;

	// 页面状态
	int m_currentPage;
	float m_zoom;
	ZoomMode m_zoomMode;
	float m_customZoom;

	// 旋转状态
	std::map<int, int> m_pageRotations;  // 每页的旋转角度

	// 页面缩放状态
	std::map<int, PageZoomState> m_pageZoomStates;

	// 拖拽平移
	CPoint m_panOffset;
	bool m_canDrag;
	HBITMAP m_hPanPageBitmap;

	// 缩略图数据
	std::map<int, ThumbnailInfo> m_thumbnailCache;
	int m_thumbnailPicWidth;
	int m_thumbnailPicHeight;

	// 页面缓存
	std::map<PageCacheKey, PageCacheItem> m_pageCache;
	std::list<PageCacheKey> m_cacheOrder;
	static const int CACHE_LIMIT = 20;

	// 当前位图
	HBITMAP m_hCurrentBitmap;

	// 搜索信息
	std::vector<SearchMatch> m_searchMatches;
	int m_currentMatchIndex;
	CString m_searchKeyword;

	// 滚动位置
	int m_scrollPosition;
};
