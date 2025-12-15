// PDFEditDialog.cpp: PDF页面编辑对话框实现文件

#include "pch.h"
#include "PDFEditDialog.h"
#include "resource.h"
#include <mupdf/pdf.h>
#include <algorithm>
#include <cmath>
#include <map>

#ifdef _DEBUG
#define new DEBUG_NEW
#endif

IMPLEMENT_DYNAMIC(CPDFEditDialog, CDialogEx)

BEGIN_MESSAGE_MAP(CPDFEditDialog, CDialogEx)
	ON_BN_CLICKED(IDC_BTN_EDIT_SAVE, &CPDFEditDialog::OnBnClickedBtnSave)
	ON_BN_CLICKED(IDC_BTN_EDIT_RESET, &CPDFEditDialog::OnBnClickedBtnReset)
	ON_BN_CLICKED(IDC_BTN_EDIT_ADD_PDF, &CPDFEditDialog::OnBnClickedBtnAddPDF)
	ON_NOTIFY(LVN_BEGINDRAG, IDC_EDIT_THUMBNAIL_LIST, &CPDFEditDialog::OnBeginDrag)
	ON_NOTIFY(NM_RCLICK, IDC_EDIT_THUMBNAIL_LIST, &CPDFEditDialog::OnNMRClick)
	ON_NOTIFY(NM_DBLCLK, IDC_EDIT_THUMBNAIL_LIST, &CPDFEditDialog::OnNMDblclk)
	ON_NOTIFY(LVN_ITEMCHANGED, IDC_EDIT_THUMBNAIL_LIST, &CPDFEditDialog::OnItemChanged)
	ON_WM_MOUSEMOVE()
	ON_WM_LBUTTONUP()
	ON_WM_LBUTTONDOWN()
	ON_WM_TIMER()
END_MESSAGE_MAP()

CPDFEditDialog::CPDFEditDialog(fz_context* ctx, fz_document* doc, const CString& filePath, CWnd* pParent)
	: CDialogEx(IDD_PDF_EDIT_DIALOG, pParent)
	, m_ctx(ctx)
	, m_doc(doc)
	, m_filePath(filePath)
	, m_totalPages(0)
	, m_isDragging(false)
	, m_potentialDrag(false)
	, m_dragIndex(-1)
	, m_dropIndex(-1)
	, m_lastDropIndex(-1)
	, m_pDragImage(nullptr)
	, m_autoScrollSpeed(0)
{
	if (m_doc)
	{
		m_totalPages = fz_count_pages(m_ctx, m_doc);
	}
}

CPDFEditDialog::~CPDFEditDialog()
{
	CleanupThumbnails();
}

void CPDFEditDialog::DoDataExchange(CDataExchange* pDX)
{
	CDialogEx::DoDataExchange(pDX);
	DDX_Control(pDX, IDC_EDIT_THUMBNAIL_LIST, m_thumbnailList);
	DDX_Control(pDX, IDC_BTN_EDIT_SAVE, m_btnSave);
	DDX_Control(pDX, IDC_BTN_EDIT_RESET, m_btnReset);
	DDX_Control(pDX, IDC_BTN_EDIT_ADD_PDF, m_btnAddPDF);
	DDX_Control(pDX, IDC_STATIC_EDIT_INFO, m_staticInfo);
}

BOOL CPDFEditDialog::OnInitDialog()
{
	CDialogEx::OnInitDialog();

	// 设置对话框标题
	CString title;
	title.Format(_T("编辑PDF - %s"), m_filePath);
	SetWindowText(title);

	// 初始化缩略图列表
	InitializeThumbnailList();

	// 加载所有页面
	LoadAllThumbnails();

	// 更新信息栏
	UpdateInfoBar();

	return TRUE;
}

void CPDFEditDialog::InitializeThumbnailList()
{
	// 设置列表控件样式为大图标
	m_thumbnailList.ModifyStyle(0, LVS_ICON | LVS_SHOWSELALWAYS | LVS_SINGLESEL);

	// 创建ImageList（220宽，根据常见PDF比例设置合适高度）
	// 使用较大的高度（180）以适应大多数PDF页面比例，避免黑色填充
	m_imageList.Create(220, 180, ILC_COLOR24, m_totalPages, 10);
	m_thumbnailList.SetImageList(&m_imageList, LVSIL_NORMAL);

	// 设置列表背景色为白色，减少黑色区域的视觉冲击
	m_thumbnailList.SetBkColor(RGB(255, 255, 255));
	m_thumbnailList.SetTextBkColor(RGB(255, 255, 255));
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

		// 渲染缩略图
		page.hThumbnail = RenderThumbnail(i);

		m_pages.push_back(page);

		// 添加到ImageList
		if (page.hThumbnail)
		{
			CBitmap bmp;
			bmp.Attach(page.hThumbnail);
			m_imageList.Add(&bmp, (CBitmap*)nullptr);
			bmp.Detach();  // 不要删除，由m_pages管理
		}

		// 添加到列表控件
		CString pageText;
		pageText.Format(_T("第 %d 页"), i + 1);
		m_thumbnailList.InsertItem(i, pageText, i);
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
}

HBITMAP CPDFEditDialog::RenderThumbnail(int pageNumber)
{
	if (!m_ctx || !m_doc || pageNumber < 0 || pageNumber >= m_totalPages)
		return nullptr;

	fz_page* page = nullptr;
	fz_pixmap* pixmap = nullptr;
	HBITMAP hBitmap = nullptr;

	fz_try(m_ctx)
	{
		// 加载页面
		page = fz_load_page(m_ctx, m_doc, pageNumber);

		// 获取页面边界
		fz_rect bounds = fz_bound_page(m_ctx, page);

		// 计算缩放比例（目标宽度220像素）
		float targetWidth = 220.0f;
		float scale = targetWidth / (bounds.x1 - bounds.x0);

		// 创建变换矩阵
		fz_matrix ctm = fz_scale(scale, scale);

		// 渲染为pixmap（不使用alpha通道）
		pixmap = fz_new_pixmap_from_page(m_ctx, page, ctm, fz_device_rgb(m_ctx), 0);

		// 转换为HBITMAP
		int width = fz_pixmap_width(m_ctx, pixmap);
		int height = fz_pixmap_height(m_ctx, pixmap);
		int stride = fz_pixmap_stride(m_ctx, pixmap);
		unsigned char* samples = fz_pixmap_samples(m_ctx, pixmap);

		// ★★★ 创建固定尺寸220×180的DIB，用于垂直居中显示PDF内容
		const int FIXED_WIDTH = 220;
		const int FIXED_HEIGHT = 180;
		BITMAPINFO bmi = { 0 };
		bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
		bmi.bmiHeader.biWidth = FIXED_WIDTH;
		bmi.bmiHeader.biHeight = -FIXED_HEIGHT;  // 负值表示自顶向下
		bmi.bmiHeader.biPlanes = 1;
		bmi.bmiHeader.biBitCount = 24;
		bmi.bmiHeader.biCompression = BI_RGB;

		HDC hdc = GetDC()->GetSafeHdc();
		void* pBits = nullptr;
		hBitmap = CreateDIBSection(hdc, &bmi, DIB_RGB_COLORS, &pBits, nullptr, 0);

		if (hBitmap && pBits)
		{
			unsigned char* dest = (unsigned char*)pBits;
			int destStride = ((FIXED_WIDTH * 3 + 3) & ~3);

			// ★★★ 第1步：先用白色填充整个220×180区域
			for (int y = 0; y < FIXED_HEIGHT; y++)
			{
				for (int x = 0; x < FIXED_WIDTH; x++)
				{
					dest[y * destStride + x * 3 + 0] = 255;  // B
					dest[y * destStride + x * 3 + 1] = 255;  // G
					dest[y * destStride + x * 3 + 2] = 255;  // R
				}
			}

			// ★★★ 第2步：计算垂直居中偏移量
			int yOffset = (FIXED_HEIGHT - height) / 2;
			if (yOffset < 0) yOffset = 0;  // 如果PDF内容太高，从顶部开始

			// ★★★ 第3步：复制PDF像素数据到居中位置（RGB -> BGR转换）
			for (int y = 0; y < height && (y + yOffset) < FIXED_HEIGHT; y++)
			{
				unsigned char* src = samples + y * stride;
				unsigned char* destRow = dest + (y + yOffset) * destStride;

				int copyWidth = (width < FIXED_WIDTH) ? width : FIXED_WIDTH;
				for (int x = 0; x < copyWidth; x++)
				{
					destRow[x * 3 + 0] = src[x * 3 + 2];  // B
					destRow[x * 3 + 1] = src[x * 3 + 1];  // G
					destRow[x * 3 + 2] = src[x * 3 + 0];  // R
				}
			}
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

void CPDFEditDialog::RefreshThumbnailList()
{
	// 清空列表
	m_thumbnailList.DeleteAllItems();
	m_imageList.DeleteImageList();
	m_imageList.Create(220, 180, ILC_COLOR24, m_totalPages, 10);
	m_thumbnailList.SetImageList(&m_imageList, LVSIL_NORMAL);

	// 重新添加未删除的页面
	int displayIndex = 0;
	for (size_t i = 0; i < m_pages.size(); i++)
	{
		if (m_pages[i].isDeleted)
			continue;

		m_pages[i].displayIndex = displayIndex;

		// 添加到ImageList
		if (m_pages[i].hThumbnail)
		{
			CBitmap bmp;
			bmp.Attach(m_pages[i].hThumbnail);
			m_imageList.Add(&bmp, (CBitmap*)nullptr);
			bmp.Detach();
		}

		// 添加到列表控件
		CString pageText;
		pageText.Format(_T("第 %d 页"), m_pages[i].originalIndex + 1);
		m_thumbnailList.InsertItem(displayIndex, pageText, displayIndex);

		displayIndex++;
	}

	UpdateInfoBar();
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
		// 获取项的位置
		CRect itemRect;
		m_thumbnailList.GetItemRect(hitIndex, &itemRect, LVIR_BOUNDS);

		// 判断鼠标在项的哪一侧
		if (point.x > itemRect.CenterPoint().x) {
			return hitIndex + 1;  // 插入到右侧
		}
		else {
			return hitIndex;  // 插入到左侧
		}
	}

	// 默认插入到末尾
	return m_thumbnailList.GetItemCount();
}

// 更新插入索引并触发动画
void CPDFEditDialog::UpdateInsertIndex(CPoint point)
{
	int newIndex = CalculateInsertIndex(point);

	// 限制索引范围
	int activeCount = 0;
	for (const auto& page : m_pages) {
		if (!page.isDeleted) activeCount++;
	}
	newIndex = max(0, min(newIndex, activeCount));

	// 只在索引变化时触发重排
	if (newIndex != m_lastDropIndex && newIndex != m_dragIndex) {
		m_dropIndex = newIndex;
		m_lastDropIndex = newIndex;

		// 找到实际的页面索引
		int actualDragIndex = -1;
		int actualDropIndex = -1;
		int displayIndex = 0;

		for (size_t i = 0; i < m_pages.size(); i++) {
			if (m_pages[i].isDeleted) continue;

			if (displayIndex == m_dragIndex)
				actualDragIndex = static_cast<int>(i);
			if (displayIndex == m_dropIndex)
				actualDropIndex = static_cast<int>(i);

			displayIndex++;
		}

		if (actualDragIndex >= 0 && actualDropIndex >= 0) {
			// 执行插入式重排
			PageInfo draggedPage = m_pages[actualDragIndex];
			m_pages.erase(m_pages.begin() + actualDragIndex);

			int insertPos = actualDropIndex;
			if (actualDragIndex < actualDropIndex) {
				insertPos--;
			}

			m_pages.insert(m_pages.begin() + insertPos, draggedPage);

			// 刷新显示
			RefreshThumbnailList();

			// 重新选中被拖动的项
			m_thumbnailList.SetItemState(m_dropIndex, LVIS_SELECTED, LVIS_SELECTED);
			m_dragIndex = m_dropIndex;
		}
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

// 开始拖拽操作
void CPDFEditDialog::BeginDragOperation()
{
	m_isDragging = true;
	m_potentialDrag = false;
	m_lastDropIndex = m_dragIndex;

	// 创建拖拽图像
	CPoint pt(0, 0);
	m_pDragImage = m_thumbnailList.CreateDragImage(m_dragIndex, &pt);
	if (!m_pDragImage)
		return;

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

	// 重置状态
	m_dragIndex = -1;
	m_dropIndex = -1;
	m_lastDropIndex = -1;
	m_potentialDrag = false;

	// 更新信息栏
	UpdateInfoBar();
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
	}

	CDialogEx::OnTimer(nIDEvent);
}

// 拖拽开始（ListView通知）
void CPDFEditDialog::OnBeginDrag(NMHDR* pNMHDR, LRESULT* pResult)
{
	NM_LISTVIEW* pNMListView = (NM_LISTVIEW*)pNMHDR;
	*pResult = 0;

	m_dragIndex = pNMListView->iItem;
	if (m_dragIndex < 0)
		return;

	// 初始化拖拽状态
	m_lastDropIndex = m_dragIndex;

	// 创建拖拽图像
	CPoint pt(0, 0);
	m_pDragImage = m_thumbnailList.CreateDragImage(m_dragIndex, &pt);
	if (!m_pDragImage)
		return;

	m_isDragging = true;

	// 开始拖拽，设置半透明效果
	m_pDragImage->BeginDrag(0, CPoint(0, 0));
	m_pDragImage->DragEnter(GetDesktopWindow(), pNMListView->ptAction);

	// 设置拖拽图像为半透明（使用DragShowNolock）
	m_pDragImage->DragShowNolock(TRUE);

	// 捕获鼠标
	SetCapture();

	// 设置拖拽光标（显示移动效果）
	SetCursor(LoadCursor(nullptr, IDC_SIZEALL));
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

// 删除指定页面
void CPDFEditDialog::DeletePage(int displayIndex)
{
	// 计算当前未删除的页面数
	int activePages = 0;
	for (const auto& page : m_pages) {
		if (!page.isDeleted)
			activePages++;
	}

	// 至少保留一页
	if (activePages <= 1) {
		MessageBox(_T("至少需要保留一页，无法删除！"), _T("提示"), MB_OK | MB_ICONWARNING);
		return;
	}

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

	if (actualIndex < 0 || actualIndex >= (int)m_pages.size())
		return;

	// 标记为删除
	m_pages[actualIndex].isDeleted = true;

	// 刷新列表
	RefreshThumbnailList();

	// 更新信息栏
	UpdateInfoBar();
}

// 右键菜单
void CPDFEditDialog::OnNMRClick(NMHDR* pNMHDR, LRESULT* pResult)
{
	NM_LISTVIEW* pNMListView = (NM_LISTVIEW*)pNMHDR;
	*pResult = 0;

	if (pNMListView->iItem >= 0) {
		// 选中该项
		m_thumbnailList.SetItemState(pNMListView->iItem, LVIS_SELECTED, LVIS_SELECTED);

		// 创建右键菜单
		CMenu menu;
		menu.CreatePopupMenu();
		menu.AppendMenu(MF_STRING, 1, _T("删除此页"));

		// 显示菜单
		CPoint pt = pNMListView->ptAction;
		m_thumbnailList.ClientToScreen(&pt);

		int cmd = menu.TrackPopupMenu(TPM_LEFTALIGN | TPM_RETURNCMD, pt.x, pt.y, this);

		// 处理菜单命令
		if (cmd == 1) {
			DeletePage(pNMListView->iItem);
		}
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

	// 刷新列表
	RefreshThumbnailList();
}

// 保存
void CPDFEditDialog::OnBnClickedBtnSave()
{
	// 弹出文件保存对话框
	CFileDialog saveDlg(FALSE, _T("pdf"), _T("edited.pdf"),
		OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST,
		_T("PDF文件 (*.pdf)|*.pdf||"));

	if (saveDlg.DoModal() != IDOK)
		return;

	m_savedFilePath = saveDlg.GetPathName();

	// 保存文件
	if (SaveToNewFile(m_savedFilePath))
	{
		MessageBox(_T("保存成功！"), _T("提示"), MB_OK | MB_ICONINFORMATION);
		EndDialog(IDOK);
	}
	else
	{
		MessageBox(_T("保存失败！请检查文件路径和权限。"), _T("错误"), MB_OK | MB_ICONERROR);
		m_savedFilePath.Empty();
	}
}

bool CPDFEditDialog::SaveToNewFile(const CString& newFilePath)
{
	if (!m_ctx)
		return false;

	pdf_write_options opts = pdf_default_write_options;
	opts.do_garbage = 1;

	bool success = false;
	pdf_document* newPdf = nullptr;
	std::map<CString, fz_document*> openDocs;
	CString errorMsg;

	fz_try(m_ctx)
	{
		// 创建新的PDF文档
		newPdf = pdf_create_document(m_ctx);

		// 复制未删除的页面（按当前顺序）
		int pageCount = 0;
		for (const auto& page : m_pages)
		{
			if (page.isDeleted)
				continue;

			pageCount++;
			CString debugMsg;
			debugMsg.Format(_T("正在处理第 %d 页：%s\n"), pageCount, page.sourceFile);
			OutputDebugString(debugMsg);

			// 判断源文件类型
			CString ext = page.sourceFile.Right(4);
			ext.MakeLower();
			CString ext5 = page.sourceFile.Right(5);
			ext5.MakeLower();

			bool isImage = (ext == _T(".jpg") || ext == _T(".png") || ext == _T(".bmp") || ext5 == _T(".jpeg"));

			if (isImage) {
				// 处理图片文件
				CW2A imagePathA(page.sourceFile, CP_UTF8);
				fz_image* image = nullptr;
				fz_buffer* contents_buf = nullptr;

				fz_try(m_ctx) {
					OutputDebugString(_T("  加载图片...\n"));
					image = fz_new_image_from_file(m_ctx, imagePathA);

					// 获取图片尺寸
					int imgWidth = image->w;
					int imgHeight = image->h;

					CString sizeMsg;
					sizeMsg.Format(_T("  图片尺寸：%d x %d\n"), imgWidth, imgHeight);
					OutputDebugString(sizeMsg);

					// 创建合适的页面尺寸（基于图片比例）
					fz_rect mediabox;
					if (imgWidth > imgHeight) {
						mediabox = fz_make_rect(0, 0, 842, 595);  // A4横向
					}
					else {
						mediabox = fz_make_rect(0, 0, 595, 842);  // A4纵向
					}

					OutputDebugString(_T("  创建PDF页面...\n"));
					// 创建页面对象
					pdf_obj* page_obj = pdf_add_page(m_ctx, newPdf, mediabox, 0, nullptr, nullptr);

					// 将页面插入到文档中（在末尾）
					pdf_insert_page(m_ctx, newPdf, -1, page_obj);

					// 计算缩放和位置（让图片在页面上居中显示）
					float pageWidth = mediabox.x1 - mediabox.x0;
					float pageHeight = mediabox.y1 - mediabox.y0;
					float scale = fmin(pageWidth / imgWidth, pageHeight / imgHeight);
					float w = imgWidth * scale;
					float h = imgHeight * scale;
					float x = (pageWidth - w) / 2;
					float y = (pageHeight - h) / 2;

					OutputDebugString(_T("  添加图片资源...\n"));
					// 添加图片到PDF文档
					pdf_obj* image_obj = pdf_add_image(m_ctx, newPdf, image);

					// 获取或创建页面的Resources字典
					pdf_obj* resources = pdf_dict_get(m_ctx, page_obj, PDF_NAME(Resources));
					if (!resources) {
						resources = pdf_new_dict(m_ctx, newPdf, 1);
						pdf_dict_put(m_ctx, page_obj, PDF_NAME(Resources), resources);
						pdf_drop_obj(m_ctx, resources);  // pdf_dict_put增加了引用计数
						resources = pdf_dict_get(m_ctx, page_obj, PDF_NAME(Resources));
					}

					// 获取或创建XObject字典
					pdf_obj* xobj_dict = pdf_dict_get(m_ctx, resources, PDF_NAME(XObject));
					if (!xobj_dict) {
						xobj_dict = pdf_new_dict(m_ctx, newPdf, 1);
						pdf_dict_put(m_ctx, resources, PDF_NAME(XObject), xobj_dict);
						pdf_drop_obj(m_ctx, xobj_dict);
						xobj_dict = pdf_dict_get(m_ctx, resources, PDF_NAME(XObject));
					}

					// 将图片添加到XObject字典中
					pdf_dict_puts(m_ctx, xobj_dict, "Im0", image_obj);
					pdf_drop_obj(m_ctx, image_obj);

					OutputDebugString(_T("  生成内容流...\n"));
					// 创建内容流（绘制图片）
					contents_buf = fz_new_buffer(m_ctx, 256);
					fz_append_printf(m_ctx, contents_buf, "q\n");
					fz_append_printf(m_ctx, contents_buf, "%g 0 0 %g %g %g cm\n", w, h, x, y);
					fz_append_printf(m_ctx, contents_buf, "/Im0 Do\n");
					fz_append_printf(m_ctx, contents_buf, "Q\n");

					// 将内容流添加到页面
					pdf_obj* contents = pdf_add_stream(m_ctx, newPdf, contents_buf, nullptr, 0);
					pdf_dict_put(m_ctx, page_obj, PDF_NAME(Contents), contents);
					pdf_drop_obj(m_ctx, contents);

					OutputDebugString(_T("  图片页面处理完成\n"));
				}
				fz_always(m_ctx) {
					// 清理资源
					if (contents_buf) {
						fz_drop_buffer(m_ctx, contents_buf);
						contents_buf = nullptr;
					}
					if (image) {
						fz_drop_image(m_ctx, image);
						image = nullptr;
					}
				}
				fz_catch(m_ctx) {
					// 图片处理失败
					const char* err = fz_caught_message(m_ctx);
					CString msg;
					msg.Format(_T("  错误：无法处理图片页面 %s\n  MuPDF错误: %S\n"), page.sourceFile, err);
					OutputDebugString(msg);
					errorMsg += msg;
					// 图片失败则抛出异常，不继续
					fz_rethrow(m_ctx);
				}
			}
			else {
				// 处理PDF文件
				OutputDebugString(_T("  处理PDF页面...\n"));
				fz_document* sourceDoc = nullptr;

				// 检查是否已经打开过这个文档
				auto it = openDocs.find(page.sourceFile);
				if (it != openDocs.end()) {
					sourceDoc = it->second;
				}
				else {
					// 打开源文档
					CW2A sourcePathA(page.sourceFile, CP_UTF8);
					sourceDoc = fz_open_document(m_ctx, sourcePathA);
					openDocs[page.sourceFile] = sourceDoc;
				}

				if (!sourceDoc)
					continue;

				// 转换为pdf_document
				pdf_document* sourcePdf = pdf_document_from_fz_document(m_ctx, sourceDoc);
				if (!sourcePdf)
					continue;

				// 从源文档复制页面到新文档
				pdf_graft_page(m_ctx, newPdf, -1, sourcePdf, page.sourcePageIndex);
				OutputDebugString(_T("  PDF页面处理完成\n"));
			}
		}

		// 验证页面数
		int finalPageCount = pdf_count_pages(m_ctx, newPdf);
		CString countMsg;
		countMsg.Format(_T("新PDF总页数：%d（预期：%d）\n"), finalPageCount, pageCount);
		OutputDebugString(countMsg);

		// 保存到文件
		OutputDebugString(_T("正在保存PDF文件...\n"));
		CW2A filePathA(newFilePath, CP_UTF8);
		pdf_save_document(m_ctx, newPdf, filePathA, &opts);
		OutputDebugString(_T("保存成功！\n"));

		success = true;
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
		// 捕获错误信息
		const char* errMsg = fz_caught_message(m_ctx);
		CString caughtMsg;
		caughtMsg.Format(_T("MuPDF错误: %S\n"), errMsg);
		OutputDebugString(caughtMsg);
		errorMsg += caughtMsg;

		success = false;
	}

	// 如果失败且有错误信息，显示给用户
	if (!success && !errorMsg.IsEmpty()) {
		CString fullMsg = _T("保存失败！\n\n详细错误：\n") + errorMsg;
		MessageBox(fullMsg, _T("错误"), MB_OK | MB_ICONERROR);
	}

	return success;
}

// ========== 添加PDF功能 ==========

// 导入PDF文件
bool CPDFEditDialog::ImportPDF(const CString& pdfPath)
{
	if (!m_ctx) return false;

	// 打开新的PDF文件
	CW2A pdfPathA(pdfPath, CP_UTF8);
	fz_document* newDoc = nullptr;

	fz_try(m_ctx) {
		newDoc = fz_open_document(m_ctx, pdfPathA);
	}
	fz_catch(m_ctx) {
		MessageBox(_T("无法打开PDF文件！"), _T("错误"), MB_OK | MB_ICONERROR);
		return false;
	}

	if (!newDoc) return false;

	// 获取新PDF的页数
	int newPageCount = fz_count_pages(m_ctx, newDoc);
	if (newPageCount <= 0) {
		fz_drop_document(m_ctx, newDoc);
		MessageBox(_T("PDF文件为空！"), _T("错误"), MB_OK | MB_ICONERROR);
		return false;
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

		// 渲染新页面的缩略图
		fz_page* page = nullptr;
		fz_pixmap* pixmap = nullptr;
		HBITMAP hBitmap = nullptr;

		fz_try(m_ctx) {
			// 加载页面
			page = fz_load_page(m_ctx, newDoc, i);

			// 获取页面边界
			fz_rect bounds = fz_bound_page(m_ctx, page);

			// 计算缩放比例
			float targetWidth = 220.0f;
			float scale = targetWidth / (bounds.x1 - bounds.x0);
			fz_matrix ctm = fz_scale(scale, scale);

			// 渲染为pixmap
			pixmap = fz_new_pixmap_from_page(m_ctx, page, ctm, fz_device_rgb(m_ctx), 0);

			// 转换为HBITMAP
			int width = fz_pixmap_width(m_ctx, pixmap);
			int height = fz_pixmap_height(m_ctx, pixmap);
			int stride = fz_pixmap_stride(m_ctx, pixmap);
			unsigned char* samples = fz_pixmap_samples(m_ctx, pixmap);

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
						dest[y * destStride + x * 3 + 0] = 255;
						dest[y * destStride + x * 3 + 1] = 255;
						dest[y * destStride + x * 3 + 2] = 255;
					}
				}

				// 垂直居中
				int yOffset = (FIXED_HEIGHT - height) / 2;
				if (yOffset < 0) yOffset = 0;

				// 复制像素
				for (int y = 0; y < height && (y + yOffset) < FIXED_HEIGHT; y++) {
					unsigned char* src = samples + y * stride;
					unsigned char* destRow = dest + (y + yOffset) * destStride;

					int copyWidth = (width < FIXED_WIDTH) ? width : FIXED_WIDTH;
					for (int x = 0; x < copyWidth; x++) {
						destRow[x * 3 + 0] = src[x * 3 + 2];  // B
						destRow[x * 3 + 1] = src[x * 3 + 1];  // G
						destRow[x * 3 + 2] = src[x * 3 + 0];  // R
					}
				}
			}

			fz_drop_pixmap(m_ctx, pixmap);
			fz_drop_page(m_ctx, page);
		}
		fz_catch(m_ctx) {
			if (pixmap) fz_drop_pixmap(m_ctx, pixmap);
			if (page) fz_drop_page(m_ctx, page);
			hBitmap = nullptr;
		}

		newPage.hThumbnail = hBitmap;
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

	return true;
}

// ========== 导入文件功能 ==========

// 导入文件（根据扩展名路由到PDF或图片导入）
bool CPDFEditDialog::ImportFile(const CString& filePath)
{
	// 获取文件扩展名
	CString ext = filePath.Right(4);
	ext.MakeLower();

	// 判断是PDF还是图片
	if (ext == _T(".pdf")) {
		return ImportPDF(filePath);
	}
	else if (ext == _T(".jpg") || ext == _T("jpeg") || ext == _T(".png") || ext == _T(".bmp")) {
		return ImportImage(filePath);
	}
	else {
		// 对于.jpeg，需要特殊处理（5个字符）
		CString ext5 = filePath.Right(5);
		ext5.MakeLower();
		if (ext5 == _T(".jpeg")) {
			return ImportImage(filePath);
		}

		MessageBox(_T("不支持的文件格式！"), _T("错误"), MB_OK | MB_ICONERROR);
		return false;
	}
}

// 导入图片文件
bool CPDFEditDialog::ImportImage(const CString& imagePath)
{
	if (!m_ctx) return false;

	fz_image* image = nullptr;
	fz_pixmap* pixmap = nullptr;
	fz_pixmap* tempPixmap = nullptr;
	HBITMAP hBitmap = nullptr;

	fz_try(m_ctx) {
		// 输出调试信息
		CString debugMsg;
		debugMsg.Format(_T("正在加载图片：%s\n"), imagePath);
		OutputDebugString(debugMsg);

		// 加载图片
		CW2A imagePathA(imagePath, CP_UTF8);
		OutputDebugString(_T("  路径转换为UTF-8...\n"));
		image = fz_new_image_from_file(m_ctx, imagePathA);
		OutputDebugString(_T("  图片加载成功\n"));

		if (!image) {
			MessageBox(_T("无法加载图片文件！"), _T("错误"), MB_OK | MB_ICONERROR);
			return false;
		}

		// 输出图片详细信息
		CString imgInfo;
		imgInfo.Format(_T("  图片信息：宽度=%d, 高度=%d, bpc=%d, n=%d, imagemask=%d\n"),
			image->w, image->h, image->bpc, image->n, image->imagemask);
		OutputDebugString(imgInfo);

		// 检查图片的颜色空间
		if (image->colorspace) {
			const char* csName = fz_colorspace_name(m_ctx, image->colorspace);
			CString csInfo;
			csInfo.Format(_T("  颜色空间：%S, 组件数=%d\n"), csName, fz_colorspace_n(m_ctx, image->colorspace));
			OutputDebugString(csInfo);
		} else {
			OutputDebugString(_T("  颜色空间：无\n"));
		}

		// 转换为pixmap - 使用RGB颜色空间，无alpha通道
		OutputDebugString(_T("  转换为pixmap...\n"));

		// 先获取原始pixmap
		OutputDebugString(_T("  调用 fz_get_pixmap_from_image...\n"));

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
		m_totalPages++;

		// 清理
		fz_drop_pixmap(m_ctx, pixmap);
		fz_drop_image(m_ctx, image);
	}
	fz_catch(m_ctx) {
		if (tempPixmap) fz_drop_pixmap(m_ctx, tempPixmap);
		if (pixmap) fz_drop_pixmap(m_ctx, pixmap);
		if (image) {
			// 输出图片信息以便调试
			CString debugInfo;
			debugInfo.Format(_T("  失败时图片信息：w=%d, h=%d, bpc=%d, n=%d\n"),
				image->w, image->h, image->bpc, image->n);
			OutputDebugString(debugInfo);

			fz_drop_image(m_ctx, image);
		}

		// 获取详细错误信息
		const char* errMsg = fz_caught_message(m_ctx);
		int errCode = fz_caught(m_ctx);
		CString errorDetails;
		errorDetails.Format(_T("导入图片失败！\n\n文件路径：%s\n\nMuPDF错误代码：%d\nMuPDF错误：%S\n\n建议：\n1. 该PNG图片可能使用了特殊的编码格式\n2. 尝试用画图工具重新保存图片\n3. 或转换为JPG格式"),
			imagePath, errCode, errMsg);

		OutputDebugString(errorDetails);
		MessageBox(errorDetails, _T("错误"), MB_OK | MB_ICONERROR);
		return false;
	}

	// 刷新列表
	RefreshThumbnailList();
	UpdateInfoBar();

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

// 双击查看（可选功能）
void CPDFEditDialog::OnNMDblclk(NMHDR* pNMHDR, LRESULT* pResult)
{
	NM_LISTVIEW* pNMListView = (NM_LISTVIEW*)pNMHDR;
	*pResult = 0;

	if (pNMListView->iItem >= 0)
	{
		// 可以添加预览功能
		MessageBox(_T("双击预览功能待实现"), _T("提示"), MB_OK);
	}
}

// 列表选择变化
void CPDFEditDialog::OnItemChanged(NMHDR* pNMHDR, LRESULT* pResult)
{
	UpdateInfoBar();
	*pResult = 0;
}

void CPDFEditDialog::OnOK()
{
	// 不做任何操作，由保存按钮触发
}

void CPDFEditDialog::OnCancel()
{
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
