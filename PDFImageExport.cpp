#include "pch.h"
#include "framework.h"
#include <shlobj.h>
#include <shlwapi.h>
#include <shobjidl.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>
#include "PDFImageExport.h"
#include "ProgressDlg.h"

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "shell32.lib")

#ifdef _DEBUG
#define new DEBUG_NEW
#endif

namespace
{
	constexpr int LONG_IMAGE_PAGE_GAP = 12;
	constexpr int LONG_IMAGE_MAX_PAGES = 60;
	constexpr int LONG_IMAGE_MAX_DIMENSION = 65000;
	constexpr double SINGLE_PAGE_MAX_PIXELS = 36.0 * 1024.0 * 1024.0;
	constexpr double LONG_IMAGE_MAX_PIXELS = 60.0 * 1024.0 * 1024.0;

	bool getGdiplusEncoderClsid(const WCHAR* mimeType, CLSID& outClsid)
	{
		UINT num = 0;
		UINT size = 0;
		if (Gdiplus::GetImageEncodersSize(&num, &size) != Gdiplus::Ok || size == 0 || num == 0)
			return false;

		std::vector<BYTE> buffer(size);
		auto* pInfos = reinterpret_cast<Gdiplus::ImageCodecInfo*>(buffer.data());
		if (Gdiplus::GetImageEncoders(num, size, pInfos) != Gdiplus::Ok)
			return false;

		for (UINT i = 0; i < num; ++i)
		{
			if (pInfos[i].MimeType && wcscmp(pInfos[i].MimeType, mimeType) == 0)
			{
				outClsid = pInfos[i].Clsid;
				return true;
			}
		}
		return false;
	}

	CString extractBaseNameWithoutExt(const CString& filePath)
	{
		if (filePath.IsEmpty())
			return _T("PDF导出");

		int slash = max(filePath.ReverseFind(_T('\\')), filePath.ReverseFind(_T('/')));
		CString name = (slash >= 0) ? filePath.Mid(slash + 1) : filePath;
		int dot = name.ReverseFind(_T('.'));
		if (dot > 0)
			name = name.Left(dot);
		return name.IsEmpty() ? CString(_T("PDF导出")) : name;
	}

	CString extractDirectory(const CString& filePath)
	{
		int slash = max(filePath.ReverseFind(_T('\\')), filePath.ReverseFind(_T('/')));
		if (slash > 0)
			return filePath.Left(slash);
		return CString();
	}

	bool pickOutputFolder(HWND hOwner, const CString& initialDir, CString& outFolder)
	{
		outFolder.Empty();
		HRESULT hrCo = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
		const bool needUninit = SUCCEEDED(hrCo);

		bool picked = false;
		IFileOpenDialog* pDialog = nullptr;
		HRESULT hr = ::CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
			IID_PPV_ARGS(&pDialog));
		if (SUCCEEDED(hr) && pDialog)
		{
			DWORD options = 0;
			if (SUCCEEDED(pDialog->GetOptions(&options)))
			{
				pDialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
			}
			pDialog->SetTitle(L"选择导出图片的保存文件夹");

			if (!initialDir.IsEmpty())
			{
				IShellItem* pInitialFolder = nullptr;
				if (SUCCEEDED(::SHCreateItemFromParsingName(initialDir, nullptr, IID_PPV_ARGS(&pInitialFolder))) &&
					pInitialFolder)
				{
					pDialog->SetFolder(pInitialFolder);
					pInitialFolder->Release();
				}
			}

			hr = pDialog->Show(hOwner);
			if (SUCCEEDED(hr))
			{
				IShellItem* pResult = nullptr;
				if (SUCCEEDED(pDialog->GetResult(&pResult)) && pResult)
				{
					PWSTR pszPath = nullptr;
					if (SUCCEEDED(pResult->GetDisplayName(SIGDN_FILESYSPATH, &pszPath)) && pszPath)
					{
						outFolder = pszPath;
						::CoTaskMemFree(pszPath);
						picked = !outFolder.IsEmpty();
					}
					pResult->Release();
				}
			}
			pDialog->Release();
			if (needUninit)
				::CoUninitialize();
			if (hr == HRESULT_FROM_WIN32(ERROR_CANCELLED))
				return false;
			if (picked)
				return true;
		}
		else if (needUninit)
		{
			::CoUninitialize();
		}

		// 回退：传统文件夹选择框
		BROWSEINFO bi = {};
		TCHAR displayName[MAX_PATH] = {};
		bi.hwndOwner = hOwner;
		bi.pszDisplayName = displayName;
		bi.lpszTitle = _T("选择导出图片的保存文件夹");
		bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
		PIDLIST_ABSOLUTE pidl = ::SHBrowseForFolder(&bi);
		if (!pidl)
			return false;

		TCHAR folderPath[MAX_PATH] = {};
		const BOOL ok = ::SHGetPathFromIDList(pidl, folderPath);
		::CoTaskMemFree(pidl);
		if (!ok || folderPath[0] == _T('\0'))
			return false;

		outFolder = folderPath;
		return true;
	}

	void revealPathInExplorer(const CString& targetPath, bool isDirectory)
	{
		if (targetPath.IsEmpty())
			return;

		if (isDirectory)
		{
			::ShellExecute(nullptr, _T("open"), targetPath, nullptr, nullptr, SW_SHOWNORMAL);
			return;
		}

		PIDLIST_ABSOLUTE pidl = ::ILCreateFromPath(targetPath);
		if (pidl)
		{
			::SHOpenFolderAndSelectItems(pidl, 0, nullptr, 0);
			::ILFree(pidl);
		}
		else
		{
			CString dir = extractDirectory(targetPath);
			if (!dir.IsEmpty())
				::ShellExecute(nullptr, _T("open"), dir, nullptr, nullptr, SW_SHOWNORMAL);
		}
	}
}

bool SaveBgrxBufferToImageFile(const BYTE* pTopDownBgrx, int width, int height,
	int stride, const CString& filePath, ExportImageFormat format,
	int dpi, int jpegQuality)
{
	if (!pTopDownBgrx || width <= 0 || height <= 0 || stride < width * 4 || filePath.IsEmpty())
		return false;

	const WCHAR* mime = (format == ExportImageFormat::Jpg) ? L"image/jpeg" : L"image/png";
	CLSID clsid = {};
	if (!getGdiplusEncoderClsid(mime, clsid))
		return false;

	Gdiplus::Bitmap bitmap(width, height, stride, PixelFormat32bppRGB,
		const_cast<BYTE*>(pTopDownBgrx));
	if (bitmap.GetLastStatus() != Gdiplus::Ok)
		return false;

	if (dpi > 0)
		bitmap.SetResolution(static_cast<Gdiplus::REAL>(dpi), static_cast<Gdiplus::REAL>(dpi));

	if (format == ExportImageFormat::Jpg)
	{
		ULONG quality = static_cast<ULONG>(max(10, min(100, jpegQuality)));
		Gdiplus::EncoderParameters params = {};
		params.Count = 1;
		params.Parameter[0].Guid = Gdiplus::EncoderQuality;
		params.Parameter[0].Type = Gdiplus::EncoderParameterValueTypeLong;
		params.Parameter[0].NumberOfValues = 1;
		params.Parameter[0].Value = &quality;
		return bitmap.Save(CStringW(filePath), &clsid, &params) == Gdiplus::Ok;
	}

	return bitmap.Save(CStringW(filePath), &clsid, nullptr) == Gdiplus::Ok;
}

bool CopyBgrxBufferToClipboard(HWND hOwnerWnd, const BYTE* pTopDownBgrx,
	int width, int height, int stride)
{
	if (!pTopDownBgrx || width <= 0 || height <= 0 || stride < width * 4)
		return false;

	const size_t rowBytes = static_cast<size_t>(width) * 4;
	const size_t imageBytes = rowBytes * static_cast<size_t>(height);
	const size_t totalBytes = sizeof(BITMAPINFOHEADER) + imageBytes;

	HGLOBAL hGlobal = ::GlobalAlloc(GMEM_MOVEABLE, totalBytes);
	if (!hGlobal)
		return false;

	BYTE* pDest = static_cast<BYTE*>(::GlobalLock(hGlobal));
	if (!pDest)
	{
		::GlobalFree(hGlobal);
		return false;
	}

	BITMAPINFOHEADER bih = {};
	bih.biSize = sizeof(BITMAPINFOHEADER);
	bih.biWidth = width;
	bih.biHeight = height; // 正值 bottom-up DIB，兼容微信、Office、画图等全部 Windows 程序
	bih.biPlanes = 1;
	bih.biBitCount = 32;
	bih.biCompression = BI_RGB;
	bih.biSizeImage = static_cast<DWORD>(imageBytes);
	std::memcpy(pDest, &bih, sizeof(bih));

	BYTE* pDestPixels = pDest + sizeof(BITMAPINFOHEADER);
	for (int y = 0; y < height; ++y)
	{
		const BYTE* pSrcRow = pTopDownBgrx + static_cast<size_t>(height - 1 - y) * stride;
		BYTE* pDstRow = pDestPixels + static_cast<size_t>(y) * rowBytes;
		std::memcpy(pDstRow, pSrcRow, rowBytes);
	}
	::GlobalUnlock(hGlobal);

	if (!::OpenClipboard(hOwnerWnd))
	{
		::GlobalFree(hGlobal);
		return false;
	}

	::EmptyClipboard();
	bool copied = false;
	if (::SetClipboardData(CF_DIB, hGlobal) != nullptr)
	{
		hGlobal = nullptr;
		copied = true;
	}
	::CloseClipboard();

	if (hGlobal)
		::GlobalFree(hGlobal);
	return copied;
}

// ============================================================================
// CPDFExportImageDlg 实现
// ============================================================================

BEGIN_MESSAGE_MAP(CPDFExportImageDlg, CDialogEx)
	ON_BN_CLICKED(IDC_EXPORT_CURRENT_PAGE, &CPDFExportImageDlg::OnScopeChanged)
	ON_BN_CLICKED(IDC_EXPORT_ALL_PAGES, &CPDFExportImageDlg::OnScopeChanged)
	ON_BN_CLICKED(IDC_EXPORT_PAGE_RANGE, &CPDFExportImageDlg::OnScopeChanged)
END_MESSAGE_MAP()

CPDFExportImageDlg::CPDFExportImageDlg(int totalPages, int currentPageOneBased, CWnd* pParent)
	: CDialogEx(IDD_EXPORT_IMAGE_DIALOG, pParent)
	, m_scope(ExportPageScope::CurrentPage)
	, m_pageFrom(1)
	, m_pageTo(max(1, totalPages))
	, m_format(ExportImageFormat::Png)
	, m_dpi(300)
	, m_mergeLongImage(false)
	, m_totalPages(max(1, totalPages))
	, m_currentPageOneBased(max(1, min(currentPageOneBased, max(1, totalPages))))
{
}

void CPDFExportImageDlg::DoDataExchange(CDataExchange* pDX)
{
	CDialogEx::DoDataExchange(pDX);
	DDX_Control(pDX, IDC_EXPORT_CURRENT_PAGE, m_radioCurrent);
	DDX_Control(pDX, IDC_EXPORT_ALL_PAGES, m_radioAll);
	DDX_Control(pDX, IDC_EXPORT_PAGE_RANGE, m_radioRange);
	DDX_Control(pDX, IDC_EXPORT_PAGE_FROM, m_editFrom);
	DDX_Control(pDX, IDC_EXPORT_PAGE_TO, m_editTo);
	DDX_Control(pDX, IDC_EXPORT_FORMAT_COMBO, m_comboFormat);
	DDX_Control(pDX, IDC_EXPORT_DPI_COMBO, m_comboDpi);
	DDX_Control(pDX, IDC_EXPORT_MODE_SEPARATE, m_radioSeparate);
	DDX_Control(pDX, IDC_EXPORT_MODE_LONG, m_radioLongImage);
}

BOOL CPDFExportImageDlg::OnInitDialog()
{
	CDialogEx::OnInitDialog();

	CString currentLabel;
	currentLabel.Format(_T("当前页 (第 %d 页)"), m_currentPageOneBased);
	m_radioCurrent.SetWindowText(currentLabel);

	CString allLabel;
	allLabel.Format(_T("全部页面 (共 %d 页)"), m_totalPages);
	m_radioAll.SetWindowText(allLabel);

	m_radioCurrent.SetCheck(m_scope == ExportPageScope::CurrentPage ? BST_CHECKED : BST_UNCHECKED);
	m_radioAll.SetCheck(m_scope == ExportPageScope::AllPages ? BST_CHECKED : BST_UNCHECKED);
	m_radioRange.SetCheck(m_scope == ExportPageScope::PageRange ? BST_CHECKED : BST_UNCHECKED);

	CString strNum;
	strNum.Format(_T("%d"), m_pageFrom);
	m_editFrom.SetWindowText(strNum);
	strNum.Format(_T("%d"), m_pageTo);
	m_editTo.SetWindowText(strNum);

	m_comboFormat.AddString(_T("PNG 高清无损图片 (*.png)"));
	m_comboFormat.AddString(_T("JPG 高清图片 (*.jpg)"));
	m_comboFormat.SetCurSel(m_format == ExportImageFormat::Jpg ? 1 : 0);

	m_comboDpi.AddString(_T("标准清晰 (150 DPI)"));
	m_comboDpi.AddString(_T("高清推荐 (300 DPI)"));
	m_comboDpi.AddString(_T("超清图纸/印刷 (600 DPI)"));
	m_comboDpi.SetCurSel(m_dpi <= 150 ? 0 : (m_dpi >= 600 ? 2 : 1));

	m_radioSeparate.SetCheck(m_mergeLongImage ? BST_UNCHECKED : BST_CHECKED);
	m_radioLongImage.SetCheck(m_mergeLongImage ? BST_CHECKED : BST_UNCHECKED);

	updateRangeControls();
	return TRUE;
}

void CPDFExportImageDlg::updateRangeControls()
{
	const BOOL enableRange = (m_radioRange.GetCheck() == BST_CHECKED);
	m_editFrom.EnableWindow(enableRange);
	m_editTo.EnableWindow(enableRange);
}

void CPDFExportImageDlg::OnScopeChanged()
{
	updateRangeControls();
}

void CPDFExportImageDlg::OnOK()
{
	if (m_radioCurrent.GetCheck() == BST_CHECKED)
	{
		m_scope = ExportPageScope::CurrentPage;
		m_pageFrom = m_currentPageOneBased;
		m_pageTo = m_currentPageOneBased;
	}
	else if (m_radioAll.GetCheck() == BST_CHECKED)
	{
		m_scope = ExportPageScope::AllPages;
		m_pageFrom = 1;
		m_pageTo = m_totalPages;
	}
	else
	{
		m_scope = ExportPageScope::PageRange;
		CString fromStr;
		CString toStr;
		m_editFrom.GetWindowText(fromStr);
		m_editTo.GetWindowText(toStr);
		m_pageFrom = _ttoi(fromStr);
		m_pageTo = _ttoi(toStr);

		if (m_pageFrom < 1 || m_pageFrom > m_totalPages ||
			m_pageTo < 1 || m_pageTo > m_totalPages ||
			m_pageFrom > m_pageTo)
		{
			CString msg;
			msg.Format(_T("页码范围无效，请输入 1 到 %d 之间的起止页码。"), m_totalPages);
			MessageBox(msg, _T("提示"), MB_OK | MB_ICONWARNING);
			return;
		}
	}

	m_format = (m_comboFormat.GetCurSel() == 1) ? ExportImageFormat::Jpg : ExportImageFormat::Png;
	const int dpiSel = m_comboDpi.GetCurSel();
	m_dpi = (dpiSel == 0) ? 150 : (dpiSel == 2 ? 600 : 300);
	m_mergeLongImage = (m_radioLongImage.GetCheck() == BST_CHECKED);

	const int count = m_pageTo - m_pageFrom + 1;
	if (m_mergeLongImage && count > LONG_IMAGE_MAX_PAGES)
	{
		CString msg;
		msg.Format(_T("合并拼接长图最多支持 %d 页（当前选择了 %d 页）。\n请缩小页码范围，或改选“逐页导出为独立图片”。"),
			LONG_IMAGE_MAX_PAGES, count);
		MessageBox(msg, _T("提示"), MB_OK | MB_ICONWARNING);
		return;
	}

	CDialogEx::OnOK();
}

// ============================================================================
// 批量导出与长图拼接主流程
// ============================================================================

bool RunExportPagesToImages(
	CWnd* pParent,
	int totalPages,
	int currentPageZeroBased,
	const CString& documentFilePath,
	const std::function<bool(int pageIndex, float& outWidthPt, float& outHeightPt)>& getPageSizePtFn,
	const RenderExportPageCallback& renderPageFn,
	CString* pOutSummaryMessage,
	int presetRangeFromOneBased,
	int presetRangeToOneBased)
{
	if (totalPages <= 0 || !renderPageFn)
		return false;

	CPDFExportImageDlg exportDlg(totalPages, currentPageZeroBased + 1, pParent);
	if (presetRangeFromOneBased >= 1 && presetRangeToOneBased >= presetRangeFromOneBased &&
		presetRangeToOneBased <= totalPages)
	{
		exportDlg.m_scope = ExportPageScope::PageRange;
		exportDlg.m_pageFrom = presetRangeFromOneBased;
		exportDlg.m_pageTo = presetRangeToOneBased;
	}

	if (exportDlg.DoModal() != IDOK)
		return false;

	const int startPage = max(0, exportDlg.m_pageFrom - 1);
	const int endPage = min(totalPages - 1, exportDlg.m_pageTo - 1);
	if (startPage > endPage)
		return false;

	const int pageCount = endPage - startPage + 1;
	const ExportImageFormat format = exportDlg.m_format;
	const LPCTSTR ext = (format == ExportImageFormat::Jpg) ? _T("jpg") : _T("png");
	const LPCTSTR filter = (format == ExportImageFormat::Jpg) ?
		_T("JPG 图片 (*.jpg)|*.jpg||") :
		_T("PNG 图片 (*.png)|*.png||");

	const CString baseName = extractBaseNameWithoutExt(documentFilePath);
	const CString initialDir = extractDirectory(documentFilePath);
	const bool singleFileOutput = exportDlg.m_mergeLongImage || (pageCount == 1);

	CString targetFilePath;
	CString targetFolderPath;
	if (singleFileOutput)
	{
		CString defaultFileName;
		if (exportDlg.m_mergeLongImage && pageCount > 1)
			defaultFileName.Format(_T("%s_第%d-%d页_长图.%s"),
				baseName.GetString(), startPage + 1, endPage + 1, ext);
		else
			defaultFileName.Format(_T("%s_第%d页.%s"),
				baseName.GetString(), startPage + 1, ext);

		CString initialPath = defaultFileName;
		if (!initialDir.IsEmpty())
			initialPath = initialDir + _T("\\") + defaultFileName;

		CFileDialog saveDlg(FALSE, ext, initialPath,
			OFN_HIDEREADONLY | OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST,
			filter, pParent);
		if (saveDlg.DoModal() != IDOK)
			return false;
		targetFilePath = saveDlg.GetPathName();
	}
	else
	{
		if (!pickOutputFolder(pParent ? pParent->GetSafeHwnd() : nullptr, initialDir, targetFolderPath))
			return false;
	}

	CProgressDlg progress(pParent);
	progress.SetLoadCancelNotification(false);
	const bool hasProgressWnd = progress.Create(IDD_PROGRESS_DIALOG, pParent) != FALSE;
	if (hasProgressWnd)
	{
		progress.SetWindowText(_T("正在导出图片"));
		progress.SetIndeterminate(false);
		progress.ShowWindow(SW_SHOW);
		if (pParent && pParent->GetSafeHwnd())
			pParent->EnableWindow(FALSE);
		progress.EnableWindow(TRUE);
	}

	auto closeProgress = [&]()
	{
		if (hasProgressWnd)
		{
			if (pParent && pParent->GetSafeHwnd())
				pParent->EnableWindow(TRUE);
			if (progress.GetSafeHwnd())
				progress.DestroyWindow();
			if (pParent && pParent->GetSafeHwnd())
				pParent->SetActiveWindow();
		}
	};

	auto pumpProgressMessages = [&]() -> bool
	{
		if (!hasProgressWnd || !progress.GetSafeHwnd())
			return true;
		MSG message;
		while (::PeekMessage(&message, progress.GetSafeHwnd(), 0, 0, PM_REMOVE))
		{
			if (!progress.IsDialogMessage(&message))
			{
				::TranslateMessage(&message);
				::DispatchMessage(&message);
			}
		}
		return !progress.IsCancelled();
	};

	float baseScale = static_cast<float>(exportDlg.m_dpi) / 72.0f;
	int effectiveDpi = exportDlg.m_dpi;

	// 长图模式：预先计算总宽高并在超限时自适应缩放，确保不超过 GDI+/JPEG 像素高度与内存上限
	if (exportDlg.m_mergeLongImage)
	{
		double maxPageWidthPt = 0.0;
		double totalHeightPt = 0.0;
		if (getPageSizePtFn)
		{
			for (int p = startPage; p <= endPage; ++p)
			{
				float wPt = 0.0f;
				float hPt = 0.0f;
				if (getPageSizePtFn(p, wPt, hPt) && wPt > 0.0f && hPt > 0.0f)
				{
					maxPageWidthPt = max(maxPageWidthPt, static_cast<double>(wPt));
					totalHeightPt += static_cast<double>(hPt);
				}
			}
		}

		if (maxPageWidthPt > 0.0 && totalHeightPt > 0.0)
		{
			const int totalGapsPx = max(0, pageCount - 1) * LONG_IMAGE_PAGE_GAP;
			const int availHeightPx = max(1000, LONG_IMAGE_MAX_DIMENSION - totalGapsPx);
			if (totalHeightPt * baseScale > availHeightPx)
				baseScale = static_cast<float>(availHeightPx / totalHeightPt);
			if (maxPageWidthPt * baseScale > LONG_IMAGE_MAX_DIMENSION)
				baseScale = static_cast<float>(LONG_IMAGE_MAX_DIMENSION / maxPageWidthPt);

			const double estWidth = maxPageWidthPt * baseScale;
			const double estHeight = totalHeightPt * baseScale + totalGapsPx;
			if (estWidth * estHeight > LONG_IMAGE_MAX_PIXELS)
			{
				baseScale *= static_cast<float>(std::sqrt(LONG_IMAGE_MAX_PIXELS / (estWidth * estHeight)));
			}
			effectiveDpi = max(72, static_cast<int>(baseScale * 72.0f + 0.5f));
		}

		std::vector<ExportBitmapBuffer> renderedPages;
		renderedPages.reserve(pageCount);
		int canvasWidth = 1;
		long long canvasHeight = 0;
		bool cancelled = false;
		int failedPage = -1;

		for (int i = 0; i < pageCount; ++i)
		{
			const int pageNum = startPage + i;
			if (hasProgressWnd)
			{
				CString status;
				status.Format(_T("正在渲染第 %d 页 (%d / %d)..."), pageNum + 1, i + 1, pageCount);
				progress.SetStatus(status);
				progress.SetProgress(static_cast<int>((i * 85LL) / pageCount));
				progress.UpdateWindow();
			}
			if (!pumpProgressMessages())
			{
				cancelled = true;
				break;
			}

			ExportBitmapBuffer pageBuf;
			if (!renderPageFn(pageNum, baseScale, SINGLE_PAGE_MAX_PIXELS, pageBuf) ||
				pageBuf.width <= 0 || pageBuf.height <= 0)
			{
				failedPage = pageNum;
				break;
			}

			canvasWidth = max(canvasWidth, pageBuf.width);
			if (i > 0)
				canvasHeight += LONG_IMAGE_PAGE_GAP;
			canvasHeight += pageBuf.height;
			renderedPages.push_back(std::move(pageBuf));
		}

		if (cancelled)
		{
			closeProgress();
			return false;
		}
		if (failedPage >= 0 || canvasHeight <= 0 || canvasHeight > LONG_IMAGE_MAX_DIMENSION)
		{
			closeProgress();
			CString msg;
			if (failedPage >= 0)
				msg.Format(_T("渲染第 %d 页失败，导出已中止。"), failedPage + 1);
			else
				msg = _T("合并后的长图尺寸超出图像编码上限，请减少页数或降低清晰度后重试。");
			if (pParent)
				pParent->MessageBox(msg, _T("导出失败"), MB_OK | MB_ICONERROR);
			return false;
		}

		if (hasProgressWnd)
		{
			progress.SetStatus(_T("正在拼接并编码保存长图..."));
			progress.SetProgress(92);
			progress.UpdateWindow();
		}

		const int finalHeight = static_cast<int>(canvasHeight);
		const int canvasStride = canvasWidth * 4;
		std::vector<BYTE> longPixels;
		try
		{
			longPixels.assign(static_cast<size_t>(canvasStride) * static_cast<size_t>(finalHeight), 0xF4);
		}
		catch (...)
		{
			closeProgress();
			if (pParent)
				pParent->MessageBox(_T("内存不足，无法分配长图画布，请减少页数或降低分辨率。"),
					_T("导出失败"), MB_OK | MB_ICONERROR);
			return false;
		}

		int curY = 0;
		for (size_t i = 0; i < renderedPages.size(); ++i)
		{
			const ExportBitmapBuffer& pageBuf = renderedPages[i];
			const int offsetX = (canvasWidth - pageBuf.width) / 2;
			for (int row = 0; row < pageBuf.height; ++row)
			{
				BYTE* pDst = longPixels.data() +
					static_cast<size_t>(curY + row) * canvasStride +
					static_cast<size_t>(offsetX) * 4;
				const BYTE* pSrc = pageBuf.bgra.data() + static_cast<size_t>(row) * pageBuf.stride;
				std::memcpy(pDst, pSrc, static_cast<size_t>(pageBuf.width) * 4);
			}
			curY += pageBuf.height;
			if (i + 1 < renderedPages.size())
			{
				// 页面分隔线，视觉层次更清晰
				const int lineY = curY + LONG_IMAGE_PAGE_GAP / 2;
				if (lineY >= 0 && lineY < finalHeight)
				{
					BYTE* pLine = longPixels.data() + static_cast<size_t>(lineY) * canvasStride;
					std::memset(pLine, 0xDC, static_cast<size_t>(canvasStride));
				}
				curY += LONG_IMAGE_PAGE_GAP;
			}
		}
		renderedPages.clear();
		renderedPages.shrink_to_fit();

		const bool saved = SaveBgrxBufferToImageFile(longPixels.data(), canvasWidth, finalHeight,
			canvasStride, targetFilePath, format, effectiveDpi);
		closeProgress();

		if (!saved)
		{
			if (pParent)
				pParent->MessageBox(_T("保存长图文件失败，请检查磁盘空间和写入权限。"),
					_T("导出失败"), MB_OK | MB_ICONERROR);
			return false;
		}

		if (pOutSummaryMessage)
			pOutSummaryMessage->Format(_T("已导出长图: %s (%d×%d)"),
				targetFilePath.GetString(), canvasWidth, finalHeight);

		if (pParent)
		{
			CString doneMsg;
			doneMsg.Format(_T("已成功将 %d 页拼接导出为长图！\n尺寸：%d × %d 像素\n\n是否打开文件所在位置？"),
				pageCount, canvasWidth, finalHeight);
			if (pParent->MessageBox(doneMsg, _T("导出成功"), MB_YESNO | MB_ICONINFORMATION) == IDYES)
				revealPathInExplorer(targetFilePath, false);
		}
		return true;
	}

	// 逐页导出模式（单页保存至指定文件；多页批量保存至目标文件夹）
	int digits = 3;
	if (totalPages >= 1000)
		digits = 4;

	bool cancelled = false;
	int failedPage = -1;
	CString firstSavedFile;

	for (int i = 0; i < pageCount; ++i)
	{
		const int pageNum = startPage + i;
		if (hasProgressWnd)
		{
			CString status;
			status.Format(_T("正在导出第 %d 页 (%d / %d)..."), pageNum + 1, i + 1, pageCount);
			progress.SetStatus(status);
			progress.SetProgress(static_cast<int>(((i + 1) * 100LL) / pageCount));
			progress.UpdateWindow();
		}
		if (!pumpProgressMessages())
		{
			cancelled = true;
			break;
		}

		ExportBitmapBuffer pageBuf;
		if (!renderPageFn(pageNum, baseScale, SINGLE_PAGE_MAX_PIXELS, pageBuf) ||
			pageBuf.width <= 0 || pageBuf.height <= 0)
		{
			failedPage = pageNum;
			break;
		}

		CString outPath = targetFilePath;
		if (!singleFileOutput)
		{
			CString fileName;
			fileName.Format(_T("%s_第%0*d页.%s"),
				baseName.GetString(), digits, pageNum + 1, ext);
			outPath = targetFolderPath;
			if (outPath.Right(1) != _T("\\") && outPath.Right(1) != _T("/"))
				outPath += _T("\\");
			outPath += fileName;
		}

		if (!SaveBgrxBufferToImageFile(pageBuf.bgra.data(), pageBuf.width, pageBuf.height,
			pageBuf.stride, outPath, format, effectiveDpi))
		{
			failedPage = pageNum;
			break;
		}
		if (firstSavedFile.IsEmpty())
			firstSavedFile = outPath;
	}

	closeProgress();

	if (cancelled)
		return false;

	if (failedPage >= 0)
	{
		CString msg;
		msg.Format(_T("导出第 %d 页时失败，请检查目标目录写入权限或磁盘空间。"), failedPage + 1);
		if (pParent)
			pParent->MessageBox(msg, _T("导出失败"), MB_OK | MB_ICONERROR);
		return false;
	}

	if (pOutSummaryMessage)
	{
		if (singleFileOutput)
			pOutSummaryMessage->Format(_T("已导出图片: %s"), targetFilePath.GetString());
		else
			pOutSummaryMessage->Format(_T("已批量导出 %d 张图片至: %s"), pageCount, targetFolderPath.GetString());
	}

	if (pParent)
	{
		CString doneMsg;
		if (singleFileOutput)
			doneMsg.Format(_T("图片导出成功！\n%s\n\n是否打开文件所在位置？"), targetFilePath.GetString());
		else
			doneMsg.Format(_T("已成功批量导出 %d 张高清图片！\n保存目录：%s\n\n是否打开输出文件夹？"),
				pageCount, targetFolderPath.GetString());
		if (pParent->MessageBox(doneMsg, _T("导出成功"), MB_YESNO | MB_ICONINFORMATION) == IDYES)
		{
			revealPathInExplorer(singleFileOutput ? targetFilePath : firstSavedFile, false);
		}
	}
	return true;
}
