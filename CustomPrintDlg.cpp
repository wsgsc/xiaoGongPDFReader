#include "pch.h"
#include "framework.h"
#include <cmath>
#include <mupdf/fitz.h>
#include "CustomPrintDlg.h"

#ifdef _DEBUG
#define new DEBUG_NEW
#endif

namespace
{
	int ClampPrintDimension(float value)
	{
		const int dimension = static_cast<int>(value);
		return (dimension > 0) ? dimension : 1;
	}

	fz_matrix BuildPrintPageMatrix(const fz_rect& bounds, float scale, int rotation)
	{
		const int normRotation = ((rotation % 360) + 360) % 360;
		fz_matrix ctm = fz_scale(scale, scale);
		if (normRotation != 0)
		{
			fz_matrix rotate = fz_rotate(static_cast<float>(normRotation));
			if (normRotation == 90)
				rotate = fz_pre_translate(rotate, -bounds.x0, -bounds.y1);
			else if (normRotation == 180)
				rotate = fz_pre_translate(rotate, -bounds.x1, -bounds.y1);
			else if (normRotation == 270)
				rotate = fz_pre_translate(rotate, -bounds.x1, -bounds.y0);
			ctm = fz_concat(rotate, ctm);
		}
		else if (bounds.x0 != 0.0f || bounds.y0 != 0.0f)
		{
			ctm = fz_pre_translate(ctm, -bounds.x0, -bounds.y0);
		}
		return ctm;
	}
}

BEGIN_MESSAGE_MAP(CCustomPrintDlg, CDialogEx)
	ON_CBN_SELCHANGE(IDC_PRINTER_COMBO, &CCustomPrintDlg::OnPrinterChange)
	ON_BN_CLICKED(IDC_PRINT_ALL, &CCustomPrintDlg::OnPrintAllRadio)
	ON_BN_CLICKED(IDC_PRINT_RANGE, &CCustomPrintDlg::OnPrintRangeRadio)
	ON_BN_CLICKED(IDC_PRINTER_PROPERTIES, &CCustomPrintDlg::OnPrinterProperties)
END_MESSAGE_MAP()

CCustomPrintDlg::CCustomPrintDlg(int totalPages, CWnd* pParent)
	: CDialogEx(IDD_PRINT_DIALOG, pParent)
	, m_bPrintAll(TRUE)
	, m_pageFrom(1)
	, m_pageTo(totalPages)
	, m_copies(1)
	, m_bCollate(TRUE)
	, m_bDuplex(FALSE)
	, m_totalPages(totalPages)
{
}

CCustomPrintDlg::~CCustomPrintDlg()
{
}

void CCustomPrintDlg::DoDataExchange(CDataExchange* pDX)
{
	CDialogEx::DoDataExchange(pDX);
	DDX_Control(pDX, IDC_PRINTER_COMBO, m_printerCombo);
	DDX_Control(pDX, IDC_PRINT_ALL, m_printAllRadio);
	DDX_Control(pDX, IDC_PRINT_RANGE, m_printRangeRadio);
	DDX_Control(pDX, IDC_PAGE_FROM, m_pageFromEdit);
	DDX_Control(pDX, IDC_PAGE_TO, m_pageToEdit);
	DDX_Control(pDX, IDC_COPIES, m_copiesEdit);
	DDX_Control(pDX, IDC_COLLATE, m_collateCheck);
	DDX_Control(pDX, IDC_DUPLEX, m_duplexCheck);
}

BOOL CCustomPrintDlg::OnInitDialog()
{
	CDialogEx::OnInitDialog();

	EnumeratePrinters();

	m_printAllRadio.SetCheck(BST_CHECKED);
	m_printRangeRadio.SetCheck(BST_UNCHECKED);

	CString str;
	str.Format(_T("%d"), m_pageFrom);
	m_pageFromEdit.SetWindowText(str);
	str.Format(_T("%d"), m_pageTo);
	m_pageToEdit.SetWindowText(str);
	str.Format(_T("%d"), m_copies);
	m_copiesEdit.SetWindowText(str);

	m_collateCheck.SetCheck(BST_CHECKED);
	m_duplexCheck.SetCheck(BST_UNCHECKED);

	UpdateControls();

	return TRUE;
}

void CCustomPrintDlg::EnumeratePrinters()
{
	DWORD dwNeeded = 0;
	DWORD dwReturned = 0;

	EnumPrinters(PRINTER_ENUM_LOCAL | PRINTER_ENUM_CONNECTIONS, NULL, 2, NULL, 0, &dwNeeded, &dwReturned);

	if (dwNeeded > 0)
	{
		std::vector<BYTE> printerBuffer(dwNeeded);
		if (EnumPrinters(PRINTER_ENUM_LOCAL | PRINTER_ENUM_CONNECTIONS, NULL, 2,
			printerBuffer.data(), dwNeeded, &dwNeeded, &dwReturned))
		{
			PRINTER_INFO_2* pInfo = reinterpret_cast<PRINTER_INFO_2*>(printerBuffer.data());
			for (DWORD i = 0; i < dwReturned; i++)
			{
				m_printerCombo.AddString(pInfo[i].pPrinterName);
			}
		}
	}

	TCHAR szDefaultPrinter[256] = { 0 };
	DWORD dwSize = _countof(szDefaultPrinter);
	if (GetDefaultPrinter(szDefaultPrinter, &dwSize))
	{
		int index = m_printerCombo.FindStringExact(-1, szDefaultPrinter);
		if (index != CB_ERR)
			m_printerCombo.SetCurSel(index);
		else if (m_printerCombo.GetCount() > 0)
			m_printerCombo.SetCurSel(0);
	}
	else if (m_printerCombo.GetCount() > 0)
	{
		m_printerCombo.SetCurSel(0);
	}
}

void CCustomPrintDlg::UpdateControls()
{
	BOOL bEnableRange = (m_printRangeRadio.GetCheck() == BST_CHECKED);
	m_pageFromEdit.EnableWindow(bEnableRange);
	m_pageToEdit.EnableWindow(bEnableRange);
}

void CCustomPrintDlg::OnPrinterChange()
{
	m_devMode.clear();
	int index = m_printerCombo.GetCurSel();
	if (index != CB_ERR)
	{
		m_printerCombo.GetLBText(index, m_printerName);
	}
}

void CCustomPrintDlg::OnPrintAllRadio()
{
	UpdateControls();
}

void CCustomPrintDlg::OnPrintRangeRadio()
{
	UpdateControls();
}

void CCustomPrintDlg::OnPrinterProperties()
{
	int index = m_printerCombo.GetCurSel();
	if (index == CB_ERR)
	{
		MessageBox(_T("请先选择打印机"), _T("提示"), MB_OK | MB_ICONINFORMATION);
		return;
	}

	CString printerName;
	m_printerCombo.GetLBText(index, printerName);

	HANDLE hPrinter = NULL;
	if (OpenPrinter((LPTSTR)(LPCTSTR)printerName, &hPrinter, NULL))
	{
		LONG result = DocumentProperties(m_hWnd, hPrinter, (LPTSTR)(LPCTSTR)printerName, NULL, NULL, 0);
		if (result > 0)
		{
			const size_t requiredWords = (static_cast<size_t>(result) + sizeof(DWORD) - 1) / sizeof(DWORD);
			std::vector<DWORD> devMode(requiredWords, 0);
			LPDEVMODE pDevMode = reinterpret_cast<LPDEVMODE>(devMode.data());
			bool hasBaseDevMode = false;
			if (m_devMode.size() == requiredWords)
			{
				devMode = m_devMode;
				pDevMode = reinterpret_cast<LPDEVMODE>(devMode.data());
				hasBaseDevMode = true;
			}
			else if (DocumentProperties(m_hWnd, hPrinter, (LPTSTR)(LPCTSTR)printerName,
				pDevMode, NULL, DM_OUT_BUFFER) == IDOK)
			{
				hasBaseDevMode = true;
			}

			if (hasBaseDevMode)
			{
				if (DocumentProperties(m_hWnd, hPrinter, (LPTSTR)(LPCTSTR)printerName,
					pDevMode, pDevMode, DM_IN_BUFFER | DM_OUT_BUFFER | DM_PROMPT) == IDOK)
				{
					m_devMode = std::move(devMode);
					if (GetDevMode()->dmFields & DM_DUPLEX)
					{
						m_duplexCheck.SetCheck(GetDevMode()->dmDuplex == DMDUP_SIMPLEX ?
							BST_UNCHECKED : BST_CHECKED);
					}
				}
			}
		}
		ClosePrinter(hPrinter);
	}
}

void CCustomPrintDlg::OnOK()
{
	int index = m_printerCombo.GetCurSel();
	if (index == CB_ERR)
	{
		MessageBox(_T("请选择打印机"), _T("提示"), MB_OK | MB_ICONWARNING);
		return;
	}
	m_printerCombo.GetLBText(index, m_printerName);

	m_bPrintAll = (m_printAllRadio.GetCheck() == BST_CHECKED);

	if (!m_bPrintAll)
	{
		CString str;
		m_pageFromEdit.GetWindowText(str);
		m_pageFrom = _ttoi(str);
		m_pageToEdit.GetWindowText(str);
		m_pageTo = _ttoi(str);

		if (m_pageFrom < 1 || m_pageFrom > m_totalPages ||
			m_pageTo < 1 || m_pageTo > m_totalPages ||
			m_pageFrom > m_pageTo)
		{
			CString msg;
			msg.Format(_T("页码范围无效！请输入 1 到 %d 之间的页码。"), m_totalPages);
			MessageBox(msg, _T("提示"), MB_OK | MB_ICONWARNING);
			return;
		}
	}
	else
	{
		m_pageFrom = 1;
		m_pageTo = m_totalPages;
	}

	CString str;
	m_copiesEdit.GetWindowText(str);
	m_copies = _ttoi(str);
	if (m_copies < 1 || m_copies > 999)
	{
		MessageBox(_T("份数必须在 1 到 999 之间"), _T("提示"), MB_OK | MB_ICONWARNING);
		return;
	}

	m_bCollate = (m_collateCheck.GetCheck() == BST_CHECKED);
	m_bDuplex = (m_duplexCheck.GetCheck() == BST_CHECKED);
	const DWORD duplexCap = DeviceCapabilities(m_printerName, NULL, DC_DUPLEX, NULL, NULL);
	if (m_bDuplex && (duplexCap == 0 || duplexCap == GDI_ERROR))
	{
		MessageBox(_T("所选打印机不支持双面打印。"), _T("提示"), MB_OK | MB_ICONWARNING);
		return;
	}
	if (m_bDuplex)
	{
		if (m_devMode.empty())
		{
			HANDLE hPrinter = NULL;
			if (OpenPrinter((LPTSTR)(LPCTSTR)m_printerName, &hPrinter, NULL))
			{
				LONG bytes = DocumentProperties(m_hWnd, hPrinter,
					(LPTSTR)(LPCTSTR)m_printerName, NULL, NULL, 0);
				if (bytes > 0)
				{
					m_devMode.resize((bytes + sizeof(DWORD) - 1) / sizeof(DWORD));
					if (DocumentProperties(m_hWnd, hPrinter, (LPTSTR)(LPCTSTR)m_printerName,
						reinterpret_cast<LPDEVMODE>(m_devMode.data()), NULL,
						DM_OUT_BUFFER) != IDOK)
					{
						m_devMode.clear();
					}
				}
				ClosePrinter(hPrinter);
			}
		}
		if (m_devMode.empty())
		{
			MessageBox(_T("无法读取打印机双面设置。"), _T("错误"), MB_OK | MB_ICONERROR);
			return;
		}
		if (!(GetDevMode()->dmFields & DM_DUPLEX) || GetDevMode()->dmDuplex == DMDUP_SIMPLEX)
		{
			GetDevMode()->dmFields |= DM_DUPLEX;
			GetDevMode()->dmDuplex = DMDUP_VERTICAL;
		}
	}
	else if (GetDevMode() && (GetDevMode()->dmFields & DM_DUPLEX))
	{
		GetDevMode()->dmDuplex = DMDUP_SIMPLEX;
	}

	CDialogEx::OnOK();
}

bool PrintPDFPageToDC(CDC& dcPrint, fz_context* ctx, fz_document* doc,
	int pageNum, int rotation, CCriticalSection& renderLock, int horRes, int verRes)
{
	if (!ctx || !doc)
		return false;

	if (dcPrint.StartPage() <= 0)
		return false;

	int renderWidth = 0;
	int renderHeight = 0;
	int outputWidth = 0;
	int outputHeight = 0;
	float scale = 1.0f;
	fz_rect bounds = { 0, 0, 0, 0 };
	bool hasBounds = false;
	fz_var(renderWidth);
	fz_var(renderHeight);
	fz_var(outputWidth);
	fz_var(outputHeight);
	fz_var(scale);
	fz_var(bounds);
	fz_var(hasBounds);

	const int dpiX = max(1, dcPrint.GetDeviceCaps(LOGPIXELSX));
	const int dpiY = max(1, dcPrint.GetDeviceCaps(LOGPIXELSY));

	{
		CSingleLock lock(&renderLock, TRUE);
		fz_page* pPage = nullptr;
		fz_var(pPage);
		fz_try(ctx)
		{
			pPage = fz_load_page(ctx, doc, pageNum);
			if (!pPage)
				fz_throw(ctx, FZ_ERROR_GENERIC, "Failed to load page %d", pageNum);

			bounds = fz_bound_page(ctx, pPage);
			const float origWidth = bounds.x1 - bounds.x0;
			const float origHeight = bounds.y1 - bounds.y0;
			const float effectiveWidth = (rotation == 90 || rotation == 270) ? origHeight : origWidth;
			const float effectiveHeight = (rotation == 90 || rotation == 270) ? origWidth : origHeight;

			const float safePageWidth = (effectiveWidth > 0.0f) ? effectiveWidth : 1.0f;
			const float safePageHeight = (effectiveHeight > 0.0f) ? effectiveHeight : 1.0f;
			const float paperInchesW = static_cast<float>(horRes) / dpiX;
			const float paperInchesH = static_cast<float>(verRes) / dpiY;
			const float pageInchesW = safePageWidth / 72.0f;
			const float pageInchesH = safePageHeight / 72.0f;
			const float fitFactor = min(paperInchesW / pageInchesW, paperInchesH / pageInchesH);
			outputWidth = min(horRes, ClampPrintDimension(pageInchesW * fitFactor * dpiX));
			outputHeight = min(verRes, ClampPrintDimension(pageInchesH * fitFactor * dpiY));

			const float baseDpi = static_cast<float>(max(dpiX, dpiY));
			scale = (fitFactor * baseDpi) / 72.0f;
			// 32-bit BGRX buffer capped at ~32 MB per page (8M pixels * 4 bytes).
			constexpr double MAX_PRINT_PIXELS = 8.0 * 1024.0 * 1024.0;
			const double requestedPixels = static_cast<double>(effectiveWidth) * effectiveHeight * scale * scale;
			if (requestedPixels > MAX_PRINT_PIXELS)
				scale *= static_cast<float>(std::sqrt(MAX_PRINT_PIXELS / requestedPixels));

			renderWidth = ClampPrintDimension(effectiveWidth * scale);
			renderHeight = ClampPrintDimension(effectiveHeight * scale);
			hasBounds = true;
		}
		fz_always(ctx)
		{
			if (pPage)
				fz_drop_page(ctx, pPage);
		}
		fz_catch(ctx)
		{
			hasBounds = false;
		}
	}

	if (!hasBounds || renderWidth <= 0 || renderHeight <= 0)
		return false;

	// Zero-copy 32-bit BGRX buffer allocated outside fz_try for longjmp safety.
	const size_t stride = static_cast<size_t>(renderWidth) * 4;
	std::vector<BYTE> dibBits(stride * static_cast<size_t>(renderHeight), 0xFF);
	BYTE* pDibData = dibBits.data();
	const int rowStride = renderWidth * 4;
	bool rendered = false;
	fz_var(rendered);

	{
		CSingleLock lock(&renderLock, TRUE);
		fz_page* pPage = nullptr;
		fz_pixmap* pPixmap = nullptr;
		fz_device* pDev = nullptr;
		fz_var(pPage);
		fz_var(pPixmap);
		fz_var(pDev);

		fz_try(ctx)
		{
			pPage = fz_load_page(ctx, doc, pageNum);
			if (!pPage)
				fz_throw(ctx, FZ_ERROR_GENERIC, "Failed to load page %d", pageNum);

			pPixmap = fz_new_pixmap_with_data(ctx, fz_device_bgr(ctx),
				renderWidth, renderHeight, nullptr, 1, rowStride, pDibData);
			fz_clear_pixmap_with_value(ctx, pPixmap, 0xff);

			const fz_matrix ctm = BuildPrintPageMatrix(bounds, scale, rotation);
			pDev = fz_new_draw_device(ctx, ctm, pPixmap);
			fz_run_page(ctx, pPage, pDev, fz_identity, nullptr);
			fz_close_device(ctx, pDev);
			rendered = true;
		}
		fz_always(ctx)
		{
			if (pDev)
				fz_drop_device(ctx, pDev);
			if (pPixmap)
				fz_drop_pixmap(ctx, pPixmap);
			if (pPage)
				fz_drop_page(ctx, pPage);
		}
		fz_catch(ctx)
		{
#ifdef _DEBUG
			const char* msg = fz_caught_message(ctx);
			CString strMsg = msg ? CString(CA2T(msg, CP_UTF8)) : _T("未知错误");
			TRACE(_T("PrintPDFPageToDC error for page %d: %s\n"), pageNum, strMsg.GetString());
#endif
			rendered = false;
		}
	}

	bool success = false;
	if (rendered)
	{
		BITMAPINFO bmi = { 0 };
		bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
		bmi.bmiHeader.biWidth = renderWidth;
		bmi.bmiHeader.biHeight = -renderHeight;
		bmi.bmiHeader.biPlanes = 1;
		bmi.bmiHeader.biBitCount = 32;
		bmi.bmiHeader.biCompression = BI_RGB;

		const int xOffset = (horRes - outputWidth) / 2;
		const int yOffset = (verRes - outputHeight) / 2;
		dcPrint.SetStretchBltMode(HALFTONE);
		::SetBrushOrgEx(dcPrint.GetSafeHdc(), 0, 0, nullptr);
		const int lines = ::StretchDIBits(
			dcPrint.GetSafeHdc(),
			xOffset, yOffset, outputWidth, outputHeight,
			0, 0, renderWidth, renderHeight,
			pDibData, &bmi, DIB_RGB_COLORS, SRCCOPY);
		success = (lines != GDI_ERROR && lines > 0);
	}

	if (success && dcPrint.EndPage() <= 0)
		success = false;
	return success;
}
