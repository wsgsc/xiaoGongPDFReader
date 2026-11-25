#pragma once

#include <afxwin.h>
#include <afxdialogex.h>
#include <functional>
#include <vector>
#include "resource.h"

enum class ExportImageFormat
{
	Png = 0,
	Jpg = 1
};

enum class ExportPageScope
{
	CurrentPage = 0,
	AllPages = 1,
	PageRange = 2
};

struct ExportBitmapBuffer
{
	int width = 0;
	int height = 0;
	int stride = 0;
	std::vector<BYTE> bgra;
};

using RenderExportPageCallback = std::function<bool(
	int pageIndex, float scale, double maxPixels, ExportBitmapBuffer& outBuffer)>;

// 将 32-bit BGRX (top-down) 缓冲区保存为 PNG 或 JPG 文件
bool SaveBgrxBufferToImageFile(const BYTE* pTopDownBgrx, int width, int height,
	int stride, const CString& filePath, ExportImageFormat format,
	int dpi = 300, int jpegQuality = 92);

// 将 32-bit BGRX (top-down) 缓冲区写入系统剪贴板 (CF_DIB)
bool CopyBgrxBufferToClipboard(HWND hOwnerWnd, const BYTE* pTopDownBgrx,
	int width, int height, int stride);

// 导出图片参数设置对话框
class CPDFExportImageDlg : public CDialogEx
{
public:
	CPDFExportImageDlg(int totalPages, int currentPageOneBased, CWnd* pParent = nullptr);
	virtual ~CPDFExportImageDlg() = default;

	enum { IDD = IDD_EXPORT_IMAGE_DIALOG };

	ExportPageScope m_scope;
	int m_pageFrom;
	int m_pageTo;
	ExportImageFormat m_format;
	int m_dpi;
	bool m_mergeLongImage;

protected:
	virtual void DoDataExchange(CDataExchange* pDX) override;
	virtual BOOL OnInitDialog() override;
	virtual void OnOK() override;

	afx_msg void OnScopeChanged();
	DECLARE_MESSAGE_MAP()

private:
	int m_totalPages;
	int m_currentPageOneBased;
	CButton m_radioCurrent;
	CButton m_radioAll;
	CButton m_radioRange;
	CEdit m_editFrom;
	CEdit m_editTo;
	CComboBox m_comboFormat;
	CComboBox m_comboDpi;
	CButton m_radioSeparate;
	CButton m_radioLongImage;

	void updateRangeControls();
};

// 弹出导出对话框并执行批量图片 / 长图导出流程
bool RunExportPagesToImages(
	CWnd* pParent,
	int totalPages,
	int currentPageZeroBased,
	const CString& documentFilePath,
	const std::function<bool(int pageIndex, float& outWidthPt, float& outHeightPt)>& getPageSizePtFn,
	const RenderExportPageCallback& renderPageFn,
	CString* pOutSummaryMessage = nullptr,
	int presetRangeFromOneBased = -1,
	int presetRangeToOneBased = -1);
