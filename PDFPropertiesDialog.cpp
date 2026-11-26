// PDFPropertiesDialog.cpp: 文档属性与页面物理尺寸对话框实现文件
//

#include "pch.h"
#include "framework.h"
#include <cmath>
#include <cstring>
#include <mupdf/fitz.h>
#include <mupdf/pdf.h>
#include "PDFDocument.h"
#include "PDFPropertiesDialog.h"

#ifdef _DEBUG
#define new DEBUG_NEW
#endif

namespace
{
	struct StandardPaperSpec
	{
		LPCTSTR name;
		double shortMm;
		double longMm;
		double toleranceMm;
	};

	const StandardPaperSpec STANDARD_PAPERS[] = {
		{ _T("A0"), 841.0, 1189.0, 3.5 },
		{ _T("A1"), 594.0, 841.0, 3.0 },
		{ _T("A2"), 420.0, 594.0, 2.5 },
		{ _T("A3"), 297.0, 420.0, 2.2 },
		{ _T("A4"), 210.0, 297.0, 2.0 },
		{ _T("A5"), 148.0, 210.0, 2.0 },
		{ _T("A6"), 105.0, 148.0, 2.0 },
		{ _T("B0"), 1000.0, 1414.0, 3.5 },
		{ _T("B1"), 707.0, 1000.0, 3.0 },
		{ _T("B2"), 500.0, 707.0, 2.5 },
		{ _T("B3"), 353.0, 500.0, 2.2 },
		{ _T("B4"), 250.0, 353.0, 2.0 },
		{ _T("B5"), 176.0, 250.0, 2.0 },
		{ _T("JIS B4"), 257.0, 364.0, 2.0 },
		{ _T("JIS B5"), 182.0, 257.0, 2.0 },
		{ _T("Letter"), 215.9, 279.4, 2.0 },
		{ _T("Legal"), 215.9, 355.6, 2.0 },
		{ _T("Tabloid"), 279.4, 431.8, 2.2 }
	};

	CString formatMmNumber(double mm)
	{
		const double roundedInt = std::round(mm);
		CString text;
		if (std::fabs(mm - roundedInt) <= 0.18)
		{
			text.Format(_T("%d"), static_cast<int>(roundedInt));
		}
		else
		{
			text.Format(_T("%.1f"), mm);
		}
		return text;
	}

	LPCTSTR detectStandardPaperName(double widthMm, double heightMm)
	{
		const double shortSide = min(widthMm, heightMm);
		const double longSide = max(widthMm, heightMm);
		const StandardPaperSpec* pBest = nullptr;
		double bestErr = 1e9;

		for (const StandardPaperSpec& spec : STANDARD_PAPERS)
		{
			const double errShort = std::fabs(shortSide - spec.shortMm);
			const double errLong = std::fabs(longSide - spec.longMm);
			if (errShort <= spec.toleranceMm && errLong <= spec.toleranceMm)
			{
				const double totalErr = errShort + errLong;
				if (totalErr < bestErr)
				{
					bestErr = totalErr;
					pBest = &spec;
				}
			}
		}
		return pBest ? pBest->name : nullptr;
	}
}

CString FormatPagePhysicalSize(float widthPt, float heightPt, bool includeInches)
{
	if (widthPt <= 0.0f || heightPt <= 0.0f)
	{
		return _T("未知尺寸");
	}

	const double widthMm = static_cast<double>(widthPt) * 25.4 / 72.0;
	const double heightMm = static_cast<double>(heightPt) * 25.4 / 72.0;
	const LPCTSTR paperName = detectStandardPaperName(widthMm, heightMm);
	const bool isLandscape = (widthMm > heightMm * 1.02);
	const bool isPortrait = (heightMm > widthMm * 1.02);

	CString suffix;
	if (paperName)
	{
		if (isLandscape)
			suffix.Format(_T(" (%s 横向)"), paperName);
		else
			suffix.Format(_T(" (%s)"), paperName);
	}
	else if (isLandscape)
	{
		suffix = _T(" (横向)");
	}
	else if (isPortrait)
	{
		suffix = _T(" (纵向)");
	}

	CString result;
	result.Format(_T("%s × %s mm%s"),
		formatMmNumber(widthMm).GetString(),
		formatMmNumber(heightMm).GetString(),
		suffix.GetString());

	if (includeInches)
	{
		CString inchPart;
		inchPart.Format(_T("  [%.2f × %.2f in]"),
			static_cast<double>(widthPt) / 72.0,
			static_cast<double>(heightPt) / 72.0);
		result += inchPart;
	}
	return result;
}

IMPLEMENT_DYNAMIC(CPDFPropertiesDialog, CDialogEx)

CPDFPropertiesDialog::CPDFPropertiesDialog(CPDFDocument* pDoc, int currentPageZeroBased,
	CCriticalSection* pRenderLock, CWnd* pParent)
	: CDialogEx(IDD_DOC_PROPERTIES_DIALOG, pParent)
	, m_pDoc(pDoc)
	, m_currentPage(currentPageZeroBased)
	, m_pRenderLock(pRenderLock)
{
}

CPDFPropertiesDialog::~CPDFPropertiesDialog()
{
	if (m_listFont.GetSafeHandle())
	{
		m_listFont.DeleteObject();
	}
}

void CPDFPropertiesDialog::DoDataExchange(CDataExchange* pDX)
{
	CDialogEx::DoDataExchange(pDX);
	DDX_Control(pDX, IDC_DOC_PROPS_LIST, m_propsList);
}

BEGIN_MESSAGE_MAP(CPDFPropertiesDialog, CDialogEx)
	ON_BN_CLICKED(IDC_BTN_COPY_PROPS, &CPDFPropertiesDialog::OnBnClickedBtnCopyProps)
END_MESSAGE_MAP()

BOOL CPDFPropertiesDialog::OnInitDialog()
{
	CDialogEx::OnInitDialog();

	SetWindowText(_T("文档属性"));

	m_propsList.SetExtendedStyle(
		m_propsList.GetExtendedStyle() |
		LVS_EX_FULLROWSELECT |
		LVS_EX_GRIDLINES |
		LVS_EX_DOUBLEBUFFER);

	if (!m_listFont.GetSafeHandle())
	{
		m_listFont.CreatePointFont(95, _T("微软雅黑"));
	}
	m_propsList.SetFont(&m_listFont);

	CRect clientRect;
	m_propsList.GetClientRect(&clientRect);
	const int nameColWidth = 128;
	const int valColWidth = max(240, clientRect.Width() - nameColWidth - GetSystemMetrics(SM_CXVSCROLL) - 4);
	m_propsList.InsertColumn(0, _T("属性项"), LVCFMT_LEFT, nameColWidth);
	m_propsList.InsertColumn(1, _T("属性值"), LVCFMT_LEFT, valColWidth);

	populateProperties();
	CenterWindow();
	return TRUE;
}

void CPDFPropertiesDialog::addPropertyRow(const CString& name, const CString& value)
{
	const CString displayVal = value.IsEmpty() ? CString(_T("-")) : value;
	const int row = m_propsList.InsertItem(static_cast<int>(m_rows.size()), name);
	if (row >= 0)
	{
		m_propsList.SetItemText(row, 1, displayVal);
	}
	m_rows.emplace_back(name, displayVal);
}

CString CPDFPropertiesDialog::lookupMetadataString(fz_context* pCtx, fz_document* pFzDoc, const char* key)
{
	if (!pCtx || !pFzDoc || !key)
	{
		return CString();
	}

	std::vector<char> buf(512, 0);
	int needed = -1;
	fz_var(needed);
	fz_try(pCtx)
	{
		needed = fz_lookup_metadata(pCtx, pFzDoc, key, buf.data(), static_cast<int>(buf.size()));
		if (needed > static_cast<int>(buf.size()))
		{
			buf.assign(static_cast<size_t>(needed) + 1, 0);
			needed = fz_lookup_metadata(pCtx, pFzDoc, key, buf.data(), static_cast<int>(buf.size()));
		}
	}
	fz_catch(pCtx)
	{
		needed = -1;
	}

	if (needed <= 0 || buf[0] == '\0')
	{
		return CString();
	}

	const int wideLen = ::MultiByteToWideChar(CP_UTF8, 0, buf.data(), -1, nullptr, 0);
	if (wideLen <= 1)
	{
		return CString();
	}

	CString wideStr;
	LPWSTR pWideBuf = wideStr.GetBuffer(wideLen);
	::MultiByteToWideChar(CP_UTF8, 0, buf.data(), -1, pWideBuf, wideLen);
	wideStr.ReleaseBuffer();
	wideStr.Trim();
	return wideStr;
}

CString CPDFPropertiesDialog::formatPdfDateString(const CString& rawPdfDate)
{
	if (rawPdfDate.IsEmpty())
	{
		return CString();
	}

	CString s = rawPdfDate;
	s.Trim();
	if (s.Left(2) == _T("D:"))
	{
		s = s.Mid(2);
	}

	// 解析标准 PDF 日期格式 YYYYMMDDHHmmSS[OHH'mm']
	int digitsCount = 0;
	while (digitsCount < s.GetLength() && s[digitsCount] >= _T('0') && s[digitsCount] <= _T('9'))
	{
		++digitsCount;
	}

	if (digitsCount >= 8)
	{
		const CString year = s.Mid(0, 4);
		const CString month = s.Mid(4, 2);
		const CString day = s.Mid(6, 2);
		CString formatted;
		if (digitsCount >= 14)
		{
			const CString hour = s.Mid(8, 2);
			const CString minute = s.Mid(10, 2);
			const CString second = s.Mid(12, 2);
			formatted.Format(_T("%s-%s-%s %s:%s:%s"),
				year.GetString(), month.GetString(), day.GetString(),
				hour.GetString(), minute.GetString(), second.GetString());
		}
		else if (digitsCount >= 12)
		{
			const CString hour = s.Mid(8, 2);
			const CString minute = s.Mid(10, 2);
			formatted.Format(_T("%s-%s-%s %s:%s"),
				year.GetString(), month.GetString(), day.GetString(),
				hour.GetString(), minute.GetString());
		}
		else
		{
			formatted.Format(_T("%s-%s-%s"),
				year.GetString(), month.GetString(), day.GetString());
		}
		return formatted;
	}

	return rawPdfDate;
}

CString CPDFPropertiesDialog::formatFileSizeString(ULONGLONG bytes)
{
	CString exactBytes;
	exactBytes.Format(_T("%I64u"), bytes);
	// 千分位分隔
	for (int pos = exactBytes.GetLength() - 3; pos > 0; pos -= 3)
	{
		exactBytes.Insert(pos, _T(','));
	}

	CString readable;
	if (bytes >= 1024ULL * 1024ULL * 1024ULL)
	{
		readable.Format(_T("%.2f GB (%s 字节)"),
			static_cast<double>(bytes) / (1024.0 * 1024.0 * 1024.0), exactBytes.GetString());
	}
	else if (bytes >= 1024ULL * 1024ULL)
	{
		readable.Format(_T("%.2f MB (%s 字节)"),
			static_cast<double>(bytes) / (1024.0 * 1024.0), exactBytes.GetString());
	}
	else if (bytes >= 1024ULL)
	{
		readable.Format(_T("%.1f KB (%s 字节)"),
			static_cast<double>(bytes) / 1024.0, exactBytes.GetString());
	}
	else
	{
		readable.Format(_T("%s 字节"), exactBytes.GetString());
	}
	return readable;
}

void CPDFPropertiesDialog::populateProperties()
{
	m_propsList.DeleteAllItems();
	m_rows.clear();

	if (!m_pDoc || !m_pDoc->IsOpen())
	{
		addPropertyRow(_T("状态"), _T("未打开文档"));
		return;
	}

	const CString filePath = m_pDoc->GetFilePath();
	addPropertyRow(_T("文件名"), m_pDoc->GetFileName());
	addPropertyRow(_T("文件路径"), filePath);

	WIN32_FILE_ATTRIBUTE_DATA fileInfo = {};
	if (!filePath.IsEmpty() && ::GetFileAttributesEx(filePath, GetFileExInfoStandard, &fileInfo))
	{
		const ULONGLONG fileSize = (static_cast<ULONGLONG>(fileInfo.nFileSizeHigh) << 32) |
			static_cast<ULONGLONG>(fileInfo.nFileSizeLow);
		addPropertyRow(_T("文件大小"), formatFileSizeString(fileSize));

		FILETIME localFt = {};
		SYSTEMTIME st = {};
		if (::FileTimeToLocalFileTime(&fileInfo.ftLastWriteTime, &localFt) &&
			::FileTimeToSystemTime(&localFt, &st))
		{
			CString modStr;
			modStr.Format(_T("%04d-%02d-%02d %02d:%02d:%02d"),
				st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
			addPropertyRow(_T("本地修改时间"), modStr);
		}
	}

	fz_context* pCtx = m_pDoc->GetContext();
	fz_document* pFzDoc = m_pDoc->GetDocument();

	CString formatVer;
	CString encryption;
	CString title;
	CString author;
	CString subject;
	CString keywords;
	CString creator;
	CString producer;
	CString creationDate;
	CString modDate;
	bool canPrint = true;
	bool canCopy = true;
	bool canAnnotate = true;

	{
		CSingleLock lock(m_pRenderLock, m_pRenderLock != nullptr);
		formatVer = lookupMetadataString(pCtx, pFzDoc, FZ_META_FORMAT);
		encryption = lookupMetadataString(pCtx, pFzDoc, FZ_META_ENCRYPTION);
		title = lookupMetadataString(pCtx, pFzDoc, FZ_META_INFO_TITLE);
		author = lookupMetadataString(pCtx, pFzDoc, FZ_META_INFO_AUTHOR);
		subject = lookupMetadataString(pCtx, pFzDoc, FZ_META_INFO_SUBJECT);
		keywords = lookupMetadataString(pCtx, pFzDoc, FZ_META_INFO_KEYWORDS);
		creator = lookupMetadataString(pCtx, pFzDoc, FZ_META_INFO_CREATOR);
		producer = lookupMetadataString(pCtx, pFzDoc, FZ_META_INFO_PRODUCER);
		creationDate = formatPdfDateString(lookupMetadataString(pCtx, pFzDoc, FZ_META_INFO_CREATIONDATE));
		modDate = formatPdfDateString(lookupMetadataString(pCtx, pFzDoc, FZ_META_INFO_MODIFICATIONDATE));

		if (pCtx && pFzDoc)
		{
			fz_try(pCtx)
			{
				canPrint = fz_has_permission(pCtx, pFzDoc, FZ_PERMISSION_PRINT) != 0;
				canCopy = fz_has_permission(pCtx, pFzDoc, FZ_PERMISSION_COPY) != 0;
				canAnnotate = fz_has_permission(pCtx, pFzDoc, FZ_PERMISSION_ANNOTATE) != 0;
			}
			fz_catch(pCtx)
			{
			}
		}
	}

	const int totalPages = m_pDoc->GetTotalPages();
	const int clampedPage = max(0, min(m_currentPage, max(0, totalPages - 1)));
	CString pagesStr;
	pagesStr.Format(_T("%d 页（当前第 %d 页）"), totalPages, clampedPage + 1);
	addPropertyRow(_T("总页数"), pagesStr);

	float curWidthPt = 0.0f;
	float curHeightPt = 0.0f;
	if (m_pDoc->GetPageBounds(clampedPage, curWidthPt, curHeightPt))
	{
		CString curSizeStr = FormatPagePhysicalSize(curWidthPt, curHeightPt, true);
		const int viewRot = m_pDoc->GetPageRotation(clampedPage);
		if (viewRot != 0)
		{
			CString rotNote;
			rotNote.Format(_T("  (已旋转 %d°)"), viewRot);
			curSizeStr += rotNote;
		}
		addPropertyRow(_T("当前页物理尺寸"), curSizeStr);

		// 检查整份文档是否具有多种图纸幅面
		bool mixedSizes = false;
		for (int i = 0; i < totalPages; ++i)
		{
			float w = 0.0f;
			float h = 0.0f;
			if (m_pDoc->GetPageBounds(i, w, h))
			{
				if (std::fabs(w - curWidthPt) > 2.0f || std::fabs(h - curHeightPt) > 2.0f)
				{
					mixedSizes = true;
					break;
				}
			}
		}
		addPropertyRow(_T("页面幅面一致性"),
			mixedSizes ? _T("混合幅面（文档内包含多种不同尺寸/方向页面）") : _T("全文档统一幅面"));
	}

	addPropertyRow(_T("PDF 版本"), formatVer);

	CString encryptDisplay;
	if (encryption.IsEmpty() || encryption.CompareNoCase(_T("None")) == 0)
	{
		encryptDisplay = _T("未加密");
	}
	else
	{
		encryptDisplay.Format(_T("已加密 (%s)"), encryption.GetString());
	}
	CString permText;
	permText.Format(_T("%s  |  打印:%s  复制:%s  批注:%s"),
		encryptDisplay.GetString(),
		canPrint ? _T("允许") : _T("受限"),
		canCopy ? _T("允许") : _T("受限"),
		canAnnotate ? _T("允许") : _T("受限"));
	addPropertyRow(_T("安全性与权限"), permText);

	addPropertyRow(_T("标题"), title);
	addPropertyRow(_T("作者"), author);
	addPropertyRow(_T("主题"), subject);
	addPropertyRow(_T("关键词"), keywords);
	addPropertyRow(_T("创建程序"), creator);
	addPropertyRow(_T("PDF 生成程序"), producer);
	addPropertyRow(_T("文档创建时间"), creationDate);
	addPropertyRow(_T("文档修改时间"), modDate);
}

void CPDFPropertiesDialog::OnBnClickedBtnCopyProps()
{
	CString allText;
	for (const auto& row : m_rows)
	{
		allText += row.first + _T(": ") + row.second + _T("\r\n");
	}
	if (allText.IsEmpty())
	{
		return;
	}

	if (::OpenClipboard(GetSafeHwnd()))
	{
		::EmptyClipboard();
		const size_t bytes = static_cast<size_t>(allText.GetLength() + 1) * sizeof(WCHAR);
		HGLOBAL hMem = ::GlobalAlloc(GMEM_MOVEABLE, bytes);
		if (hMem)
		{
			void* pDst = ::GlobalLock(hMem);
			if (pDst)
			{
				std::memcpy(pDst, static_cast<LPCTSTR>(allText), bytes);
				::GlobalUnlock(hMem);
				if (::SetClipboardData(CF_UNICODETEXT, hMem))
				{
					hMem = nullptr;
				}
			}
			if (hMem)
			{
				::GlobalFree(hMem);
			}
		}
		::CloseClipboard();
		MessageBox(_T("已将文档属性与图纸幅面信息复制到剪贴板。"), _T("提示"), MB_OK | MB_ICONINFORMATION);
	}
}
