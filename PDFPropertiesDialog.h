// PDFPropertiesDialog.h: 文档属性与页面物理尺寸对话框头文件
//

#pragma once

#include <afxwin.h>
#include <afxcmn.h>
#include <afxmt.h>
#include <afxdialogex.h>
#include <utility>
#include <vector>
#include <mupdf/fitz.h>
#include "resource.h"

class CPDFDocument;

// 根据页面宽高（72 DPI 点）换算物理毫米尺寸与标准图纸幅面名称
// 例如："210 × 297 mm (A4)" 或 "420 × 297 mm (A3 横向)"
CString FormatPagePhysicalSize(float widthPt, float heightPt, bool includeInches = false);

// CPDFPropertiesDialog 对话框
class CPDFPropertiesDialog : public CDialogEx
{
	DECLARE_DYNAMIC(CPDFPropertiesDialog)

public:
	CPDFPropertiesDialog(CPDFDocument* pDoc, int currentPageZeroBased,
		CCriticalSection* pRenderLock, CWnd* pParent = nullptr);
	virtual ~CPDFPropertiesDialog();

	enum { IDD = IDD_DOC_PROPERTIES_DIALOG };

protected:
	virtual void DoDataExchange(CDataExchange* pDX) override;
	virtual BOOL OnInitDialog() override;

	afx_msg void OnBnClickedBtnCopyProps();
	DECLARE_MESSAGE_MAP()

private:
	CPDFDocument* m_pDoc;
	int m_currentPage;
	CCriticalSection* m_pRenderLock;
	CListCtrl m_propsList;
	CFont m_listFont;
	std::vector<std::pair<CString, CString>> m_rows;

	void populateProperties();
	void addPropertyRow(const CString& name, const CString& value);
	static CString lookupMetadataString(fz_context* pCtx, fz_document* pFzDoc, const char* key);
	static CString formatPdfDateString(const CString& rawPdfDate);
	static CString formatFileSizeString(ULONGLONG bytes);
};
