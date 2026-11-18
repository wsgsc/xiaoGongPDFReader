// PDFEditDialog.cpp: PDF页面编辑对话框实现文件

#include "pch.h"
#include "PDFEditDialog.h"
#include "resource.h"
#include <mupdf/pdf.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>
#include <map>
#include <objidl.h>
#include <utility>
#include <gdiplus.h>

#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "ole32.lib")

#ifdef _DEBUG
#define new DEBUG_NEW
#endif

namespace
{
	class CEditPasswordDialog : public CDialogEx
	{
	public:
		CEditPasswordDialog(const CString& fileName, bool invalid, CWnd* pParent)
			: CDialogEx(IDD_PASSWORD_DIALOG, pParent), m_fileName(fileName), m_invalid(invalid) {}
		~CEditPasswordDialog()
		{
			if (!m_password.IsEmpty())
			{
				SecureZeroMemory(m_password.GetBuffer(), m_password.GetLength() * sizeof(TCHAR));
				m_password.ReleaseBuffer(0);
			}
		}
		const CString& GetPassword() const { return m_password; }
	protected:
		BOOL OnInitDialog() override
		{
			CDialogEx::OnInitDialog();
			SetDlgItemText(IDC_PASSWORD_FILE, m_fileName);
			if (m_invalid)
				SetDlgItemText(IDC_PASSWORD_PROMPT, _T("密码错误，请重新输入"));
			GetDlgItem(IDC_EDIT_PASSWORD)->SetFocus();
			return FALSE;
		}
		void OnOK() override
		{
			GetDlgItemText(IDC_EDIT_PASSWORD, m_password);
			CDialogEx::OnOK();
		}
	private:
		CString m_fileName;
		CString m_password;
		bool m_invalid;
	};

	const int THUMBNAIL_WIDTH = 220;
	const int THUMBNAIL_HEIGHT = 280;
	const int THUMBNAIL_ACTION_HEIGHT = 318;
	const CRect PAGE_ACTION_BUTTONS[] = {
		CRect(38, 282, 86, 316), CRect(134, 282, 182, 316)
	};

	void DrawPngResource(Gdiplus::Graphics& graphics, UINT resourceId,
		const Gdiplus::RectF& bounds)
	{
		HINSTANCE module = AfxGetResourceHandle();
		HRSRC resource = ::FindResource(module, MAKEINTRESOURCE(resourceId), RT_RCDATA);
		if (!resource)
			return;
		DWORD size = ::SizeofResource(module, resource);
		const void* data = ::LockResource(::LoadResource(module, resource));
		if (!data || size == 0)
			return;
		HGLOBAL copy = ::GlobalAlloc(GMEM_MOVEABLE, size);
		if (!copy)
			return;
		void* copiedData = ::GlobalLock(copy);
		if (!copiedData)
		{
			::GlobalFree(copy);
			return;
		}
		std::copy_n(static_cast<const BYTE*>(data), size, static_cast<BYTE*>(copiedData));
		::GlobalUnlock(copy);
		IStream* stream = nullptr;
		if (FAILED(::CreateStreamOnHGlobal(copy, TRUE, &stream)))
		{
			::GlobalFree(copy);
			return;
		}
		{
			Gdiplus::Bitmap icon(stream);
			if (icon.GetLastStatus() == Gdiplus::Ok)
				graphics.DrawImage(&icon, bounds);
		}
		stream->Release();
	}

	void DrawPageActionButtons(BYTE* destination, int destinationStride)
	{
		const int actionHeight = THUMBNAIL_ACTION_HEIGHT - THUMBNAIL_HEIGHT;
		Gdiplus::Bitmap buttonBitmap(THUMBNAIL_WIDTH, actionHeight, PixelFormat32bppARGB);
		if (buttonBitmap.GetLastStatus() != Gdiplus::Ok)
			return;
		{
			Gdiplus::Graphics graphics(&buttonBitmap);
			graphics.Clear(Gdiplus::Color(0, 0, 0, 0));
			graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
			graphics.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
			Gdiplus::SolidBrush background(Gdiplus::Color(255, 250, 252, 253));
			Gdiplus::Pen border(Gdiplus::Color(255, 203, 216, 224), 1.0f);
			for (int action = 0; action < 2; ++action)
			{
				const CRect& button = PAGE_ACTION_BUTTONS[action];
				float x = static_cast<float>(button.left) + 0.5f;
				float y = static_cast<float>(button.top - THUMBNAIL_HEIGHT) + 0.5f;
				float width = static_cast<float>(button.Width() - 1);
				float height = static_cast<float>(button.Height() - 1);
				const float radius = 4.0f;
				Gdiplus::GraphicsPath outline;
				outline.AddArc(x, y, radius * 2, radius * 2, 180, 90);
				outline.AddArc(x + width - radius * 2, y, radius * 2, radius * 2, 270, 90);
				outline.AddArc(x + width - radius * 2, y + height - radius * 2,
					radius * 2, radius * 2, 0, 90);
				outline.AddArc(x, y + height - radius * 2, radius * 2, radius * 2, 90, 90);
				outline.CloseFigure();
				graphics.FillPath(&background, &outline);
				graphics.DrawPath(&border, &outline);

				Gdiplus::RectF iconBounds(
					(button.left + button.right - 26) / 2.0f,
					(button.top + button.bottom - 26) / 2.0f - THUMBNAIL_HEIGHT,
					26.0f, 26.0f);
				DrawPngResource(graphics, action == 0 ? IDR_ROTATE_LEFT_PNG : IDR_ROTATE_RIGHT_PNG,
					iconBounds);
			}
		}

		Gdiplus::Rect area(0, 0, THUMBNAIL_WIDTH, actionHeight);
		Gdiplus::BitmapData pixels = { 0 };
		if (buttonBitmap.LockBits(&area, Gdiplus::ImageLockModeRead,
			PixelFormat32bppARGB, &pixels) != Gdiplus::Ok)
			return;
		for (int y = 0; y < actionHeight; ++y)
		{
			const BYTE* source = static_cast<const BYTE*>(pixels.Scan0) + y * pixels.Stride;
			BYTE* target = destination + (THUMBNAIL_HEIGHT + y) * destinationStride;
			for (int x = 0; x < THUMBNAIL_WIDTH; ++x)
			{
				int alpha = source[x * 4 + 3];
				if (alpha == 0)
					continue;
				for (int channel = 0; channel < 3; ++channel)
					target[x * 3 + channel] = static_cast<BYTE>(
						(source[x * 4 + channel] * alpha + target[x * 3 + channel] * (255 - alpha)) / 255);
			}
		}
		buttonBitmap.UnlockBits(&pixels);
	}

	CString NormalizePathForCompare(const CString& path)
	{
		if (path.IsEmpty())
			return CString();
		DWORD needed = ::GetFullPathName(path, 0, nullptr, nullptr);
		if (needed == 0)
			return path;
		std::vector<TCHAR> buffer(needed + 1, 0);
		if (::GetFullPathName(path, needed + 1, buffer.data(), nullptr) == 0)
			return path;
		return CString(buffer.data());
	}

	bool IsImageFilePath(const CString& filePath)
	{
		int dot = filePath.ReverseFind(_T('.'));
		if (dot < 0)
			return false;
		CString ext = filePath.Mid(dot);
		ext.MakeLower();
		return ext == _T(".jpg") || ext == _T(".jpeg") || ext == _T(".png") || ext == _T(".bmp");
	}

	void InsertImagePageToPdf(fz_context* ctx, pdf_document* targetPdf, int insertIndex,
		const CString& imagePath)
	{
		CW2A imagePathA(imagePath, CP_UTF8);
		fz_image* image = nullptr;
		fz_buffer* contents_buf = nullptr;
		fz_var(image);
		fz_var(contents_buf);

		fz_try(ctx)
		{
			image = fz_new_image_from_file(ctx, imagePathA);
			if (!image || image->w <= 0 || image->h <= 0)
				fz_throw(ctx, FZ_ERROR_GENERIC, "无效的图片尺寸");

			const int imgWidth = image->w;
			const int imgHeight = image->h;

			// 按未旋转的原始宽高比选择 A4 纵向/横向页面；
			// 页面旋转统一由后续 /Rotate 属性完成，避免二次旋转和比例压缩变形。
			const fz_rect mediabox = (imgWidth > imgHeight) ?
				fz_make_rect(0, 0, 842, 595) :
				fz_make_rect(0, 0, 595, 842);

			pdf_obj* page_obj = pdf_add_page(ctx, targetPdf, mediabox, 0, nullptr, nullptr);
			pdf_insert_page(ctx, targetPdf, insertIndex, page_obj);

			const float pageWidth = mediabox.x1 - mediabox.x0;
			const float pageHeight = mediabox.y1 - mediabox.y0;
			const float scale = fmin(pageWidth / static_cast<float>(imgWidth),
				pageHeight / static_cast<float>(imgHeight));
			const float w = imgWidth * scale;
			const float h = imgHeight * scale;
			const float x = (pageWidth - w) / 2.0f;
			const float y = (pageHeight - h) / 2.0f;

			pdf_obj* image_obj = pdf_add_image(ctx, targetPdf, image);
			pdf_obj* resources = pdf_dict_get(ctx, page_obj, PDF_NAME(Resources));
			if (!resources)
			{
				resources = pdf_new_dict(ctx, targetPdf, 1);
				pdf_dict_put(ctx, page_obj, PDF_NAME(Resources), resources);
				pdf_drop_obj(ctx, resources);
				resources = pdf_dict_get(ctx, page_obj, PDF_NAME(Resources));
			}

			pdf_obj* xobj_dict = pdf_dict_get(ctx, resources, PDF_NAME(XObject));
			if (!xobj_dict)
			{
				xobj_dict = pdf_new_dict(ctx, targetPdf, 1);
				pdf_dict_put(ctx, resources, PDF_NAME(XObject), xobj_dict);
				pdf_drop_obj(ctx, xobj_dict);
				xobj_dict = pdf_dict_get(ctx, resources, PDF_NAME(XObject));
			}

			pdf_dict_puts(ctx, xobj_dict, "Im0", image_obj);
			pdf_drop_obj(ctx, image_obj);

			contents_buf = fz_new_buffer(ctx, 256);
			fz_append_printf(ctx, contents_buf, "q\n%g 0 0 %g %g %g cm\n/Im0 Do\nQ\n",
				w, h, x, y);

			pdf_obj* contents = pdf_add_stream(ctx, targetPdf, contents_buf, nullptr, 0);
			pdf_dict_put(ctx, page_obj, PDF_NAME(Contents), contents);
			pdf_drop_obj(ctx, contents);
		}
		fz_always(ctx)
		{
			if (contents_buf)
				fz_drop_buffer(ctx, contents_buf);
			if (image)
				fz_drop_image(ctx, image);
		}
		fz_catch(ctx)
		{
			fz_rethrow(ctx);
		}
	}

	bool WriteAndValidateTempPdf(fz_context* ctx, pdf_document* pdf, const CString& targetPath,
		int expectedPages, const pdf_write_options& options, CString& outTempPath,
		const std::vector<char>* password = nullptr)
	{
		outTempPath.Empty();
		if (expectedPages < 1 || pdf_count_pages(ctx, pdf) != expectedPages)
			return false;

		int slash = max(targetPath.ReverseFind(_T('\\')), targetPath.ReverseFind(_T('/')));
		CString directory = slash >= 0 ? targetPath.Left(slash + 1) : _T(".\\");
		TCHAR tempName[MAX_PATH] = { 0 };
		if (!GetTempFileName(directory, _T("XGP"), 0, tempName))
			return false;

		fz_document* checkDoc = nullptr;
		fz_var(checkDoc);
		volatile bool valid = false;
		CW2A tempPath(tempName, CP_UTF8);
		fz_try(ctx)
		{
			pdf_save_document(ctx, pdf, tempPath, &options);
			checkDoc = fz_open_document(ctx, tempPath);
			if (fz_needs_password(ctx, checkDoc) &&
				(!password || password->empty() ||
				 !fz_authenticate_password(ctx, checkDoc, password->data())))
				fz_throw(ctx, FZ_ERROR_GENERIC, "Cannot authenticate saved PDF");
			valid = fz_count_pages(ctx, checkDoc) == expectedPages;
		}
		fz_always(ctx)
		{
			if (checkDoc)
				fz_drop_document(ctx, checkDoc);
		}
		fz_catch(ctx)
		{
			valid = false;
		}

		if (!valid)
		{
			DeleteFile(tempName);
			return false;
		}
		outTempPath = tempName;
		return true;
	}

	bool FinalizeValidatedPdf(const CString& tempPath, const CString& targetPath,
		const std::function<void(const CString&)>& onBeforeReplace,
		const std::function<void(const CString&, bool)>& onAfterReplace)
	{
		if (tempPath.IsEmpty())
			return false;
		if (onBeforeReplace)
			onBeforeReplace(targetPath);
		const bool moved = MoveFileEx(tempPath, targetPath,
			MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
		if (!moved)
			DeleteFile(tempPath);
		if (onAfterReplace)
			onAfterReplace(targetPath, moved);
		return moved;
	}
}

IMPLEMENT_DYNAMIC(CPDFEditDialog, CDialogEx)

BEGIN_MESSAGE_MAP(CPDFEditDialog, CDialogEx)
	ON_BN_CLICKED(IDC_BTN_EDIT_SAVE, &CPDFEditDialog::OnBnClickedBtnSave)
	ON_BN_CLICKED(IDC_BTN_EDIT_RESET, &CPDFEditDialog::OnBnClickedBtnReset)
	ON_BN_CLICKED(IDC_BTN_EDIT_PREVIEW, &CPDFEditDialog::OnBnClickedBtnPreview)
	ON_BN_CLICKED(IDC_BTN_EDIT_UNDO, &CPDFEditDialog::OnBnClickedBtnUndo)
	ON_BN_CLICKED(IDC_BTN_EDIT_REDO, &CPDFEditDialog::OnBnClickedBtnRedo)
	ON_BN_CLICKED(IDC_BTN_EDIT_ADD_PDF, &CPDFEditDialog::OnBnClickedBtnAddPDF)
	ON_NOTIFY(LVN_BEGINDRAG, IDC_EDIT_THUMBNAIL_LIST, &CPDFEditDialog::OnBeginDrag)
	ON_NOTIFY(NM_RCLICK, IDC_EDIT_THUMBNAIL_LIST, &CPDFEditDialog::OnNMRClick)
	ON_NOTIFY(NM_CLICK, IDC_EDIT_THUMBNAIL_LIST, &CPDFEditDialog::OnNMClick)
	ON_NOTIFY(LVN_ITEMCHANGED, IDC_EDIT_THUMBNAIL_LIST, &CPDFEditDialog::OnItemChanged)
	ON_NOTIFY(NM_CUSTOMDRAW, IDC_EDIT_THUMBNAIL_LIST, &CPDFEditDialog::OnCustomDraw)
	ON_WM_MOUSEMOVE()
	ON_WM_LBUTTONUP()
	ON_WM_LBUTTONDOWN()
	ON_WM_TIMER()
END_MESSAGE_MAP()

CPDFEditDialog::CPDFEditDialog(fz_context* ctx, fz_document* doc, const CString& filePath,
	const std::vector<char>& password, CWnd* pParent)
	: CDialogEx(IDD_PDF_EDIT_DIALOG, pParent)
	, m_ctx(ctx)
	, m_doc(doc)
	, m_filePath(filePath)
	, m_password(password)
	, m_totalPages(0)
	, m_isDragging(false)
	, m_potentialDrag(false)
	, m_dragIndex(-1)
	, m_dropIndex(-1)
	, m_lastDropIndex(-1)
	, m_pDragImage(nullptr)
	, m_autoScrollSpeed(0)
	, m_draggedPageOriginalPos(-1)
{
	if (m_doc)
	{
		m_totalPages = fz_count_pages(m_ctx, m_doc);
	}
}

CPDFEditDialog::~CPDFEditDialog()
{
	CleanupThumbnails();
	if (!m_password.empty())
		SecureZeroMemory(m_password.data(), m_password.size());
	for (auto& entry : m_importPasswords)
		if (!entry.second.empty())
			SecureZeroMemory(entry.second.data(), entry.second.size());
}

fz_document* CPDFEditDialog::OpenAuthenticatedSource(const CString& path)
{
	CW2A utf8Path(path, CP_UTF8);
	fz_document* source = nullptr;
	fz_var(source);
	fz_try(m_ctx)
	{
		source = fz_open_document(m_ctx, utf8Path);
		if (fz_needs_password(m_ctx, source))
		{
			const std::vector<char>* password = &m_password;
			if (path.CompareNoCase(m_filePath) != 0)
			{
				auto it = m_importPasswords.find(path);
				password = it == m_importPasswords.end() ? nullptr : &it->second;
			}
			if (!password || password->empty() ||
				!fz_authenticate_password(m_ctx, source, password->data()))
				fz_throw(m_ctx, FZ_ERROR_GENERIC, "PDF password is required");
		}
	}
	fz_catch(m_ctx)
	{
		if (source)
			fz_drop_document(m_ctx, source);
		fz_rethrow(m_ctx);
	}
	return source;
}

void CPDFEditDialog::DoDataExchange(CDataExchange* pDX)
{
	CDialogEx::DoDataExchange(pDX);
	DDX_Control(pDX, IDC_EDIT_THUMBNAIL_LIST, m_thumbnailList);
	DDX_Control(pDX, IDC_BTN_EDIT_SAVE, m_btnSave);
	DDX_Control(pDX, IDC_BTN_EDIT_RESET, m_btnReset);
	DDX_Control(pDX, IDC_BTN_EDIT_UNDO, m_btnUndo);
	DDX_Control(pDX, IDC_BTN_EDIT_REDO, m_btnRedo);
	DDX_Control(pDX, IDC_BTN_EDIT_ADD_PDF, m_btnAddPDF);
	DDX_Control(pDX, IDC_STATIC_EDIT_INFO, m_staticInfo);
}

BOOL CPDFEditDialog::OnInitDialog()
{
	CDialogEx::OnInitDialog();

	// 设置对话框标题
	SetWindowText(_T("PDF编辑工具箱"));
	CRect resetRect;
	CRect undoRect;
	m_btnReset.GetWindowRect(&resetRect);
	m_btnUndo.GetWindowRect(&undoRect);
	ScreenToClient(&resetRect);
	ScreenToClient(&undoRect);
	int buttonOffset = undoRect.left - resetRect.left;
	m_btnPreview.Create(_T("预览"), WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
		resetRect, this, IDC_BTN_EDIT_PREVIEW);
	m_btnPreview.SetFont(m_btnReset.GetFont());
	CButton* buttons[] = { &m_btnReset, &m_btnUndo, &m_btnRedo, &m_btnAddPDF, &m_btnSave };
	for (CButton* button : buttons)
	{
		CRect rect;
		button->GetWindowRect(&rect);
		ScreenToClient(&rect);
		rect.OffsetRect(buttonOffset, 0);
		button->MoveWindow(&rect);
	}

	// 初始化缩略图列表
	InitializeThumbnailList();

	// 建立页面列表，缩略图按需生成。
	LoadAllThumbnails();
	RecordEditState();
	SetTimer(TIMER_VISIBLE_THUMBNAILS, 180, NULL);

	// 更新信息栏
	UpdateInfoBar();

	return TRUE;
}

BOOL CPDFEditDialog::PreTranslateMessage(MSG* pMsg)
{
	if (pMsg->message == WM_KEYDOWN && (GetKeyState(VK_CONTROL) & 0x8000))
	{
		if (pMsg->wParam == 'Z')
		{
			if (GetKeyState(VK_SHIFT) & 0x8000)
				OnBnClickedBtnRedo();
			else
				OnBnClickedBtnUndo();
			return TRUE;
		}
		if (pMsg->wParam == 'Y')
		{
			OnBnClickedBtnRedo();
			return TRUE;
		}
	}
	return CDialogEx::PreTranslateMessage(pMsg);
}

void CPDFEditDialog::UpdateHistoryButtons()
{
	if (m_btnUndo.GetSafeHwnd())
		m_btnUndo.EnableWindow(m_historyIndex > 0);
	if (m_btnRedo.GetSafeHwnd())
		m_btnRedo.EnableWindow(m_historyIndex + 1 < m_history.size());
}

void CPDFEditDialog::RecordEditState()
{
	EditState state;
	state.totalPages = m_totalPages;
	state.pages = m_pages;
	for (auto& page : state.pages)
	{
		page.hThumbnail = nullptr;
		page.isDragging = false;
	}

	if (!m_history.empty())
	{
		const EditState& current = m_history[m_historyIndex];
		bool same = current.totalPages == state.totalPages && current.pages.size() == state.pages.size();
		for (size_t i = 0; same && i < state.pages.size(); ++i)
		{
			const PageInfo& a = current.pages[i];
			const PageInfo& b = state.pages[i];
			same = a.originalIndex == b.originalIndex && a.isDeleted == b.isDeleted &&
				a.sourceFile == b.sourceFile && a.sourcePageIndex == b.sourcePageIndex &&
				a.rotationAngle == b.rotationAngle;
		}
		if (same)
			return;
		m_history.erase(m_history.begin() + m_historyIndex + 1, m_history.end());
	}

	m_history.push_back(std::move(state));
	if (m_history.size() > HISTORY_LIMIT)
		m_history.erase(m_history.begin());
	m_historyIndex = m_history.size() - 1;
	UpdateHistoryButtons();
}

void CPDFEditDialog::RestoreEditState(size_t index)
{
	for (auto& page : m_pages)
	{
		if (page.hThumbnail)
			::DeleteObject(page.hThumbnail);
	}
	m_pages = m_history[index].pages;
	m_totalPages = m_history[index].totalPages;
	m_historyIndex = index;
	RefreshThumbnailList();
	UpdateHistoryButtons();
}

void CPDFEditDialog::OnBnClickedBtnUndo()
{
	if (m_historyIndex > 0)
		RestoreEditState(m_historyIndex - 1);
}

void CPDFEditDialog::OnBnClickedBtnRedo()
{
	if (m_historyIndex + 1 < m_history.size())
		RestoreEditState(m_historyIndex + 1);
}

void CPDFEditDialog::InitializeThumbnailList()
{
	// 设置列表控件样式为大图标，支持多选
	m_thumbnailList.ModifyStyle(LVS_SINGLESEL, LVS_ICON | LVS_SHOWSELALWAYS);

	// 创建页面缩略图列表
	ResetThumbnailImageList();

	// 设置列表背景色为浅灰色，现代化视觉效果
	m_thumbnailList.SetBkColor(RGB(248, 248, 248));
	m_thumbnailList.SetTextBkColor(RGB(248, 248, 248));
	m_thumbnailList.SetTextColor(RGB(50, 50, 50));

	m_thumbnailList.SetIconSpacing(CSize(250, 340));
}

void CPDFEditDialog::LayoutThumbnailItems()
{
	CRect client;
	m_thumbnailList.GetClientRect(&client);
	const int edgeMargin = 8;
	const int minimumGap = 30;
	const int cellWidth = THUMBNAIL_WIDTH + minimumGap;
	int usableWidth = max(cellWidth, client.Width() - GetSystemMetrics(SM_CXVSCROLL));
	int columns = max(1, (usableWidth - edgeMargin * 2 + minimumGap) / cellWidth);
	int spacing = cellWidth;
	if (columns > 1)
		spacing = (usableWidth - edgeMargin * 2 - cellWidth) / (columns - 1);
	for (int item = 0; item < m_thumbnailList.GetItemCount(); ++item)
		m_thumbnailList.SetItemPosition(item,
			CPoint(edgeMargin + (item % columns) * spacing, (item / columns) * 340));
}

void CPDFEditDialog::ResetThumbnailImageList()
{
	if (m_imageList.GetSafeHandle())
		m_imageList.DeleteImageList();
	m_imageList.Create(THUMBNAIL_WIDTH, THUMBNAIL_ACTION_HEIGHT, ILC_COLOR24 | ILC_MASK, 1, 16);
	m_thumbnailList.SetImageList(&m_imageList, LVSIL_NORMAL);
	HBITMAP hPlaceholder = CreatePlaceholderBitmap();
	if (hPlaceholder)
	{
		HBITMAP hActionBitmap = CreateActionThumbnail(hPlaceholder, 0, 0);
		if (hActionBitmap)
		{
			CBitmap actionBitmap;
			actionBitmap.Attach(hActionBitmap);
			m_imageList.Add(&actionBitmap, (CBitmap*)nullptr);
			actionBitmap.Detach();
			::DeleteObject(hActionBitmap);
		}
		::DeleteObject(hPlaceholder);
	}
}

HBITMAP CPDFEditDialog::CreateActionThumbnail(HBITMAP hThumbnail, int currentPos, int originalPos)
{
	::GdiFlush();
	CClientDC dc(this);
	CDC bitmapDC;
	if (!bitmapDC.CreateCompatibleDC(&dc))
		return nullptr;
	BITMAPINFO resultInfo = { 0 };
	resultInfo.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
	resultInfo.bmiHeader.biWidth = THUMBNAIL_WIDTH;
	resultInfo.bmiHeader.biHeight = -THUMBNAIL_ACTION_HEIGHT;
	resultInfo.bmiHeader.biPlanes = 1;
	resultInfo.bmiHeader.biBitCount = 24;
	resultInfo.bmiHeader.biCompression = BI_RGB;
	void* resultBits = nullptr;
	HBITMAP hResult = ::CreateDIBSection(dc.GetSafeHdc(), &resultInfo, DIB_RGB_COLORS,
		&resultBits, nullptr, 0);
	if (!hResult || !resultBits)
		return nullptr;
	HBITMAP hOldBitmap = (HBITMAP)bitmapDC.SelectObject(hResult);
	const int resultStride = (THUMBNAIL_WIDTH * 3 + 3) & ~3;
	std::fill_n(static_cast<BYTE*>(resultBits), resultStride * THUMBNAIL_ACTION_HEIGHT,
		static_cast<BYTE>(248));
	BITMAP sourceInfo = { 0 };
	::GetObject(hThumbnail, sizeof(sourceInfo), &sourceInfo);
	int copyWidth = min(THUMBNAIL_WIDTH, sourceInfo.bmWidth);
	int copyHeight = min(THUMBNAIL_HEIGHT, sourceInfo.bmHeight);
	if (copyWidth > 0 && copyHeight > 0)
	{
		BITMAPINFO sourceFormat = { 0 };
		sourceFormat.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
		sourceFormat.bmiHeader.biWidth = sourceInfo.bmWidth;
		sourceFormat.bmiHeader.biHeight = -sourceInfo.bmHeight;
		sourceFormat.bmiHeader.biPlanes = 1;
		sourceFormat.bmiHeader.biBitCount = 24;
		sourceFormat.bmiHeader.biCompression = BI_RGB;
		int sourceStride = (sourceInfo.bmWidth * 3 + 3) & ~3;
		std::vector<BYTE> sourceBits(sourceStride * sourceInfo.bmHeight);
		if (::GetDIBits(dc.GetSafeHdc(), hThumbnail, 0, sourceInfo.bmHeight,
			sourceBits.data(), &sourceFormat, DIB_RGB_COLORS) == sourceInfo.bmHeight)
		{
			int sourceX = (sourceInfo.bmWidth - copyWidth) / 2;
			int sourceY = (sourceInfo.bmHeight - copyHeight) / 2;
			int destX = (THUMBNAIL_WIDTH - copyWidth) / 2;
			int destY = (THUMBNAIL_HEIGHT - copyHeight) / 2;
			for (int y = 0; y < copyHeight; ++y)
			{
				const BYTE* sourceRow = sourceBits.data() + (sourceY + y) * sourceStride + sourceX * 3;
				BYTE* destRow = static_cast<BYTE*>(resultBits) + (destY + y) * resultStride + destX * 3;
				std::copy_n(sourceRow, copyWidth * 3, destRow);
			}
		}
		else
		{
			CDC sourceDC;
			if (sourceDC.CreateCompatibleDC(&dc))
			{
				HBITMAP hOldSource = (HBITMAP)sourceDC.SelectObject(hThumbnail);
				bitmapDC.BitBlt((THUMBNAIL_WIDTH - copyWidth) / 2,
					(THUMBNAIL_HEIGHT - copyHeight) / 2,
					copyWidth, copyHeight, &sourceDC,
					(sourceInfo.bmWidth - copyWidth) / 2,
					(sourceInfo.bmHeight - copyHeight) / 2, SRCCOPY);
				sourceDC.SelectObject(hOldSource);
			}
		}
	}

	if (currentPos > 0)
	{
		CBrush badgeBrush(RGB(33, 150, 243));
		CPen* oldPen = (CPen*)bitmapDC.SelectStockObject(NULL_PEN);
		CBrush* oldBrush = bitmapDC.SelectObject(&badgeBrush);
		bitmapDC.Ellipse(10, 10, 38, 38);
		bitmapDC.SelectObject(oldBrush);
		bitmapDC.SelectObject(oldPen);
		bitmapDC.FillSolidRect(4, THUMBNAIL_HEIGHT - 30,
			THUMBNAIL_WIDTH - 8, 26, RGB(50, 50, 50));
		CFont badgeFont;
		badgeFont.CreateFont(-14, 0, 0, 0, FW_BOLD, FALSE, FALSE, 0,
			DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
			ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_SWISS, _T("Arial"));
		CFont* oldFont = bitmapDC.SelectObject(&badgeFont);
		bitmapDC.SetBkMode(TRANSPARENT);
		bitmapDC.SetTextColor(RGB(255, 255, 255));
		CString pageText;
		pageText.Format(_T("%d"), currentPos);
		CRect badgeRect(10, 10, 38, 38);
		bitmapDC.DrawText(pageText, &badgeRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
		pageText.Format(_T("%d"), originalPos);
		CRect footerRect(4, THUMBNAIL_HEIGHT - 30,
			THUMBNAIL_WIDTH - 4, THUMBNAIL_HEIGHT - 4);
		bitmapDC.DrawText(pageText, &footerRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
		bitmapDC.SelectObject(oldFont);
	}

	::GdiFlush();
	DrawPageActionButtons(static_cast<BYTE*>(resultBits), resultStride);
	bitmapDC.SelectObject(hOldBitmap);
	return hResult;
}

int CPDFEditDialog::GetPageIndex(int displayIndex) const
{
	for (size_t i = 0; i < m_pages.size(); ++i)
	{
		if (!m_pages[i].isDeleted && m_pages[i].displayIndex == displayIndex)
			return static_cast<int>(i);
	}
	return -1;
}

int CPDFEditDialog::HitPageAction(int displayIndex, CPoint point)
{
	if (displayIndex < 0)
		return -1;
	CRect icon;
	if (!m_thumbnailList.GetItemRect(displayIndex, &icon, LVIR_ICON))
		return -1;
	int x = point.x - icon.left;
	int y = point.y - icon.top;
	CPoint localPoint(x, y);
	for (int action = 0; action < 2; ++action)
	{
		if (PAGE_ACTION_BUTTONS[action].PtInRect(localPoint))
			return action;
	}
	return -1;
}

void CPDFEditDialog::RotatePage(int displayIndex, int angle)
{
	int pageIndex = GetPageIndex(displayIndex);
	if (pageIndex < 0)
		return;
	PageInfo& page = m_pages[pageIndex];
	int newAngle = (page.rotationAngle + angle + 360) % 360;
	if (newAngle == page.rotationAngle)
		return;

	LVITEM item = { 0 };
	item.mask = LVIF_IMAGE;
	item.iItem = displayIndex;
	if (!m_thumbnailList.GetItem(&item))
		return;

	HBITMAP hNewThumbnail = page.sourceFile == m_filePath ?
		RenderThumbnail(page.originalIndex, displayIndex + 1, page.originalIndex + 1, newAngle) :
		RenderThumbnailFromSource(page.sourceFile, page.sourcePageIndex,
			displayIndex + 1, page.originalIndex + 1, newAngle);
	if (!hNewThumbnail)
		return;
	HBITMAP hActionBitmap = CreateActionThumbnail(hNewThumbnail,
		displayIndex + 1, page.originalIndex + 1);
	if (!hActionBitmap)
	{
		::DeleteObject(hNewThumbnail);
		return;
	}

	CBitmap actionBitmap;
	actionBitmap.Attach(hActionBitmap);
	bool updated = item.iImage > 0 && item.iImage < m_imageList.GetImageCount() &&
		m_imageList.Replace(item.iImage, &actionBitmap, nullptr) != FALSE;
	if (!updated)
	{
		int imageIndex = m_imageList.Add(&actionBitmap, (CBitmap*)nullptr);
		if (imageIndex >= 0)
		{
			item.iImage = imageIndex;
			updated = m_thumbnailList.SetItem(&item) != FALSE;
		}
	}
	actionBitmap.Detach();
	::DeleteObject(hActionBitmap);
	if (!updated)
	{
		::DeleteObject(hNewThumbnail);
		return;
	}
	if (page.hThumbnail)
		::DeleteObject(page.hThumbnail);
	page.hThumbnail = hNewThumbnail;
	page.rotationAngle = newAngle;
	CRect icon;
	if (m_thumbnailList.GetItemRect(displayIndex, &icon, LVIR_ICON))
		m_thumbnailList.InvalidateRect(&icon, FALSE);
	RecordEditState();
}

void CPDFEditDialog::LoadAllThumbnails()
{
	if (!m_ctx || !m_doc || m_totalPages <= 0)
		return;

	// 初始化页面数组
	m_pages.clear();
	m_pages.reserve(m_totalPages);

	for (int i = 0; i < m_totalPages; i++)
	{
		PageInfo page(i);
		page.displayIndex = i;
		page.sourceFile = m_filePath;      // 设置源文件路径
		page.sourcePageIndex = i;          // 源文件中的页码

		m_pages.push_back(page);
		m_thumbnailList.InsertItem(i, _T(""), 0);
	}

	// 保存原始状态（用于重置）- 深拷贝HBITMAP以避免内存泄漏
	m_originalPages.clear();
	m_originalPages.reserve(m_pages.size());
	for (const auto& page : m_pages)
	{
		PageInfo originalPage = page;
		// 深拷贝HBITMAP
		if (page.hThumbnail)
		{
			originalPage.hThumbnail = (HBITMAP)::CopyImage(page.hThumbnail, IMAGE_BITMAP, 0, 0, LR_CREATEDIBSECTION);
		}
		m_originalPages.push_back(originalPage);
	}

	// 保存原始总页数
	m_originalTotalPages = m_totalPages;
	LayoutThumbnailItems();
	RenderVisibleThumbnails();
}

HBITMAP CPDFEditDialog::RenderThumbnail(int pageNumber, int currentPos, int originalPos, int rotationAngle)
{
	if (!m_ctx || !m_doc || pageNumber < 0 || pageNumber >= m_totalPages)
		return nullptr;

	fz_page* page = nullptr;
	fz_pixmap* pixmap = nullptr;
	HBITMAP hBitmap = nullptr;
	CClientDC dc(this);
	if (!dc.GetSafeHdc())
		return nullptr;

	fz_try(m_ctx)
	{
		// 加载页面
		page = fz_load_page(m_ctx, m_doc, pageNumber);

		// 获取页面边界
		fz_rect bounds = fz_bound_page(m_ctx, page);

		float pageWidth = bounds.x1 - bounds.x0;
		float pageHeight = bounds.y1 - bounds.y0;
		if (rotationAngle == 90 || rotationAngle == 270)
			std::swap(pageWidth, pageHeight);
		float scale = min(212.0f / pageWidth, 232.0f / pageHeight);
		fz_matrix ctm = fz_pre_rotate(fz_scale(scale, scale), rotationAngle);

		// 渲染为pixmap（不使用alpha通道）
		pixmap = fz_new_pixmap_from_page(m_ctx, page, ctm, fz_device_rgb(m_ctx), 0);

		// 转换为HBITMAP
		int width = fz_pixmap_width(m_ctx, pixmap);
		int height = fz_pixmap_height(m_ctx, pixmap);
		int stride = fz_pixmap_stride(m_ctx, pixmap);
		unsigned char* samples = fz_pixmap_samples(m_ctx, pixmap);

		// 创建固定尺寸缩略图，页面内容保持原始比例。
		const int FIXED_WIDTH = THUMBNAIL_WIDTH;
		const int FIXED_HEIGHT = THUMBNAIL_HEIGHT;
		BITMAPINFO bmi = { 0 };
		bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
		bmi.bmiHeader.biWidth = FIXED_WIDTH;
		bmi.bmiHeader.biHeight = -FIXED_HEIGHT;  // 负值表示自顶向下
		bmi.bmiHeader.biPlanes = 1;
		bmi.bmiHeader.biBitCount = 24;
		bmi.bmiHeader.biCompression = BI_RGB;

		void* pBits = nullptr;
		hBitmap = CreateDIBSection(dc.GetSafeHdc(), &bmi, DIB_RGB_COLORS, &pBits, nullptr, 0);

		if (hBitmap && pBits)
		{
			unsigned char* dest = (unsigned char*)pBits;
			int destStride = ((FIXED_WIDTH * 3 + 3) & ~3);

			// ★★★ 第1步：先用浅灰色填充缩略图区域（匹配背景）
			for (int y = 0; y < FIXED_HEIGHT; y++)
			{
				for (int x = 0; x < FIXED_WIDTH; x++)
				{
					dest[y * destStride + x * 3 + 0] = 248;  // B
					dest[y * destStride + x * 3 + 1] = 248;  // G
					dest[y * destStride + x * 3 + 2] = 248;  // R
				}
			}

			// ★★★ 第2步：绘制白色卡片背景（留出边距）
			int cardMargin = 4;
			for (int y = cardMargin; y < FIXED_HEIGHT - cardMargin; y++)
			{
				for (int x = cardMargin; x < FIXED_WIDTH - cardMargin; x++)
				{
					dest[y * destStride + x * 3 + 0] = 255;  // B
					dest[y * destStride + x * 3 + 1] = 255;  // G
					dest[y * destStride + x * 3 + 2] = 255;  // R
				}
			}

			// ★★★ 第3步：计算居中偏移量（水平和垂直都要考虑cardMargin）
			int contentAreaWidth = FIXED_WIDTH - cardMargin * 2;  // 可用内容区域宽度
			int contentAreaHeight = FIXED_HEIGHT - cardMargin * 2;  // 可用内容区域高度

			// 计算垂直居中偏移量（相对于整个图像）
			int yOffset = cardMargin + (contentAreaHeight - height) / 2;
			if (yOffset < cardMargin) yOffset = cardMargin;  // 不能超过上边距

			// 计算水平居中偏移量（相对于整个图像）
			int xOffset = cardMargin + (contentAreaWidth - width) / 2;
			if (xOffset < cardMargin) xOffset = cardMargin;  // 不能超过左边距

			// ★★★ 第4步：复制PDF像素数据到居中位置（RGB -> BGR转换）
			for (int y = 0; y < height && (y + yOffset) < (FIXED_HEIGHT - cardMargin); y++)
			{
				unsigned char* src = samples + y * stride;
				unsigned char* destRow = dest + (y + yOffset) * destStride;

				int copyWidth = width;
				// 确保不超过右边界
				if (xOffset + copyWidth > FIXED_WIDTH - cardMargin)
					copyWidth = FIXED_WIDTH - cardMargin - xOffset;

				for (int x = 0; x < copyWidth; x++)
				{
					int destX = xOffset + x;
					if (destX >= cardMargin && destX < FIXED_WIDTH - cardMargin)
					{
						destRow[destX * 3 + 0] = src[x * 3 + 2];  // B
						destRow[destX * 3 + 1] = src[x * 3 + 1];  // G
						destRow[destX * 3 + 2] = src[x * 3 + 0];  // R
					}
				}
			}

			// ★★★ 第5步：使用GDI+绘制装饰元素
			CDC memDC;
			memDC.CreateCompatibleDC(&dc);
			HBITMAP hOldBitmap = (HBITMAP)memDC.SelectObject(hBitmap);

			Gdiplus::Graphics graphics(memDC.GetSafeHdc());
			graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
			graphics.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAlias);

			// 绘制卡片边框
			Gdiplus::RectF cardRect(
				(Gdiplus::REAL)cardMargin,
				(Gdiplus::REAL)cardMargin,
				(Gdiplus::REAL)(FIXED_WIDTH - cardMargin * 2),
				(Gdiplus::REAL)(FIXED_HEIGHT - cardMargin * 2)
			);
			Gdiplus::Pen borderPen(Gdiplus::Color(255, 220, 220, 220), 2.0f);
			graphics.DrawRectangle(&borderPen, cardRect);

			// 绘制蓝色圆形页码标记（左上角） - 显示当前位置
			int badgeSize = 28;  // 增大以容纳两位数
			Gdiplus::RectF badgeRect(10.0f, 10.0f, (Gdiplus::REAL)badgeSize, (Gdiplus::REAL)badgeSize);
			Gdiplus::SolidBrush blueBrush(Gdiplus::Color(255, 33, 150, 243));
			graphics.FillEllipse(&blueBrush, badgeRect);

			// 在蓝色圆形标记上绘制当前位置 - 使用GDI而不是GDI+
			// （GDI+ Font对象存在字形缓存问题，导致数字显示错误）
			CString currentPosText;
			currentPosText.Format(_T("%d"), currentPos);

			// 暂时退出GDI+，使用原生GDI绘制文字
			graphics.Flush(Gdiplus::FlushIntentionSync);
			graphics.ReleaseHDC(memDC.GetSafeHdc());

			// 使用GDI绘制蓝色徽章上的数字
			memDC.SetBkMode(TRANSPARENT);
			CFont badgeFont;
			badgeFont.CreateFont(-14, 0, 0, 0, FW_BOLD, FALSE, FALSE, 0,
				ANSI_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
				ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_SWISS, _T("Arial"));
			CFont* pOldFont = memDC.SelectObject(&badgeFont);
			memDC.SetTextColor(RGB(255, 255, 255));
			CRect badgeTextRect(10, 10, 38, 38);
			memDC.DrawText(currentPosText, &badgeTextRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
			memDC.SelectObject(pOldFont);
			badgeFont.DeleteObject();

			// 重新获取GDI+ Graphics对象继续绘制
			Gdiplus::Graphics graphics2(memDC.GetSafeHdc());
			graphics2.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);

			// 绘制底部深色标签栏
			Gdiplus::RectF bottomRect(
				(Gdiplus::REAL)cardMargin,
				(Gdiplus::REAL)(FIXED_HEIGHT - 30),
				(Gdiplus::REAL)(FIXED_WIDTH - cardMargin * 2),
				26.0f
			);
			Gdiplus::SolidBrush darkBrush(Gdiplus::Color(230, 50, 50, 50));
			graphics2.FillRectangle(&darkBrush, bottomRect);

			// 在底部标签上绘制原始位置 - 使用GDI而不是GDI+
			CString originalPosText;
			originalPosText.Format(_T("%d"), originalPos);

			// 再次退出GDI+，使用GDI绘制底部文字
			graphics2.Flush(Gdiplus::FlushIntentionSync);
			graphics2.ReleaseHDC(memDC.GetSafeHdc());

			// 使用GDI绘制底部黑色条上的数字
			CRect bottomTextRect(cardMargin, FIXED_HEIGHT - 30, FIXED_WIDTH - cardMargin, FIXED_HEIGHT - 4);
			memDC.DrawText(originalPosText, &bottomTextRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

			// 不再需要Graphics对象，直接退出

			memDC.SelectObject(hOldBitmap);
		}

		// 清理
		fz_drop_pixmap(m_ctx, pixmap);
		fz_drop_page(m_ctx, page);
	}
	fz_catch(m_ctx)
	{
		if (pixmap) fz_drop_pixmap(m_ctx, pixmap);
		if (page) fz_drop_page(m_ctx, page);
		return nullptr;
	}

	return hBitmap;
}

// 从源文件渲染缩略图（用于导入的PDF页面）
HBITMAP CPDFEditDialog::RenderThumbnailFromSource(const CString& sourceFile, int pageIndex, int currentPos, int originalPos, int rotationAngle)
{
	if (!m_ctx || sourceFile.IsEmpty() || pageIndex < 0)
		return nullptr;

	fz_document* sourceDoc = nullptr;
	fz_page* page = nullptr;
	fz_pixmap* pix = nullptr;
	HBITMAP hBitmap = nullptr;

	fz_try(m_ctx)
	{
		// 打开源文件
		sourceDoc = OpenAuthenticatedSource(sourceFile);
		if (!sourceDoc)
			fz_throw(m_ctx, FZ_ERROR_GENERIC, "无法打开源文件");

		// 检查页码是否有效
		int pageCount = fz_count_pages(m_ctx, sourceDoc);
		if (pageIndex >= pageCount)
			fz_throw(m_ctx, FZ_ERROR_GENERIC, "页码超出范围");

		// 加载页面
		page = fz_load_page(m_ctx, sourceDoc, pageIndex);

		// 获取页面边界
		fz_rect bounds = fz_bound_page(m_ctx, page);

		// 按缩略图的内容区域渲染，避免放大旧位图造成模糊。
		float targetWidth = 212.0f;
		float targetHeight = 232.0f;
		float pageWidth = bounds.x1 - bounds.x0;
		float pageHeight = bounds.y1 - bounds.y0;

		// 根据旋转角度调整尺寸
		if (rotationAngle == 90 || rotationAngle == 270) {
			std::swap(pageWidth, pageHeight);
		}

		float scaleX = targetWidth / pageWidth;
		float scaleY = targetHeight / pageHeight;
		float scale = min(scaleX, scaleY);

		// 创建变换矩阵（缩放 + 旋转）
		fz_matrix ctm = fz_scale(scale, scale);
		ctm = fz_pre_rotate(ctm, rotationAngle);

		// 渲染页面到pixmap
		pix = fz_new_pixmap_from_page(m_ctx, page, ctm, fz_device_rgb(m_ctx), 0);

		// 转换为HBITMAP
		int width = fz_pixmap_width(m_ctx, pix);
		int height = fz_pixmap_height(m_ctx, pix);
		int stride = fz_pixmap_stride(m_ctx, pix);
		unsigned char* samples = fz_pixmap_samples(m_ctx, pix);

		// 创建位图
		HDC hdc = ::GetDC(NULL);
		BITMAPINFO bmi = { 0 };
		bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
		bmi.bmiHeader.biWidth = width;
		bmi.bmiHeader.biHeight = -height;  // 负值表示从上到下
		bmi.bmiHeader.biPlanes = 1;
		bmi.bmiHeader.biBitCount = 24;
		bmi.bmiHeader.biCompression = BI_RGB;

		void* pBits = nullptr;
		hBitmap = ::CreateDIBSection(hdc, &bmi, DIB_RGB_COLORS, &pBits, NULL, 0);
		::ReleaseDC(NULL, hdc);

		if (hBitmap && pBits)
		{
			// 复制像素数据（RGB -> BGR）
			unsigned char* dest = (unsigned char*)pBits;
			int destStride = (width * 3 + 3) & ~3;
			for (int y = 0; y < height; y++)
			{
				unsigned char* src = samples + y * stride;
				unsigned char* destPixel = dest + y * destStride;
				for (int x = 0; x < width; x++)
				{
					destPixel[0] = src[2];  // B
					destPixel[1] = src[1];  // G
					destPixel[2] = src[0];  // R
					destPixel += 3;
					src += 3;
				}
			}
		}

		// 清理资源
		fz_drop_pixmap(m_ctx, pix);
		pix = nullptr;
		fz_drop_page(m_ctx, page);
		page = nullptr;
		fz_drop_document(m_ctx, sourceDoc);
		sourceDoc = nullptr;
	}
	fz_catch(m_ctx)
	{
		if (pix) fz_drop_pixmap(m_ctx, pix);
		if (page) fz_drop_page(m_ctx, page);
		if (sourceDoc) fz_drop_document(m_ctx, sourceDoc);
		return nullptr;
	}

	return hBitmap;
}

// 渲染预览位图（用于双击预览，较大尺寸）
HBITMAP CPDFEditDialog::RenderPreviewBitmap(const CString& sourceFile, int pageIndex, int rotationAngle)
{
	if (!m_ctx || sourceFile.IsEmpty() || pageIndex < 0)
		return nullptr;

	fz_document* sourceDoc = nullptr;
	fz_page* page = nullptr;
	fz_pixmap* pix = nullptr;
	HBITMAP hBitmap = nullptr;

	fz_try(m_ctx)
	{
		// 打开源文件
		sourceDoc = OpenAuthenticatedSource(sourceFile);
		if (!sourceDoc)
			fz_throw(m_ctx, FZ_ERROR_GENERIC, "无法打开源文件");

		// 检查页码是否有效
		int pageCount = fz_count_pages(m_ctx, sourceDoc);
		if (pageIndex >= pageCount)
			fz_throw(m_ctx, FZ_ERROR_GENERIC, "页码超出范围");

		// 加载页面
		page = fz_load_page(m_ctx, sourceDoc, pageIndex);

		// 获取页面边界
		fz_rect bounds = fz_bound_page(m_ctx, page);

		// 计算缩放比例（目标宽度：600px，保持宽高比）
		float targetWidth = 600.0f;
		float pageWidth = bounds.x1 - bounds.x0;
		float pageHeight = bounds.y1 - bounds.y0;

		// 根据旋转角度调整尺寸
		if (rotationAngle == 90 || rotationAngle == 270) {
			std::swap(pageWidth, pageHeight);
		}

		float scale = targetWidth / pageWidth;

		// 创建变换矩阵（缩放 + 旋转）
		fz_matrix ctm = fz_scale(scale, scale);
		ctm = fz_pre_rotate(ctm, rotationAngle);

		// 渲染页面到pixmap
		pix = fz_new_pixmap_from_page(m_ctx, page, ctm, fz_device_rgb(m_ctx), 0);

		// 转换为HBITMAP
		int width = fz_pixmap_width(m_ctx, pix);
		int height = fz_pixmap_height(m_ctx, pix);
		int stride = fz_pixmap_stride(m_ctx, pix);
		unsigned char* samples = fz_pixmap_samples(m_ctx, pix);

		// 创建位图
		HDC hdc = ::GetDC(NULL);
		BITMAPINFO bmi = { 0 };
		bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
		bmi.bmiHeader.biWidth = width;
		bmi.bmiHeader.biHeight = -height;  // 负值表示从上到下
		bmi.bmiHeader.biPlanes = 1;
		bmi.bmiHeader.biBitCount = 24;
		bmi.bmiHeader.biCompression = BI_RGB;

		void* pBits = nullptr;
		hBitmap = ::CreateDIBSection(hdc, &bmi, DIB_RGB_COLORS, &pBits, NULL, 0);
		::ReleaseDC(NULL, hdc);

		if (hBitmap && pBits)
		{
			// 复制像素数据（RGB -> BGR）
			unsigned char* dest = (unsigned char*)pBits;
			int destStride = (width * 3 + 3) & ~3;
			for (int y = 0; y < height; y++)
			{
				unsigned char* src = samples + y * stride;
				unsigned char* destPixel = dest + y * destStride;
				for (int x = 0; x < width; x++)
				{
					destPixel[0] = src[2];  // B
					destPixel[1] = src[1];  // G
					destPixel[2] = src[0];  // R
					destPixel += 3;
					src += 3;
				}
			}
		}

		// 清理资源
		fz_drop_pixmap(m_ctx, pix);
		pix = nullptr;
		fz_drop_page(m_ctx, page);
		page = nullptr;
		fz_drop_document(m_ctx, sourceDoc);
		sourceDoc = nullptr;
	}
	fz_catch(m_ctx)
	{
		if (pix) fz_drop_pixmap(m_ctx, pix);
		if (page) fz_drop_page(m_ctx, page);
		if (sourceDoc) fz_drop_document(m_ctx, sourceDoc);
		return nullptr;
	}

	return hBitmap;
}

void CPDFEditDialog::RefreshThumbnailList()
{
	int topIndex = m_thumbnailList.GetTopIndex();
	std::vector<int> selected;
	POSITION pos = m_thumbnailList.GetFirstSelectedItemPosition();
	while (pos)
		selected.push_back(m_thumbnailList.GetNextSelectedItem(pos));
	m_thumbnailList.SetRedraw(FALSE);
	m_thumbnailList.DeleteAllItems();
	ResetThumbnailImageList();
	int displayIndex = 0;
	for (auto& page : m_pages)
	{
		if (page.hThumbnail)
		{
			::DeleteObject(page.hThumbnail);
			page.hThumbnail = nullptr;
		}
		if (page.isDeleted)
			continue;
		page.displayIndex = displayIndex;
		m_thumbnailList.InsertItem(displayIndex++, _T(""), 0);
	}
	LayoutThumbnailItems();
	for (int index : selected)
		if (index < displayIndex)
			m_thumbnailList.SetItemState(index, LVIS_SELECTED, LVIS_SELECTED);
	if (topIndex >= 0 && topIndex < displayIndex)
		m_thumbnailList.EnsureVisible(topIndex, FALSE);
	m_thumbnailList.SetRedraw(TRUE);
	m_thumbnailList.Invalidate(FALSE);
	RenderVisibleThumbnails();
	UpdateInfoBar();
}

void CPDFEditDialog::RenderVisibleThumbnails()
{
	if (!m_thumbnailList.GetSafeHwnd())
		return;
	if (m_imageList.GetImageCount() > 80 && !m_isDragging)
	{
		RefreshThumbnailList();
		return;
	}
	std::vector<PageInfo*> visiblePages;
	for (auto& page : m_pages)
		if (!page.isDeleted)
			visiblePages.push_back(&page);
	CRect clientRect;
	m_thumbnailList.GetClientRect(&clientRect);
	const int listItemCount = m_thumbnailList.GetItemCount();
	const int pageCount = static_cast<int>(visiblePages.size());
	const int itemCount = listItemCount < pageCount ? listItemCount : pageCount;
	int firstVisible = 0;
	int end = itemCount;
	while (firstVisible < end)
	{
		const int middle = firstVisible + (end - firstVisible) / 2;
		CRect itemRect;
		if (!m_thumbnailList.GetItemRect(middle, &itemRect, LVIR_BOUNDS))
			return;
		if (itemRect.bottom <= clientRect.top)
			firstVisible = middle + 1;
		else
			end = middle;
	}
	for (int item = firstVisible; item < itemCount; ++item)
	{
		CRect itemRect;
		if (!m_thumbnailList.GetItemRect(item, &itemRect, LVIR_BOUNDS))
			continue;
		if (itemRect.top >= clientRect.bottom)
			break;
		if (itemRect.right <= clientRect.left || itemRect.left >= clientRect.right ||
			itemRect.bottom <= clientRect.top)
			continue;
		LVITEM state = { 0 };
		state.mask = LVIF_IMAGE;
		state.iItem = item;
		if (!m_thumbnailList.GetItem(&state) || state.iImage != 0)
			continue;
		PageInfo* page = visiblePages[item];
		if (page->isDragging)
			continue;
		if (!page->hThumbnail)
		{
			int originalPos = page->originalIndex + 1;
			int currentPos = item + 1;
			page->hThumbnail = (page->sourceFile == m_filePath) ?
				RenderThumbnail(page->originalIndex, currentPos, originalPos, page->rotationAngle) :
				RenderThumbnailFromSource(page->sourceFile, page->sourcePageIndex,
					currentPos, originalPos, page->rotationAngle);
		}
		if (page->hThumbnail)
		{
			HBITMAP hActionBitmap = CreateActionThumbnail(page->hThumbnail, item + 1,
				page->originalIndex + 1);
			int imageIndex = -1;
			if (hActionBitmap)
			{
				CBitmap actionBitmap;
				actionBitmap.Attach(hActionBitmap);
				imageIndex = m_imageList.Add(&actionBitmap, (CBitmap*)nullptr);
				actionBitmap.Detach();
				::DeleteObject(hActionBitmap);
			}
			if (imageIndex >= 0)
			{
				state.iImage = imageIndex;
				m_thumbnailList.SetItem(&state);
			}
		}
	}
}

void CPDFEditDialog::UpdateInfoBar()
{
	// 计算未删除的页面数
	int activePages = 0;
	for (const auto& page : m_pages)
	{
		if (!page.isDeleted)
			activePages++;
	}

	CString info;
	info.Format(_T("总页数: %d  当前页数: %d  已删除: %d"),
		m_totalPages, activePages, m_totalPages - activePages);
	m_staticInfo.SetWindowText(info);
}

// ========== 动画系统实现 ==========

// 线性插值
CPoint CPDFEditDialog::LerpPoint(CPoint start, CPoint end, float t)
{
	return CPoint(
		static_cast<int>(start.x + (end.x - start.x) * t),
		static_cast<int>(start.y + (end.y - start.y) * t)
	);
}

// Ease-out-cubic 缓动函数
float CPDFEditDialog::EaseOutCubic(float t)
{
	float f = t - 1.0f;
	return f * f * f + 1.0f;
}

// 启动重排动画
void CPDFEditDialog::StartReflowAnimation()
{
	// 停止之前的动画
	StopAnimation();

	// 记录当前位置（First）
	m_animState.startPositions.clear();
	m_animState.targetPositions.clear();

	int activeCount = 0;
	for (const auto& page : m_pages) {
		if (!page.isDeleted) activeCount++;
	}

	// 遍历所有未删除的页面，记录当前和目标位置
	int displayIndex = 0;
	for (size_t i = 0; i < m_pages.size(); i++) {
		if (m_pages[i].isDeleted) continue;

		// 当前位置
		LVFINDINFO findInfo = { 0 };
		findInfo.flags = LVFI_PARAM;
		findInfo.lParam = displayIndex;
		int itemIndex = m_thumbnailList.FindItem(&findInfo, -1);

		CRect itemRect;
		if (itemIndex >= 0) {
			m_thumbnailList.GetItemRect(itemIndex, &itemRect, LVIR_BOUNDS);
		}
		m_animState.startPositions.push_back(itemRect.TopLeft());

		displayIndex++;
	}

	// 设置动画参数
	m_animState.duration = (m_totalPages > 500) ? ANIM_DURATION_FAST : ANIM_DURATION_NORMAL;
	m_animState.startTime = GetTickCount();
	m_animState.isAnimating = true;

	// 启动定时器 (60fps = 16ms)
	SetTimer(TIMER_ANIMATION, 16, NULL);
}

// 更新动画
void CPDFEditDialog::UpdateAnimation()
{
	if (!m_animState.isAnimating) return;

	DWORD elapsed = GetTickCount() - m_animState.startTime;
	float progress = min(1.0f, (float)elapsed / m_animState.duration);

	// 应用缓动
	progress = EaseOutCubic(progress);

	// 更新所有项的位置（这里简化处理，实际MFC中需要特殊处理）
	// 由于MFC ListView不支持直接动画，我们在RefreshThumbnailList后立即完成

	if (progress >= 1.0f) {
		StopAnimation();
	}
}

// 停止动画
void CPDFEditDialog::StopAnimation()
{
	if (m_animState.isAnimating) {
		KillTimer(TIMER_ANIMATION);
		m_animState.isAnimating = false;
	}
}

// ========== 拖拽辅助函数 ==========

// 计算插入索引（根据鼠标位置）
int CPDFEditDialog::CalculateInsertIndex(CPoint point)
{
	// 将客户端坐标转换为ListView坐标
	m_thumbnailList.ScreenToClient(&point);

	// 命中测试
	LVHITTESTINFO hitTest = { 0 };
	hitTest.pt = point;
	int hitIndex = m_thumbnailList.HitTest(&hitTest);

	if (hitIndex >= 0) {
		// 根据被拖拽页面的位置决定插入位置
		// 如果拖拽页面在目标页面之前，插入位置是 hitIndex（因为删除后索引前移）
		// 如果拖拽页面在目标页面之后，插入位置是 hitIndex + 1（删除不影响目标索引）
		if (hitIndex < m_dragIndex) {
			return hitIndex +1;  // 拖拽页面在目标之前
		} else {
			return hitIndex ;  // 拖拽页面在目标之后或相同位置
		}
	}

	// 默认插入到末尾
	return m_thumbnailList.GetItemCount();
}

// 更新插入索引并触发动画
void CPDFEditDialog::UpdateInsertIndex(CPoint point)
{
	int newIndex = CalculateInsertIndex(point);

	// 限制索引范围（被拖拽的页面已经被移除，所以总数少1）
	int activeCount = 0;
	for (const auto& page : m_pages) {
		if (!page.isDeleted) activeCount++;
	}
	newIndex = max(0, min(newIndex, activeCount));

	// 只在索引变化时更新
	if (newIndex != m_lastDropIndex) {
		m_dropIndex = newIndex;
		m_lastDropIndex = newIndex;
		// ★★★ 新逻辑：拖拽过程中不刷新列表，只记录drop位置
		// 实际移动在EndDragOperation中进行
	}
}

// 处理自动滚动
void CPDFEditDialog::HandleAutoScroll(CPoint point)
{
	CRect clientRect;
	m_thumbnailList.GetClientRect(&clientRect);
	m_thumbnailList.ClientToScreen(&clientRect);

	int scrollSpeed = 0;

	// 上边缘
	if (point.y < clientRect.top + AUTO_SCROLL_ZONE) {
		scrollSpeed = -(AUTO_SCROLL_ZONE - (point.y - clientRect.top)) / 2;
	}
	// 下边缘
	else if (point.y > clientRect.bottom - AUTO_SCROLL_ZONE) {
		scrollSpeed = ((point.y - (clientRect.bottom - AUTO_SCROLL_ZONE))) / 2;
	}

	m_autoScrollSpeed = scrollSpeed;

	if (scrollSpeed != 0) {
		SetTimer(TIMER_AUTO_SCROLL, 50, NULL);
	}
	else {
		KillTimer(TIMER_AUTO_SCROLL);
	}
}

// 创建占位虚影位图
HBITMAP CPDFEditDialog::CreatePlaceholderBitmap()
{
	const int FIXED_WIDTH = THUMBNAIL_WIDTH;
	const int FIXED_HEIGHT = THUMBNAIL_HEIGHT;
	const int cardMargin = 4;

	BITMAPINFO bmi = { 0 };
	bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
	bmi.bmiHeader.biWidth = FIXED_WIDTH;
	bmi.bmiHeader.biHeight = -FIXED_HEIGHT;
	bmi.bmiHeader.biPlanes = 1;
	bmi.bmiHeader.biBitCount = 24;
	bmi.bmiHeader.biCompression = BI_RGB;

	CClientDC dc(this);
	void* pBits = nullptr;
	HBITMAP hBitmap = CreateDIBSection(dc.GetSafeHdc(), &bmi, DIB_RGB_COLORS, &pBits, nullptr, 0);

	if (hBitmap && pBits) {
		unsigned char* dest = (unsigned char*)pBits;
		int destStride = ((FIXED_WIDTH * 3 + 3) & ~3);

		// 填充浅灰色背景
		for (int y = 0; y < FIXED_HEIGHT; y++) {
			for (int x = 0; x < FIXED_WIDTH; x++) {
				dest[y * destStride + x * 3 + 0] = 248;  // B
				dest[y * destStride + x * 3 + 1] = 248;  // G
				dest[y * destStride + x * 3 + 2] = 248;  // R
			}
		}

		// 绘制半透明灰色卡片
		for (int y = cardMargin; y < FIXED_HEIGHT - cardMargin; y++) {
			for (int x = cardMargin; x < FIXED_WIDTH - cardMargin; x++) {
				dest[y * destStride + x * 3 + 0] = 220;  // B
				dest[y * destStride + x * 3 + 1] = 220;  // G
				dest[y * destStride + x * 3 + 2] = 220;  // R
			}
		}

		// 使用GDI+绘制虚线边框
		CDC memDC;
		memDC.CreateCompatibleDC(&dc);
		HBITMAP hOldBitmap = (HBITMAP)memDC.SelectObject(hBitmap);

		Gdiplus::Graphics graphics(memDC.GetSafeHdc());
		graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);

		// 绘制虚线边框
		Gdiplus::RectF borderRect(
			(Gdiplus::REAL)cardMargin,
			(Gdiplus::REAL)cardMargin,
			(Gdiplus::REAL)(FIXED_WIDTH - cardMargin * 2),
			(Gdiplus::REAL)(FIXED_HEIGHT - cardMargin * 2)
		);
		Gdiplus::Pen dashedPen(Gdiplus::Color(255, 150, 150, 150), 2.0f);
		dashedPen.SetDashStyle(Gdiplus::DashStyleDash);
		graphics.DrawRectangle(&dashedPen, borderRect);

		memDC.SelectObject(hOldBitmap);
	}

	return hBitmap;
}

// 开始拖拽操作
void CPDFEditDialog::BeginDragOperation()
{
	m_isDragging = true;
	m_potentialDrag = false;

	// 找到被拖拽页面在m_pages中的实际索引（只计算未删除的页面）
	m_draggedPageOriginalPos = -1;
	int currentDisplayIndex = 0;
	for (size_t i = 0; i < m_pages.size(); i++) {
		if (m_pages[i].isDeleted)
			continue;

		if (currentDisplayIndex == m_dragIndex) {
			m_draggedPageOriginalPos = static_cast<int>(i);
			break;
		}
		currentDisplayIndex++;
	}

	if (m_draggedPageOriginalPos < 0)
		return;

	TRACE(_T("BeginDragOperation: m_dragIndex=%d, m_draggedPageOriginalPos=%d\n"), m_dragIndex, m_draggedPageOriginalPos);

	// 保存被拖拽的页面信息（深拷贝HBITMAP）
	m_draggedPage = m_pages[m_draggedPageOriginalPos];
	HBITMAP oldBitmap = m_draggedPage.hThumbnail;
	if (oldBitmap) {
		m_draggedPage.hThumbnail = (HBITMAP)::CopyImage(oldBitmap, IMAGE_BITMAP, 0, 0, LR_CREATEDIBSECTION);
	}

	// ★★★ 先标记为isDragging并刷新列表显示占位虚影
	m_pages[m_draggedPageOriginalPos].isDragging = true;
	TRACE(_T("BeginDragOperation: Set isDragging=true for page %d\n"), m_draggedPageOriginalPos);

	// 刷新列表显示占位虚影（RefreshThumbnailList会检测isDragging）
	RefreshThumbnailList();

	// 创建拖拽图像（基于原始缩略图，从m_draggedPage中）
	if (m_draggedPage.hThumbnail) {
		m_pDragImage = new CImageList();
		if (m_pDragImage->Create(THUMBNAIL_WIDTH, THUMBNAIL_HEIGHT, ILC_COLOR24, 1, 0)) {
			CBitmap bmp;
			BITMAP bmInfo;
			::GetObject(m_draggedPage.hThumbnail, sizeof(BITMAP), &bmInfo);
			bmp.Attach(m_draggedPage.hThumbnail);
			m_pDragImage->Add(&bmp, (CBitmap*)nullptr);
			bmp.Detach();
			TRACE(_T("BeginDragOperation: Created drag image from saved thumbnail\n"));
		}
	}

	if (!m_pDragImage || m_pDragImage->GetImageCount() == 0) {
		TRACE(_T("BeginDragOperation: Failed to create drag image, aborting\n"));
		if (m_pDragImage) {
			delete m_pDragImage;
			m_pDragImage = nullptr;
		}
		m_pages[m_draggedPageOriginalPos].isDragging = false;
		m_isDragging = false;
		return;
	}

	// 初始化drop索引为当前拖拽位置
	m_dropIndex = m_dragIndex;
	m_lastDropIndex = m_dragIndex;

	// 开始拖拽图像
	m_pDragImage->BeginDrag(0, CPoint(0, 0));
	m_pDragImage->DragEnter(GetDesktopWindow(), m_lastMousePoint);
	m_pDragImage->DragShowNolock(TRUE);

	SetCapture();
	SetCursor(LoadCursor(nullptr, IDC_SIZEALL));
}

// 结束拖拽操作
void CPDFEditDialog::EndDragOperation()
{
	if (!m_isDragging) return;

	m_isDragging = false;
	ReleaseCapture();

	if (m_pDragImage) {
		m_pDragImage->DragLeave(GetDesktopWindow());
		m_pDragImage->EndDrag();
		delete m_pDragImage;
		m_pDragImage = nullptr;
	}

	// 停止定时器
	KillTimer(TIMER_AUTO_SCROLL);
	KillTimer(TIMER_ANIMATION);

	if (m_draggedPageOriginalPos < 0 || m_draggedPageOriginalPos >= (int)m_pages.size()) {
		return;
	}

	// ★★★ 关键修复：先计算插入位置（此时isDragging仍为true，会被正确跳过）
	// 然后再取消isDragging标记
	int insertActualPos = 0;
	int displayIdx = 0;

	if (m_dropIndex == 0) {
		// 插入到最前面
		for (size_t i = 0; i < m_pages.size(); i++) {
			if (!m_pages[i].isDeleted && !m_pages[i].isDragging) {
				insertActualPos = static_cast<int>(i);
				break;
			}
		}
	} else {
		// 插入到第m_dropIndex个位置
		for (size_t i = 0; i < m_pages.size(); i++) {
			if (m_pages[i].isDeleted || m_pages[i].isDragging)
				continue;

			displayIdx++;
			if (displayIdx == m_dropIndex) {
				insertActualPos = static_cast<int>(i) + 1;
				break;
			}
		}
		// 如果到达末尾
		if (displayIdx < m_dropIndex) {
			insertActualPos = static_cast<int>(m_pages.size());
		}
	}

	// 调整插入位置：如果插入位置在原位置之后，需要减1（因为要先删除原位置）
	int finalInsertPos = insertActualPos;
	if (insertActualPos > m_draggedPageOriginalPos) {
		finalInsertPos = insertActualPos - 1;
	}

	// 现在取消isDragging标记
	m_pages[m_draggedPageOriginalPos].isDragging = false;

	// 删除原位置的HBITMAP（不是m_draggedPage的，是m_pages中的）
	if (m_pages[m_draggedPageOriginalPos].hThumbnail) {
		::DeleteObject(m_pages[m_draggedPageOriginalPos].hThumbnail);
		m_pages[m_draggedPageOriginalPos].hThumbnail = nullptr;
	}

	// 从原位置删除
	m_pages.erase(m_pages.begin() + m_draggedPageOriginalPos);

	// 插入到新位置（m_draggedPage有独立拷贝的HBITMAP）
	m_pages.insert(m_pages.begin() + finalInsertPos, m_draggedPage);

	// 刷新列表显示最终结果
	RefreshThumbnailList();

	// 清理拖拽状态
	m_dragIndex = -1;
	m_dropIndex = -1;
	m_lastDropIndex = -1;
	m_potentialDrag = false;
	m_draggedPageOriginalPos = -1;

	// 更新信息栏
	UpdateInfoBar();
	RecordEditState();
}

// ========== 消息处理函数 ==========

// 鼠标左键按下
void CPDFEditDialog::OnLButtonDown(UINT nFlags, CPoint point)
{
	// 转换为ListView坐标
	CPoint lvPoint = point;
	ClientToScreen(&lvPoint);
	m_thumbnailList.ScreenToClient(&lvPoint);

	// 命中测试
	LVHITTESTINFO hitTest = { 0 };
	hitTest.pt = lvPoint;
	int hitIndex = m_thumbnailList.HitTest(&hitTest);

	if (hitIndex >= 0) {
		// ★★★ 批量选择功能：处理Shift和Ctrl键
		bool shiftPressed = (nFlags & MK_SHIFT) != 0;
		bool ctrlPressed = (nFlags & MK_CONTROL) != 0;

		if (shiftPressed) {
			// Shift+点击：连续选择
			// 找到第一个已选中的项
			int firstSelected = -1;
			POSITION pos = m_thumbnailList.GetFirstSelectedItemPosition();
			if (pos) {
				firstSelected = m_thumbnailList.GetNextSelectedItem(pos);
			}

			if (firstSelected >= 0) {
				// 清除所有选中
				pos = m_thumbnailList.GetFirstSelectedItemPosition();
				while (pos) {
					int nItem = m_thumbnailList.GetNextSelectedItem(pos);
					m_thumbnailList.SetItemState(nItem, 0, LVIS_SELECTED);
				}

				// 选中从firstSelected到hitIndex之间的所有项
				int start = min(firstSelected, hitIndex);
				int end = max(firstSelected, hitIndex);
				for (int i = start; i <= end; i++) {
					m_thumbnailList.SetItemState(i, LVIS_SELECTED, LVIS_SELECTED);
				}
			}
			else {
				// 没有已选中的项，只选中当前项
				m_thumbnailList.SetItemState(hitIndex, LVIS_SELECTED, LVIS_SELECTED);
			}

			// Shift选择时不启动拖拽
			m_potentialDrag = false;
		}
		else if (ctrlPressed) {
			// Ctrl+点击：切换选中状态（多选）
			UINT state = m_thumbnailList.GetItemState(hitIndex, LVIS_SELECTED);
			if (state & LVIS_SELECTED) {
				// 已选中，取消选中
				m_thumbnailList.SetItemState(hitIndex, 0, LVIS_SELECTED);
			}
			else {
				// 未选中，选中
				m_thumbnailList.SetItemState(hitIndex, LVIS_SELECTED, LVIS_SELECTED);
			}

			// Ctrl选择时不启动拖拽
			m_potentialDrag = false;
		}
		else {
			// 普通点击：启动拖拽逻辑
			// 记录拖拽起始点和索引
			m_dragStartPoint = point;
			m_dragIndex = hitIndex;
			m_potentialDrag = true;

			// 计算点击偏移
			CRect itemRect;
			m_thumbnailList.GetItemRect(hitIndex, &itemRect, LVIR_BOUNDS);
			m_thumbnailList.ClientToScreen(&itemRect);
			ClientToScreen(&point);
			m_clickOffset = CPoint(point.x - itemRect.left, point.y - itemRect.top);
		}
	}

	CDialogEx::OnLButtonDown(nFlags, point);
}

// 定时器处理
void CPDFEditDialog::OnTimer(UINT_PTR nIDEvent)
{
	switch (nIDEvent) {
	case TIMER_ANIMATION:
		UpdateAnimation();
		break;

	case TIMER_AUTO_SCROLL:
		if (m_isDragging && m_autoScrollSpeed != 0) {
			// 执行滚动
			m_thumbnailList.Scroll(CSize(0, m_autoScrollSpeed));

			// 保持拖拽图像跟随
			if (m_pDragImage) {
				m_pDragImage->DragMove(m_lastMousePoint);
			}
		}
		break;
	case TIMER_VISIBLE_THUMBNAILS:
		RenderVisibleThumbnails();
		break;
	}

	CDialogEx::OnTimer(nIDEvent);
}

// 拖拽开始（ListView通知）
void CPDFEditDialog::OnBeginDrag(NMHDR* pNMHDR, LRESULT* pResult)
{
	NM_LISTVIEW* pNMListView = (NM_LISTVIEW*)pNMHDR;
	*pResult = 0;
	if (HitPageAction(pNMListView->iItem, pNMListView->ptAction) >= 0)
		return;

	m_dragIndex = pNMListView->iItem;
	if (m_dragIndex < 0)
		return;

	TRACE(_T("OnBeginDrag: m_dragIndex=%d\n"), m_dragIndex);

	// 找到被拖拽页面在m_pages中的实际索引（只计算未删除的页面）
	m_draggedPageOriginalPos = -1;
	int currentDisplayIndex = 0;
	for (size_t i = 0; i < m_pages.size(); i++) {
		if (m_pages[i].isDeleted)
			continue;

		if (currentDisplayIndex == m_dragIndex) {
			m_draggedPageOriginalPos = static_cast<int>(i);
			break;
		}
		currentDisplayIndex++;
	}

	if (m_draggedPageOriginalPos < 0) {
		TRACE(_T("OnBeginDrag: m_draggedPageOriginalPos not found\n"));
		return;
	}

	TRACE(_T("OnBeginDrag: m_draggedPageOriginalPos=%d\n"), m_draggedPageOriginalPos);

	// 保存被拖拽的页面信息（深拷贝HBITMAP）
	m_draggedPage = m_pages[m_draggedPageOriginalPos];
	HBITMAP oldBitmap = m_draggedPage.hThumbnail;
	if (oldBitmap) {
		m_draggedPage.hThumbnail = (HBITMAP)::CopyImage(oldBitmap, IMAGE_BITMAP, 0, 0, LR_CREATEDIBSECTION);
		TRACE(_T("OnBeginDrag: Copied HBITMAP\n"));
	}

	// ★★★ 新逻辑：标记为isDragging并刷新列表显示占位虚影
	m_pages[m_draggedPageOriginalPos].isDragging = true;
	TRACE(_T("OnBeginDrag: Set isDragging=true for page %d\n"), m_draggedPageOriginalPos);

	// 刷新列表显示占位虚影（RefreshThumbnailList会检测isDragging）
	RefreshThumbnailList();

	// 创建拖拽图像（基于保存的原始缩略图）
	if (m_draggedPage.hThumbnail) {
		m_pDragImage = new CImageList();
		if (m_pDragImage->Create(THUMBNAIL_WIDTH, THUMBNAIL_HEIGHT, ILC_COLOR24, 1, 0)) {
			CBitmap bmp;
			bmp.Attach(m_draggedPage.hThumbnail);
			m_pDragImage->Add(&bmp, (CBitmap*)nullptr);
			bmp.Detach();
			TRACE(_T("OnBeginDrag: Created drag image from saved thumbnail\n"));
		}
	}

	if (!m_pDragImage || m_pDragImage->GetImageCount() == 0) {
		TRACE(_T("OnBeginDrag: Failed to create drag image, aborting\n"));
		if (m_pDragImage) {
			delete m_pDragImage;
			m_pDragImage = nullptr;
		}
		m_pages[m_draggedPageOriginalPos].isDragging = false;
		return;
	}

	// 初始化drop索引为当前拖拽位置
	m_dropIndex = m_dragIndex;
	m_lastDropIndex = m_dragIndex;

	// 开始拖拽图像
	m_pDragImage->BeginDrag(0, CPoint(0, 0));

	CPoint pt = pNMListView->ptAction;
	ClientToScreen(&pt);
	m_pDragImage->DragEnter(GetDesktopWindow(), pt);
	m_pDragImage->DragShowNolock(TRUE);

	m_isDragging = true;
	SetCapture();
	SetCursor(LoadCursor(nullptr, IDC_SIZEALL));

	TRACE(_T("OnBeginDrag: Drag started\n"));
}

void CPDFEditDialog::OnMouseMove(UINT nFlags, CPoint point)
{
	// ========== 防误触检测 ==========
	if (m_potentialDrag && !m_isDragging) {
		// 计算移动距离
		int dx = point.x - m_dragStartPoint.x;
		int dy = point.y - m_dragStartPoint.y;
		int distance = static_cast<int>(sqrt(dx * dx + dy * dy));

		// 超过阈值才开始拖拽
		if (distance > DRAG_THRESHOLD) {
			ClientToScreen(&point);
			m_lastMousePoint = point;
			BeginDragOperation();
		}
	}

	// ========== 拖拽中 ==========
	if (m_isDragging && m_pDragImage)
	{
		// 转换为屏幕坐标
		CPoint screenPt = point;
		ClientToScreen(&screenPt);
		m_lastMousePoint = screenPt;

		// 更新拖拽图像位置
		m_pDragImage->DragMove(screenPt);

		// 检测是否在ListView区域内
		CRect listRect;
		m_thumbnailList.GetWindowRect(&listRect);

		if (listRect.PtInRect(screenPt))
		{
			// 暂停拖拽图像以进行重绘
			m_pDragImage->DragShowNolock(FALSE);

			// 更新插入索引并触发重排
			UpdateInsertIndex(screenPt);

			// 恢复拖拽图像
			m_pDragImage->DragShowNolock(TRUE);
		}

		// 处理自动滚动
		HandleAutoScroll(screenPt);
	}

	CDialogEx::OnMouseMove(nFlags, point);
}

void CPDFEditDialog::OnLButtonUp(UINT nFlags, CPoint point)
{
	// 取消潜在拖拽
	m_potentialDrag = false;

	// 如果正在拖拽，结束拖拽操作
	if (m_isDragging) {
		EndDragOperation();
	}

	CDialogEx::OnLButtonUp(nFlags, point);
}

// ========== 右键删除功能 ==========

// 删除选中的页面
void CPDFEditDialog::DeleteSelectedPages()
{
	// 获取所有选中项的索引
	std::vector<int> selectedIndices;
	POSITION pos = m_thumbnailList.GetFirstSelectedItemPosition();
	while (pos) {
		int nItem = m_thumbnailList.GetNextSelectedItem(pos);
		selectedIndices.push_back(nItem);
	}

	// 如果没有选中项
	if (selectedIndices.empty()) {
		MessageBox(_T("请先选择要删除的页面！"), _T("提示"), MB_OK | MB_ICONWARNING);
		return;
	}

	// 计算当前未删除的页面数
	int activePages = 0;
	for (const auto& page : m_pages) {
		if (!page.isDeleted)
			activePages++;
	}

	// 检查删除后是否至少保留一页
	if (activePages - selectedIndices.size() < 1) {
		MessageBox(_T("至少需要保留一页，无法删除所有页面！"), _T("提示"), MB_OK | MB_ICONWARNING);
		return;
	}

	// 确认删除
	CString confirmMsg;
	if (selectedIndices.size() == 1) {
		confirmMsg = _T("确定要删除选中的 1 页吗？");
	} else {
		confirmMsg.Format(_T("确定要删除选中的 %d 页吗？"), selectedIndices.size());
	}

	if (MessageBox(confirmMsg, _T("确认删除"), MB_YESNO | MB_ICONQUESTION) != IDYES)
		return;

	// 将displayIndex转换为实际的页面索引并标记为删除
	for (int displayIndex : selectedIndices) {
		int actualIndex = -1;
		int currentDisplayIndex = 0;

		for (size_t i = 0; i < m_pages.size(); i++) {
			if (m_pages[i].isDeleted)
				continue;

			if (currentDisplayIndex == displayIndex) {
				actualIndex = static_cast<int>(i);
				break;
			}

			currentDisplayIndex++;
		}

		if (actualIndex >= 0 && actualIndex < (int)m_pages.size()) {
			m_pages[actualIndex].isDeleted = true;
		}
	}

	// 刷新列表
	RefreshThumbnailList();

	// 更新信息栏
	UpdateInfoBar();
	RecordEditState();
}

// ========== 提取功能 ==========

void CPDFEditDialog::ExtractSelectedPages()
{
	// 获取所有选中项的索引
	std::vector<int> selectedIndices;
	POSITION pos = m_thumbnailList.GetFirstSelectedItemPosition();
	while (pos) {
		int nItem = m_thumbnailList.GetNextSelectedItem(pos);
		selectedIndices.push_back(nItem);
	}

	// 必须至少选中1页
	if (selectedIndices.empty()) {
		MessageBox(_T("请至少选中1页进行提取！"), _T("提示"), MB_OK | MB_ICONWARNING);
		return;
	}

	// 确认提取
	CString confirmMsg;
	if (selectedIndices.size() == 1) {
		confirmMsg = _T("确定要提取选中的 1 页为新PDF吗？");
	} else {
		confirmMsg.Format(_T("确定要提取选中的 %d 页为新PDF吗？"), selectedIndices.size());
	}

	if (MessageBox(confirmMsg, _T("确认提取"), MB_YESNO | MB_ICONQUESTION) != IDYES)
		return;

	// 弹出文件保存对话框
	CFileDialog saveDlg(FALSE, _T("pdf"), _T("extracted.pdf"),
		OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST,
		_T("PDF文件 (*.pdf)|*.pdf||"));

	if (saveDlg.DoModal() != IDOK)
		return;

	CString outputPath = saveDlg.GetPathName();
	if (!m_filePath.IsEmpty() &&
		NormalizePathForCompare(outputPath).CompareNoCase(NormalizePathForCompare(m_filePath)) == 0)
	{
		MessageBox(_T("提取页面时不能直接覆盖当前正在编辑的原文件。\n请选择其他文件名，或使用底部“保存”按钮覆盖原文件。"),
			_T("提示"), MB_OK | MB_ICONWARNING);
		return;
	}

	// 收集要提取的页面信息
	std::vector<PageInfo> pagesToExtract;
	for (int displayIndex : selectedIndices) {
		int actualIndex = -1;
		int currentDisplayIndex = 0;

		for (size_t i = 0; i < m_pages.size(); i++) {
			if (m_pages[i].isDeleted)
				continue;

			if (currentDisplayIndex == displayIndex) {
				actualIndex = static_cast<int>(i);
				break;
			}

			currentDisplayIndex++;
		}

		if (actualIndex >= 0 && actualIndex < (int)m_pages.size()) {
			pagesToExtract.push_back(m_pages[actualIndex]);
		}
	}

	// 创建新PDF并提取页面
	pdf_write_options opts = pdf_default_write_options;
	opts.do_garbage = 1;

	bool tempReady = false;
	CString tempFilePath;
	pdf_document* newPdf = nullptr;
	fz_var(newPdf);
	std::map<CString, fz_document*> openDocs;

	fz_try(m_ctx)
	{
		// 创建新的PDF文档
		newPdf = pdf_create_document(m_ctx);

		// 遍历要提取的页面
		for (const auto& pageInfo : pagesToExtract)
		{
			if (IsImageFilePath(pageInfo.sourceFile))
			{
				InsertImagePageToPdf(m_ctx, newPdf, -1, pageInfo.sourceFile);
			}
			else
			{
				fz_document* sourceDoc = nullptr;

				auto it = openDocs.find(pageInfo.sourceFile);
				if (it != openDocs.end()) {
					sourceDoc = it->second;
				}
				else {
					sourceDoc = OpenAuthenticatedSource(pageInfo.sourceFile);
					openDocs[pageInfo.sourceFile] = sourceDoc;
				}

				if (!sourceDoc)
					continue;

				pdf_document* sourcePdf = pdf_document_from_fz_document(m_ctx, sourceDoc);
				if (!sourcePdf)
					continue;

				pdf_graft_page(m_ctx, newPdf, -1, sourcePdf, pageInfo.sourcePageIndex);
			}

			// 应用旋转（如果有）
			if (pageInfo.rotationAngle != 0) {
				int newPageIndex = pdf_count_pages(m_ctx, newPdf) - 1;
				pdf_obj* pageObj = pdf_lookup_page_obj(m_ctx, newPdf, newPageIndex);
				if (pageObj) {
					int originalRotate = pdf_to_int(m_ctx, pdf_dict_get(m_ctx, pageObj, PDF_NAME(Rotate)));
					int newRotate = (originalRotate + pageInfo.rotationAngle) % 360;
					pdf_dict_put_int(m_ctx, pageObj, PDF_NAME(Rotate), newRotate);
				}
			}
		}

		// 先验证页数并写入临时文件；待 fz_always 释放源文件句柄后再替换目标文件。
		tempReady = WriteAndValidateTempPdf(m_ctx, newPdf, outputPath,
			static_cast<int>(pagesToExtract.size()), opts, tempFilePath);
	}
	fz_always(m_ctx)
	{
		// 清理：关闭所有打开的源文档
		for (auto& pair : openDocs) {
			if (pair.second) {
				fz_drop_document(m_ctx, pair.second);
			}
		}
		openDocs.clear();

		// 清理新文档
		if (newPdf) {
			pdf_drop_document(m_ctx, newPdf);
			newPdf = nullptr;
		}
	}
	fz_catch(m_ctx)
	{
		tempReady = false;
	}

	const bool success = tempReady && FinalizeValidatedPdf(
		tempFilePath, outputPath, m_onBeforeReplaceTarget, m_onAfterReplaceTarget);
	MessageBox(success ? _T("页面提取成功！") :
		_T("页面提取失败。请检查源文件、页面数量及保存位置。"),
		success ? _T("提示") : _T("错误"),
		MB_OK | (success ? MB_ICONINFORMATION : MB_ICONERROR));
}

// ========== 交换功能 ==========

void CPDFEditDialog::SwapSelectedPages()
{
	// 获取所有选中项的索引
	std::vector<int> selectedIndices;
	POSITION pos = m_thumbnailList.GetFirstSelectedItemPosition();
	while (pos) {
		int nItem = m_thumbnailList.GetNextSelectedItem(pos);
		selectedIndices.push_back(nItem);
	}

	// 必须选中正好2页
	if (selectedIndices.size() != 2) {
		MessageBox(_T("请选中正好2页进行交换！"), _T("提示"), MB_OK | MB_ICONWARNING);
		return;
	}

	// 将displayIndex转换为实际的页面索引
	int actualIndex1 = -1, actualIndex2 = -1;
	int currentDisplayIndex = 0;

	for (size_t i = 0; i < m_pages.size(); i++) {
		if (m_pages[i].isDeleted)
			continue;

		if (currentDisplayIndex == selectedIndices[0]) {
			actualIndex1 = static_cast<int>(i);
		}
		if (currentDisplayIndex == selectedIndices[1]) {
			actualIndex2 = static_cast<int>(i);
		}

		currentDisplayIndex++;
	}

	// 验证索引有效性
	if (actualIndex1 < 0 || actualIndex1 >= (int)m_pages.size() ||
		actualIndex2 < 0 || actualIndex2 >= (int)m_pages.size()) {
		MessageBox(_T("页面索引无效！"), _T("错误"), MB_OK | MB_ICONERROR);
		return;
	}

	// 交换两个页面
	std::swap(m_pages[actualIndex1], m_pages[actualIndex2]);

	// 刷新列表
	RefreshThumbnailList();

	// 重新选中这两页（交换后的位置）
	m_thumbnailList.SetItemState(selectedIndices[0], LVIS_SELECTED, LVIS_SELECTED);
	m_thumbnailList.SetItemState(selectedIndices[1], LVIS_SELECTED, LVIS_SELECTED);
	RecordEditState();
}

// ========== 旋转功能 ==========

// 旋转位图
HBITMAP CPDFEditDialog::RotateBitmap(HBITMAP hBitmap, int angle)
{
	if (!hBitmap || (angle % 90 != 0))
		return nullptr;

	// 规范化角度到0-360范围
	angle = angle % 360;
	if (angle < 0) angle += 360;

	// 如果角度是0，直接返回原图的副本
	if (angle == 0) {
		return (HBITMAP)::CopyImage(hBitmap, IMAGE_BITMAP, 0, 0, LR_CREATEDIBSECTION);
	}

	// 获取原始位图信息
	BITMAP bm;
	::GetObject(hBitmap, sizeof(BITMAP), &bm);

	int srcWidth = bm.bmWidth;
	int srcHeight = bm.bmHeight;

	// 固定输出尺寸为220x180（与ImageList一致）
	const int FIXED_WIDTH = 220;
	const int FIXED_HEIGHT = 180;

	// 创建源DC和目标DC
	HDC hdcScreen = ::GetDC(NULL);
	HDC hdcSrc = ::CreateCompatibleDC(hdcScreen);
	HDC hdcDst = ::CreateCompatibleDC(hdcScreen);

	// 创建固定尺寸的目标位图
	BITMAPINFO bmi = { 0 };
	bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
	bmi.bmiHeader.biWidth = FIXED_WIDTH;
	bmi.bmiHeader.biHeight = -FIXED_HEIGHT;  // 负值表示自顶向下
	bmi.bmiHeader.biPlanes = 1;
	bmi.bmiHeader.biBitCount = 24;
	bmi.bmiHeader.biCompression = BI_RGB;

	void* pDstBits = nullptr;
	HBITMAP hDstBitmap = ::CreateDIBSection(hdcScreen, &bmi, DIB_RGB_COLORS, &pDstBits, nullptr, 0);

	if (!hDstBitmap) {
		::DeleteDC(hdcSrc);
		::DeleteDC(hdcDst);
		::ReleaseDC(NULL, hdcScreen);
		return nullptr;
	}

	// 选择位图到DC
	HBITMAP hOldSrc = (HBITMAP)::SelectObject(hdcSrc, hBitmap);
	HBITMAP hOldDst = (HBITMAP)::SelectObject(hdcDst, hDstBitmap);

	// 设置白色背景
	RECT rc = { 0, 0, FIXED_WIDTH, FIXED_HEIGHT };
	HBRUSH hBrush = ::CreateSolidBrush(RGB(255, 255, 255));
	::FillRect(hdcDst, &rc, hBrush);
	::DeleteObject(hBrush);

	// 计算旋转后的内容尺寸
	int rotatedWidth = (angle == 90 || angle == 270) ? srcHeight : srcWidth;
	int rotatedHeight = (angle == 90 || angle == 270) ? srcWidth : srcHeight;

	// 计算居中偏移（旋转后居中显示）
	int offsetX = (FIXED_WIDTH - rotatedWidth) / 2;
	int offsetY = (FIXED_HEIGHT - rotatedHeight) / 2;

	// 根据角度进行旋转绘制
	int oldMode = ::SetGraphicsMode(hdcDst, GM_ADVANCED);

	XFORM xform;
	switch (angle) {
	case 90:
		// 向右旋转90度
		xform.eM11 = 0.0f;
		xform.eM12 = 1.0f;
		xform.eM21 = -1.0f;
		xform.eM22 = 0.0f;
		xform.eDx = (float)(offsetX + rotatedWidth);
		xform.eDy = (float)offsetY;
		break;

	case 180:
		// 旋转180度
		xform.eM11 = -1.0f;
		xform.eM12 = 0.0f;
		xform.eM21 = 0.0f;
		xform.eM22 = -1.0f;
		xform.eDx = (float)(offsetX + rotatedWidth);
		xform.eDy = (float)(offsetY + rotatedHeight);
		break;

	case 270:
		// 向右旋转270度
		xform.eM11 = 0.0f;
		xform.eM12 = -1.0f;
		xform.eM21 = 1.0f;
		xform.eM22 = 0.0f;
		xform.eDx = (float)offsetX;
		xform.eDy = (float)(offsetY + rotatedHeight);
		break;

	default:
		xform.eM11 = 1.0f;
		xform.eM12 = 0.0f;
		xform.eM21 = 0.0f;
		xform.eM22 = 1.0f;
		xform.eDx = (float)offsetX;
		xform.eDy = (float)offsetY;
		break;
	}

	::SetWorldTransform(hdcDst, &xform);
	::BitBlt(hdcDst, 0, 0, srcWidth, srcHeight, hdcSrc, 0, 0, SRCCOPY);

	// 恢复
	::SetGraphicsMode(hdcDst, oldMode);
	::SelectObject(hdcSrc, hOldSrc);
	::SelectObject(hdcDst, hOldDst);
	::DeleteDC(hdcSrc);
	::DeleteDC(hdcDst);
	::ReleaseDC(NULL, hdcScreen);

	return hDstBitmap;
}

// 旋转选中的页面
void CPDFEditDialog::RotateSelectedPages(int angle)
{
	// 获取所有选中项的索引
	std::vector<int> selectedIndices;
	POSITION pos = m_thumbnailList.GetFirstSelectedItemPosition();
	while (pos) {
		int nItem = m_thumbnailList.GetNextSelectedItem(pos);
		selectedIndices.push_back(nItem);
	}

	// 如果没有选中项
	if (selectedIndices.empty()) {
		MessageBox(_T("请先选择要旋转的页面！"), _T("提示"), MB_OK | MB_ICONWARNING);
		return;
	}

	// 旋转每个选中的页面
	for (int displayIndex : selectedIndices) {
		// 找到实际的页面索引
		int actualIndex = -1;
		int currentDisplayIndex = 0;

		for (size_t i = 0; i < m_pages.size(); i++) {
			if (m_pages[i].isDeleted)
				continue;

			if (currentDisplayIndex == displayIndex) {
				actualIndex = static_cast<int>(i);
				break;
			}

			currentDisplayIndex++;
		}

		if (actualIndex >= 0 && actualIndex < (int)m_pages.size()) {
			// 更新旋转角度
			m_pages[actualIndex].rotationAngle = (m_pages[actualIndex].rotationAngle + angle) % 360;
			if (m_pages[actualIndex].rotationAngle < 0)
				m_pages[actualIndex].rotationAngle += 360;

			// 删除旧缩略图
			if (m_pages[actualIndex].hThumbnail) {
				::DeleteObject(m_pages[actualIndex].hThumbnail);
				m_pages[actualIndex].hThumbnail = nullptr;
			}

			// 重新渲染缩略图（会自动应用旋转角度）
			int currentPos = currentDisplayIndex + 1;
			int originalPos = m_pages[actualIndex].originalIndex + 1;
			const PageInfo& pageInfo = m_pages[actualIndex];
			m_pages[actualIndex].hThumbnail = (pageInfo.sourceFile == m_filePath) ?
				RenderThumbnail(pageInfo.originalIndex, currentPos, originalPos, pageInfo.rotationAngle) :
				RenderThumbnailFromSource(pageInfo.sourceFile, pageInfo.sourcePageIndex,
					currentPos, originalPos, pageInfo.rotationAngle);
		}
	}

	// 刷新列表显示
	RefreshThumbnailList();
	RecordEditState();
}

// 右键菜单
void CPDFEditDialog::OnNMRClick(NMHDR* pNMHDR, LRESULT* pResult)
{
	NM_LISTVIEW* pNMListView = (NM_LISTVIEW*)pNMHDR;
	*pResult = 0;

	if (pNMListView->iItem >= 0) {
		// 检查右键点击的项是否已经被选中
		UINT state = m_thumbnailList.GetItemState(pNMListView->iItem, LVIS_SELECTED);
		bool isAlreadySelected = (state & LVIS_SELECTED) != 0;

		// 如果右键点击的项未被选中，则清除其他选中项，只选中当前项
		if (!isAlreadySelected) {
			// 清除所有选中
			POSITION pos = m_thumbnailList.GetFirstSelectedItemPosition();
			while (pos) {
				int nItem = m_thumbnailList.GetNextSelectedItem(pos);
				m_thumbnailList.SetItemState(nItem, 0, LVIS_SELECTED);
			}
			// 选中当前项
			m_thumbnailList.SetItemState(pNMListView->iItem, LVIS_SELECTED, LVIS_SELECTED);
		}

		// 统计选中的项数
		int selectedCount = m_thumbnailList.GetSelectedCount();

		// 创建右键菜单
		CMenu menu;
		menu.CreatePopupMenu();

		// 旋转子菜单
		CMenu rotateMenu;
		rotateMenu.CreatePopupMenu();
		rotateMenu.AppendMenu(MF_STRING, 2, _T("向右旋转 90°"));
		rotateMenu.AppendMenu(MF_STRING, 3, _T("向右旋转 180°"));
		rotateMenu.AppendMenu(MF_STRING, 4, _T("向右旋转 270°"));

		// 添加主菜单项
		menu.AppendMenu(MF_POPUP, (UINT_PTR)rotateMenu.m_hMenu, _T("旋转"));

		// 如果选中2页，添加交换位置菜单项
		if (selectedCount == 2) {
			menu.AppendMenu(MF_STRING, 5, _T("交换这两页的位置"));
		}

		// 添加提取页面菜单项
		CString extractText;
		if (selectedCount > 1) {
			extractText.Format(_T("提取选中的 %d 页为新PDF"), selectedCount);
		} else {
			extractText = _T("提取此页为新PDF");
		}
		menu.AppendMenu(MF_STRING, 6, extractText);

		// 添加删除菜单项
		CString menuText;
		if (selectedCount > 1) {
			menuText.Format(_T("删除选中的 %d 页"), selectedCount);
		} else {
			menuText = _T("删除此页");
		}
		menu.AppendMenu(MF_STRING, 1, menuText);

		// 显示菜单
		CPoint pt = pNMListView->ptAction;
		m_thumbnailList.ClientToScreen(&pt);

		int cmd = menu.TrackPopupMenu(TPM_LEFTALIGN | TPM_RETURNCMD, pt.x, pt.y, this);

		// 处理菜单命令
		if (cmd == 1) {
			DeleteSelectedPages();
		}
		else if (cmd == 2) {
			RotateSelectedPages(90);
		}
		else if (cmd == 3) {
			RotateSelectedPages(180);
		}
		else if (cmd == 4) {
			RotateSelectedPages(270);
		}
		else if (cmd == 5) {
			SwapSelectedPages();
		}
		else if (cmd == 6) {
			ExtractSelectedPages();
		}

		// 确保子菜单不被销毁前保持有效
		rotateMenu.Detach();
	}
}

// 重置
void CPDFEditDialog::OnBnClickedBtnReset()
{
	if (MessageBox(_T("确定要重置所有修改吗？"), _T("确认"), MB_YESNO | MB_ICONQUESTION) != IDYES)
		return;

	// 清理当前m_pages中的HBITMAP
	for (auto& page : m_pages)
	{
		if (page.hThumbnail)
		{
			::DeleteObject(page.hThumbnail);
			page.hThumbnail = nullptr;
		}
	}

	// 恢复到原始状态 - 深拷贝HBITMAP
	m_pages.clear();
	m_pages.reserve(m_originalPages.size());
	for (const auto& page : m_originalPages)
	{
		PageInfo restoredPage = page;
		// 深拷贝HBITMAP
		if (page.hThumbnail)
		{
			restoredPage.hThumbnail = (HBITMAP)::CopyImage(page.hThumbnail, IMAGE_BITMAP, 0, 0, LR_CREATEDIBSECTION);
		}
		m_pages.push_back(restoredPage);
	}

	// 恢复原始总页数
	m_totalPages = m_originalTotalPages;

	// 刷新列表
	RefreshThumbnailList();
	RecordEditState();
}

// 保存
void CPDFEditDialog::OnBnClickedBtnSave()
{
	// 弹出文件保存对话框（默认定位到当前文件路径，支持直接覆盖保存或另存为）
	LPCTSTR defaultPath = m_filePath.IsEmpty() ? _T("edited.pdf") : static_cast<LPCTSTR>(m_filePath);
	CFileDialog saveDlg(FALSE, _T("pdf"), defaultPath,
		OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST,
		_T("PDF文件 (*.pdf)|*.pdf||"), this);

	if (saveDlg.DoModal() != IDOK)
		return;

	m_savedFilePath = saveDlg.GetPathName();

	// 保存文件
	if (SaveToNewFile(m_savedFilePath))
	{
		MessageBox(_T("保存成功！"), _T("提示"), MB_OK | MB_ICONINFORMATION);
		KillTimer(TIMER_VISIBLE_THUMBNAILS);
		EndDialog(IDOK);
	}
	else
	{
		m_savedFilePath.Empty();
	}
}

bool CPDFEditDialog::SaveToNewFile(const CString& newFilePath)
{
	if (!m_ctx)
		return false;

	pdf_write_options opts = pdf_default_write_options;
	opts.do_garbage = 1;

	bool tempReady = false;
	CString tempFilePath;
	pdf_document* newPdf = nullptr;
	pdf_obj* catalogMetadata = nullptr;
	fz_var(newPdf);
	fz_var(catalogMetadata);
	std::map<CString, fz_document*> openDocs;
	CString errorMsg;

	fz_try(m_ctx)
	{
		// Keep the source document's metadata and surviving outline destinations.
		std::vector<int> originalPages;
		for (const auto& page : m_pages)
		{
			if (!page.isDeleted && page.sourceFile.CompareNoCase(m_filePath) == 0)
				originalPages.push_back(page.sourcePageIndex);
		}
		fz_document* baseDoc = OpenAuthenticatedSource(m_filePath);
		if (!baseDoc)
			fz_throw(m_ctx, FZ_ERROR_GENERIC, "无法打开源PDF文档");
		newPdf = pdf_document_from_fz_document(m_ctx, baseDoc);
		if (!newPdf)
		{
			fz_drop_document(m_ctx, baseDoc);
			fz_throw(m_ctx, FZ_ERROR_GENERIC, "源文档不是有效的PDF格式");
		}
		pdf_obj* oldRoot = pdf_dict_get(m_ctx, pdf_trailer(m_ctx, newPdf), PDF_NAME(Root));
		catalogMetadata = pdf_keep_obj(m_ctx, pdf_dict_get(m_ctx, oldRoot, PDF_NAME(Metadata)));
		pdf_rearrange_pages(m_ctx, newPdf, static_cast<int>(originalPages.size()),
			originalPages.empty() ? nullptr : originalPages.data());
		if (catalogMetadata)
		{
			pdf_obj* newRoot = pdf_dict_get(m_ctx, pdf_trailer(m_ctx, newPdf), PDF_NAME(Root));
			pdf_dict_put(m_ctx, newRoot, PDF_NAME(Metadata), catalogMetadata);
		}

		// 复制未删除的页面（按当前顺序）
		int pageCount = 0;
		for (const auto& page : m_pages)
		{
			if (page.isDeleted)
				continue;

			const int insertIndex = pageCount;
			pageCount++;
			if (page.sourceFile.CompareNoCase(m_filePath) == 0)
				continue;

			if (IsImageFilePath(page.sourceFile))
			{
				fz_try(m_ctx)
				{
					InsertImagePageToPdf(m_ctx, newPdf, insertIndex, page.sourceFile);
				}
				fz_catch(m_ctx)
				{
					const char* err = fz_caught_message(m_ctx);
					CString errText = err ? CString(CA2W(err, CP_UTF8)) : _T("未知错误");
					CString msg;
					msg.Format(_T("  错误：无法处理图片页面 %s\n  MuPDF错误: %s\n"),
						static_cast<LPCTSTR>(page.sourceFile), static_cast<LPCTSTR>(errText));
					errorMsg += msg;
					fz_rethrow(m_ctx);
				}
			}
			else
			{
				fz_document* sourceDoc = nullptr;

				auto it = openDocs.find(page.sourceFile);
				if (it != openDocs.end()) {
					sourceDoc = it->second;
				}
				else {
					sourceDoc = OpenAuthenticatedSource(page.sourceFile);
					openDocs[page.sourceFile] = sourceDoc;
				}

				if (!sourceDoc)
					continue;

				pdf_document* sourcePdf = pdf_document_from_fz_document(m_ctx, sourceDoc);
				if (!sourcePdf)
					continue;

				pdf_graft_page(m_ctx, newPdf, insertIndex, sourcePdf, page.sourcePageIndex);
			}
		}
		int outputIndex = 0;
		for (const auto& page : m_pages)
		{
			if (page.isDeleted)
				continue;
			if (page.rotationAngle != 0)
			{
				pdf_obj* pageObj = pdf_lookup_page_obj(m_ctx, newPdf, outputIndex);
				int originalRotate = pdf_dict_get_int(m_ctx, pageObj, PDF_NAME(Rotate));
				pdf_dict_put_int(m_ctx, pageObj, PDF_NAME(Rotate),
					(originalRotate + page.rotationAngle) % 360);
			}
			++outputIndex;
		}

		// 先写入临时文件并校验页数；待 fz_always 关闭所有源文件句柄后再执行覆盖替换。
		tempReady = WriteAndValidateTempPdf(m_ctx, newPdf, newFilePath, pageCount, opts,
			tempFilePath, &m_password);
		if (!tempReady)
			errorMsg = _T("页面数量不符，或无法完成临时文件写入与验证。\n");
	}
	fz_always(m_ctx)
	{
		if (catalogMetadata)
			pdf_drop_obj(m_ctx, catalogMetadata);
		// 清理：关闭所有打开的源文档（包括当前被覆盖的源文档句柄）
		for (auto& pair : openDocs) {
			if (pair.second) {
				fz_drop_document(m_ctx, pair.second);
			}
		}
		openDocs.clear();

		// 清理新文档（同时释放 OpenAuthenticatedSource(m_filePath) 持有的文件流）
		if (newPdf) {
			pdf_drop_document(m_ctx, newPdf);
			newPdf = nullptr;
		}
	}
	fz_catch(m_ctx)
	{
		const char* errMsg = fz_caught_message(m_ctx);
		CString errText = errMsg ? CString(CA2W(errMsg, CP_UTF8)) : _T("未知错误");
		CString caughtMsg;
		caughtMsg.Format(_T("MuPDF错误: %s\n"), static_cast<LPCTSTR>(errText));
		OutputDebugString(caughtMsg);
		errorMsg += caughtMsg;

		tempReady = false;
	}

	bool success = false;
	if (tempReady)
	{
		success = FinalizeValidatedPdf(
			tempFilePath, newFilePath, m_onBeforeReplaceTarget, m_onAfterReplaceTarget);
		if (!success && errorMsg.IsEmpty())
			errorMsg = _T("目标文件正在被其他程序占用或无写入权限，无法覆盖保存。\n");
	}

	// 如果失败且有错误信息，显示给用户
	if (!success) {
		if (errorMsg.IsEmpty())
			errorMsg = _T("请检查源文件、页面数量及保存位置。\n");
		CString fullMsg = _T("保存失败！\n\n详细错误：\n") + errorMsg;
		MessageBox(fullMsg, _T("错误"), MB_OK | MB_ICONERROR);
	}

	return success;
}

// ========== 添加PDF功能 ==========

// 导入PDF文件
bool CPDFEditDialog::ImportPDF(const CString& pdfPath)
{
	if (!m_ctx)
		return false;

	fz_document* newDoc = nullptr;
	fz_var(newDoc);
	std::vector<char> password;
	volatile int pageCount = 0;
	bool promptForPassword = false;
	bool invalidPassword = false;
	for (;;)
	{
		if (promptForPassword)
		{
			CEditPasswordDialog dialog(pdfPath.Mid(pdfPath.ReverseFind(_T('\\')) + 1),
				invalidPassword, this);
			if (dialog.DoModal() != IDOK)
				return false;
			CW2A utf8Password(dialog.GetPassword(), CP_UTF8);
			const char* pUtf8Password = utf8Password;
			password.assign(pUtf8Password, pUtf8Password + std::strlen(pUtf8Password) + 1);
		}
		volatile bool failed = false;
		volatile bool needsPassword = false;
		CW2A utf8Path(pdfPath, CP_UTF8);
		fz_try(m_ctx)
		{
			newDoc = fz_open_document(m_ctx, utf8Path);
			if (fz_needs_password(m_ctx, newDoc))
			{
				needsPassword = password.empty() ||
					!fz_authenticate_password(m_ctx, newDoc, password.data());
			}
			if (!needsPassword)
				pageCount = fz_count_pages(m_ctx, newDoc);
		}
		fz_catch(m_ctx)
		{
			failed = true;
		}
		if (needsPassword || failed)
		{
			if (newDoc)
				fz_drop_document(m_ctx, newDoc);
			newDoc = nullptr;
		}
		if (failed)
		{
			if (!password.empty())
				SecureZeroMemory(password.data(), password.size());
			MessageBox(_T("无法打开PDF文件！"), _T("错误"), MB_OK | MB_ICONERROR);
			return false;
		}
		if (!needsPassword)
			break;
		invalidPassword = promptForPassword;
		promptForPassword = true;
		if (!password.empty())
			SecureZeroMemory(password.data(), password.size());
		password.clear();
	}

	int newPageCount = static_cast<int>(pageCount);
	if (newPageCount <= 0) {
		fz_drop_document(m_ctx, newDoc);
		if (!password.empty())
			SecureZeroMemory(password.data(), password.size());
		MessageBox(_T("PDF文件为空！"), _T("错误"), MB_OK | MB_ICONERROR);
		return false;
	}
	if (!password.empty())
	{
		auto existing = m_importPasswords.find(pdfPath);
		if (existing != m_importPasswords.end() && !existing->second.empty())
			SecureZeroMemory(existing->second.data(), existing->second.size());
		m_importPasswords[pdfPath] = password;
		SecureZeroMemory(password.data(), password.size());
	}

	// 计算起始索引（添加到现有页面后面）
	int startIndex = m_totalPages;

	// 导入所有页面
	for (int i = 0; i < newPageCount; i++) {
		PageInfo newPage;
		newPage.originalIndex = startIndex + i;
		newPage.isDeleted = false;
		newPage.displayIndex = static_cast<int>(m_pages.size());
		newPage.sourceFile = pdfPath;       // 设置源文件路径
		newPage.sourcePageIndex = i;        // 在源文件中的页码

		newPage.hThumbnail = nullptr;
		m_pages.push_back(newPage);
	}

	// 更新总页数
	m_totalPages += newPageCount;

	// 关闭新文档
	fz_drop_document(m_ctx, newDoc);

	// 刷新列表
	RefreshThumbnailList();

	// 更新信息栏
	UpdateInfoBar();
	RecordEditState();

	return true;
}

// ========== 导入文件功能 ==========

// 导入文件（根据扩展名路由到PDF或图片导入）
bool CPDFEditDialog::ImportFile(const CString& filePath)
{
	CString ext = filePath.Right(4);
	ext.MakeLower();

	if (ext == _T(".pdf")) {
		return ImportPDF(filePath);
	}
	if (IsImageFilePath(filePath)) {
		return ImportImage(filePath);
	}

	MessageBox(_T("不支持的文件格式！"), _T("错误"), MB_OK | MB_ICONERROR);
	return false;
}

// 导入图片文件
bool CPDFEditDialog::ImportImage(const CString& imagePath)
{
	if (!m_ctx) return false;

	fz_image* image = nullptr;
	fz_pixmap* pixmap = nullptr;
	fz_pixmap* tempPixmap = nullptr;
	HBITMAP hBitmap = nullptr;
	bool storedBitmap = false;
	fz_var(image);
	fz_var(pixmap);
	fz_var(tempPixmap);
	fz_var(hBitmap);
	fz_var(storedBitmap);

	fz_try(m_ctx) {
		// 加载图片
		CW2A imagePathA(imagePath, CP_UTF8);
		image = fz_new_image_from_file(m_ctx, imagePathA);
		if (!image || image->w <= 0 || image->h <= 0) {
			fz_throw(m_ctx, FZ_ERROR_GENERIC, "无法加载图片文件或图片尺寸无效");
		}

		// 尝试获取 pixmap，如果失败会抛出异常到 fz_catch
		tempPixmap = fz_get_pixmap_from_image(m_ctx, image, NULL, NULL, NULL, NULL);

		OutputDebugString(_T("  fz_get_pixmap_from_image 成功返回\n"));

		if (!tempPixmap) {
			OutputDebugString(_T("  错误：无法从图片创建pixmap（返回NULL）\n"));
			fz_throw(m_ctx, FZ_ERROR_GENERIC, "无法从图片创建pixmap");
		}

		// 输出 pixmap 信息
		CString pixmapInfo;
		pixmapInfo.Format(_T("  pixmap信息：w=%d, h=%d, n=%d, alpha=%d\n"),
			tempPixmap->w, tempPixmap->h, tempPixmap->n, tempPixmap->alpha);
		OutputDebugString(pixmapInfo);

		// 确保转换为RGB格式（如果原图有alpha通道或其他颜色空间）
		if (tempPixmap->alpha || tempPixmap->colorspace != fz_device_rgb(m_ctx)) {
			OutputDebugString(_T("  转换颜色空间为RGB...\n"));
			pixmap = fz_convert_pixmap(m_ctx, tempPixmap, fz_device_rgb(m_ctx), NULL, NULL, fz_default_color_params, 1);
			fz_drop_pixmap(m_ctx, tempPixmap);
			tempPixmap = nullptr;
		} else {
			pixmap = tempPixmap;
			tempPixmap = nullptr;
		}
		OutputDebugString(_T("  pixmap转换成功\n"));

		// 获取图片尺寸
		int imgWidth = fz_pixmap_width(m_ctx, pixmap);
		int imgHeight = fz_pixmap_height(m_ctx, pixmap);
		int stride = fz_pixmap_stride(m_ctx, pixmap);
		unsigned char* samples = fz_pixmap_samples(m_ctx, pixmap);

		// 计算缩放比例（目标宽度220像素）
		float targetWidth = 220.0f;
		float scale = targetWidth / imgWidth;
		int scaledHeight = static_cast<int>(imgHeight * scale);

		// 限制高度不超过180
		if (scaledHeight > 180) {
			scale = 180.0f / imgHeight;
			scaledHeight = 180;
		}

		int scaledWidth = static_cast<int>(imgWidth * scale);

		// 创建固定尺寸220×180的DIB
		const int FIXED_WIDTH = 220;
		const int FIXED_HEIGHT = 180;
		BITMAPINFO bmi = { 0 };
		bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
		bmi.bmiHeader.biWidth = FIXED_WIDTH;
		bmi.bmiHeader.biHeight = -FIXED_HEIGHT;
		bmi.bmiHeader.biPlanes = 1;
		bmi.bmiHeader.biBitCount = 24;
		bmi.bmiHeader.biCompression = BI_RGB;

		HDC hdc = ::GetDC(NULL);
		void* pBits = nullptr;
		hBitmap = CreateDIBSection(hdc, &bmi, DIB_RGB_COLORS, &pBits, nullptr, 0);
		::ReleaseDC(NULL, hdc);

		if (hBitmap && pBits) {
			unsigned char* dest = (unsigned char*)pBits;
			int destStride = ((FIXED_WIDTH * 3 + 3) & ~3);

			// 填充白色背景
			for (int y = 0; y < FIXED_HEIGHT; y++) {
				for (int x = 0; x < FIXED_WIDTH; x++) {
					dest[y * destStride + x * 3 + 0] = 255;  // B
					dest[y * destStride + x * 3 + 1] = 255;  // G
					dest[y * destStride + x * 3 + 2] = 255;  // R
				}
			}

			// 计算居中偏移
			int xOffset = (FIXED_WIDTH - scaledWidth) / 2;
			int yOffset = (FIXED_HEIGHT - scaledHeight) / 2;

			// 简单最近邻缩放并复制像素
			int n = fz_pixmap_components(m_ctx, pixmap);
			for (int y = 0; y < scaledHeight; y++) {
				for (int x = 0; x < scaledWidth; x++) {
					int srcX = static_cast<int>(x / scale);
					int srcY = static_cast<int>(y / scale);

					if (srcX >= imgWidth) srcX = imgWidth - 1;
					if (srcY >= imgHeight) srcY = imgHeight - 1;

					unsigned char* src = samples + srcY * stride + srcX * n;
					unsigned char* destPixel = dest + (y + yOffset) * destStride + (x + xOffset) * 3;

					if (n == 1) {
						// 灰度图
						destPixel[0] = src[0];
						destPixel[1] = src[0];
						destPixel[2] = src[0];
					}
					else if (n == 3) {
						// RGB
						destPixel[0] = src[2];  // B
						destPixel[1] = src[1];  // G
						destPixel[2] = src[0];  // R
					}
					else if (n == 4) {
						// RGBA或CMYK，取前3个通道
						destPixel[0] = src[2];  // B
						destPixel[1] = src[1];  // G
						destPixel[2] = src[0];  // R
					}
				}
			}
		}

		// 创建新的PageInfo
		PageInfo newPage;
		newPage.originalIndex = m_totalPages;
		newPage.isDeleted = false;
		newPage.displayIndex = static_cast<int>(m_pages.size());
		newPage.sourceFile = imagePath;
		newPage.sourcePageIndex = 0;  // 图片只有一页
		newPage.hThumbnail = hBitmap;

		m_pages.push_back(newPage);
		storedBitmap = true;
		m_totalPages++;

		// 清理
		fz_drop_pixmap(m_ctx, pixmap);
		pixmap = nullptr;
		fz_drop_image(m_ctx, image);
		image = nullptr;
	}
	fz_catch(m_ctx) {
		if (!storedBitmap && hBitmap) DeleteObject(hBitmap);
		if (tempPixmap) fz_drop_pixmap(m_ctx, tempPixmap);
		if (pixmap) fz_drop_pixmap(m_ctx, pixmap);
		if (image) fz_drop_image(m_ctx, image);

		const char* errMsg = fz_caught_message(m_ctx);
		CString errText = errMsg ? CString(CA2W(errMsg, CP_UTF8)) : _T("未知错误");
		int errCode = fz_caught(m_ctx);
		CString errorDetails;
		errorDetails.Format(_T("导入图片失败！\n\n文件路径：%s\n\nMuPDF错误代码：%d\nMuPDF错误：%s\n\n建议：\n1. 该PNG图片可能使用了特殊的编码格式\n2. 尝试用画图工具重新保存图片\n3. 或转换为JPG格式"),
			static_cast<LPCTSTR>(imagePath), errCode, static_cast<LPCTSTR>(errText));

		OutputDebugString(errorDetails);
		MessageBox(errorDetails, _T("错误"), MB_OK | MB_ICONERROR);
		return false;
	}

	// 刷新列表
	RefreshThumbnailList();
	UpdateInfoBar();
	RecordEditState();

	return true;
}

// 添加按钮点击
void CPDFEditDialog::OnBnClickedBtnAddPDF()
{
	// 弹出文件选择对话框，支持PDF和图片
	CFileDialog openDlg(TRUE, nullptr, nullptr,
		OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST,
		_T("所有支持的文件|*.pdf;*.jpg;*.jpeg;*.png;*.bmp|PDF文件 (*.pdf)|*.pdf|图片文件 (*.jpg;*.jpeg;*.png;*.bmp)|*.jpg;*.jpeg;*.png;*.bmp||"));

	if (openDlg.DoModal() != IDOK)
		return;

	CString filePath = openDlg.GetPathName();

	// 导入文件
	if (ImportFile(filePath)) {
		CString msg;
		msg.Format(_T("成功导入文件！\n当前总页数：%d"), m_totalPages);
		MessageBox(msg, _T("提示"), MB_OK | MB_ICONINFORMATION);
	}
}

class CEditedPreviewCanvas : public CWnd
{
public:
	void SetBitmap(HBITMAP hBitmap)
	{
		m_hBitmap = hBitmap;
		Invalidate(FALSE);
	}

private:
	afx_msg void OnPaint()
	{
		CPaintDC dc(this);
		CRect client;
		GetClientRect(&client);
		dc.FillSolidRect(&client, RGB(232, 235, 238));
		if (!m_hBitmap)
			return;
		BITMAP bitmap = { 0 };
		::GetObject(m_hBitmap, sizeof(bitmap), &bitmap);
		if (bitmap.bmWidth <= 0 || bitmap.bmHeight <= 0)
			return;
		float scale = min(static_cast<float>(client.Width() - 24) / bitmap.bmWidth,
			static_cast<float>(client.Height() - 24) / bitmap.bmHeight);
		if (scale <= 0)
			return;
		int width = static_cast<int>(bitmap.bmWidth * scale);
		int height = static_cast<int>(bitmap.bmHeight * scale);
		int x = (client.Width() - width) / 2;
		int y = (client.Height() - height) / 2;
		CDC sourceDC;
		sourceDC.CreateCompatibleDC(&dc);
		HBITMAP hOldBitmap = (HBITMAP)sourceDC.SelectObject(m_hBitmap);
		dc.SetStretchBltMode(HALFTONE);
		dc.StretchBlt(x, y, width, height, &sourceDC, 0, 0,
			bitmap.bmWidth, bitmap.bmHeight, SRCCOPY);
		sourceDC.SelectObject(hOldBitmap);
		dc.Draw3dRect(x - 1, y - 1, width + 2, height + 2,
			RGB(190, 195, 200), RGB(190, 195, 200));
	}

	DECLARE_MESSAGE_MAP()
	HBITMAP m_hBitmap = nullptr;
};

BEGIN_MESSAGE_MAP(CEditedPreviewCanvas, CWnd)
	ON_WM_PAINT()
END_MESSAGE_MAP()

class CEditedPDFPreviewDlg : public CDialogEx
{
public:
	CEditedPDFPreviewDlg(int pageCount, std::function<HBITMAP(int)> renderPage, CWnd* pParent)
		: CDialogEx(IDD_PROGRESS_DIALOG, pParent), m_pageCount(pageCount),
		m_renderPage(std::move(renderPage))
	{
	}

	~CEditedPDFPreviewDlg()
	{
		if (m_hBitmap)
			::DeleteObject(m_hBitmap);
	}

protected:
	BOOL OnInitDialog() override
	{
		CDialogEx::OnInitDialog();
		SetWindowText(_T("编辑结果预览"));
		ModifyStyle(0, WS_SYSMENU, SWP_FRAMECHANGED);
		for (int id : { IDC_PROGRESS_STATUS, IDC_PROGRESS_BAR, IDCANCEL })
		{
			CWnd* control = GetDlgItem(id);
			if (control && control->GetSafeHwnd())
				control->ShowWindow(SW_HIDE);
		}
		int width = min(850, GetSystemMetrics(SM_CXSCREEN) - 60);
		int height = min(700, GetSystemMetrics(SM_CYSCREEN) - 80);
		SetWindowPos(nullptr, 0, 0, width, height, SWP_NOMOVE | SWP_NOZORDER);
		CenterWindow();
		CRect client;
		GetClientRect(&client);
		CString canvasClass = AfxRegisterWndClass(CS_HREDRAW | CS_VREDRAW,
			::LoadCursor(nullptr, IDC_ARROW), (HBRUSH)(COLOR_WINDOW + 1));
		m_canvas.CreateEx(0, canvasClass, _T(""), WS_CHILD | WS_VISIBLE,
			CRect(12, 12, client.right - 12, client.bottom - 56), this, 2001);
		int center = client.Width() / 2;
		int footerTop = client.bottom - 40;
		m_btnPrev.Create(_T("上一页"), WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
			CRect(center - 145, footerTop, center - 75, footerTop + 26), this, 2002);
		m_pageLabel.Create(_T(""), WS_CHILD | WS_VISIBLE | SS_CENTER,
			CRect(center - 70, footerTop + 5, center + 70, footerTop + 23), this);
		m_btnNext.Create(_T("下一页"), WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
			CRect(center + 75, footerTop, center + 145, footerTop + 26), this, 2003);
		m_btnClose.Create(_T("关闭"), WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
			CRect(client.right - 82, footerTop, client.right - 12, footerTop + 26), this, 2004);
		CFont* font = GetFont();
		m_btnPrev.SetFont(font);
		m_btnNext.SetFont(font);
		m_btnClose.SetFont(font);
		m_pageLabel.SetFont(font);
		if (!ShowPage(0))
			EndDialog(IDCANCEL);
		return TRUE;
	}

	bool ShowPage(int index)
	{
		if (index < 0 || index >= m_pageCount)
			return false;
		HBITMAP hBitmap = m_renderPage(index);
		if (!hBitmap)
		{
			MessageBox(_T("无法渲染预览页面。"), _T("错误"), MB_OK | MB_ICONERROR);
			return false;
		}
		m_canvas.SetBitmap(hBitmap);
		if (m_hBitmap)
			::DeleteObject(m_hBitmap);
		m_hBitmap = hBitmap;
		m_currentPage = index;
		CString pageText;
		pageText.Format(_T("%d / %d"), index + 1, m_pageCount);
		m_pageLabel.SetWindowText(pageText);
		m_btnPrev.EnableWindow(index > 0);
		m_btnNext.EnableWindow(index + 1 < m_pageCount);
		return true;
	}

	void HandleWheel(short delta)
	{
		m_wheelRemainder += delta;
		while (m_wheelRemainder >= WHEEL_DELTA)
		{
			ShowPage(m_currentPage - 1);
			m_wheelRemainder -= WHEEL_DELTA;
		}
		while (m_wheelRemainder <= -WHEEL_DELTA)
		{
			ShowPage(m_currentPage + 1);
			m_wheelRemainder += WHEEL_DELTA;
		}
	}

	BOOL PreTranslateMessage(MSG* pMsg) override
	{
		if (pMsg->message == WM_MOUSEWHEEL)
		{
			HandleWheel(GET_WHEEL_DELTA_WPARAM(pMsg->wParam));
			return TRUE;
		}
		return CDialogEx::PreTranslateMessage(pMsg);
	}

	afx_msg BOOL OnMouseWheel(UINT, short zDelta, CPoint)
	{
		HandleWheel(zDelta);
		return TRUE;
	}

	afx_msg void OnPrev() { ShowPage(m_currentPage - 1); }
	afx_msg void OnNext() { ShowPage(m_currentPage + 1); }
	afx_msg void OnClosePreview() { EndDialog(IDCANCEL); }
	DECLARE_MESSAGE_MAP()

private:
	int m_pageCount;
	int m_currentPage = 0;
	int m_wheelRemainder = 0;
	HBITMAP m_hBitmap = nullptr;
	std::function<HBITMAP(int)> m_renderPage;
	CEditedPreviewCanvas m_canvas;
	CButton m_btnPrev;
	CButton m_btnNext;
	CButton m_btnClose;
	CStatic m_pageLabel;
};

BEGIN_MESSAGE_MAP(CEditedPDFPreviewDlg, CDialogEx)
	ON_WM_MOUSEWHEEL()
	ON_BN_CLICKED(2002, &CEditedPDFPreviewDlg::OnPrev)
	ON_BN_CLICKED(2003, &CEditedPDFPreviewDlg::OnNext)
	ON_BN_CLICKED(2004, &CEditedPDFPreviewDlg::OnClosePreview)
END_MESSAGE_MAP()

void CPDFEditDialog::OnBnClickedBtnPreview()
{
	std::vector<int> pageIndexes;
	for (size_t i = 0; i < m_pages.size(); ++i)
	{
		if (!m_pages[i].isDeleted)
			pageIndexes.push_back(static_cast<int>(i));
	}
	if (pageIndexes.empty())
	{
		MessageBox(_T("没有可预览的页面。"), _T("提示"), MB_OK | MB_ICONINFORMATION);
		return;
	}
	CEditedPDFPreviewDlg previewDlg(static_cast<int>(pageIndexes.size()),
		[this, pageIndexes](int index)
		{
			const PageInfo& page = m_pages[pageIndexes[index]];
			return RenderPreviewBitmap(page.sourceFile, page.sourcePageIndex, page.rotationAngle);
		}, this);
	previewDlg.DoModal();
}

void CPDFEditDialog::OnNMClick(NMHDR* pNMHDR, LRESULT* pResult)
{
	NM_LISTVIEW* pNMListView = (NM_LISTVIEW*)pNMHDR;
	*pResult = 0;
	int displayIndex = pNMListView->iItem;
	int action = HitPageAction(displayIndex, pNMListView->ptAction);
	switch (action)
	{
	case 0: RotatePage(displayIndex, -90); break;
	case 1: RotatePage(displayIndex, 90); break;
	}
}

// 列表选择变化
void CPDFEditDialog::OnItemChanged(NMHDR* pNMHDR, LRESULT* pResult)
{
	UpdateInfoBar();
	*pResult = 0;
}

// 保留列表的选中状态，但只绘制页面图，避免默认的选中边框和高亮。
void CPDFEditDialog::OnCustomDraw(NMHDR* pNMHDR, LRESULT* pResult)
{
	NMLVCUSTOMDRAW* pDraw = reinterpret_cast<NMLVCUSTOMDRAW*>(pNMHDR);
	if (pDraw->nmcd.dwDrawStage == CDDS_PREPAINT)
	{
		*pResult = CDRF_NOTIFYITEMDRAW;
		return;
	}
	if (pDraw->nmcd.dwDrawStage != CDDS_ITEMPREPAINT)
	{
		*pResult = CDRF_DODEFAULT;
		return;
	}

	int item = static_cast<int>(pDraw->nmcd.dwItemSpec);
	CRect bounds;
	CRect icon;
	if (item < 0 || item >= m_thumbnailList.GetItemCount() ||
		!m_thumbnailList.GetItemRect(item, &bounds, LVIR_BOUNDS) ||
		!m_thumbnailList.GetItemRect(item, &icon, LVIR_ICON))
	{
		*pResult = CDRF_SKIPDEFAULT;
		return;
	}

	CDC dc;
	dc.Attach(pDraw->nmcd.hdc);
	dc.FillSolidRect(&bounds, RGB(248, 248, 248));
	LVITEM state = { 0 };
	state.mask = LVIF_IMAGE;
	state.iItem = item;
	if (m_imageList.GetSafeHandle() && m_thumbnailList.GetItem(&state))
		m_imageList.Draw(&dc, state.iImage, icon.TopLeft(), ILD_NORMAL);
	dc.Detach();
	*pResult = CDRF_SKIPDEFAULT;
}

void CPDFEditDialog::OnOK()
{
	// 不做任何操作，由保存按钮触发
}

void CPDFEditDialog::OnCancel()
{
	KillTimer(TIMER_VISIBLE_THUMBNAILS);
	CDialogEx::OnCancel();
}

void CPDFEditDialog::CleanupThumbnails()
{
	// 清理所有位图
	for (auto& page : m_pages)
	{
		if (page.hThumbnail)
		{
			::DeleteObject(page.hThumbnail);
			page.hThumbnail = nullptr;
		}
	}

	// 清理原始页面的位图
	for (auto& page : m_originalPages)
	{
		if (page.hThumbnail)
		{
			::DeleteObject(page.hThumbnail);
			page.hThumbnail = nullptr;
		}
	}

	// 清理ImageList
	if (m_imageList.GetSafeHandle())
	{
		m_imageList.DeleteImageList();
	}
}
