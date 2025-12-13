// SearchTypes.h: 搜索相关的类型定义
// 独立的头文件，避免循环依赖

#pragma once
#include <mupdf/fitz.h>
#include <afxwin.h>

// 搜索匹配项结构
struct SearchMatch {
	int pageNumber;          // 页码
	fz_quad quad;           // 文本的四边形位置
	CString context;        // 上下文文本
};
