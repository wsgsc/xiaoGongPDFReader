// PDFDocument.cpp: PDF文档数据封装类实现

#include "pch.h"
#include "PDFDocument.h"

CPDFDocument::CPDFDocument(fz_context* ctx)
	: m_ctx(ctx)
	, m_doc(nullptr)
	, m_currentPageObj(nullptr)
	, m_totalPages(0)
	, m_currentPage(0)
	, m_zoom(1.0f)
	, m_zoomMode(ZOOM_FIT_PAGE)
	, m_customZoom(1.0f)
	, m_panOffset(0, 0)
	, m_canDrag(false)
	, m_hPanPageBitmap(NULL)
	, m_thumbnailPicWidth(0)
	, m_thumbnailPicHeight(0)
	, m_hCurrentBitmap(NULL)
	, m_currentMatchIndex(-1)
{
}

CPDFDocument::~CPDFDocument()
{
#ifdef _DEBUG
	TRACE(_T("~CPDFDocument() 析构函数，m_doc=%p, m_ctx=%p\n"), m_doc, m_ctx);
#endif
	CloseDocument();
#ifdef _DEBUG
	TRACE(_T("~CPDFDocument() 完成\n"));
#endif
}

bool CPDFDocument::OpenDocument(const char* filename)
{
#ifdef _DEBUG
	TRACE(_T("CPDFDocument::OpenDocument() 开始\n"));
	TRACE(_T("filename指针: %p\n"), filename);
	if (filename) {
		// 将UTF-8转换为Unicode用于调试输出
		int wlen = MultiByteToWideChar(CP_UTF8, 0, filename, -1, NULL, 0);
		if (wlen > 0) {
			wchar_t* wstr = new wchar_t[wlen];
			MultiByteToWideChar(CP_UTF8, 0, filename, -1, wstr, wlen);
			TRACE(_T("文件路径(UTF-8转Unicode): %s\n"), wstr);
			delete[] wstr;
		}
		TRACE(_T("文件路径(原始UTF-8): %s\n"), filename);  // 这会显示乱码，但能看到长度
	}
#endif

	if (!m_ctx || !filename) {
#ifdef _DEBUG
		TRACE(_T("错误: m_ctx或filename为空! m_ctx=%p, filename=%p\n"), m_ctx, filename);
#endif
		return false;
	}

	// 关闭现有文档
	CloseDocument();

	// 打开新文档
	fz_try(m_ctx)
	{
#ifdef _DEBUG
		TRACE(_T("调用 fz_open_document...\n"));
#endif
		m_doc = fz_open_document(m_ctx, filename);
#ifdef _DEBUG
		TRACE(_T("fz_open_document 返回: %p\n"), m_doc);
#endif
		m_totalPages = fz_count_pages(m_ctx, m_doc);
#ifdef _DEBUG
		TRACE(_T("总页数: %d\n"), m_totalPages);
#endif
	}
	fz_catch(m_ctx)
	{
#ifdef _DEBUG
		const char* errMsg = fz_caught_message(m_ctx);
		TRACE(_T("fz_open_document 异常: %s\n"), errMsg);
#endif
		m_doc = nullptr;
		return false;
	}

	if (m_doc && m_totalPages > 0) {
		// 将 UTF-8 char* 正确转换为 CString (Unicode)
		int wideLength = MultiByteToWideChar(CP_UTF8, 0, filename, -1, NULL, 0);
		if (wideLength > 0) {
			wchar_t* wideStr = new wchar_t[wideLength];
			MultiByteToWideChar(CP_UTF8, 0, filename, -1, wideStr, wideLength);
			m_filePath = CString(wideStr);
			delete[] wideStr;
		}
		m_currentPage = 0;
		m_zoomMode = ZOOM_FIT_PAGE;
		m_customZoom = 1.0f;
		m_zoom = 1.0f;
		m_panOffset = CPoint(0, 0);
		m_canDrag = false;
		return true;
	}

	return false;
}

void CPDFDocument::CloseDocument()
{
#ifdef _DEBUG
	TRACE(_T("CloseDocument() 开始，m_doc=%p, m_ctx=%p\n"), m_doc, m_ctx);
#endif

	// 清理当前页面
	CleanupCurrentPage();
#ifdef _DEBUG
	TRACE(_T("CleanupCurrentPage() 完成\n"));
#endif

	// 清理位图
	CleanupBitmap();
	CleanupPanPageBitmap();
#ifdef _DEBUG
	TRACE(_T("清理位图完成\n"));
#endif

	// 清理缩略图
	CleanupThumbnails();
#ifdef _DEBUG
	TRACE(_T("CleanupThumbnails() 完成\n"));
#endif

	// 清理页面缓存
	ClearPageCache();
#ifdef _DEBUG
	TRACE(_T("ClearPageCache() 完成\n"));
#endif

	// 释放文档
	// 注意：必须在m_ctx有效时释放文档，否则会内存泄漏
	if (m_doc) {
#ifdef _DEBUG
		TRACE(_T("准备释放 m_doc=%p, m_ctx=%p\n"), m_doc, m_ctx);
#endif
		if (m_ctx) {
#ifdef _DEBUG
			TRACE(_T("调用 fz_drop_document...\n"));
#endif
			fz_drop_document(m_ctx, m_doc);
#ifdef _DEBUG
			TRACE(_T("fz_drop_document 完成\n"));
#endif
		}
		else {
#ifdef _DEBUG
			TRACE(_T("警告: m_ctx 为 nullptr，无法释放 m_doc!\n"));
#endif
		}
		m_doc = nullptr;
	}
	else {
#ifdef _DEBUG
		TRACE(_T("m_doc 已经是 nullptr，无需释放\n"));
#endif
	}

	// 重置状态
	m_filePath.Empty();
	m_totalPages = 0;
	m_currentPage = 0;
	m_zoom = 1.0f;
	m_zoomMode = ZOOM_FIT_PAGE;
	m_customZoom = 1.0f;
	m_panOffset = CPoint(0, 0);
	m_canDrag = false;
	m_pageRotations.clear();
	m_pageZoomStates.clear();

#ifdef _DEBUG
	TRACE(_T("CloseDocument() 完成\n"));
#endif
}

CString CPDFDocument::GetFileName() const
{
	if (m_filePath.IsEmpty()) {
		return _T("");
	}

	int pos = m_filePath.ReverseFind(_T('\\'));
	if (pos == -1) {
		pos = m_filePath.ReverseFind(_T('/'));
	}

	if (pos != -1) {
		return m_filePath.Mid(pos + 1);
	}

	return m_filePath;
}

void CPDFDocument::SetCurrentPage(int page)
{
	if (page >= 0 && page < m_totalPages) {
		m_currentPage = page;
	}
}

void CPDFDocument::LoadPageObject(int pageNumber)
{
	if (!m_doc || !m_ctx) {
		return;
	}

	// 清理旧的页面对象
	CleanupCurrentPage();

	// 加载新页面
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
	if (m_currentPageObj && m_ctx) {
		fz_drop_page(m_ctx, m_currentPageObj);
		m_currentPageObj = nullptr;
	}
}

void CPDFDocument::SetZoom(float zoom, ZoomMode mode)
{
	m_zoom = zoom;
	m_zoomMode = mode;
	if (mode == ZOOM_CUSTOM) {
		m_customZoom = zoom;
	}
}

int CPDFDocument::GetPageRotation(int pageNumber)
{
	auto it = m_pageRotations.find(pageNumber);
	if (it != m_pageRotations.end()) {
		return it->second;
	}
	return 0;  // 默认无旋转
}

void CPDFDocument::SetPageRotation(int pageNumber, int rotation)
{
	// 规范化旋转角度到 0, 90, 180, 270
	rotation = rotation % 360;
	if (rotation < 0) rotation += 360;

	m_pageRotations[pageNumber] = rotation;
}

void CPDFDocument::SaveCurrentPageZoomState()
{
	PageZoomState state(m_zoomMode, m_customZoom, m_panOffset);
	m_pageZoomStates[m_currentPage] = state;
}

void CPDFDocument::RestorePageZoomState(int pageNumber)
{
	auto it = m_pageZoomStates.find(pageNumber);
	if (it != m_pageZoomStates.end()) {
		m_zoomMode = it->second.zoomMode;
		m_customZoom = it->second.customZoom;
		m_panOffset = it->second.panOffset;

		if (m_zoomMode == ZOOM_CUSTOM) {
			m_zoom = m_customZoom;
		}
	}
	else {
		// 使用默认状态
		m_zoomMode = ZOOM_FIT_PAGE;
		m_customZoom = 1.0f;
		m_panOffset = CPoint(0, 0);
	}
}

PageZoomState CPDFDocument::GetPageZoomState(int pageNumber)
{
	auto it = m_pageZoomStates.find(pageNumber);
	if (it != m_pageZoomStates.end()) {
		return it->second;
	}
	return PageZoomState();  // 返回默认状态
}

void CPDFDocument::CleanupThumbnails()
{
	// 删除所有缩略图位图
	for (auto& pair : m_thumbnailCache) {
		if (pair.second.hBitmap) {
			DeleteObject(pair.second.hBitmap);
		}
	}
	m_thumbnailCache.clear();
}

void CPDFDocument::ClearPageCache()
{
	// 删除所有缓存的位图
	for (auto& pair : m_pageCache) {
		if (pair.second.hBitmap) {
			DeleteObject(pair.second.hBitmap);
		}
	}
	m_pageCache.clear();
	m_cacheOrder.clear();
}

void CPDFDocument::SetCurrentBitmap(HBITMAP hBitmap)
{
	// 先清理旧位图
	CleanupBitmap();
	m_hCurrentBitmap = hBitmap;
}

void CPDFDocument::CleanupBitmap()
{
	if (m_hCurrentBitmap) {
		// 验证是否是有效的 GDI 对象
		BITMAP bm;
		if (::GetObject(m_hCurrentBitmap, sizeof(BITMAP), &bm) != 0)
		{
			// 是有效的位图，可以安全删除
			DeleteObject(m_hCurrentBitmap);
		}
		m_hCurrentBitmap = NULL;
	}
}

HBITMAP CPDFDocument::TransferCurrentBitmap()
{
	// 转移位图所有权，不删除位图
	HBITMAP hBitmap = m_hCurrentBitmap;
	m_hCurrentBitmap = NULL;
	return hBitmap;
}

void CPDFDocument::SetPanPageBitmap(HBITMAP hBitmap)
{
	// 先清理旧位图
	CleanupPanPageBitmap();
	m_hPanPageBitmap = hBitmap;
}

void CPDFDocument::CleanupPanPageBitmap()
{
	if (m_hPanPageBitmap) {
		DeleteObject(m_hPanPageBitmap);
		m_hPanPageBitmap = NULL;
	}
}

HBITMAP CPDFDocument::TransferPanPageBitmap()
{
	// 转移拖拽位图所有权，不删除位图
	HBITMAP hBitmap = m_hPanPageBitmap;
	m_hPanPageBitmap = NULL;
	return hBitmap;
}
