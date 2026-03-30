#include "pch.h"
#include "PDFDocument.h"

namespace
{
	constexpr size_t MUPDF_STORE_LIMIT = 256u * 1024u * 1024u;
}

CPDFDocument::CPDFDocument()
	: m_ctx(nullptr)
	, m_doc(nullptr)
	, m_currentPageObj(nullptr)
	, m_totalPages(0)
	, m_currentPage(0)
	, m_zoom(1.0f)
	, m_zoomMode(ZOOM_CUSTOM)
	, m_customZoom(1.0f)
	, m_uniformScale(0.0f)
	, m_panOffset(0, 0)
	, m_canDrag(false)
	, m_hPanPageBitmap(NULL)
	, m_thumbnailPicWidth(0)
	, m_thumbnailPicHeight(0)
	, m_hCurrentBitmap(NULL)
	, m_currentMatchIndex(-1)
	, m_scrollPosition(0)
	, m_pageBoundsCached(false)
	, m_maxPageWidth(0.0f)
{
	m_ctx = fz_new_context(nullptr, nullptr, MUPDF_STORE_LIMIT);
	if (m_ctx)
	{
		fz_try(m_ctx)
		{
			fz_register_document_handlers(m_ctx);
		}
		fz_catch(m_ctx)
		{
			fz_drop_context(m_ctx);
			m_ctx = nullptr;
		}
	}
}

CPDFDocument::~CPDFDocument()
{
	CloseDocument();
	if (m_ctx)
	{
		fz_drop_context(m_ctx);
		m_ctx = nullptr;
	}
}

bool CPDFDocument::OpenDocument(const char* filename)
{
	if (!m_ctx || !filename)
	{
		return false;
	}

	CloseDocument();

	fz_try(m_ctx)
	{
		m_doc = fz_open_document(m_ctx, filename);
		m_totalPages = fz_count_pages(m_ctx, m_doc);
	}
	fz_catch(m_ctx)
	{
		m_doc = nullptr;
		m_totalPages = 0;
		return false;
	}

	if (!m_doc || m_totalPages <= 0)
	{
		CloseDocument();
		return false;
	}

	int wideLength = MultiByteToWideChar(CP_UTF8, 0, filename, -1, NULL, 0);
	if (wideLength > 0)
	{
		std::vector<wchar_t> widePath(wideLength);
		MultiByteToWideChar(CP_UTF8, 0, filename, -1, widePath.data(), wideLength);
		m_filePath = CString(widePath.data());
	}

	m_currentPage = 0;
	m_zoomMode = ZOOM_CUSTOM;
	m_customZoom = 1.0f;
	m_zoom = 1.0f;
	m_uniformScale = 0.0f;
	m_panOffset = CPoint(0, 0);
	m_canDrag = false;
	m_scrollPosition = 0;

	CachePageseBounds();
	return true;
}

void CPDFDocument::CloseDocument()
{
	CleanupCurrentPage();
	CleanupBitmap();
	CleanupPanPageBitmap();
	CleanupThumbnails();
	ClearPageCache();

	if (m_doc && m_ctx)
	{
		fz_drop_document(m_ctx, m_doc);
	}
	m_doc = nullptr;

	m_filePath.Empty();
	m_totalPages = 0;
	m_currentPage = 0;
	m_zoom = 1.0f;
	m_zoomMode = ZOOM_CUSTOM;
	m_customZoom = 1.0f;
	m_uniformScale = 0.0f;
	m_panOffset = CPoint(0, 0);
	m_canDrag = false;
	m_pageRotations.clear();
	m_pageZoomStates.clear();
	m_scrollPosition = 0;
	m_searchMatches.clear();
	m_currentMatchIndex = -1;
	m_searchKeyword.Empty();

	m_pageBoundsCache.clear();
	m_pageBoundsCached = false;
	m_maxPageWidth = 0.0f;
}

CString CPDFDocument::GetFileName() const
{
	if (m_filePath.IsEmpty())
	{
		return _T("");
	}

	int pos = m_filePath.ReverseFind(_T('\\'));
	if (pos == -1)
	{
		pos = m_filePath.ReverseFind(_T('/'));
	}

	if (pos != -1)
	{
		return m_filePath.Mid(pos + 1);
	}

	return m_filePath;
}

void CPDFDocument::SetCurrentPage(int page)
{
	if (page >= 0 && page < m_totalPages)
	{
		m_currentPage = page;
	}
}

void CPDFDocument::LoadPageObject(int pageNumber)
{
	if (!m_doc || !m_ctx)
	{
		return;
	}

	CleanupCurrentPage();

	fz_try(m_ctx)
	{
		m_currentPageObj = fz_load_page(m_ctx, m_doc, pageNumber);
	}
	fz_catch(m_ctx)
	{
		m_currentPageObj = nullptr;
	}
}

void CPDFDocument::CleanupCurrentPage()
{
	if (m_currentPageObj && m_ctx)
	{
		fz_drop_page(m_ctx, m_currentPageObj);
		m_currentPageObj = nullptr;
	}
}

void CPDFDocument::SetZoom(float zoom, ZoomMode mode)
{
	m_zoom = zoom;
	m_zoomMode = mode;
	if (mode == ZOOM_CUSTOM)
	{
		m_customZoom = zoom;
	}
}

int CPDFDocument::GetPageRotation(int pageNumber)
{
	auto it = m_pageRotations.find(pageNumber);
	if (it != m_pageRotations.end())
	{
		return it->second;
	}
	return 0;
}

void CPDFDocument::SetPageRotation(int pageNumber, int rotation)
{
	rotation %= 360;
	if (rotation < 0)
	{
		rotation += 360;
	}

	m_pageRotations[pageNumber] = rotation;
	InvalidatePagesBoundsCache();
}

void CPDFDocument::InvalidatePagesBoundsCache()
{
	RecalculateMaxPageWidth();
}

bool CPDFDocument::GetPageBounds(int pageNumber, float& outWidth, float& outHeight) const
{
	if (!m_pageBoundsCached ||
		pageNumber < 0 ||
		pageNumber >= (int)m_pageBoundsCache.size())
	{
		return false;
	}

	const PageBounds& cached = m_pageBoundsCache[pageNumber];
	outWidth = cached.width;
	outHeight = cached.height;

	auto rotationIt = m_pageRotations.find(pageNumber);
	int rotation = (rotationIt != m_pageRotations.end()) ? rotationIt->second : 0;
	if (rotation == 90 || rotation == 270)
	{
		float temp = outWidth;
		outWidth = outHeight;
		outHeight = temp;
	}

	return true;
}

bool CPDFDocument::GetPageBaseBounds(int pageNumber, fz_rect& outBounds) const
{
	if (!m_pageBoundsCached ||
		pageNumber < 0 ||
		pageNumber >= (int)m_pageBoundsCache.size())
	{
		return false;
	}

	const PageBounds& cached = m_pageBoundsCache[pageNumber];
	outBounds.x0 = cached.x0;
	outBounds.y0 = cached.y0;
	outBounds.x1 = cached.x0 + cached.width;
	outBounds.y1 = cached.y0 + cached.height;
	return true;
}

void CPDFDocument::RecalculateMaxPageWidth()
{
	m_maxPageWidth = 0.0f;
	if (!m_pageBoundsCached)
	{
		return;
	}

	for (int i = 0; i < (int)m_pageBoundsCache.size(); ++i)
	{
		float width = m_pageBoundsCache[i].width;
		float height = m_pageBoundsCache[i].height;
		int rotation = GetPageRotation(i);
		if (rotation == 90 || rotation == 270)
		{
			float temp = width;
			width = height;
			height = temp;
		}

		if (width > m_maxPageWidth)
		{
			m_maxPageWidth = width;
		}
	}
}

void CPDFDocument::SaveCurrentPageZoomState()
{
	m_pageZoomStates[m_currentPage] = PageZoomState(m_zoomMode, m_customZoom, m_panOffset);
}

void CPDFDocument::RestorePageZoomState(int pageNumber)
{
	auto it = m_pageZoomStates.find(pageNumber);
	if (it != m_pageZoomStates.end())
	{
		m_zoomMode = it->second.zoomMode;
		m_customZoom = it->second.customZoom;
		m_panOffset = it->second.panOffset;
		if (m_zoomMode == ZOOM_CUSTOM)
		{
			m_zoom = m_customZoom;
		}
	}
	else
	{
		m_zoomMode = ZOOM_CUSTOM;
		m_customZoom = 1.0f;
		m_panOffset = CPoint(0, 0);
	}
}

PageZoomState CPDFDocument::GetPageZoomState(int pageNumber)
{
	auto it = m_pageZoomStates.find(pageNumber);
	if (it != m_pageZoomStates.end())
	{
		return it->second;
	}
	return PageZoomState();
}

void CPDFDocument::CleanupThumbnails()
{
	for (auto& pair : m_thumbnailCache)
	{
		if (pair.second.hBitmap)
		{
			DeleteObject(pair.second.hBitmap);
		}
	}
	m_thumbnailCache.clear();
	m_thumbnailCacheOrder.clear();
}

void CPDFDocument::LimitThumbnailCache(int maxCount)
{
	while ((int)m_thumbnailCache.size() > maxCount && !m_thumbnailCacheOrder.empty())
	{
		int oldestPage = m_thumbnailCacheOrder.back();
		m_thumbnailCacheOrder.pop_back();

		auto it = m_thumbnailCache.find(oldestPage);
		if (it != m_thumbnailCache.end())
		{
			if (it->second.hBitmap)
			{
				DeleteObject(it->second.hBitmap);
			}
			m_thumbnailCache.erase(it);
		}
	}
}

void CPDFDocument::UpdateThumbnailLRU(int pageNumber)
{
	m_thumbnailCacheOrder.remove(pageNumber);
	m_thumbnailCacheOrder.push_front(pageNumber);
}

void CPDFDocument::ClearPageCache()
{
	for (auto& pair : m_pageCache)
	{
		if (pair.second.hBitmap)
		{
			DeleteObject(pair.second.hBitmap);
		}
	}
	m_pageCache.clear();
	m_cacheOrder.clear();
}

void CPDFDocument::SetCurrentBitmap(HBITMAP hBitmap)
{
	CleanupBitmap();
	m_hCurrentBitmap = hBitmap;
}

void CPDFDocument::CleanupBitmap()
{
	if (m_hCurrentBitmap)
	{
		BITMAP bm;
		if (::GetObject(m_hCurrentBitmap, sizeof(BITMAP), &bm) != 0)
		{
			DeleteObject(m_hCurrentBitmap);
		}
		m_hCurrentBitmap = NULL;
	}
}

HBITMAP CPDFDocument::TransferCurrentBitmap()
{
	HBITMAP hBitmap = m_hCurrentBitmap;
	m_hCurrentBitmap = NULL;
	return hBitmap;
}

void CPDFDocument::SetPanPageBitmap(HBITMAP hBitmap)
{
	CleanupPanPageBitmap();
	m_hPanPageBitmap = hBitmap;
}

void CPDFDocument::CleanupPanPageBitmap()
{
	if (m_hPanPageBitmap)
	{
		DeleteObject(m_hPanPageBitmap);
		m_hPanPageBitmap = NULL;
	}
}

HBITMAP CPDFDocument::TransferPanPageBitmap()
{
	HBITMAP hBitmap = m_hPanPageBitmap;
	m_hPanPageBitmap = NULL;
	return hBitmap;
}

bool CPDFDocument::CachePageseBounds()
{
	if (!m_doc || !m_ctx || m_totalPages <= 0)
	{
		return false;
	}

	m_pageBoundsCache.clear();
	m_pageBoundsCache.resize(m_totalPages);

	for (int i = 0; i < m_totalPages; ++i)
	{
		fz_page* page = nullptr;
		fz_try(m_ctx)
		{
			page = fz_load_page(m_ctx, m_doc, i);
			if (page)
			{
				fz_rect bounds = fz_bound_page(m_ctx, page);
				m_pageBoundsCache[i].x0 = bounds.x0;
				m_pageBoundsCache[i].y0 = bounds.y0;
				m_pageBoundsCache[i].width = bounds.x1 - bounds.x0;
				m_pageBoundsCache[i].height = bounds.y1 - bounds.y0;

				fz_drop_page(m_ctx, page);
				page = nullptr;
			}
		}
		fz_catch(m_ctx)
		{
			if (page)
			{
				fz_drop_page(m_ctx, page);
				page = nullptr;
			}

			m_pageBoundsCache[i].x0 = 0.0f;
			m_pageBoundsCache[i].y0 = 0.0f;
			m_pageBoundsCache[i].width = 595.0f;
			m_pageBoundsCache[i].height = 842.0f;
		}
	}

	m_pageBoundsCached = true;
	RecalculateMaxPageWidth();
	return true;
}
