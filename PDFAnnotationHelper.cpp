// PDFAnnotationHelper.cpp: 轻量级 PDF 批注与原文件保存实现
//

#include "pch.h"
#include "framework.h"
#include <imm.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <utility>
#include <vector>
#include <mupdf/fitz.h>
#include <mupdf/pdf.h>
#include "PDFDocument.h"
#include "PDFAnnotationHelper.h"

#pragma comment(lib, "imm32.lib")

#ifdef _DEBUG
#define new DEBUG_NEW
#endif

namespace
{
	constexpr float MIN_ANNOT_FONT_PT = 8.0f;
	constexpr float MAX_ANNOT_FONT_PT = 48.0f;
	constexpr float MIN_ANNOT_CHAR_SPACING_PT = 0.0f;
	constexpr float MAX_ANNOT_CHAR_SPACING_PT = 24.0f;
	constexpr float FIXED_ANNOT_LINE_HEIGHT_MULT = 1.32f;
	constexpr float FREETEXT_PAD_X_PT = 4.0f;
	constexpr float FREETEXT_PAD_Y_PT = 3.0f;

	std::vector<char> WideToUtf8(const CString& text)
	{
		if (text.IsEmpty())
		{
			return { '\0' };
		}
		const int len = ::WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
		if (len <= 0)
		{
			return { '\0' };
		}
		std::vector<char> buf(static_cast<size_t>(len), 0);
		::WideCharToMultiByte(CP_UTF8, 0, text, -1, buf.data(), len, nullptr, nullptr);
		return buf;
	}

	CString Utf8ToWide(const char* pUtf8)
	{
		if (!pUtf8 || !*pUtf8)
		{
			return CString();
		}
		const int len = ::MultiByteToWideChar(CP_UTF8, 0, pUtf8, -1, nullptr, 0);
		if (len <= 1)
		{
			return CString();
		}
		CString out;
		LPWSTR pBuf = out.GetBuffer(len);
		::MultiByteToWideChar(CP_UTF8, 0, pUtf8, -1, pBuf, len);
		out.ReleaseBuffer();
		return out;
	}

	LPCTSTR AnnotTypeDisplayName(enum pdf_annot_type type)
	{
		switch (type)
		{
		case PDF_ANNOT_HIGHLIGHT:
			return _T("高亮批注");
		case PDF_ANNOT_UNDERLINE:
			return _T("下划线批注");
		case PDF_ANNOT_STRIKE_OUT:
			return _T("删除线批注");
		case PDF_ANNOT_SQUIGGLY:
			return _T("波浪线批注");
		case PDF_ANNOT_TEXT:
			return _T("便签批注");
		case PDF_ANNOT_FREE_TEXT:
			return _T("文字批注");
		case PDF_ANNOT_STAMP:
			return _T("图章批注");
		case PDF_ANNOT_INK:
			return _T("手写墨迹批注");
		case PDF_ANNOT_SQUARE:
		case PDF_ANNOT_CIRCLE:
		case PDF_ANNOT_LINE:
			return _T("图形批注");
		default:
			return _T("PDF 批注");
		}
	}

	float EstimateCharAdvancePt(WCHAR ch, float fontSizePt)
	{
		if (ch == _T('\t'))
			return fontSizePt * 2.0f;
		if (ch < 0x7F)
		{
			if (ch == _T(' ') || ch == _T('i') || ch == _T('l') || ch == _T('I') ||
				ch == _T('.') || ch == _T(',') || ch == _T(':') || ch == _T(';') || ch == _T('!'))
			{
				return fontSizePt * 0.30f;
			}
			if (ch == _T('m') || ch == _T('w') || ch == _T('M') || ch == _T('W'))
				return fontSizePt * 0.82f;
			return fontSizePt * 0.56f;
		}
		return fontSizePt * 1.0f;
	}

	float MeasureSpanPt(HDC hDC, HFONT hFont, float pxPerPt,
		LPCTSTR pChars, int count, float fontSizePt, float charSpacingPt)
	{
		if (count <= 0 || !pChars)
			return 0.0f;
		if (hDC && hFont && pxPerPt > 0.01f)
		{
			const int px = MeasureTextWithCharSpacingPx(
				hDC, hFont, pChars, count, charSpacingPt * pxPerPt);
			return static_cast<float>(px) / pxPerPt;
		}
		float sum = 0.0f;
		for (int i = 0; i < count; ++i)
		{
			if (pChars[i] >= 0xDC00 && pChars[i] <= 0xDFFF)
				continue;
			sum += EstimateCharAdvancePt(pChars[i], fontSizePt) + charSpacingPt;
		}
		return sum;
	}

	// 解析 FreeText 批注上的 /DA 与 /DS 默认样式（字号、颜色、文字间距 letter-spacing）
	void ParseFreeTextStyle(fz_context* pCtx, pdf_annot* pAnnot,
		COLORREF& outColor, float& outFontSizePt, float& outCharSpacingPt)
	{
		outColor = RGB(0, 114, 206);
		outFontSizePt = 12.0f;
		outCharSpacingPt = 0.0f;

		const char* pFontName = nullptr;
		float daSize = 12.0f;
		int colorCount = 0;
		float daColor[4] = { 0.0f, 0.447f, 0.808f, 0.0f };
		pdf_annot_default_appearance(pCtx, pAnnot, &pFontName, &daSize, &colorCount, daColor);
		if (daSize >= MIN_ANNOT_FONT_PT && daSize <= MAX_ANNOT_FONT_PT)
			outFontSizePt = daSize;

		if (colorCount == 3)
		{
			outColor = RGB(
				static_cast<int>(max(0.0f, min(1.0f, daColor[0])) * 255.0f + 0.5f),
				static_cast<int>(max(0.0f, min(1.0f, daColor[1])) * 255.0f + 0.5f),
				static_cast<int>(max(0.0f, min(1.0f, daColor[2])) * 255.0f + 0.5f));
		}
		else if (colorCount == 1)
		{
			const int g = static_cast<int>(max(0.0f, min(1.0f, daColor[0])) * 255.0f + 0.5f);
			outColor = RGB(g, g, g);
		}

		pdf_obj* pAnnotObj = pdf_annot_obj(pCtx, pAnnot);
		pdf_obj* pDsObj = pAnnotObj ? pdf_dict_get(pCtx, pAnnotObj, PDF_NAME(DS)) : nullptr;
		const char* pDs = pDsObj ? pdf_to_text_string(pCtx, pDsObj) : nullptr;
		if (!pDs || !*pDs)
			return;

		if (const char* pLs = std::strstr(pDs, "letter-spacing:"))
		{
			pLs += std::strlen("letter-spacing:");
			while (*pLs == ' ' || *pLs == '\t')
				++pLs;
			char* pEnd = nullptr;
			const float val = std::strtof(pLs, &pEnd);
			if (pEnd != pLs && val >= 0.0f)
			{
				if (pEnd && pEnd[0] == 'e' && pEnd[1] == 'm')
					outCharSpacingPt = val * outFontSizePt;
				else
					outCharSpacingPt = val;
				outCharSpacingPt = max(MIN_ANNOT_CHAR_SPACING_PT,
					min(MAX_ANNOT_CHAR_SPACING_PT, outCharSpacingPt));
			}
		}

		if (const char* pFs = std::strstr(pDs, "font-size:"))
		{
			pFs += std::strlen("font-size:");
			while (*pFs == ' ' || *pFs == '\t')
				++pFs;
			char* pEnd = nullptr;
			const float fsVal = std::strtof(pFs, &pEnd);
			if (pEnd != pFs && fsVal >= MIN_ANNOT_FONT_PT && fsVal <= MAX_ANNOT_FONT_PT)
				outFontSizePt = fsVal;
		}

		if (const char* pCol = std::strstr(pDs, "color:"))
		{
			pCol += std::strlen("color:");
			while (*pCol == ' ' || *pCol == '\t')
				++pCol;
			if (*pCol == '#' && std::strlen(pCol) >= 7)
			{
				unsigned int hexVal = 0;
				if (sscanf_s(pCol + 1, "%06x", &hexVal) == 1)
				{
					outColor = RGB((hexVal >> 16) & 0xFF, (hexVal >> 8) & 0xFF, hexVal & 0xFF);
				}
			}
		}
	}

	// 将单行 UTF-16 文本写入 PDF 内容流（ASCII 使用 /Helv，中文与全角符号使用 /Heiti UTF-16BE）
	void AppendPdfTextRun(fz_context* pCtx, fz_buffer* pBuf, const CString& lineText, float fontSizePt)
	{
		if (lineText.IsEmpty())
			return;

		int curMode = 0; // 1 = ASCII (/Helv), 2 = Unicode/CJK (/Heiti)
		for (int i = 0; i < lineText.GetLength(); ++i)
		{
			const WCHAR wc = lineText[i];
			const bool isAscii = (wc >= 0x20 && wc <= 0x7E);
			const int nextMode = isAscii ? 1 : 2;
			if (nextMode != curMode)
			{
				if (curMode == 1)
					fz_append_string(pCtx, pBuf, ") Tj\n");
				else if (curMode == 2)
					fz_append_string(pCtx, pBuf, "> Tj\n");

				if (nextMode == 1)
				{
					fz_append_printf(pCtx, pBuf, "/Helv %g Tf\n(", fontSizePt);
				}
				else
				{
					fz_append_printf(pCtx, pBuf, "/Heiti %g Tf\n<", fontSizePt);
				}
				curMode = nextMode;
			}

			if (curMode == 1)
			{
				if (wc == _T('(') || wc == _T(')') || wc == _T('\\'))
					fz_append_byte(pCtx, pBuf, '\\');
				fz_append_byte(pCtx, pBuf, static_cast<unsigned char>(wc));
			}
			else
			{
				fz_append_printf(pCtx, pBuf, "%04X", static_cast<unsigned int>(static_cast<uint16_t>(wc)));
			}
		}

		if (curMode == 1)
			fz_append_string(pCtx, pBuf, ") Tj\n");
		else if (curMode == 2)
			fz_append_string(pCtx, pBuf, "> Tj\n");
	}

	// 为无边框透明 FreeText 批注构建支持自定义文字间距 (Tc)、颜色、字号与页面旋转的 PDF /AP /N 外观流
	void BuildCustomFreeTextAppearance(
		fz_context* pCtx,
		pdf_document* pPdfDoc,
		pdf_annot* pAnnot,
		int totalRotDegrees,
		const std::vector<FreeTextVisualLine>& lines,
		COLORREF textColor,
		float fontSizePt,
		float charSpacingPt)
	{
		pdf_obj* pAnnotObj = pdf_annot_obj(pCtx, pAnnot);
		const fz_rect pdfRect = pdf_dict_get_rect(pCtx, pAnnotObj, PDF_NAME(Rect));
		float w = max(4.0f, pdfRect.x1 - pdfRect.x0);
		float h = max(4.0f, pdfRect.y1 - pdfRect.y0);
		const int rot = ((totalRotDegrees % 360) + 360) % 360;
		if (rot == 90 || rot == 270)
			std::swap(w, h);

		fz_matrix tfm = fz_rotate(static_cast<float>(rot));
		if (rot == 270)
			tfm.f += w;
		else if (rot == 90)
			tfm.e += h;
		else if (rot == 180)
		{
			tfm.e += w;
			tfm.f += h;
		}
		tfm.e += pdfRect.x0;
		tfm.f += pdfRect.y0;

		const float r = GetRValue(textColor) / 255.0f;
		const float g = GetGValue(textColor) / 255.0f;
		const float b = GetBValue(textColor) / 255.0f;
		const float lineStepPt = fontSizePt * FIXED_ANNOT_LINE_HEIGHT_MULT;

		fz_font* pHelvFont = nullptr;
		fz_font* pCjkFont = nullptr;
		pdf_obj* pRes = nullptr;
		fz_buffer* pBuf = nullptr;
		fz_var(pHelvFont);
		fz_var(pCjkFont);
		fz_var(pRes);
		fz_var(pBuf);

		fz_try(pCtx)
		{
			pHelvFont = fz_new_base14_font(pCtx, "Helvetica");
			pCjkFont = fz_new_cjk_font(pCtx, FZ_ADOBE_GB);
			pRes = pdf_new_dict(pCtx, pPdfDoc, 2);
			pdf_obj* pFontDict = pdf_dict_put_dict(pCtx, pRes, PDF_NAME(Font), 2);
			pdf_dict_puts_drop(pCtx, pFontDict, "Helv",
				pdf_add_simple_font(pCtx, pPdfDoc, pHelvFont, PDF_SIMPLE_ENCODING_LATIN));
			pdf_dict_puts_drop(pCtx, pFontDict, "Heiti",
				pdf_add_cjk_font(pCtx, pPdfDoc, pCjkFont, FZ_ADOBE_GB, 0, 0));

			pBuf = fz_new_buffer(pCtx, 512);
			fz_append_printf(pCtx, pBuf, "q\n%g %g %g %g %g %g cm\n",
				tfm.a, tfm.b, tfm.c, tfm.d, tfm.e, tfm.f);
			fz_append_printf(pCtx, pBuf, "0 0 %g %g re\nW\nn\n", w, h);
			fz_append_string(pCtx, pBuf, "BT\n");
			fz_append_printf(pCtx, pBuf, "%g %g %g rg\n", r, g, b);
			if (charSpacingPt > 0.005f)
				fz_append_printf(pCtx, pBuf, "%g Tc\n", charSpacingPt);

			const float startX = FREETEXT_PAD_X_PT;
			const float firstBaselineY = h - FREETEXT_PAD_Y_PT - fontSizePt * 0.86f;
			for (size_t i = 0; i < lines.size(); ++i)
			{
				const float curY = firstBaselineY - static_cast<float>(i) * lineStepPt;
				if (i == 0)
					fz_append_printf(pCtx, pBuf, "%g %g Td\n", startX, curY);
				else
					fz_append_printf(pCtx, pBuf, "0 %g Td\n", -lineStepPt);

				if (!lines[i].text.IsEmpty())
					AppendPdfTextRun(pCtx, pBuf, lines[i].text, fontSizePt);
			}

			fz_append_string(pCtx, pBuf, "ET\nQ\n");

			pdf_set_annot_appearance(pCtx, pAnnot, "N", nullptr, fz_identity, pdfRect, pRes, pBuf);
		}
		fz_always(pCtx)
		{
			if (pBuf)
				fz_drop_buffer(pCtx, pBuf);
			if (pRes)
				pdf_drop_obj(pCtx, pRes);
			if (pCjkFont)
				fz_drop_font(pCtx, pCjkFont);
			if (pHelvFont)
				fz_drop_font(pCtx, pHelvFont);
		}
		fz_catch(pCtx)
		{
			fz_rethrow(pCtx);
		}
	}
}

int MeasureTextWithCharSpacingPx(
	HDC hDC,
	HFONT hFont,
	LPCTSTR pText,
	int unitCount,
	float charSpacingPx)
{
	if (!hDC || !pText || unitCount <= 0)
		return 0;

	HGDIOBJ hOld = hFont ? ::SelectObject(hDC, hFont) : nullptr;
	SIZE sz = { 0, 0 };
	const BOOL ok = ::GetTextExtentPoint32W(hDC, pText, unitCount, &sz);
	if (hOld)
		::SelectObject(hDC, hOld);
	if (!ok)
		return 0;

	if (charSpacingPx <= 0.005f)
		return sz.cx;

	int glyphCount = 0;
	for (int i = 0; i < unitCount; ++i)
	{
		if (pText[i] >= 0xDC00 && pText[i] <= 0xDFFF)
			continue;
		++glyphCount;
	}
	return static_cast<int>(std::round(static_cast<float>(sz.cx) + glyphCount * charSpacingPx));
}

void DrawTextWithCharSpacingPx(
	HDC hDC,
	int x,
	int y,
	const CString& lineText,
	float charSpacingPx)
{
	const int len = lineText.GetLength();
	if (!hDC || len <= 0)
		return;

	if (charSpacingPx <= 0.005f)
	{
		::ExtTextOutW(hDC, x, y, 0, nullptr, lineText, len, nullptr);
		return;
	}

	std::vector<INT> baseCum(static_cast<size_t>(len), 0);
	SIZE sz = { 0, 0 };
	if (!::GetTextExtentExPointW(hDC, lineText, len, 0, nullptr, baseCum.data(), &sz))
	{
		::ExtTextOutW(hDC, x, y, 0, nullptr, lineText, len, nullptr);
		return;
	}

	std::vector<INT> dx(static_cast<size_t>(len), 0);
	int prevOriginPx = 0;
	int glyphSeen = 0;
	for (int i = 0; i < len; ++i)
	{
		if (i + 1 < len &&
			lineText[i] >= 0xD800 && lineText[i] <= 0xDBFF &&
			lineText[i + 1] >= 0xDC00 && lineText[i + 1] <= 0xDFFF)
		{
			dx[i] = 0;
			continue;
		}
		++glyphSeen;
		const int nextOriginPx = static_cast<int>(std::round(
			static_cast<float>(baseCum[i]) + glyphSeen * charSpacingPx));
		dx[i] = max(0, nextOriginPx - prevOriginPx);
		prevOriginPx = nextOriginPx;
	}

	::ExtTextOutW(hDC, x, y, 0, nullptr, lineText, len, dx.data());
}

void LayoutFreeTextVisualLines(
	HDC hMeasureDC,
	HFONT hMeasureFont,
	float pxPerPt,
	const CString& fullText,
	float fontSizePt,
	float charSpacingPt,
	float availWidthPt,
	std::vector<FreeTextVisualLine>& outLines)
{
	outLines.clear();
	const float safeAvailPt = max(fontSizePt * 1.2f, availWidthPt);
	const int len = fullText.GetLength();
	if (len == 0)
	{
		outLines.push_back({ CString(), 0, 0 });
		return;
	}

	int pos = 0;
	while (pos <= len)
	{
		// 查找当前自然段结尾（\r\n 或 \n）
		int paraEnd = pos;
		while (paraEnd < len && fullText[paraEnd] != _T('\r') && fullText[paraEnd] != _T('\n'))
			++paraEnd;

		if (paraEnd == pos)
		{
			outLines.push_back({ CString(), pos, pos });
		}
		else
		{
			int segStart = pos;
			while (segStart < paraEnd)
			{
				int cur = segStart;
				int lastSpaceBreak = -1;
				while (cur < paraEnd)
				{
					int next = cur + 1;
					if (next < paraEnd &&
						fullText[cur] >= 0xD800 && fullText[cur] <= 0xDBFF &&
						fullText[next] >= 0xDC00 && fullText[next] <= 0xDFFF)
					{
						++next;
					}
					if (fullText[cur] == _T(' ') || fullText[cur] == _T('\t'))
						lastSpaceBreak = next;

					const float wPt = MeasureSpanPt(hMeasureDC, hMeasureFont, pxPerPt,
						static_cast<LPCTSTR>(fullText) + segStart, next - segStart, fontSizePt, charSpacingPt);
					if (wPt > safeAvailPt && cur > segStart)
					{
						if (lastSpaceBreak > segStart &&
							(cur - lastSpaceBreak) <= 12 &&
							fullText[cur] < 0x2E80)
						{
							cur = lastSpaceBreak;
						}
						break;
					}
					cur = next;
				}
				if (cur <= segStart)
					cur = min(paraEnd, segStart + 1);

				FreeTextVisualLine line;
				line.text = fullText.Mid(segStart, cur - segStart);
				line.startChar = segStart;
				line.endChar = cur;
				outLines.push_back(std::move(line));
				segStart = cur;
			}
		}

		if (paraEnd >= len)
			break;

		// 跳过换行符；若文本以换行符结尾，则补一个空视觉行以承接光标
		int nextPos = paraEnd + 1;
		if (fullText[paraEnd] == _T('\r') && nextPos < len && fullText[nextPos] == _T('\n'))
			++nextPos;
		pos = nextPos;
		if (pos == len)
		{
			outLines.push_back({ CString(), len, len });
			break;
		}
	}

	if (outLines.empty())
		outLines.push_back({ CString(), 0, 0 });
}

bool AddQuadMarkupAnnotation(
	CPDFDocument* pDoc,
	int pageNumber,
	enum pdf_annot_type annotType,
	const std::vector<fz_quad>& quads,
	COLORREF color,
	const CString& contents)
{
	if (!pDoc || !pDoc->IsOpen() || quads.empty() ||
		pageNumber < 0 || pageNumber >= pDoc->GetTotalPages())
	{
		return false;
	}

	fz_context* pCtx = pDoc->GetContext();
	fz_document* pFzDoc = pDoc->GetDocument();
	if (!pCtx || !pFzDoc)
	{
		return false;
	}

	const std::vector<char> utf8Contents = WideToUtf8(contents);
	const float rgb[3] = {
		GetRValue(color) / 255.0f,
		GetGValue(color) / 255.0f,
		GetBValue(color) / 255.0f
	};

	pdf_page* pPdfPage = nullptr;
	pdf_annot* pAnnot = nullptr;
	bool ok = false;
	fz_var(pPdfPage);
	fz_var(pAnnot);
	fz_var(ok);

	fz_try(pCtx)
	{
		pdf_document* pPdfDoc = pdf_document_from_fz_document(pCtx, pFzDoc);
		if (!pPdfDoc)
		{
			fz_throw(pCtx, FZ_ERROR_GENERIC, "Not a PDF document");
		}
		pPdfPage = pdf_load_page(pCtx, pPdfDoc, pageNumber);
		pAnnot = pdf_create_annot(pCtx, pPdfPage, annotType);
		pdf_set_annot_color(pCtx, pAnnot, 3, rgb);
		pdf_set_annot_flags(pCtx, pAnnot, PDF_ANNOT_IS_PRINT);
		pdf_set_annot_quad_points(pCtx, pAnnot, static_cast<int>(quads.size()), quads.data());
		pdf_set_annot_author(pCtx, pAnnot, "XiaoGongPDF");
		const int64_t now = static_cast<int64_t>(std::time(nullptr));
		pdf_set_annot_creation_date(pCtx, pAnnot, now);
		pdf_set_annot_modification_date(pCtx, pAnnot, now);
		if (!contents.IsEmpty())
		{
			pdf_set_annot_contents(pCtx, pAnnot, utf8Contents.data());
		}
		pdf_update_page(pCtx, pPdfPage);
		ok = true;
	}
	fz_always(pCtx)
	{
		if (pAnnot)
			pdf_drop_annot(pCtx, pAnnot);
		if (pPdfPage)
			pdf_drop_page(pCtx, pPdfPage);
	}
	fz_catch(pCtx)
	{
		ok = false;
	}

	if (ok)
	{
		pDoc->InvalidatePageRenderCache(pageNumber);
		pDoc->SetUnsavedAnnotations(true);
	}
	return ok;
}

bool UpsertFreeTextAnnotation(
	CPDFDocument* pDoc,
	int pageNumber,
	int existingAnnotIndex,
	const fz_rect& fitzRect,
	const CString& noteText,
	COLORREF textColor,
	float fontSizePt,
	float charSpacingPt,
	int* pOutAnnotIndex)
{
	if (pOutAnnotIndex)
		*pOutAnnotIndex = -1;
	if (!pDoc || !pDoc->IsOpen() || noteText.IsEmpty() ||
		pageNumber < 0 || pageNumber >= pDoc->GetTotalPages())
	{
		return false;
	}

	fz_context* pCtx = pDoc->GetContext();
	fz_document* pFzDoc = pDoc->GetDocument();
	if (!pCtx || !pFzDoc)
		return false;

	const float clampedFontSize = max(MIN_ANNOT_FONT_PT, min(MAX_ANNOT_FONT_PT, fontSizePt));
	const float clampedCharSpacing = max(MIN_ANNOT_CHAR_SPACING_PT,
		min(MAX_ANNOT_CHAR_SPACING_PT, charSpacingPt));
	const int viewRot = pDoc->GetPageRotation(pageNumber);
	const std::vector<char> utf8Note = WideToUtf8(noteText);
	const float rgb[3] = {
		GetRValue(textColor) / 255.0f,
		GetGValue(textColor) / 255.0f,
		GetBValue(textColor) / 255.0f
	};

	char dsBuf[160] = { 0 };
	sprintf_s(dsBuf, sizeof(dsBuf),
		"font-family: sans-serif; font-size: %.1fpt; letter-spacing: %.2fpt; color: #%02X%02X%02X;",
		clampedFontSize, clampedCharSpacing,
		GetRValue(textColor), GetGValue(textColor), GetBValue(textColor));

	const float boxVisualW = (viewRot == 90 || viewRot == 270) ?
		(fitzRect.y1 - fitzRect.y0) : (fitzRect.x1 - fitzRect.x0);
	std::vector<FreeTextVisualLine> wrappedLines;
	LayoutFreeTextVisualLines(nullptr, nullptr, 1.0f, noteText, clampedFontSize, clampedCharSpacing,
		max(24.0f, boxVisualW - FREETEXT_PAD_X_PT * 2.0f), wrappedLines);

	pdf_page* pPdfPage = nullptr;
	pdf_annot* pCreatedAnnot = nullptr;
	bool ok = false;
	int finalAnnotIndex = -1;
	fz_var(pPdfPage);
	fz_var(pCreatedAnnot);
	fz_var(ok);
	fz_var(finalAnnotIndex);

	fz_try(pCtx)
	{
		pdf_document* pPdfDoc = pdf_document_from_fz_document(pCtx, pFzDoc);
		if (!pPdfDoc)
			fz_throw(pCtx, FZ_ERROR_GENERIC, "Not a PDF document");

		pPdfPage = pdf_load_page(pCtx, pPdfDoc, pageNumber);
		pdf_annot* pTargetAnnot = nullptr;

		if (existingAnnotIndex >= 0)
		{
			int idx = 0;
			for (pdf_annot* pCur = pdf_first_annot(pCtx, pPdfPage);
				pCur != nullptr;
				pCur = pdf_next_annot(pCtx, pCur), ++idx)
			{
				if (idx == existingAnnotIndex && pdf_annot_type(pCtx, pCur) == PDF_ANNOT_FREE_TEXT)
				{
					pTargetAnnot = pCur;
					finalAnnotIndex = idx;
					break;
				}
			}
		}

		const int64_t now = static_cast<int64_t>(std::time(nullptr));
		if (!pTargetAnnot)
		{
			pCreatedAnnot = pdf_create_annot(pCtx, pPdfPage, PDF_ANNOT_FREE_TEXT);
			pTargetAnnot = pCreatedAnnot;
			pdf_set_annot_creation_date(pCtx, pTargetAnnot, now);
			pdf_set_annot_author(pCtx, pTargetAnnot, "XiaoGongPDF");

			int countIdx = 0;
			for (pdf_annot* pCur = pdf_first_annot(pCtx, pPdfPage);
				pCur != nullptr;
				pCur = pdf_next_annot(pCtx, pCur), ++countIdx)
			{
				if (pCur == pTargetAnnot)
					finalAnnotIndex = countIdx;
			}
		}

		pdf_obj* pAnnotObj = pdf_annot_obj(pCtx, pTargetAnnot);
		const int origRot = pdf_dict_get_inheritable_int(pCtx, pPdfPage->obj, PDF_NAME(Rotate));
		const int totalRot = ((origRot + viewRot) % 360 + 360) % 360;
		if (totalRot != 0)
			pdf_dict_put_int(pCtx, pAnnotObj, PDF_NAME(Rotate), totalRot);
		else
			pdf_dict_del(pCtx, pAnnotObj, PDF_NAME(Rotate));

		// 清除背景填充色与外框宽度，实现图片 3.png 所示纯文字透明叠加效果
		pdf_dict_del(pCtx, pAnnotObj, PDF_NAME(C));
		pdf_dict_del(pCtx, pAnnotObj, PDF_NAME(IC));
		pdf_dict_del(pCtx, pAnnotObj, PDF_NAME(RD));
		pdf_dict_del(pCtx, pAnnotObj, PDF_NAME(RC));

		pdf_set_annot_intent(pCtx, pTargetAnnot, PDF_ANNOT_IT_FREETEXT_TYPEWRITER);
		pdf_set_annot_language(pCtx, pTargetAnnot, FZ_LANG_zh_Hans);
		pdf_set_annot_border_width(pCtx, pTargetAnnot, 0.0f);
		pdf_set_annot_opacity(pCtx, pTargetAnnot, 1.0f);
		pdf_set_annot_rect(pCtx, pTargetAnnot, fitzRect);
		pdf_set_annot_default_appearance(pCtx, pTargetAnnot, "Helv", clampedFontSize, 3, rgb);
		pdf_dict_put_text_string(pCtx, pAnnotObj, PDF_NAME(DS), dsBuf);
		pdf_set_annot_flags(pCtx, pTargetAnnot, PDF_ANNOT_IS_PRINT);
		pdf_set_annot_modification_date(pCtx, pTargetAnnot, now);
		pdf_set_annot_contents(pCtx, pTargetAnnot, utf8Note.data());

		// 最后写入自定义外观流（含精确文字间距 Tc 与中文字体），避免默认合成覆盖样式设置
		BuildCustomFreeTextAppearance(pCtx, pPdfDoc, pTargetAnnot, totalRot,
			wrappedLines, textColor, clampedFontSize, clampedCharSpacing);

		pdf_update_page(pCtx, pPdfPage);
		ok = true;
	}
	fz_always(pCtx)
	{
		if (pCreatedAnnot)
			pdf_drop_annot(pCtx, pCreatedAnnot);
		if (pPdfPage)
			pdf_drop_page(pCtx, pPdfPage);
	}
	fz_catch(pCtx)
	{
		ok = false;
	}

	if (ok)
	{
		if (pOutAnnotIndex)
			*pOutAnnotIndex = finalAnnotIndex;
		pDoc->InvalidatePageRenderCache(pageNumber);
		pDoc->SetUnsavedAnnotations(true);
	}
	return ok;
}

bool SetPageAnnotationEditingHidden(
	CPDFDocument* pDoc,
	int pageNumber,
	int annotIndex,
	bool hidden)
{
	if (!pDoc || !pDoc->IsOpen() || annotIndex < 0 ||
		pageNumber < 0 || pageNumber >= pDoc->GetTotalPages())
	{
		return false;
	}

	fz_context* pCtx = pDoc->GetContext();
	fz_document* pFzDoc = pDoc->GetDocument();
	if (!pCtx || !pFzDoc)
		return false;

	pdf_page* pPdfPage = nullptr;
	bool ok = false;
	fz_var(pPdfPage);
	fz_var(ok);

	fz_try(pCtx)
	{
		pdf_document* pPdfDoc = pdf_document_from_fz_document(pCtx, pFzDoc);
		if (pPdfDoc)
		{
			pPdfPage = pdf_load_page(pCtx, pPdfDoc, pageNumber);
			int idx = 0;
			for (pdf_annot* pAnnot = pdf_first_annot(pCtx, pPdfPage);
				pAnnot != nullptr;
				pAnnot = pdf_next_annot(pCtx, pAnnot), ++idx)
			{
				if (idx == annotIndex)
				{
					int flags = pdf_annot_flags(pCtx, pAnnot);
					if (hidden)
						flags |= PDF_ANNOT_IS_HIDDEN;
					else
						flags &= ~PDF_ANNOT_IS_HIDDEN;
					pdf_set_annot_flags(pCtx, pAnnot, flags);
					pdf_update_page(pCtx, pPdfPage);
					ok = true;
					break;
				}
			}
		}
	}
	fz_always(pCtx)
	{
		if (pPdfPage)
			pdf_drop_page(pCtx, pPdfPage);
	}
	fz_catch(pCtx)
	{
		ok = false;
	}

	if (ok)
	{
		pDoc->InvalidatePageRenderCache(pageNumber);
	}
	return ok;
}

bool AddTextNoteAnnotation(
	CPDFDocument* pDoc,
	int pageNumber,
	float fitzX,
	float fitzY,
	bool freeTextStyle,
	const CString& noteText,
	COLORREF textColor,
	float fontSizePt,
	float charSpacingPt)
{
	if (!pDoc || !pDoc->IsOpen() || noteText.IsEmpty() ||
		pageNumber < 0 || pageNumber >= pDoc->GetTotalPages())
	{
		return false;
	}

	if (freeTextStyle)
	{
		fz_context* pCtx = pDoc->GetContext();
		fz_document* pFzDoc = pDoc->GetDocument();
		if (!pCtx || !pFzDoc)
			return false;

		const float clampedFontSize = max(MIN_ANNOT_FONT_PT, min(MAX_ANNOT_FONT_PT, fontSizePt));
		const float clampedCharSpacing = max(MIN_ANNOT_CHAR_SPACING_PT,
			min(MAX_ANNOT_CHAR_SPACING_PT, charSpacingPt));
		const int viewRot = pDoc->GetPageRotation(pageNumber);

		std::vector<FreeTextVisualLine> lines;
		LayoutFreeTextVisualLines(nullptr, nullptr, 1.0f, noteText, clampedFontSize,
			clampedCharSpacing, 220.0f, lines);
		float maxLineW = clampedFontSize * 4.0f;
		for (const auto& ln : lines)
		{
			maxLineW = max(maxLineW,
				MeasureSpanPt(nullptr, nullptr, 1.0f, ln.text, ln.text.GetLength(),
					clampedFontSize, clampedCharSpacing));
		}
		float boxW = min(240.0f, maxLineW + FREETEXT_PAD_X_PT * 2.0f + 8.0f);
		float boxH = max(clampedFontSize * 1.4f,
			static_cast<float>(lines.size()) * clampedFontSize * FIXED_ANNOT_LINE_HEIGHT_MULT +
			FREETEXT_PAD_Y_PT * 2.0f + 4.0f);
		if (viewRot == 90 || viewRot == 270)
			std::swap(boxW, boxH);

		fz_rect pageBounds = { 0.0f, 0.0f, 595.0f, 842.0f };
		pDoc->GetPageBaseBounds(pageNumber, pageBounds);
		const float x0 = max(pageBounds.x0 + 2.0f, min(fitzX, pageBounds.x1 - boxW - 2.0f));
		const float y0 = max(pageBounds.y0 + 2.0f, min(fitzY, pageBounds.y1 - boxH - 2.0f));
		const fz_rect fitzRect = fz_make_rect(
			x0, y0, min(pageBounds.x1, x0 + boxW), min(pageBounds.y1, y0 + boxH));
		return UpsertFreeTextAnnotation(pDoc, pageNumber, -1, fitzRect,
			noteText, textColor, clampedFontSize, clampedCharSpacing);
	}

	fz_context* pCtx = pDoc->GetContext();
	fz_document* pFzDoc = pDoc->GetDocument();
	if (!pCtx || !pFzDoc)
		return false;

	const std::vector<char> utf8Note = WideToUtf8(noteText);
	const float rgb[3] = {
		GetRValue(textColor) / 255.0f,
		GetGValue(textColor) / 255.0f,
		GetBValue(textColor) / 255.0f
	};

	pdf_page* pPdfPage = nullptr;
	pdf_annot* pAnnot = nullptr;
	bool ok = false;
	fz_var(pPdfPage);
	fz_var(pAnnot);
	fz_var(ok);

	fz_try(pCtx)
	{
		pdf_document* pPdfDoc = pdf_document_from_fz_document(pCtx, pFzDoc);
		if (!pPdfDoc)
			fz_throw(pCtx, FZ_ERROR_GENERIC, "Not a PDF document");
		pPdfPage = pdf_load_page(pCtx, pPdfDoc, pageNumber);
		const fz_rect pageBounds = fz_bound_page(pCtx, &pPdfPage->super);

		pAnnot = pdf_create_annot(pCtx, pPdfPage, PDF_ANNOT_TEXT);
		const float half = 10.0f;
		const float cx = max(pageBounds.x0 + half, min(fitzX, pageBounds.x1 - half));
		const float cy = max(pageBounds.y0 + half, min(fitzY, pageBounds.y1 - half));
		const fz_rect iconRect = fz_make_rect(cx - half, cy - half, cx + half, cy + half);
		pdf_set_annot_icon_name(pCtx, pAnnot, "Comment");
		pdf_set_annot_rect(pCtx, pAnnot, iconRect);
		pdf_set_annot_color(pCtx, pAnnot, 3, rgb);
		pdf_set_annot_author(pCtx, pAnnot, "XiaoGongPDF");
		const int64_t now = static_cast<int64_t>(std::time(nullptr));
		pdf_set_annot_creation_date(pCtx, pAnnot, now);
		pdf_set_annot_modification_date(pCtx, pAnnot, now);
		pdf_set_annot_contents(pCtx, pAnnot, utf8Note.data());
		const fz_rect popupRect = fz_make_rect(
			min(cx + 14.0f, max(pageBounds.x0, pageBounds.x1 - 180.0f)),
			max(pageBounds.y0, cy - 10.0f),
			min(pageBounds.x1, cx + 194.0f),
			min(pageBounds.y1, cy + 90.0f));
		pdf_set_annot_popup(pCtx, pAnnot, popupRect);
		pdf_update_page(pCtx, pPdfPage);
		ok = true;
	}
	fz_always(pCtx)
	{
		if (pAnnot)
			pdf_drop_annot(pCtx, pAnnot);
		if (pPdfPage)
			pdf_drop_page(pCtx, pPdfPage);
	}
	fz_catch(pCtx)
	{
		ok = false;
	}

	if (ok)
	{
		pDoc->InvalidatePageRenderCache(pageNumber);
		pDoc->SetUnsavedAnnotations(true);
	}
	return ok;
}

bool HitTestPageAnnotation(
	CPDFDocument* pDoc,
	int pageNumber,
	float fitzX,
	float fitzY,
	PageAnnotHitInfo& outHit)
{
	outHit = PageAnnotHitInfo{};
	if (!pDoc || !pDoc->IsOpen() || pageNumber < 0 || pageNumber >= pDoc->GetTotalPages())
	{
		return false;
	}

	fz_context* pCtx = pDoc->GetContext();
	fz_document* pFzDoc = pDoc->GetDocument();
	if (!pCtx || !pFzDoc)
	{
		return false;
	}

	pdf_page* pPdfPage = nullptr;
	bool found = false;
	int hitIdx = -1;
	enum pdf_annot_type hitType = PDF_ANNOT_UNKNOWN;
	fz_rect hitRect = { 0.0f, 0.0f, 0.0f, 0.0f };
	COLORREF hitColor = RGB(0, 114, 206);
	float hitFontSize = 12.0f;
	float hitCharSpacing = 0.0f;
	std::string hitContentsUtf8;

	fz_var(pPdfPage);
	fz_var(found);
	fz_var(hitIdx);
	fz_var(hitType);
	fz_var(hitRect);
	fz_var(hitColor);
	fz_var(hitFontSize);
	fz_var(hitCharSpacing);

	const fz_point pt = fz_make_point(fitzX, fitzY);

	fz_try(pCtx)
	{
		pdf_document* pPdfDoc = pdf_document_from_fz_document(pCtx, pFzDoc);
		if (pPdfDoc)
		{
			pPdfPage = pdf_load_page(pCtx, pPdfDoc, pageNumber);
			int idx = 0;
			for (pdf_annot* pAnnot = pdf_first_annot(pCtx, pPdfPage);
				pAnnot != nullptr;
				pAnnot = pdf_next_annot(pCtx, pAnnot), ++idx)
			{
				const enum pdf_annot_type t = pdf_annot_type(pCtx, pAnnot);
				if (t == PDF_ANNOT_LINK || t == PDF_ANNOT_POPUP || t == PDF_ANNOT_WIDGET)
				{
					continue;
				}
				if ((pdf_annot_flags(pCtx, pAnnot) & PDF_ANNOT_IS_HIDDEN) != 0)
				{
					continue;
				}
				const fz_rect r = pdf_bound_annot(pCtx, pAnnot);
				if (fz_is_point_inside_rect(pt, fz_expand_rect(r, 2.0f)))
				{
					found = true;
					hitIdx = idx;
					hitType = t;
					hitRect = r;
					const char* pText = pdf_annot_contents(pCtx, pAnnot);
					hitContentsUtf8 = pText ? pText : "";
					if (t == PDF_ANNOT_FREE_TEXT)
					{
						ParseFreeTextStyle(pCtx, pAnnot, hitColor, hitFontSize, hitCharSpacing);
					}
				}
			}
		}
	}
	fz_always(pCtx)
	{
		if (pPdfPage)
			pdf_drop_page(pCtx, pPdfPage);
	}
	fz_catch(pCtx)
	{
		found = false;
	}

	if (found)
	{
		outHit.annotIndex = hitIdx;
		outHit.type = hitType;
		outHit.fitzBounds = hitRect;
		outHit.contents = Utf8ToWide(hitContentsUtf8.c_str());
		outHit.typeLabel = AnnotTypeDisplayName(hitType);
		outHit.textColor = hitColor;
		outHit.fontSizePt = hitFontSize;
		outHit.charSpacingPt = hitCharSpacing;
	}
	return found;
}

bool UpdatePageAnnotationContents(
	CPDFDocument* pDoc,
	int pageNumber,
	int annotIndex,
	const CString& newContents)
{
	if (!pDoc || !pDoc->IsOpen() || annotIndex < 0 ||
		pageNumber < 0 || pageNumber >= pDoc->GetTotalPages())
	{
		return false;
	}

	fz_context* pCtx = pDoc->GetContext();
	fz_document* pFzDoc = pDoc->GetDocument();
	if (!pCtx || !pFzDoc)
	{
		return false;
	}

	const std::vector<char> utf8Text = WideToUtf8(newContents);
	pdf_page* pPdfPage = nullptr;
	bool ok = false;
	bool isFreeText = false;
	fz_rect freeRect = { 0.0f, 0.0f, 0.0f, 0.0f };
	COLORREF ftColor = RGB(0, 114, 206);
	float ftSize = 12.0f;
	float ftCharSpacing = 0.0f;
	fz_var(pPdfPage);
	fz_var(ok);
	fz_var(isFreeText);
	fz_var(freeRect);
	fz_var(ftColor);
	fz_var(ftSize);
	fz_var(ftCharSpacing);

	fz_try(pCtx)
	{
		pdf_document* pPdfDoc = pdf_document_from_fz_document(pCtx, pFzDoc);
		if (pPdfDoc)
		{
			pPdfPage = pdf_load_page(pCtx, pPdfDoc, pageNumber);
			int idx = 0;
			for (pdf_annot* pAnnot = pdf_first_annot(pCtx, pPdfPage);
				pAnnot != nullptr;
				pAnnot = pdf_next_annot(pCtx, pAnnot), ++idx)
			{
				if (idx == annotIndex)
				{
					if (pdf_annot_type(pCtx, pAnnot) == PDF_ANNOT_FREE_TEXT)
					{
						isFreeText = true;
						freeRect = pdf_bound_annot(pCtx, pAnnot);
						ParseFreeTextStyle(pCtx, pAnnot, ftColor, ftSize, ftCharSpacing);
					}
					else
					{
						pdf_set_annot_contents(pCtx, pAnnot, utf8Text.data());
						pdf_set_annot_modification_date(pCtx, pAnnot, static_cast<int64_t>(std::time(nullptr)));
						pdf_update_page(pCtx, pPdfPage);
						ok = true;
					}
					break;
				}
			}
		}
	}
	fz_always(pCtx)
	{
		if (pPdfPage)
			pdf_drop_page(pCtx, pPdfPage);
	}
	fz_catch(pCtx)
	{
		ok = false;
	}

	if (isFreeText)
	{
		return UpsertFreeTextAnnotation(pDoc, pageNumber, annotIndex, freeRect,
			newContents, ftColor, ftSize, ftCharSpacing);
	}

	if (ok)
	{
		pDoc->InvalidatePageRenderCache(pageNumber);
		pDoc->SetUnsavedAnnotations(true);
	}
	return ok;
}

bool DeletePageAnnotationAt(
	CPDFDocument* pDoc,
	int pageNumber,
	int annotIndex)
{
	if (!pDoc || !pDoc->IsOpen() || annotIndex < 0 ||
		pageNumber < 0 || pageNumber >= pDoc->GetTotalPages())
	{
		return false;
	}

	fz_context* pCtx = pDoc->GetContext();
	fz_document* pFzDoc = pDoc->GetDocument();
	if (!pCtx || !pFzDoc)
	{
		return false;
	}

	pdf_page* pPdfPage = nullptr;
	bool ok = false;
	fz_var(pPdfPage);
	fz_var(ok);

	fz_try(pCtx)
	{
		pdf_document* pPdfDoc = pdf_document_from_fz_document(pCtx, pFzDoc);
		if (pPdfDoc)
		{
			pPdfPage = pdf_load_page(pCtx, pPdfDoc, pageNumber);
			int idx = 0;
			for (pdf_annot* pAnnot = pdf_first_annot(pCtx, pPdfPage);
				pAnnot != nullptr;
				pAnnot = pdf_next_annot(pCtx, pAnnot), ++idx)
			{
				if (idx == annotIndex)
				{
					pdf_delete_annot(pCtx, pPdfPage, pAnnot);
					pdf_update_page(pCtx, pPdfPage);
					ok = true;
					break;
				}
			}
		}
	}
	fz_always(pCtx)
	{
		if (pPdfPage)
			pdf_drop_page(pCtx, pPdfPage);
	}
	fz_catch(pCtx)
	{
		ok = false;
	}

	if (ok)
	{
		pDoc->InvalidatePageRenderCache(pageNumber);
		pDoc->SetUnsavedAnnotations(true);
	}
	return ok;
}

bool SaveDocumentWithAnnotationsAndRotations(
	CPDFDocument* pDoc,
	const CString& targetFilePath,
	const std::map<int, int>& viewerRotations,
	const std::function<void(const CString&)>& onBeforeReplace,
	const std::function<void(const CString&, bool)>& onAfterReplace,
	CString& outErrorMsg)
{
	outErrorMsg.Empty();
	if (!pDoc || !pDoc->IsOpen() || targetFilePath.IsEmpty())
	{
		outErrorMsg = _T("文档未打开或目标路径为空。");
		return false;
	}

	fz_context* pCtx = pDoc->GetContext();
	fz_document* pFzDoc = pDoc->GetDocument();
	if (!pCtx || !pFzDoc)
	{
		outErrorMsg = _T("无效的 PDF 上下文。");
		return false;
	}

	const int expectedPages = pDoc->GetTotalPages();
	const std::vector<char>& password = pDoc->GetPassword();

	int slash = max(targetFilePath.ReverseFind(_T('\\')), targetFilePath.ReverseFind(_T('/')));
	CString directory = (slash >= 0) ? targetFilePath.Left(slash + 1) : _T(".\\");
	TCHAR tempFileName[MAX_PATH] = { 0 };
	if (!::GetTempFileName(directory, _T("XGA"), 0, tempFileName))
	{
		outErrorMsg = _T("无法在目标目录创建临时文件，请检查目录写入权限。");
		return false;
	}

	const std::vector<char> utf8TempPath = WideToUtf8(CString(tempFileName));
	std::vector<std::pair<int, int>> savedOrigRotates;
	fz_document* pCheckDoc = nullptr;
	bool tempValid = false;
	fz_var(pCheckDoc);
	fz_var(tempValid);

	pdf_write_options writeOpts = pdf_default_write_options;
	writeOpts.do_garbage = 2;
	writeOpts.do_compress = 1;

	fz_try(pCtx)
	{
		pdf_document* pPdfDoc = pdf_document_from_fz_document(pCtx, pFzDoc);
		if (!pPdfDoc)
		{
			fz_throw(pCtx, FZ_ERROR_GENERIC, "当前文件不是可编辑保存的 PDF 格式");
		}

		// 将当前阅读器中的单页旋转角度写入页面字典 /Rotate
		for (const auto& pair : viewerRotations)
		{
			const int pageIdx = pair.first;
			const int deltaRot = ((pair.second % 360) + 360) % 360;
			if (deltaRot == 0 || pageIdx < 0 || pageIdx >= expectedPages)
				continue;

			pdf_obj* pPageObj = pdf_lookup_page_obj(pCtx, pPdfDoc, pageIdx);
			if (pPageObj)
			{
				pdf_obj* pRotObj = pdf_dict_get_inheritable(pCtx, pPageObj, PDF_NAME(Rotate));
				const int origRotate = pRotObj ? pdf_to_int(pCtx, pRotObj) : 0;
				savedOrigRotates.emplace_back(pageIdx, origRotate);
				pdf_dict_put_int(pCtx, pPageObj, PDF_NAME(Rotate),
					((origRotate + deltaRot) % 360 + 360) % 360);
			}
		}

		pdf_save_document(pCtx, pPdfDoc, utf8TempPath.data(), &writeOpts);

		// 还原内存中的原始 /Rotate，避免“另存为”或替换失败时原标签页二次旋转
		for (const auto& origPair : savedOrigRotates)
		{
			pdf_obj* pPageObj = pdf_lookup_page_obj(pCtx, pPdfDoc, origPair.first);
			if (pPageObj)
			{
				pdf_dict_put_int(pCtx, pPageObj, PDF_NAME(Rotate), origPair.second);
			}
		}
		savedOrigRotates.clear();

		pCheckDoc = fz_open_document(pCtx, utf8TempPath.data());
		if (fz_needs_password(pCtx, pCheckDoc))
		{
			if (password.empty() || !fz_authenticate_password(pCtx, pCheckDoc, password.data()))
			{
				fz_throw(pCtx, FZ_ERROR_GENERIC, "保存后的加密 PDF 密码校验失败");
			}
		}
		tempValid = (fz_count_pages(pCtx, pCheckDoc) == expectedPages);
	}
	fz_always(pCtx)
	{
		if (pCheckDoc)
		{
			fz_drop_document(pCtx, pCheckDoc);
			pCheckDoc = nullptr;
		}
		if (!savedOrigRotates.empty())
		{
			fz_try(pCtx)
			{
				pdf_document* pPdfDoc = pdf_document_from_fz_document(pCtx, pFzDoc);
				if (pPdfDoc)
				{
					for (const auto& origPair : savedOrigRotates)
					{
						pdf_obj* pPageObj = pdf_lookup_page_obj(pCtx, pPdfDoc, origPair.first);
						if (pPageObj)
							pdf_dict_put_int(pCtx, pPageObj, PDF_NAME(Rotate), origPair.second);
					}
				}
			}
			fz_catch(pCtx)
			{
			}
		}
	}
	fz_catch(pCtx)
	{
		const char* pErr = fz_caught_message(pCtx);
		outErrorMsg = pErr ? Utf8ToWide(pErr) : _T("写入 PDF 时发生未知错误");
		tempValid = false;
	}

	if (!tempValid)
	{
		::DeleteFile(tempFileName);
		if (outErrorMsg.IsEmpty())
		{
			outErrorMsg = _T("临时 PDF 文件写入或页数校验失败。");
		}
		return false;
	}

	if (onBeforeReplace)
	{
		onBeforeReplace(targetFilePath);
	}

	const bool moved = ::MoveFileEx(tempFileName, targetFilePath,
		MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
	if (!moved)
	{
		::DeleteFile(tempFileName);
		outErrorMsg = _T("目标 PDF 文件正被其他程序占用或无写入权限，无法覆盖保存。");
	}

	if (onAfterReplace)
	{
		onAfterReplace(targetFilePath, moved);
	}
	return moved;
}

// ============================================================================
// CInlineAnnotEdit 实现（透明输入代理，完整支持搜狗/微软拼音等中文输入法跟随光标）
// ============================================================================

BEGIN_MESSAGE_MAP(CInlineAnnotEdit, CEdit)
	ON_WM_GETDLGCODE()
	ON_WM_ERASEBKGND()
	ON_WM_PAINT()
	ON_WM_KEYDOWN()
	ON_WM_KEYUP()
	ON_WM_CHAR()
	ON_MESSAGE(WM_IME_STARTCOMPOSITION, &CInlineAnnotEdit::OnImeStartComposition)
	ON_MESSAGE(WM_IME_COMPOSITION, &CInlineAnnotEdit::OnImeComposition)
	ON_MESSAGE(WM_PASTE, &CInlineAnnotEdit::OnClipboardOrUndo)
	ON_MESSAGE(WM_CUT, &CInlineAnnotEdit::OnClipboardOrUndo)
	ON_MESSAGE(WM_CLEAR, &CInlineAnnotEdit::OnClipboardOrUndo)
	ON_MESSAGE(WM_UNDO, &CInlineAnnotEdit::OnClipboardOrUndo)
END_MESSAGE_MAP()

bool CInlineAnnotEdit::CreateHiddenProxy(CWnd* pParent, UINT nID)
{
	if (!pParent)
		return false;
	if (GetSafeHwnd())
		return true;

	const DWORD style = WS_CHILD | ES_MULTILINE | ES_WANTRETURN | ES_AUTOVSCROLL | ES_NOHIDESEL;
	if (!Create(style, CRect(0, 0, 80, 28), pParent, nID))
		return false;

	// 使用空裁剪区域屏蔽原生白底绘制与鼠标命中，同时保持输入法与键盘焦点完全可用
	HRGN hEmptyRgn = ::CreateRectRgn(0, 0, 0, 0);
	if (hEmptyRgn)
	{
		if (!SetWindowRgn(hEmptyRgn, FALSE))
			::DeleteObject(hEmptyRgn);
	}
	LimitText(8192);
	return true;
}

void CInlineAnnotEdit::SetCaretAnchorInEdit(CPoint caretPt, int lineHeightPx)
{
	m_caretLocalPt = caretPt;
	m_caretLineHeightPx = max(12, lineHeightPx);
	if (GetSafeHwnd() && ::GetFocus() == GetSafeHwnd())
		syncImeWindowPosition();
}

void CInlineAnnotEdit::syncImeWindowPosition()
{
	HWND hWnd = GetSafeHwnd();
	if (!hWnd)
		return;
	HIMC hImc = ::ImmGetContext(hWnd);
	if (!hImc)
		return;

	COMPOSITIONFORM cf = {};
	cf.dwStyle = CFS_POINT;
	cf.ptCurrentPos = m_caretLocalPt;
	::ImmSetCompositionWindow(hImc, &cf);

	CANDIDATEFORM cand = {};
	cand.dwIndex = 0;
	cand.dwStyle = CFS_EXCLUDE;
	cand.ptCurrentPos = CPoint(m_caretLocalPt.x, m_caretLocalPt.y + m_caretLineHeightPx);
	cand.rcArea = CRect(
		m_caretLocalPt.x,
		m_caretLocalPt.y,
		m_caretLocalPt.x + 2,
		m_caretLocalPt.y + m_caretLineHeightPx + 2);
	::ImmSetCandidateWindow(hImc, &cand);

	::ImmReleaseContext(hWnd, hImc);
}

void CInlineAnnotEdit::notifyChanged()
{
	if (m_onTextOrCaretChanged)
		m_onTextOrCaretChanged();
}

UINT CInlineAnnotEdit::OnGetDlgCode()
{
	return CEdit::OnGetDlgCode() | DLGC_WANTALLKEYS | DLGC_WANTCHARS | DLGC_WANTARROWS;
}

BOOL CInlineAnnotEdit::OnEraseBkgnd(CDC* /*pDC*/)
{
	return TRUE;
}

void CInlineAnnotEdit::OnPaint()
{
	CPaintDC dc(this);
}

void CInlineAnnotEdit::OnKeyDown(UINT nChar, UINT nRepCnt, UINT nFlags)
{
	if (nChar == VK_ESCAPE)
	{
		if (m_onEscapePressed)
			m_onEscapePressed();
		return;
	}

	const bool ctrlDown = (::GetKeyState(VK_CONTROL) & 0x8000) != 0;
	const bool shiftDown = (::GetKeyState(VK_SHIFT) & 0x8000) != 0;
	if (!ctrlDown && m_onVerticalOrLineNav &&
		(nChar == VK_UP || nChar == VK_DOWN || nChar == VK_HOME || nChar == VK_END))
	{
		if (m_onVerticalOrLineNav(nChar, shiftDown))
		{
			notifyChanged();
			return;
		}
	}

	if (ctrlDown && !shiftDown && nChar == 'A')
	{
		SetSel(0, -1);
		notifyChanged();
		return;
	}

	CEdit::OnKeyDown(nChar, nRepCnt, nFlags);
	notifyChanged();
}

void CInlineAnnotEdit::OnKeyUp(UINT nChar, UINT nRepCnt, UINT nFlags)
{
	CEdit::OnKeyUp(nChar, nRepCnt, nFlags);
	notifyChanged();
}

void CInlineAnnotEdit::OnChar(UINT nChar, UINT nRepCnt, UINT nFlags)
{
	if (nChar == VK_ESCAPE)
		return;
	CEdit::OnChar(nChar, nRepCnt, nFlags);
	notifyChanged();
}

LRESULT CInlineAnnotEdit::OnImeStartComposition(WPARAM wParam, LPARAM lParam)
{
	syncImeWindowPosition();
	return DefWindowProc(WM_IME_STARTCOMPOSITION, wParam, lParam);
}

LRESULT CInlineAnnotEdit::OnImeComposition(WPARAM wParam, LPARAM lParam)
{
	syncImeWindowPosition();
	const LRESULT res = DefWindowProc(WM_IME_COMPOSITION, wParam, lParam);
	notifyChanged();
	return res;
}

LRESULT CInlineAnnotEdit::OnClipboardOrUndo(WPARAM wParam, LPARAM lParam)
{
	const LRESULT res = DefWindowProc(GetCurrentMessage()->message, wParam, lParam);
	notifyChanged();
	return res;
}

// ============================================================================
// CPDFAnnotNoteDlg 实现（兼容便签图标与文本高亮附注编辑）
// ============================================================================

BEGIN_MESSAGE_MAP(CPDFAnnotNoteDlg, CDialogEx)
	ON_BN_CLICKED(IDC_ANNOT_STYLE_FREETEXT, &CPDFAnnotNoteDlg::OnStyleChanged)
	ON_BN_CLICKED(IDC_ANNOT_STYLE_ICON, &CPDFAnnotNoteDlg::OnStyleChanged)
END_MESSAGE_MAP()

CPDFAnnotNoteDlg::CPDFAnnotNoteDlg(bool editExistingOnly, const CString& initialText, CWnd* pParent)
	: CDialogEx(IDD_ANNOT_NOTE_DIALOG, pParent)
	, m_freeTextStyle(true)
	, m_color(RGB(0, 114, 206))
	, m_fontSizePt(12.0f)
	, m_noteText(initialText)
	, m_editExistingOnly(editExistingOnly)
{
}

void CPDFAnnotNoteDlg::DoDataExchange(CDataExchange* pDX)
{
	CDialogEx::DoDataExchange(pDX);
	DDX_Control(pDX, IDC_ANNOT_STYLE_FREETEXT, m_radioFreeText);
	DDX_Control(pDX, IDC_ANNOT_STYLE_ICON, m_radioIcon);
	DDX_Control(pDX, IDC_ANNOT_COLOR_COMBO, m_comboColor);
	DDX_Control(pDX, IDC_ANNOT_FONTSIZE_COMBO, m_comboFontSize);
	DDX_Control(pDX, IDC_ANNOT_NOTE_TEXT, m_editNote);
}

BOOL CPDFAnnotNoteDlg::OnInitDialog()
{
	CDialogEx::OnInitDialog();

	SetWindowText(m_editExistingOnly ? _T("编辑批注内容") : _T("添加便签批注"));

	m_radioFreeText.SetCheck(m_freeTextStyle ? BST_CHECKED : BST_UNCHECKED);
	m_radioIcon.SetCheck(!m_freeTextStyle ? BST_CHECKED : BST_UNCHECKED);

	m_comboColor.AddString(_T("批注红（醒目审阅）"));
	m_comboColor.AddString(_T("深海蓝（工程批注）"));
	m_comboColor.AddString(_T("经典黑（正文补注）"));
	m_comboColor.AddString(_T("琥珀金（便签图标）"));
	m_comboColor.SetCurSel(1);

	m_comboFontSize.AddString(_T("10 pt（小号）"));
	m_comboFontSize.AddString(_T("12 pt（标准）"));
	m_comboFontSize.AddString(_T("14 pt（中号）"));
	m_comboFontSize.AddString(_T("18 pt（大号）"));
	m_comboFontSize.SetCurSel(1);

	if (m_editExistingOnly)
	{
		m_radioFreeText.EnableWindow(FALSE);
		m_radioIcon.EnableWindow(FALSE);
		m_comboColor.EnableWindow(FALSE);
		m_comboFontSize.EnableWindow(FALSE);
	}
	else
	{
		updateStyleControls();
	}

	m_editNote.SetWindowText(m_noteText);
	m_editNote.SetFocus();
	m_editNote.SetSel(m_noteText.GetLength(), m_noteText.GetLength());
	return FALSE;
}

void CPDFAnnotNoteDlg::OnStyleChanged()
{
	updateStyleControls();
}

void CPDFAnnotNoteDlg::updateStyleControls()
{
	if (m_editExistingOnly)
		return;
	const bool isFreeText = (m_radioFreeText.GetCheck() == BST_CHECKED);
	m_comboFontSize.EnableWindow(isFreeText);
	if (!isFreeText && m_comboColor.GetCurSel() == 0)
	{
		m_comboColor.SetCurSel(3);
	}
	else if (isFreeText && m_comboColor.GetCurSel() == 3)
	{
		m_comboColor.SetCurSel(0);
	}
}

void CPDFAnnotNoteDlg::OnOK()
{
	m_editNote.GetWindowText(m_noteText);
	CString trimmed = m_noteText;
	trimmed.Trim();
	if (trimmed.IsEmpty() && !m_editExistingOnly)
	{
		MessageBox(_T("请输入批注内容。"), _T("提示"), MB_OK | MB_ICONINFORMATION);
		m_editNote.SetFocus();
		return;
	}

	m_freeTextStyle = (m_radioFreeText.GetCheck() == BST_CHECKED);

	switch (m_comboColor.GetCurSel())
	{
	case 1:
		m_color = RGB(0, 114, 206);
		break;
	case 2:
		m_color = RGB(30, 33, 38);
		break;
	case 3:
		m_color = RGB(245, 192, 24);
		break;
	default:
		m_color = RGB(210, 35, 35);
		break;
	}

	const float sizes[] = { 10.0f, 12.0f, 14.0f, 18.0f };
	const int sizeIdx = m_comboFontSize.GetCurSel();
	m_fontSizePt = (sizeIdx >= 0 && sizeIdx < static_cast<int>(_countof(sizes))) ?
		sizes[sizeIdx] : 12.0f;

	CDialogEx::OnOK();
}
