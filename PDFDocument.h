#pragma once

#include <afxwin.h>
#include <mupdf/fitz.h>

#include <list>
#include <map>
#include <vector>

#include "SearchTypes.h"

struct ThumbnailInfo {
	HBITMAP hBitmap;
	int pageNumber;
	bool isCurrentPage;
};

enum ZoomMode { ZOOM_CUSTOM, ZOOM_FIT_WIDTH, ZOOM_FIT_PAGE };

struct PageZoomState {
	ZoomMode zoomMode;
	float customZoom;
	CPoint panOffset;

	PageZoomState()
		: zoomMode(ZOOM_CUSTOM)
		, customZoom(1.0f)
		, panOffset(0, 0)
	{
	}

	PageZoomState(ZoomMode mode, float zoom, CPoint offset)
		: zoomMode(mode)
		, customZoom(zoom)
		, panOffset(offset)
	{
	}
};

struct PageCacheKey {
	int pageNumber;
	int width;
	int height;
	int rotation;

	bool operator<(const PageCacheKey& other) const noexcept {
		if (pageNumber != other.pageNumber) return pageNumber < other.pageNumber;
		if (width != other.width) return width < other.width;
		if (height != other.height) return height < other.height;
		return rotation < other.rotation;
	}

	bool operator==(const PageCacheKey& other) const noexcept {
		return pageNumber == other.pageNumber &&
			width == other.width &&
			height == other.height &&
			rotation == other.rotation;
	}
};

struct PageCacheItem {
	HBITMAP hBitmap;
};

struct PageBounds {
	float x0;
	float y0;
	float width;
	float height;

	PageBounds()
		: x0(0.0f)
		, y0(0.0f)
		, width(0.0f)
		, height(0.0f)
	{
	}
};

class CPDFDocument
{
public:
	CPDFDocument();
	~CPDFDocument();

	bool OpenDocument(const char* filename);
	void CloseDocument();
	bool IsOpen() const { return m_doc != nullptr; }

	CString GetFilePath() const { return m_filePath; }
	CString GetFileName() const;
	int GetTotalPages() const { return m_totalPages; }

	int GetCurrentPage() const { return m_currentPage; }
	void SetCurrentPage(int page);
	fz_page* GetCurrentPageObj() const { return m_currentPageObj; }
	void LoadPageObject(int pageNumber);
	void CleanupCurrentPage();

	ZoomMode GetZoomMode() const { return m_zoomMode; }
	float GetZoom() const { return m_zoom; }
	float GetCustomZoom() const { return m_customZoom; }
	void SetZoom(float zoom, ZoomMode mode = ZOOM_CUSTOM);
	float GetUniformScale() const { return m_uniformScale; }
	void SetUniformScale(float scale) { m_uniformScale = scale; }

	int GetPageRotation(int pageNumber);
	void SetPageRotation(int pageNumber, int rotation);
	std::map<int, int>& GetPageRotations() { return m_pageRotations; }

	bool CachePageseBounds();
	void InvalidatePagesBoundsCache();
	bool HasPageBoundsCache() const { return m_pageBoundsCached; }
	bool GetPageBounds(int pageNumber, float& outWidth, float& outHeight) const;
	bool GetPageBaseBounds(int pageNumber, fz_rect& outBounds) const;
	float GetMaxPageWidth() const { return m_maxPageWidth; }

	void SaveCurrentPageZoomState();
	void RestorePageZoomState(int pageNumber);
	PageZoomState GetPageZoomState(int pageNumber);

	CPoint GetPanOffset() const { return m_panOffset; }
	void SetPanOffset(CPoint offset) { m_panOffset = offset; }
	void ResetPanOffset() { m_panOffset = CPoint(0, 0); }
	bool GetCanDrag() const { return m_canDrag; }
	void SetCanDrag(bool canDrag) { m_canDrag = canDrag; }

	int GetScrollPosition() const { return m_scrollPosition; }
	void SetScrollPosition(int pos) { m_scrollPosition = pos; }

	std::map<int, ThumbnailInfo>& GetThumbnailCache() { return m_thumbnailCache; }
	std::list<int>& GetThumbnailCacheOrder() { return m_thumbnailCacheOrder; }
	void CleanupThumbnails();
	void LimitThumbnailCache(int maxCount = 100);
	void UpdateThumbnailLRU(int pageNumber);

	std::map<PageCacheKey, PageCacheItem>& GetPageCache() { return m_pageCache; }
	std::list<PageCacheKey>& GetCacheOrder() { return m_cacheOrder; }
	void ClearPageCache();

	HBITMAP GetCurrentBitmap() const { return m_hCurrentBitmap; }
	void SetCurrentBitmap(HBITMAP hBitmap);
	void CleanupBitmap();
	HBITMAP TransferCurrentBitmap();
	HBITMAP GetPanPageBitmap() const { return m_hPanPageBitmap; }
	void SetPanPageBitmap(HBITMAP hBitmap);
	void CleanupPanPageBitmap();
	HBITMAP TransferPanPageBitmap();

	fz_context* GetContext() const { return m_ctx; }
	fz_document* GetDocument() const { return m_doc; }

	int GetThumbnailPicWidth() const { return m_thumbnailPicWidth; }
	void SetThumbnailPicWidth(int width) { m_thumbnailPicWidth = width; }
	int GetThumbnailPicHeight() const { return m_thumbnailPicHeight; }
	void SetThumbnailPicHeight(int height) { m_thumbnailPicHeight = height; }

	std::vector<SearchMatch>& GetSearchMatches() { return m_searchMatches; }
	void SetSearchMatches(const std::vector<SearchMatch>& matches) { m_searchMatches = matches; }
	int GetCurrentMatchIndex() const { return m_currentMatchIndex; }
	void SetCurrentMatchIndex(int index) { m_currentMatchIndex = index; }
	CString GetSearchKeyword() const { return m_searchKeyword; }
	void SetSearchKeyword(const CString& keyword) { m_searchKeyword = keyword; }

private:
	void RecalculateMaxPageWidth();

	fz_context* m_ctx;
	fz_document* m_doc;
	fz_page* m_currentPageObj;

	CString m_filePath;
	int m_totalPages;

	int m_currentPage;
	float m_zoom;
	ZoomMode m_zoomMode;
	float m_customZoom;
	float m_uniformScale;

	std::map<int, int> m_pageRotations;

	std::vector<PageBounds> m_pageBoundsCache;
	bool m_pageBoundsCached;
	float m_maxPageWidth;

	std::map<int, PageZoomState> m_pageZoomStates;

	CPoint m_panOffset;
	bool m_canDrag;
	HBITMAP m_hPanPageBitmap;

	std::map<int, ThumbnailInfo> m_thumbnailCache;
	std::list<int> m_thumbnailCacheOrder;
	int m_thumbnailPicWidth;
	int m_thumbnailPicHeight;

	std::map<PageCacheKey, PageCacheItem> m_pageCache;
	std::list<PageCacheKey> m_cacheOrder;
	static const int CACHE_LIMIT = 50;

	HBITMAP m_hCurrentBitmap;

	std::vector<SearchMatch> m_searchMatches;
	int m_currentMatchIndex;
	CString m_searchKeyword;

	int m_scrollPosition;
};
