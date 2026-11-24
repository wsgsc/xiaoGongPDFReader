#include "pch.h"
#include "framework.h"
#include <string>
#include <vector>
#include <mupdf/fitz.h>
#include "resource.h"
#include "PDFDocument.h"
#include "PDFSearch.h"

#ifdef _DEBUG
#define new DEBUG_NEW
#endif

namespace
{
	struct SearchHitCollector
	{
		int pageNumber;
		std::vector<SearchMatch>* pResults;
	};

	int CollectSearchHit(fz_context*, void* opaque, int count, fz_quad* quads)
	{
		if (count <= 0)
			return 0;
		try
		{
			SearchHitCollector* pCollector = static_cast<SearchHitCollector*>(opaque);
			fz_rect bounds = fz_rect_from_quad(quads[0]);
			for (int i = 1; i < count; ++i)
				bounds = fz_union_rect(bounds, fz_rect_from_quad(quads[i]));
			SearchMatch match;
			match.pageNumber = pCollector->pageNumber;
			match.quad = fz_quad_from_rect(bounds);
			pCollector->pResults->push_back(match);
			return 0;
		}
		catch (...)
		{
			return 1;
		}
	}

	CString BuildSearchContext(fz_stext_page* pTextPage, const fz_quad& quad)
	{
		const fz_rect hit = fz_rect_from_quad(quad);
		for (fz_stext_block* pBlock = pTextPage->first_block; pBlock; pBlock = pBlock->next)
		{
			if (pBlock->type != FZ_STEXT_BLOCK_TEXT)
				continue;
			for (fz_stext_line* pLine = pBlock->u.t.first_line; pLine; pLine = pLine->next)
			{
				if (pLine->bbox.x1 < hit.x0 || pLine->bbox.x0 > hit.x1 ||
					pLine->bbox.y1 < hit.y0 || pLine->bbox.y0 > hit.y1)
					continue;
				std::vector<int> runes;
				int hitIndex = -1;
				for (fz_stext_char* pCh = pLine->first_char; pCh; pCh = pCh->next)
				{
					const fz_rect charRect = fz_rect_from_quad(pCh->quad);
					if (hitIndex < 0 && charRect.x1 >= hit.x0 && charRect.x0 <= hit.x1 &&
						charRect.y1 >= hit.y0 && charRect.y0 <= hit.y1)
						hitIndex = static_cast<int>(runes.size());
					runes.push_back(pCh->c);
				}
				if (runes.empty())
					continue;
				const int start = max(0, (hitIndex < 0 ? 0 : hitIndex) - 24);
				const int end = min(static_cast<int>(runes.size()),
					(hitIndex < 0 ? 0 : hitIndex) + 56);
				std::string utf8;
				if (start > 0)
					utf8 = "...";
				for (int i = start; i < end; ++i)
				{
					char encoded[8] = {};
					const int length = fz_runetochar(encoded, runes[i]);
					if (length > 0)
						utf8.append(encoded, length);
				}
				if (end < static_cast<int>(runes.size()))
					utf8 += "...";
				CString context(CA2W(utf8.c_str(), CP_UTF8));
				context.Replace(_T("\t"), _T(" "));
				context.Trim();
				return context;
			}
		}
		return CString();
	}

	void FindCaseSensitiveHits(fz_stext_page* pTextPage, const std::vector<int>& needle,
		int pageNumber, std::vector<SearchMatch>& results)
	{
		if (needle.empty())
			return;
		for (fz_stext_block* pBlock = pTextPage->first_block; pBlock; pBlock = pBlock->next)
		{
			if (pBlock->type != FZ_STEXT_BLOCK_TEXT)
				continue;
			for (fz_stext_line* pLine = pBlock->u.t.first_line; pLine; pLine = pLine->next)
			{
				for (fz_stext_char* pStart = pLine->first_char; pStart; pStart = pStart->next)
				{
					fz_stext_char* pCurrent = pStart;
					fz_rect bounds = fz_rect_from_quad(pStart->quad);
					size_t matched = 0;
					while (pCurrent && matched < needle.size() && pCurrent->c == needle[matched])
					{
						bounds = fz_union_rect(bounds, fz_rect_from_quad(pCurrent->quad));
						pCurrent = pCurrent->next;
						++matched;
					}
					if (matched == needle.size())
					{
						SearchMatch match;
						match.pageNumber = pageNumber;
						match.quad = fz_quad_from_rect(bounds);
						results.push_back(match);
					}
				}
			}
		}
	}
}

UINT PDFSearchThreadProc(LPVOID pParam)
{
	PDFSearchParams* pParams = static_cast<PDFSearchParams*>(pParam);
	if (!pParams)
		return 1;

	constexpr size_t SEARCH_STORE_LIMIT = 128u * 1024u * 1024u;
	fz_context* ctx = fz_new_context(nullptr, GetMuPDFLocksContext(), SEARCH_STORE_LIMIT);
	if (!ctx)
		pParams->failed = true;

	fz_document* doc = nullptr;
	fz_var(doc);
	const char* keyword = pParams->utf8Keyword.data();
	std::vector<int> exactNeedle;
	if (pParams->caseSensitive)
	{
		for (const char* p = keyword; *p; )
		{
			int rune = 0;
			p += fz_chartorune(&rune, p);
			exactNeedle.push_back(rune);
		}
	}

	if (ctx)
	{
		fz_try(ctx)
		{
			fz_register_document_handlers(ctx);
			if (!pParams->cancelled.load())
				doc = fz_open_document(ctx, pParams->utf8Path.data());
			if (doc && fz_needs_password(ctx, doc) &&
				(pParams->utf8Password.empty() ||
				 !fz_authenticate_password(ctx, doc, pParams->utf8Password.data())))
			{
				pParams->failed = true;
			}
		}
		fz_catch(ctx)
		{
			pParams->failed = true;
		}
	}

	if (!doc && !pParams->cancelled.load())
		pParams->failed = true;

	if (doc && !pParams->failed)
	{
		int totalPages = 0;
		fz_var(totalPages);
		fz_try(ctx)
		{
			totalPages = fz_count_pages(ctx, doc);
		}
		fz_catch(ctx)
		{
			totalPages = 0;
			pParams->failed = true;
		}

		for (int i = 0; i < totalPages && !pParams->cancelled.load(); ++i)
		{
			fz_page* pPage = nullptr;
			fz_stext_page* pStext = nullptr;
			fz_var(pPage);
			fz_var(pStext);
			const size_t firstResult = pParams->results.size();

			fz_try(ctx)
			{
				pPage = fz_load_page(ctx, doc, i);
				pStext = fz_new_stext_page_from_page(ctx, pPage, nullptr);
				if (!pParams->caseSensitive)
				{
					SearchHitCollector collector{ i, &pParams->results };
					fz_search_stext_page_cb(ctx, pStext, keyword, CollectSearchHit, &collector);
				}
			}
			fz_catch(ctx)
			{
				// 单页损坏不影响其他页面的搜索。
			}

			if (pStext && !pParams->cancelled.load())
			{
				if (pParams->caseSensitive)
					FindCaseSensitiveHits(pStext, exactNeedle, i, pParams->results);
				for (size_t resultIndex = firstResult; resultIndex < pParams->results.size(); ++resultIndex)
				{
					pParams->results[resultIndex].context =
						BuildSearchContext(pStext, pParams->results[resultIndex].quad);
				}
			}
			if (pStext)
				fz_drop_stext_page(ctx, pStext);
			if (pPage)
				fz_drop_page(ctx, pPage);
			if (i % 10 == 0 && pParams->notifyWnd)
			{
				::PostMessage(pParams->notifyWnd, WM_SEARCH_PROGRESS,
					pParams->taskId, static_cast<LPARAM>(i + 1));
			}
		}
	}

	if (doc)
		fz_drop_document(ctx, doc);
	if (ctx)
		fz_drop_context(ctx);

	if (pParams->notifyWnd)
		::PostMessage(pParams->notifyWnd, WM_SEARCH_COMPLETE, pParams->taskId, (LPARAM)pParams);

	return 0;
}
