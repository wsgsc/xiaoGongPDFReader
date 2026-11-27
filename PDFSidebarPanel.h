// PDFSidebarPanel.h: 左侧导航侧边栏三合一面板（目录 / 页面缩略图 / 全文档批注列表）
//

#pragma once

#include <afxwin.h>
#include <gdiplus.h>
#include <functional>
#include <vector>
#include "PDFDocument.h"
#include "PDFAnnotationHelper.h"

// 侧边栏顶部三段式切换栏（目录 | 缩略图 | 批注）
class CPDFSidebarHeaderWnd : public CWnd
{
public:
	static const int HEADER_HEIGHT = 38;

	CPDFSidebarHeaderWnd();
	virtual ~CPDFSidebarHeaderWnd() override;

	bool CreatePanel(CWnd* pParent, UINT nID);
	void SetActiveTab(SidebarTabMode tab);
	SidebarTabMode GetActiveTab() const { return m_activeTab; }
	void SetAnnotationCount(int count);
	void SetDark(bool dark);

	std::function<void(SidebarTabMode)> m_onTabSelected;

protected:
	afx_msg BOOL OnEraseBkgnd(CDC* pDC);
	afx_msg void OnPaint();
	afx_msg void OnMouseMove(UINT nFlags, CPoint point);
	afx_msg void OnMouseLeave();
	afx_msg void OnLButtonDown(UINT nFlags, CPoint point);
	afx_msg void OnLButtonUp(UINT nFlags, CPoint point);
	afx_msg BOOL OnSetCursor(CWnd* pWnd, UINT nHitTest, UINT message);
	afx_msg void OnCaptureChanged(CWnd* pWnd);
	DECLARE_MESSAGE_MAP()

private:
	int hitTestTab(CPoint point) const;
	CRect getSegmentRect(int index, const CRect& clientRect) const;
	void ensureFonts();

	SidebarTabMode m_activeTab;
	int m_hoverTab;
	int m_pressedTab;
	int m_annotCount;
	bool m_dark;
	BOOL m_mouseTracking;
	CFont m_normalFont;
	CFont m_boldFont;
};

// 虚拟化按需渲染页面缩略图列表控件（支持千页级文档流畅滚动与暗色模式）
class CPDFThumbnailListWnd : public CWnd
{
public:
	static const int MAX_THUMB_WIDTH = 132;
	static const int MAX_THUMB_HEIGHT = 152;
	static const int ITEM_STRIDE_Y = 188;

	CPDFThumbnailListWnd();
	virtual ~CPDFThumbnailListWnd() override;

	bool CreatePanel(CWnd* pParent, UINT nID);
	void SetDocumentPageCount(int totalPages, int currentPage);
	void SetCurrentPage(int currentPage, bool ensureVisible = true);
	void EnsurePageVisible(int pageIndex);
	void NotifyPageInvalidated(int pageIndex);
	void ClearThumbnails();
	void SetDark(bool dark);
	void TriggerVisibleRender();

	using QueryThumbnailCallback = std::function<bool(
		int pageIndex,
		int maxWidth,
		int maxHeight,
		HBITMAP& outBitmap,
		CSize& outPagePxSize,
		bool renderIfMissing)>;

	QueryThumbnailCallback m_queryThumbnailFn;
	std::function<void(int /*pageIndex*/)> m_onPageSelected;
	std::function<void(int /*pageIndex*/, CPoint /*screenPt*/)> m_onPageContextMenu;

protected:
	afx_msg BOOL OnEraseBkgnd(CDC* pDC);
	afx_msg void OnPaint();
	afx_msg void OnSize(UINT nType, int cx, int cy);
	afx_msg void OnMouseMove(UINT nFlags, CPoint point);
	afx_msg void OnMouseLeave();
	afx_msg void OnLButtonDown(UINT nFlags, CPoint point);
	afx_msg void OnLButtonUp(UINT nFlags, CPoint point);
	afx_msg void OnRButtonUp(UINT nFlags, CPoint point);
	afx_msg BOOL OnMouseWheel(UINT nFlags, short zDelta, CPoint pt);
	afx_msg BOOL OnSetCursor(CWnd* pWnd, UINT nHitTest, UINT message);
	afx_msg void OnTimer(UINT_PTR nIDEvent);
	afx_msg void OnCaptureChanged(CWnd* pWnd);
	afx_msg void OnDestroy();
	DECLARE_MESSAGE_MAP()

private:
	static const UINT_PTR TIMER_ID_RENDER = 2101;
	static const int SCROLLBAR_WIDTH = 10;
	static const int TOP_PADDING = 8;

	int totalContentHeight() const;
	int maxScrollY(int viewHeight) const;
	void clampScroll();
	void scrollToY(int newScrollY);
	CRect getScrollbarThumbRect(const CRect& clientRect) const;
	CRect getItemCardRect(int pageIndex, const CRect& clientRect) const;
	int hitTestPageItem(CPoint point, const CRect& clientRect) const;
	void getVisiblePageRange(const CRect& clientRect, int& outFirst, int& outLast) const;
	void ensureFonts();

	int m_totalPages;
	int m_currentPage;
	int m_hoverPage;
	int m_pressedPage;
	int m_scrollY;
	bool m_dark;
	bool m_draggingScrollbar;
	int m_dragScrollOffset;
	BOOL m_mouseTracking;
	CFont m_labelFont;
	CFont m_activeLabelFont;
};

// 全文档批注汇总列表控件（点击跳转对应批注、双击编辑、快捷删除與右键菜单）
class CPDFAnnotationListWnd : public CWnd
{
public:
	CPDFAnnotationListWnd();
	virtual ~CPDFAnnotationListWnd() override;

	bool CreatePanel(CWnd* pParent, UINT nID);
	void SetAnnotations(std::vector<DocumentAnnotationItem> items);
	int GetAnnotationCount() const { return static_cast<int>(m_items.size()); }
	void ClearAnnotations();
	void SetDark(bool dark);

	std::function<void(const DocumentAnnotationItem&)> m_onItemClicked;
	std::function<void(const DocumentAnnotationItem&)> m_onItemDoubleClicked;
	std::function<void(const DocumentAnnotationItem&)> m_onItemDelete;
	std::function<void(const DocumentAnnotationItem&, CPoint /*screenPt*/)> m_onItemContextMenu;

protected:
	afx_msg BOOL OnEraseBkgnd(CDC* pDC);
	afx_msg void OnPaint();
	afx_msg void OnSize(UINT nType, int cx, int cy);
	afx_msg void OnMouseMove(UINT nFlags, CPoint point);
	afx_msg void OnMouseLeave();
	afx_msg void OnLButtonDown(UINT nFlags, CPoint point);
	afx_msg void OnLButtonUp(UINT nFlags, CPoint point);
	afx_msg void OnLButtonDblClk(UINT nFlags, CPoint point);
	afx_msg void OnRButtonUp(UINT nFlags, CPoint point);
	afx_msg BOOL OnMouseWheel(UINT nFlags, short zDelta, CPoint pt);
	afx_msg BOOL OnSetCursor(CWnd* pWnd, UINT nHitTest, UINT message);
	afx_msg void OnCaptureChanged(CWnd* pWnd);
	DECLARE_MESSAGE_MAP()

private:
	static const int ITEM_STRIDE_Y = 72;
	static const int SCROLLBAR_WIDTH = 10;
	static const int TOP_PADDING = 8;

	int totalContentHeight() const;
	int maxScrollY(int viewHeight) const;
	void clampScroll();
	void scrollToY(int newScrollY);
	CRect getScrollbarThumbRect(const CRect& clientRect) const;
	CRect getItemCardRect(int index, const CRect& clientRect) const;
	CRect getDeleteButtonRect(const CRect& cardRect) const;
	int hitTestItem(CPoint point, const CRect& clientRect, bool& outOnDeleteBtn) const;
	CString buildItemDisplayText(const DocumentAnnotationItem& item) const;
	void ensureFonts();

	std::vector<DocumentAnnotationItem> m_items;
	int m_selectedIndex;
	int m_hoverIndex;
	bool m_hoverDeleteBtn;
	int m_pressedIndex;
	bool m_pressedDeleteBtn;
	int m_scrollY;
	bool m_dark;
	bool m_draggingScrollbar;
	int m_dragScrollOffset;
	BOOL m_mouseTracking;
	CFont m_titleFont;
	CFont m_bodyFont;
	CFont m_pagePillFont;
};
