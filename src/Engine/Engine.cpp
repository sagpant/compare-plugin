/*
 * This file is part of Compare plugin for Notepad++
 * Copyright (C)2011 Jean-Sebastien Leroy (jean.sebastien.leroy@gmail.com)
 * Copyright (C)2017-2018 Pavel Nedev (pg.nedev@gmail.com)
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <climits>
#include <exception>
#include <cstdint>
#include <utility>
#include <set>
#include <unordered_map>
#include <map>
#include <algorithm>

#include <windows.h>

#include "Engine.h"
#include "diff.h"
#include "ProgressDlg.h"


namespace {

enum class charType
{
	SPACECHAR,
	ALPHANUMCHAR,
	OTHERCHAR
};


struct DocCmpInfo
{
	int			view;
	section_t	section;

	int			blockDiffMask;
};


struct diffLine
{
	diffLine(int lineNum) : line(lineNum) {}

	int line;
	std::vector<section_t> changes;
};


struct blockDiffInfo
{
	const diff_info<blockDiffInfo>*	matchBlock {nullptr};

	std::vector<diffLine>					changedLines;

	// If the section is moved (matched equal times in both files) the bool is true
	std::vector<std::pair<section_t, bool>>	matches;

	inline int matchedSection(int line, bool& isMoved) const
	{
		for (const auto& match: matches)
		{
			if (line >= match.first.off && line <= match.first.off + match.first.len - 1)
			{
				isMoved = match.second;

				return match.first.len;
			}
		}

		return 0;
	}
};


using diffInfo = diff_info<blockDiffInfo>;


struct CompareInfo
{
	// Input data
	DocCmpInfo				doc1;
	DocCmpInfo				doc2;

	bool					selectionCompare;

	// Output data - filled by the compare engine
	std::vector<diffInfo>	blockDiffs;
};


struct MatchInfo
{
	section_t								sec;
	std::vector<std::pair<diffInfo*, int>>	matchesIn1;
	std::vector<std::pair<diffInfo*, int>>	matchesIn2;
};


struct Word
{
	charType type;

	int line;
	int pos;
	int length;

	uint64_t hash;

	inline bool operator==(const Word& rhs) const
	{
		return (hash == rhs.hash);
	}

	inline bool operator!=(const Word& rhs) const
	{
		return (hash != rhs.hash);
	}

	inline bool operator==(uint64_t rhs) const
	{
		return (hash == rhs);
	}

	inline bool operator!=(uint64_t rhs) const
	{
		return (hash != rhs);
	}
};


const uint64_t cHashSeed = 0x84222325;

inline uint64_t Hash(uint64_t hval, char letter)
{
	hval ^= static_cast<uint64_t>(letter);

	hval += (hval << 1) + (hval << 4) + (hval << 5) + (hval << 7) + (hval << 8) + (hval << 40);

	return hval;
}


std::vector<uint64_t> computeLineHashes(DocCmpInfo& doc, const UserSettings& settings)
{
	const int monitorCancelEveryXLine = 500;

	progress_ptr& progress = ProgressDlg::Get();

	int lineCount = CallScintilla(doc.view, SCI_GETLENGTH, 0, 0);

	if (lineCount)
		lineCount = CallScintilla(doc.view, SCI_GETLINECOUNT, 0, 0);

	if ((doc.section.len <= 0) || (doc.section.off + doc.section.len > lineCount))
		doc.section.len = lineCount - doc.section.off;

	if (progress)
		progress->SetMaxCount((doc.section.len / monitorCancelEveryXLine) + 1);

	std::vector<uint64_t> lineHashes(doc.section.len, cHashSeed);

	for (int lineNum = 0; lineNum < doc.section.len; ++lineNum)
	{
		if (progress && (lineNum % monitorCancelEveryXLine == 0) && !progress->Advance())
			return std::vector<uint64_t>{};

		const int lineStart = CallScintilla(doc.view, SCI_POSITIONFROMLINE, lineNum + doc.section.off, 0);
		const int lineEnd = CallScintilla(doc.view, SCI_GETLINEENDPOSITION, lineNum + doc.section.off, 0);

		if (lineEnd - lineStart)
		{
			std::vector<char> line = getText(doc.view, lineStart, lineEnd);
			const int lineLen = static_cast<int>(line.size()) - 1;

			if (settings.IgnoreCase)
				toLowerCase(line);

			for (int i = 0; i < lineLen; ++i)
			{
				if (settings.IgnoreSpaces && (line[i] == ' ' || line[i] == '\t'))
					continue;

				lineHashes[lineNum] = Hash(lineHashes[lineNum], line[i]);
			}
		}
	}

	if (doc.section.len && lineHashes.back() == cHashSeed)
	{
		lineHashes.pop_back();
		--doc.section.len;
	}

	return lineHashes;
}


charType getCharType(char letter)
{
	if (letter == ' ' || letter == '\t')
		return charType::SPACECHAR;

	if (::IsCharAlphaNumericA(letter) || letter == '_')
		return charType::ALPHANUMCHAR;

	return charType::OTHERCHAR;
}


std::vector<std::vector<Word>> getWords(int lineOffset, int lineCount, int view, const UserSettings& settings)
{
	std::vector<std::vector<Word>> words(lineCount);

	for (int lineNum = 0; lineNum < lineCount; ++lineNum)
	{
		const int docLineNum = lineNum + lineOffset;
		const int docLineStart = CallScintilla(view, SCI_POSITIONFROMLINE, docLineNum, 0);
		const int docLineEnd = CallScintilla(view, SCI_GETLINEENDPOSITION, docLineNum, 0);

		std::vector<char> line = getText(view, docLineStart, docLineEnd);
		const int lineLen = static_cast<int>(line.size()) - 1;

		if (lineLen > 0)
		{
			if (settings.IgnoreCase)
				toLowerCase(line);

			Word word;
			word.type = getCharType(line[0]);
			word.hash = Hash(cHashSeed, line[0]);
			word.line = lineNum;
			word.pos = 0;
			word.length = 1;

			for (int i = 1; i < lineLen; ++i)
			{
				charType newType = getCharType(line[i]);

				if (newType == word.type)
				{
					++word.length;
					word.hash = Hash(word.hash, line[i]);
				}
				else
				{
					if (!settings.IgnoreSpaces || word.type != charType::SPACECHAR)
						words[lineNum].push_back(word);

					word.type = newType;
					word.hash = Hash(cHashSeed, line[i]);
					word.pos = i;
					word.length = 1;
				}
			}

			if (!settings.IgnoreSpaces || word.type != charType::SPACECHAR)
				words[lineNum].push_back(word);
		}
	}

	return words;
}


// Scan for best matching blocks in 2 (containing ei1 element).
void findMatches(CompareInfo& cmpInfo,
		const std::vector<uint64_t>& lineHashes1, const std::vector<uint64_t>& lineHashes2,
		const diffInfo& diff1, const int ei1, MatchInfo& mi)
{
	mi.sec.len = 0;
	mi.matchesIn1.clear();
	mi.matchesIn2.clear();

	int minMatchLen = 1;

	for (diffInfo& diff2: cmpInfo.blockDiffs)
	{
		if (diff2.type != diff_type::DIFF_IN_2)
			continue;

		for (int ei2 = 0; diff2.len - ei2 >= minMatchLen; ++ei2)
		{
			if (lineHashes1[diff1.off + ei1] != lineHashes2[diff2.off + ei2])
				continue;

			bool isMoved;
			int matchedLen = diff2.info.matchedSection(ei2, isMoved);

			if (matchedLen)
			{
				ei2 += (matchedLen - 1);
				continue;
			}

			int start1	= ei1 - 1;
			int end1	= ei1 + 1;
			int start2	= ei2 - 1;

			// Check for the beginning of the matched block (containing ei1 element).
			for (; start1 >= 0 && start2 >= 0 && !diff2.info.matchedSection(start2, isMoved) &&
					lineHashes1[diff1.off + start1] == lineHashes2[diff2.off + start2]; --start1, --start2);

			// Check for the end of the matched block (containing ei1 element).
			for (int end2 = ei2 + 1; end1 < diff1.len && end2 < diff2.len &&
					!diff2.info.matchedSection(end2, isMoved) &&
					lineHashes1[diff1.off + end1] == lineHashes2[diff2.off + end2]; ++end1, ++end2);

			++start1;
			++start2;
			--end1;

			const int matchLen = end1 - start1 + 1;

			if (mi.sec.len > matchLen)
				continue;

			if (mi.sec.len < matchLen)
			{
				mi.sec.off = start1;
				mi.sec.len = matchLen;
				mi.matchesIn2.clear();
				minMatchLen = matchLen;
			}

			if (mi.sec.len == matchLen)
			{
				mi.matchesIn2.emplace_back(&diff2, start2);
				ei2 = start2 + matchLen - 1;
			}
		}
	}
}


// Scan for better matching sub-block in 1 (containing ei element).
void findBetterMatch(CompareInfo& cmpInfo,
		const std::vector<uint64_t>& lineHashes1, const std::vector<uint64_t>& lineHashes2,
		diffInfo& diff, int& ei, diffInfo*& bestMatchDiff, MatchInfo& best_mi)
{
	if (!bestMatchDiff)
		return;

	int i = (&diff == bestMatchDiff) ? best_mi.sec.off + best_mi.sec.len : 0;

	for (; diff.len - i >= best_mi.sec.len; ++i)
	{
		// Skip to the first matching element
		if (lineHashes1[diff.off + i] != lineHashes1[bestMatchDiff->off + ei])
			continue;

		bool isMoved;
		int matchedLen = diff.info.matchedSection(i, isMoved);

		// Skip already detected matches
		if (matchedLen)
		{
			i += (matchedLen - 1);
			continue;
		}

		MatchInfo mi;

		findMatches(cmpInfo, lineHashes1, lineHashes2, diff, i, mi);

		if (mi.sec.len == 0)
			continue;

		// The alternative match is actually better - the length of the matching block is bigger
		if (best_mi.sec.len < mi.sec.len)
		{
			bestMatchDiff = &diff;
			best_mi = mi;
			ei = i;
			i = mi.sec.off + mi.sec.len - 1;
		}
		// Both matching blocks are of equal size - check if those are actually one and the same block
		else if (best_mi.sec.len == mi.sec.len)
		{
			int k = 0;

			for (; k < mi.sec.len &&
					lineHashes1[bestMatchDiff->off + best_mi.sec.off + k] ==
					lineHashes1[diff.off + mi.sec.off + k]; ++k);

			if (k == mi.sec.len)
			{
				best_mi.matchesIn1.emplace_back(&diff, mi.sec.off);
				i = mi.sec.off + mi.sec.len - 1;
			}
		}
	}
}


void findMoves(CompareInfo& cmpInfo,
		const std::vector<uint64_t>& lineHashes1, const std::vector<uint64_t>& lineHashes2)
{
	const int diffSize = static_cast<int>(cmpInfo.blockDiffs.size());

	for (int di1 = 0; di1 < diffSize; ++di1)
	{
		if (cmpInfo.blockDiffs[di1].type != diff_type::DIFF_IN_1)
			continue;

		// Go through all diff1's elements and check if each is matched
		for (int ei1 = 0; ei1 < cmpInfo.blockDiffs[di1].len; ++ei1)
		{
			bool isMoved;
			int matchedLen = cmpInfo.blockDiffs[di1].info.matchedSection(ei1, isMoved);

			// Skip already detected matches
			if (matchedLen)
			{
				ei1 += (matchedLen - 1);
				continue;
			}

			// Skip blanks
			if (lineHashes1[cmpInfo.blockDiffs[di1].off + ei1] == cHashSeed)
				continue;

			diffInfo*	bestMatchDiff = &(cmpInfo.blockDiffs[di1]);
			MatchInfo	best_mi;

			findMatches(cmpInfo, lineHashes1, lineHashes2, *bestMatchDiff, ei1, best_mi);

			if (best_mi.sec.len == 0)
				continue;

			int bmi = ei1;

			// Search for the same element in the same block - potential better or equal match
			findBetterMatch(cmpInfo, lineHashes1, lineHashes2, *bestMatchDiff, bmi, bestMatchDiff, best_mi);

			// Search for the same element in different diff1 block - potential better match
			for (int di2 = di1 + 1; di2 < diffSize; ++di2)
			{
				if (cmpInfo.blockDiffs[di2].type == diff_type::DIFF_IN_1)
					findBetterMatch(cmpInfo, lineHashes1, lineHashes2, cmpInfo.blockDiffs[di2], bmi,
							bestMatchDiff, best_mi);
			}

			isMoved = ((best_mi.matchesIn1.size() + 1 == best_mi.matchesIn2.size()) &&
					!((best_mi.sec.len == 1) && (best_mi.matchesIn2.size() > 1)));

			bestMatchDiff->info.matches.emplace_back(
					std::make_pair(section_t(best_mi.sec.off, best_mi.sec.len), isMoved));

			for (auto& match: best_mi.matchesIn1)
				match.first->info.matches.emplace_back(
						std::make_pair(section_t(match.second, best_mi.sec.len), isMoved));

			for (auto& match: best_mi.matchesIn2)
				match.first->info.matches.emplace_back(
						std::make_pair(section_t(match.second, best_mi.sec.len), isMoved));

			// If the best element matching block is the current then skip checks to the end of the match block
			if ((bestMatchDiff == &(cmpInfo.blockDiffs[di1])) && (bmi == ei1))
				ei1 = best_mi.sec.off + best_mi.sec.len - 1;
			// Otherwise the current element is still not matched - recheck it
			else
				--ei1;
		}
	}
}


void compareLines(diffInfo& blockDiff1, diffInfo& blockDiff2,
		const std::vector<std::vector<Word>>& chunk1, const std::vector<std::vector<Word>>& chunk2,
		const std::map<int, std::pair<int, int>>& lineMappings)
{
	int lastLine2 = -1;

	for (const auto& lm: lineMappings)
	{
		diffInfo* pBlockDiff1 = &blockDiff1;
		diffInfo* pBlockDiff2 = &blockDiff2;

		// lines1 are stored in ascending order and to have a match lines2 must also be in ascending order
		if (lm.second.second <= lastLine2)
			continue;

		int line1 = lm.first;
		int line2 = lm.second.second;

		lastLine2 = line2;

		const std::vector<Word>* pLine1 = &chunk1[line1];
		const std::vector<Word>* pLine2 = &chunk2[line2];

		if (pLine1->size() < pLine2->size())
		{
			std::swap(pBlockDiff1, pBlockDiff2);
			std::swap(pLine1, pLine2);
			std::swap(line1, line2);
		}

		const std::vector<diff_info<void>> linesDiff = DiffCalc<Word>(*pLine1, *pLine2)();
		if (linesDiff.size() == 1 && linesDiff[0].type == diff_type::DIFF_MATCH)
			continue;

		pBlockDiff1->info.changedLines.emplace_back(line1);
		pBlockDiff2->info.changedLines.emplace_back(line2);

		for (const auto& ld: linesDiff)
		{
			if (ld.type == diff_type::DIFF_IN_1)
			{
				section_t change;

				change.off = (*pLine1)[ld.off].pos;
				change.len = (*pLine1)[ld.off + ld.len - 1].pos - change.off + (*pLine1)[ld.off + ld.len - 1].length;

				pBlockDiff1->info.changedLines.back().changes.emplace_back(change);
			}
			else if (ld.type == diff_type::DIFF_IN_2)
			{
				section_t change;

				change.off = (*pLine2)[ld.off].pos;
				change.len = (*pLine2)[ld.off + ld.len - 1].pos - change.off + (*pLine2)[ld.off + ld.len - 1].length;

				pBlockDiff2->info.changedLines.back().changes.emplace_back(change);
			}
		}
	}
}


void compareBlocks(const DocCmpInfo& doc1, const DocCmpInfo& doc2, const UserSettings& settings,
		diffInfo& blockDiff1, diffInfo& blockDiff2)
{
	const std::vector<std::vector<Word>> chunk1 = getWords(blockDiff1.off, blockDiff1.len, doc1.view, settings);
	const std::vector<std::vector<Word>> chunk2 = getWords(blockDiff2.off, blockDiff2.len, doc2.view, settings);

	const int linesCount1 = static_cast<int>(chunk1.size());
	const int linesCount2 = static_cast<int>(chunk2.size());

	struct conv_key
	{
		int convergence;
		int line1;
		int line2;

		conv_key(int c, int l1, int l2) : convergence(c), line1(l1), line2(l2)
		{}

		bool operator<(const conv_key& rhs) const
		{
			return ((convergence > rhs.convergence) ||
					((convergence == rhs.convergence) && ((line1 < rhs.line1) ||
						((line1 == rhs.line1) && ((line2 < rhs.line2))))));
		}
	};

	std::set<conv_key> orderedLinesConvergence;

	for (int line1 = 0; line1 < linesCount1; ++line1)
	{
		if (chunk1[line1].empty())
			continue;

		bool isMoved;
		int matchedLen = blockDiff1.info.matchedSection(line1, isMoved);

		if (matchedLen && isMoved)
		{
			line1 += (matchedLen - 1);
			continue;
		}

		int line1Len = 0;
		for (const auto& word: chunk1[line1])
			line1Len += word.length;

		for (int line2 = 0; line2 < linesCount2; ++line2)
		{
			if (chunk2[line2].empty())
				continue;

			matchedLen = blockDiff2.info.matchedSection(line2, isMoved);

			if (matchedLen && isMoved)
			{
				line2 += (matchedLen - 1);
				continue;
			}

			const std::vector<Word>* pLine1 = &chunk1[line1];
			const std::vector<Word>* pLine2 = &chunk2[line2];

			if (pLine1->size() < pLine2->size())
				std::swap(pLine1, pLine2);

			if (pLine1->size() > 2 * pLine2->size())
				continue;

			const std::vector<diff_info<void>> linesDiff = DiffCalc<Word>(*pLine1, *pLine2)();

			int line2Len = 0;
			for (const auto& word: chunk2[line2])
				line2Len += word.length;

			if (line1Len < line2Len)
				line1Len = line2Len;

			int linesConvergence = 0;		// Convergence by words count
			int linesLenConvergence = 0;	// Convergence by length (symbols/chars count)

			for (const auto& ld: linesDiff)
			{
				if (ld.type == diff_type::DIFF_MATCH)
				{
					linesConvergence += ld.len;

					for (int i = 0; i < ld.len; ++i)
						linesLenConvergence += (*pLine1)[ld.off + i].length;
				}
			}

			linesConvergence = linesConvergence * 100 / pLine1->size();
			linesLenConvergence = linesLenConvergence * 100 / line1Len;

			// Take the better convergence of the two
			if (linesConvergence < linesLenConvergence)
				linesConvergence = linesLenConvergence;

			if (linesConvergence >= 50)
				orderedLinesConvergence.emplace(conv_key(linesConvergence, line1, line2));
		}
	}

	std::map<int, std::pair<int, int>> bestLineMappings;
	int bestBlockConvergence = 0;

	for (auto startItr = orderedLinesConvergence.begin(); startItr != orderedLinesConvergence.end(); ++startItr)
	{
		std::map<int, std::pair<int, int>> lineMappings;

		std::vector<bool> mappedLines1(linesCount1, false);
		std::vector<bool> mappedLines2(linesCount2, false);

		int mappedLinesCount1 = 0;
		int mappedLinesCount2 = 0;

		for (auto ocItr = startItr; ocItr != orderedLinesConvergence.end(); ++ocItr)
		{
			if (!mappedLines1[ocItr->line1] && !mappedLines2[ocItr->line2])
			{
				lineMappings.emplace(ocItr->line1, std::pair<int, int>(ocItr->convergence, ocItr->line2));

				if ((++mappedLinesCount1 == linesCount1) || (++mappedLinesCount2 == linesCount2))
					break;

				mappedLines1[ocItr->line1] = true;
				mappedLines2[ocItr->line2] = true;
			}
		}

		int currentBlockConvergence = 0;
		int lastLine2 = -1;

		for (const auto& lm: lineMappings)
		{
			// lines1 are stored in ascending order and to have a match lines2 must also be in ascending order
			if (lm.second.second > lastLine2)
			{
				currentBlockConvergence += lm.second.first;
				lastLine2 = lm.second.second;
			}
		}

		if (bestBlockConvergence < currentBlockConvergence)
		{
			bestBlockConvergence = currentBlockConvergence;
			bestLineMappings = std::move(lineMappings);
		}
	}

	if (!bestLineMappings.empty())
		compareLines(blockDiff1, blockDiff2, chunk1, chunk2, bestLineMappings);

	return;
}


void markSection(const diffInfo& bd, const DocCmpInfo& doc)
{
	const int endOff = doc.section.off + doc.section.len;

	for (int i = doc.section.off, line = bd.off + doc.section.off; i < endOff; ++i, ++line)
	{
		bool isMoved = false;
		int matchedLen = bd.info.matchedSection(i, isMoved);

		if (matchedLen > doc.section.len)
			matchedLen = doc.section.len;

		if (matchedLen == 0)
		{
			CallScintilla(doc.view, SCI_MARKERADDSET, line, doc.blockDiffMask);
		}
		else
		{
			if (!isMoved)
			{
				const int mark =
					(doc.blockDiffMask == MARKER_MASK_ADDED) ? MARKER_MASK_ADDED_LOCAL : MARKER_MASK_REMOVED_LOCAL;

				for (int j = 0; j < matchedLen; ++j, ++line)
					CallScintilla(doc.view, SCI_MARKERADDSET, line, mark);

				--line;
				i += (matchedLen - 1);
			}
			else if (matchedLen == 1)
			{
				CallScintilla(doc.view, SCI_MARKERADDSET, line, MARKER_MASK_MOVED_LINE);
			}
			else
			{
				CallScintilla(doc.view, SCI_MARKERADDSET, line, MARKER_MASK_MOVED_BEGIN);
				++line;
				--matchedLen;

				for (int j = 1; j < matchedLen; ++j, ++line)
					CallScintilla(doc.view, SCI_MARKERADDSET, line, MARKER_MASK_MOVED_MID);

				CallScintilla(doc.view, SCI_MARKERADDSET, line, MARKER_MASK_MOVED_END);
				i += matchedLen;
			}
		}
	}
}


void markLineDiffs(int view1, int view2, const diffInfo& bd, int lineIdx)
{
	int line = bd.off + bd.info.changedLines[lineIdx].line;
	int linePos = CallScintilla(view1, SCI_POSITIONFROMLINE, line, 0);

	for (const auto& change: bd.info.changedLines[lineIdx].changes)
		markTextAsChanged(view1, linePos + change.off, change.len);

	CallScintilla(view1, SCI_MARKERADDSET, line, MARKER_MASK_CHANGED);

	line = bd.info.matchBlock->off + bd.info.matchBlock->info.changedLines[lineIdx].line;
	linePos = CallScintilla(view2, SCI_POSITIONFROMLINE, line, 0);

	for (const auto& change: bd.info.matchBlock->info.changedLines[lineIdx].changes)
		markTextAsChanged(view2, linePos + change.off, change.len);

	CallScintilla(view2, SCI_MARKERADDSET, line, MARKER_MASK_CHANGED);
}


bool markAllDiffs(CompareInfo& cmpInfo, AlignmentInfo_t& alignmentInfo)
{
	progress_ptr& progress = ProgressDlg::Get();

	alignmentInfo.clear();

	const int blockDiffSize = static_cast<int>(cmpInfo.blockDiffs.size());

	if (progress)
		progress->SetMaxCount(blockDiffSize);

	AlignmentPair alignPair;

	AlignmentViewData* pMainAlignData	= &alignPair.main;
	AlignmentViewData* pSubAlignData	= &alignPair.sub;

	// Make sure pMainAlignData is linked to doc1
	if (cmpInfo.doc1.view == SUB_VIEW)
		std::swap(pMainAlignData, pSubAlignData);

	pMainAlignData->line	= cmpInfo.doc1.section.off;
	pSubAlignData->line		= cmpInfo.doc2.section.off;

	for (int i = 0; i < blockDiffSize; ++i)
	{
		const diffInfo& bd = cmpInfo.blockDiffs[i];

		if (bd.type == diff_type::DIFF_MATCH)
		{
			pMainAlignData->diffMask	= 0;
			pSubAlignData->diffMask		= 0;

			alignmentInfo.push_back(alignPair);

			pMainAlignData->line	+= bd.len;
			pSubAlignData->line		+= bd.len;
		}
		else if (bd.type == diff_type::DIFF_IN_2)
		{
			cmpInfo.doc2.section.off = 0;
			cmpInfo.doc2.section.len = bd.len;
			markSection(bd, cmpInfo.doc2);

			pMainAlignData->diffMask	= 0;
			pSubAlignData->diffMask		= cmpInfo.doc2.blockDiffMask;

			alignmentInfo.push_back(alignPair);

			pSubAlignData->line += bd.len;
		}
		else if (bd.type == diff_type::DIFF_IN_1)
		{
			if (bd.info.matchBlock)
			{
				const int changedLinesCount = static_cast<int>(bd.info.changedLines.size());

				cmpInfo.doc1.section.off = 0;
				cmpInfo.doc2.section.off = 0;

				for (int j = 0; j < changedLinesCount; ++j)
				{
					cmpInfo.doc1.section.len = bd.info.changedLines[j].line - cmpInfo.doc1.section.off;
					cmpInfo.doc2.section.len = bd.info.matchBlock->info.changedLines[j].line - cmpInfo.doc2.section.off;

					if (cmpInfo.doc1.section.len || cmpInfo.doc2.section.len)
					{
						pMainAlignData->diffMask	= cmpInfo.doc1.blockDiffMask;
						pSubAlignData->diffMask		= cmpInfo.doc2.blockDiffMask;

						alignmentInfo.push_back(alignPair);

						if (cmpInfo.doc1.section.len)
						{
							markSection(bd, cmpInfo.doc1);
							pMainAlignData->line += cmpInfo.doc1.section.len;
						}

						if (cmpInfo.doc2.section.len)
						{
							markSection(*bd.info.matchBlock, cmpInfo.doc2);
							pSubAlignData->line += cmpInfo.doc2.section.len;
						}
					}

					pMainAlignData->diffMask	= MARKER_MASK_CHANGED;
					pSubAlignData->diffMask		= MARKER_MASK_CHANGED;

					alignmentInfo.push_back(alignPair);

					markLineDiffs(cmpInfo.doc1.view, cmpInfo.doc2.view, bd, j);

					cmpInfo.doc1.section.off = bd.info.changedLines[j].line + 1;
					cmpInfo.doc2.section.off = bd.info.matchBlock->info.changedLines[j].line + 1;

					++(pMainAlignData->line);
					++(pSubAlignData->line);
				}

				cmpInfo.doc1.section.len = bd.len - cmpInfo.doc1.section.off;
				cmpInfo.doc2.section.len = bd.info.matchBlock->len - cmpInfo.doc2.section.off;

				if (cmpInfo.doc1.section.len || cmpInfo.doc2.section.len)
				{
					pMainAlignData->diffMask	= cmpInfo.doc1.blockDiffMask;
					pSubAlignData->diffMask		= cmpInfo.doc2.blockDiffMask;

					alignmentInfo.push_back(alignPair);

					if (cmpInfo.doc1.section.len)
					{
						markSection(bd, cmpInfo.doc1);
						pMainAlignData->line += cmpInfo.doc1.section.len;
					}

					if (cmpInfo.doc2.section.len)
					{
						markSection(*bd.info.matchBlock, cmpInfo.doc2);
						pSubAlignData->line += cmpInfo.doc2.section.len;
					}
				}

				++i;
			}
			else
			{
				cmpInfo.doc1.section.off = 0;
				cmpInfo.doc1.section.len = bd.len;
				markSection(bd, cmpInfo.doc1);

				pMainAlignData->diffMask	= cmpInfo.doc1.blockDiffMask;
				pSubAlignData->diffMask		= 0;

				alignmentInfo.push_back(alignPair);

				pMainAlignData->line += bd.len;
			}
		}

		if (progress && !progress->Advance())
			return false;
	}

	if (cmpInfo.selectionCompare)
	{
		pMainAlignData->diffMask	= 0;
		pSubAlignData->diffMask		= 0;

		alignmentInfo.push_back(alignPair);
	}

	if (progress && !progress->NextPhase())
		return false;

	return true;
}


CompareResult runCompare(const section_t& mainViewSection, const section_t& subViewSection,
		const UserSettings& settings, AlignmentInfo_t& alignmentInfo)
{
	progress_ptr& progress = ProgressDlg::Get();

	CompareInfo cmpInfo;

	cmpInfo.doc1.view			= MAIN_VIEW;
	cmpInfo.doc1.section		= mainViewSection;
	cmpInfo.doc2.view			= SUB_VIEW;
	cmpInfo.doc2.section		= subViewSection;

	cmpInfo.selectionCompare	= (cmpInfo.doc1.section.len || cmpInfo.doc2.section.len);

	cmpInfo.doc1.blockDiffMask = (settings.OldFileViewId == MAIN_VIEW) ? MARKER_MASK_REMOVED : MARKER_MASK_ADDED;
	cmpInfo.doc2.blockDiffMask = (settings.OldFileViewId == MAIN_VIEW) ? MARKER_MASK_ADDED : MARKER_MASK_REMOVED;

	const std::vector<uint64_t> doc1LineHashes = computeLineHashes(cmpInfo.doc1, settings);

	if (progress && !progress->NextPhase())
		return CompareResult::COMPARE_CANCELLED;

	const std::vector<uint64_t> doc2LineHashes = computeLineHashes(cmpInfo.doc2, settings);

	if (progress && !progress->NextPhase())
		return CompareResult::COMPARE_CANCELLED;

	const std::vector<uint64_t>* pLineHashes1 = &doc1LineHashes;
	const std::vector<uint64_t>* pLineHashes2 = &doc2LineHashes;

	if (pLineHashes1->size() < pLineHashes2->size())
	{
		std::swap(pLineHashes1, pLineHashes2);
		std::swap(cmpInfo.doc1, cmpInfo.doc2);
	}

	cmpInfo.blockDiffs = DiffCalc<uint64_t, blockDiffInfo>(*pLineHashes1, *pLineHashes2)();

	const int blockDiffSize = static_cast<int>(cmpInfo.blockDiffs.size());

	if (blockDiffSize == 1 && cmpInfo.blockDiffs[0].type == diff_type::DIFF_MATCH)
		return CompareResult::COMPARE_MATCH;

	if (settings.DetectMoves)
		findMoves(cmpInfo, *pLineHashes1, *pLineHashes2);

	// Currently it is impossible to set Sci annotation in the beginning of the doc so if there is a diff in the
	// beginning (alignment via annotation will probably be necessary) we insert blank line in each doc's beginning.
	// This is a workaround until it becomes possible to insert Sci annotation in the beginning of the doc.
	if ((cmpInfo.blockDiffs[0].type != diff_type::DIFF_MATCH) &&
			(!cmpInfo.doc1.section.off || !cmpInfo.doc2.section.off))
	{
		const BOOL doc1Modified = (BOOL)CallScintilla(cmpInfo.doc1.view, SCI_GETMODIFY, 0, 0);
		const BOOL doc2Modified = (BOOL)CallScintilla(cmpInfo.doc2.view, SCI_GETMODIFY, 0, 0);

		ScopedViewWriteEnabler writeEn1(cmpInfo.doc1.view);
		ScopedViewWriteEnabler writeEn2(cmpInfo.doc2.view);

		CallScintilla(cmpInfo.doc1.view, SCI_INSERTTEXT, 0,	(LPARAM)"\n");
		if (!doc1Modified)
			CallScintilla(cmpInfo.doc1.view, SCI_SETSAVEPOINT, 0, 0);

		CallScintilla(cmpInfo.doc2.view, SCI_INSERTTEXT, 0,	(LPARAM)"\n");
		if (!doc2Modified)
			CallScintilla(cmpInfo.doc2.view, SCI_SETSAVEPOINT, 0, 0);

		++cmpInfo.doc1.section.off;
		++cmpInfo.doc2.section.off;
	}

	if (cmpInfo.doc1.section.off || cmpInfo.doc2.section.off)
	{
		for (auto& bd: cmpInfo.blockDiffs)
		{
			if (bd.type == diff_type::DIFF_IN_1 || bd.type == diff_type::DIFF_MATCH)
				bd.off += cmpInfo.doc1.section.off;
			else if (bd.type == diff_type::DIFF_IN_2)
				bd.off += cmpInfo.doc2.section.off;
		}
	}

	if (progress && !progress->NextPhase())
		return CompareResult::COMPARE_CANCELLED;

	if (progress)
		progress->SetMaxCount(blockDiffSize);

	// Do block compares
	for (int i = 0; i < blockDiffSize; ++i)
	{
		if (cmpInfo.blockDiffs[i].type == diff_type::DIFF_IN_2)
		{
			// Check if the DIFF_IN_1 / DIFF_IN_2 pair includes changed lines or it's a completely replaced block
			if (i != 0 && cmpInfo.blockDiffs[i - 1].type == diff_type::DIFF_IN_1)
			{
				diffInfo& blockDiff1 = cmpInfo.blockDiffs[i - 1];
				diffInfo& blockDiff2 = cmpInfo.blockDiffs[i];

				blockDiff1.info.matchBlock = &blockDiff2;
				blockDiff2.info.matchBlock = &blockDiff1;

				compareBlocks(cmpInfo.doc1, cmpInfo.doc2, settings, blockDiff1, blockDiff2);
			}
		}

		if (progress && !progress->Advance())
			return CompareResult::COMPARE_CANCELLED;
	}

	if (progress && !progress->NextPhase())
		return CompareResult::COMPARE_CANCELLED;

	if (!markAllDiffs(cmpInfo, alignmentInfo))
		return CompareResult::COMPARE_CANCELLED;

	return CompareResult::COMPARE_MISMATCH;
}


CompareResult runFindUnique(const section_t& mainViewSection, const section_t& subViewSection,
		const UserSettings& settings, AlignmentInfo_t& alignmentInfo)
{
	progress_ptr& progress = ProgressDlg::Get();

	alignmentInfo.clear();

	DocCmpInfo doc1;
	DocCmpInfo doc2;

	doc1.view		= MAIN_VIEW;
	doc1.section	= mainViewSection;
	doc2.view		= SUB_VIEW;
	doc2.section	= subViewSection;

	if (settings.OldFileViewId == MAIN_VIEW)
	{
		doc1.blockDiffMask = MARKER_MASK_REMOVED;
		doc2.blockDiffMask = MARKER_MASK_ADDED;
	}
	else
	{
		doc1.blockDiffMask = MARKER_MASK_ADDED;
		doc2.blockDiffMask = MARKER_MASK_REMOVED;
	}

	std::vector<uint64_t> doc1LineHashes = computeLineHashes(doc1, settings);

	if (progress && !progress->NextPhase())
		return CompareResult::COMPARE_CANCELLED;

	std::vector<uint64_t> doc2LineHashes = computeLineHashes(doc2, settings);

	if (progress && !progress->NextPhase())
		return CompareResult::COMPARE_CANCELLED;

	std::unordered_map<uint64_t, std::vector<int>> doc1UniqueLines;

	int docHashesSize = static_cast<int>(doc1LineHashes.size());

	for (int i = 0; i < docHashesSize; ++i)
	{
		auto insertPair = doc1UniqueLines.emplace(doc1LineHashes[i], std::vector<int>{i});
		if (!insertPair.second)
			insertPair.first->second.emplace_back(i);
	}

	doc1LineHashes.clear();

	if (progress && !progress->NextPhase())
		return CompareResult::COMPARE_CANCELLED;

	std::unordered_map<uint64_t, std::vector<int>> doc2UniqueLines;

	docHashesSize = static_cast<int>(doc2LineHashes.size());

	for (int i = 0; i < docHashesSize; ++i)
	{
		auto insertPair = doc2UniqueLines.emplace(doc2LineHashes[i], std::vector<int>{i});
		if (!insertPair.second)
			insertPair.first->second.emplace_back(i);
	}

	doc2LineHashes.clear();

	if (progress && !progress->NextPhase())
		return CompareResult::COMPARE_CANCELLED;

	int doc1UniqueLinesCount = 0;

	for (std::unordered_map<uint64_t, std::vector<int>>::iterator doc1it = doc1UniqueLines.begin();
		doc1it != doc1UniqueLines.end(); ++doc1it)
	{
		std::unordered_map<uint64_t, std::vector<int>>::iterator doc2it = doc2UniqueLines.find(doc1it->first);

		if (doc2it != doc2UniqueLines.end())
		{
			doc2UniqueLines.erase(doc2it);
		}
		else
		{
			for (const auto& line: doc1it->second)
			{
				CallScintilla(doc1.view, SCI_MARKERADDSET, line + doc1.section.off, doc1.blockDiffMask);
				++doc1UniqueLinesCount;
			}
		}
	}

	if (doc1UniqueLinesCount == 0 && doc2UniqueLines.empty())
		return CompareResult::COMPARE_MATCH;

	for (const auto& uniqueLine: doc2UniqueLines)
	{
		for (const auto& line: uniqueLine.second)
		{
			CallScintilla(doc2.view, SCI_MARKERADDSET, line + doc2.section.off, doc2.blockDiffMask);
		}
	}

	AlignmentPair align;
	align.main.line	= mainViewSection.off;
	align.sub.line	= subViewSection.off;

	alignmentInfo.push_back(align);

	return CompareResult::COMPARE_MISMATCH;
}

}


CompareResult compareViews(const section_t& mainViewSection, const section_t& subViewSection, bool findUniqueMode,
		const UserSettings& settings, const TCHAR* progressInfo, AlignmentInfo_t& alignmentInfo)
{
	CompareResult result = CompareResult::COMPARE_ERROR;

	if (progressInfo)
		ProgressDlg::Open(progressInfo);

	try
	{
		if (findUniqueMode)
			result = runFindUnique(mainViewSection, subViewSection, settings, alignmentInfo);
		else
			result = runCompare(mainViewSection, subViewSection, settings, alignmentInfo);

		ProgressDlg::Close();
	}
	catch (std::exception& e)
	{
		ProgressDlg::Close();

		char msg[128];
		_snprintf_s(msg, _countof(msg), _TRUNCATE, "Exception occurred: %s", e.what());
		::MessageBoxA(nppData._nppHandle, msg, "Compare", MB_OK | MB_ICONWARNING);
	}
	catch (...)
	{
		ProgressDlg::Close();

		::MessageBoxA(nppData._nppHandle, "Unknown exception occurred.", "Compare", MB_OK | MB_ICONWARNING);
	}

	return result;
}
