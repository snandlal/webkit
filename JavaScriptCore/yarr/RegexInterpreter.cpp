/*
 * Copyright (C) 2009 Apple Inc. All rights reserved.
 * Copyright (C) 2010 Peter Varga (pvarga@inf.u-szeged.hu), University of Szeged
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#include "RegexInterpreter.h"

#include "RegexCompiler.h"
#include "RegexPattern.h"
#include <wtf/BumpPointerAllocator.h>

#ifndef NDEBUG
#include <stdio.h>
#endif

using namespace WTF;

namespace JSC { namespace Yarr {

class Interpreter {
public:
    struct ParenthesesDisjunctionContext;

    struct BackTrackInfoPatternCharacter {
        uintptr_t matchAmount;
    };
    struct BackTrackInfoCharacterClass {
        uintptr_t matchAmount;
    };
    struct BackTrackInfoBackReference {
        uintptr_t begin; // Not really needed for greedy quantifiers.
        uintptr_t matchAmount; // Not really needed for fixed quantifiers.
    };
    struct BackTrackInfoAlternative {
        uintptr_t offset;
    };
    struct BackTrackInfoParentheticalAssertion {
        uintptr_t begin;
    };
    struct BackTrackInfoParenthesesOnce {
        uintptr_t begin;
    };
    struct BackTrackInfoParenthesesTerminal {
        uintptr_t begin;
    };
    struct BackTrackInfoParentheses {
        uintptr_t matchAmount;
        ParenthesesDisjunctionContext* lastContext;
        uintptr_t prevBegin;
        uintptr_t prevEnd;
    };

    static inline void appendParenthesesDisjunctionContext(BackTrackInfoParentheses* backTrack, ParenthesesDisjunctionContext* context)
    {
        context->next = backTrack->lastContext;
        backTrack->lastContext = context;
        ++backTrack->matchAmount;
    }

    static inline void popParenthesesDisjunctionContext(BackTrackInfoParentheses* backTrack)
    {
        ASSERT(backTrack->matchAmount);
        ASSERT(backTrack->lastContext);
        backTrack->lastContext = backTrack->lastContext->next;
        --backTrack->matchAmount;
    }

    struct DisjunctionContext
    {
        DisjunctionContext()
            : term(0)
        {
        }

        void* operator new(size_t, void* where)
        {
            return where;
        }

        int term;
        unsigned matchBegin;
        unsigned matchEnd;
        uintptr_t frame[1];
    };

    DisjunctionContext* allocDisjunctionContext(ByteDisjunction* disjunction)
    {
        size_t size = sizeof(DisjunctionContext) - sizeof(uintptr_t) + disjunction->m_frameSize * sizeof(uintptr_t);
        allocatorPool = allocatorPool->ensureCapacity(size);
        if (!allocatorPool)
            CRASH();
        return new(allocatorPool->alloc(size)) DisjunctionContext();
    }

    void freeDisjunctionContext(DisjunctionContext* context)
    {
        allocatorPool = allocatorPool->dealloc(context);
    }

    struct ParenthesesDisjunctionContext
    {
        ParenthesesDisjunctionContext(int* output, ByteTerm& term)
            : next(0)
        {
            unsigned firstSubpatternId = term.atom.subpatternId;
            unsigned numNestedSubpatterns = term.atom.parenthesesDisjunction->m_numSubpatterns;

            for (unsigned i = 0; i < (numNestedSubpatterns << 1); ++i) {
                subpatternBackup[i] = output[(firstSubpatternId << 1) + i];
                output[(firstSubpatternId << 1) + i] = -1;
            }

            new(getDisjunctionContext(term)) DisjunctionContext();
        }

        void* operator new(size_t, void* where)
        {
            return where;
        }

        void restoreOutput(int* output, unsigned firstSubpatternId, unsigned numNestedSubpatterns)
        {
            for (unsigned i = 0; i < (numNestedSubpatterns << 1); ++i)
                output[(firstSubpatternId << 1) + i] = subpatternBackup[i];
        }

        DisjunctionContext* getDisjunctionContext(ByteTerm& term)
        {
            return reinterpret_cast<DisjunctionContext*>(&(subpatternBackup[term.atom.parenthesesDisjunction->m_numSubpatterns << 1]));
        }

        ParenthesesDisjunctionContext* next;
        int subpatternBackup[1];
    };

    ParenthesesDisjunctionContext* allocParenthesesDisjunctionContext(ByteDisjunction* disjunction, int* output, ByteTerm& term)
    {
        size_t size = sizeof(ParenthesesDisjunctionContext) - sizeof(int) + (term.atom.parenthesesDisjunction->m_numSubpatterns << 1) * sizeof(int) + sizeof(DisjunctionContext) - sizeof(uintptr_t) + disjunction->m_frameSize * sizeof(uintptr_t);
        allocatorPool = allocatorPool->ensureCapacity(size);
        if (!allocatorPool)
            CRASH();
        return new(allocatorPool->alloc(size)) ParenthesesDisjunctionContext(output, term);
    }

    void freeParenthesesDisjunctionContext(ParenthesesDisjunctionContext* context)
    {
        allocatorPool = allocatorPool->dealloc(context);
    }

    class InputStream {
    public:
        InputStream(const UChar* input, unsigned start, unsigned length)
            : input(input)
            , pos(start)
            , length(length)
        {
        }

        void next()
        {
            ++pos;
        }

        void rewind(unsigned amount)
        {
            ASSERT(pos >= amount);
            pos -= amount;
        }

        int read()
        {
            ASSERT(pos < length);
            if (pos < length)
                return input[pos];
            return -1;
        }

        int readPair()
        {
            ASSERT(pos + 1 < length);
            return input[pos] | input[pos + 1] << 16;
        }

        int readChecked(int position)
        {
            ASSERT(position < 0);
            ASSERT((unsigned)-position <= pos);
            unsigned p = pos + position;
            ASSERT(p < length);
            return input[p];
        }

        int reread(unsigned from)
        {
            ASSERT(from < length);
            return input[from];
        }

        int prev()
        {
            ASSERT(!(pos > length));
            if (pos && length)
                return input[pos - 1];
            return -1;
        }

        unsigned getPos()
        {
            return pos;
        }

        void setPos(unsigned p)
        {
            pos = p;
        }

        bool atStart()
        {
            return pos == 0;
        }

        bool atEnd()
        {
            return pos == length;
        }

        bool checkInput(int count)
        {
            if ((pos + count) <= length) {
                pos += count;
                return true;
            } else
                return false;
        }

        void uncheckInput(int count)
        {
            pos -= count;
        }

        bool atStart(int position)
        {
            return (pos + position) == 0;
        }

        bool atEnd(int position)
        {
            return (pos + position) == length;
        }

        bool isNotAvailableInput(int position)
        {
            return (pos + position) > length;
        }

    private:
        const UChar* input;
        unsigned pos;
        unsigned length;
    };

    bool testCharacterClass(CharacterClass* characterClass, int ch)
    {
        if (ch & 0xFF80) {
            for (unsigned i = 0; i < characterClass->m_matchesUnicode.size(); ++i)
                if (ch == characterClass->m_matchesUnicode[i])
                    return true;
            for (unsigned i = 0; i < characterClass->m_rangesUnicode.size(); ++i)
                if ((ch >= characterClass->m_rangesUnicode[i].begin) && (ch <= characterClass->m_rangesUnicode[i].end))
                    return true;
        } else {
            for (unsigned i = 0; i < characterClass->m_matches.size(); ++i)
                if (ch == characterClass->m_matches[i])
                    return true;
            for (unsigned i = 0; i < characterClass->m_ranges.size(); ++i)
                if ((ch >= characterClass->m_ranges[i].begin) && (ch <= characterClass->m_ranges[i].end))
                    return true;
        }

        return false;
    }

    bool checkCharacter(int testChar, int inputPosition)
    {
        return testChar == input.readChecked(inputPosition);
    }

    bool checkCasedCharacter(int loChar, int hiChar, int inputPosition)
    {
        int ch = input.readChecked(inputPosition);
        return (loChar == ch) || (hiChar == ch);
    }

    bool checkCharacterClass(CharacterClass* characterClass, bool invert, int inputPosition)
    {
        bool match = testCharacterClass(characterClass, input.readChecked(inputPosition));
        return invert ? !match : match;
    }

    bool tryConsumeBackReference(int matchBegin, int matchEnd, int inputOffset)
    {
        int matchSize = matchEnd - matchBegin;

        if (!input.checkInput(matchSize))
            return false;

        if (pattern->m_ignoreCase) {
            for (int i = 0; i < matchSize; ++i) {
                int ch = input.reread(matchBegin + i);

                int lo = Unicode::toLower(ch);
                int hi = Unicode::toUpper(ch);

                if ((lo != hi) ? (!checkCasedCharacter(lo, hi, inputOffset - matchSize + i)) : (!checkCharacter(ch, inputOffset - matchSize + i))) {
                    input.uncheckInput(matchSize);
                    return false;
                }
            }
        } else {
            for (int i = 0; i < matchSize; ++i) {
                if (!checkCharacter(input.reread(matchBegin + i), inputOffset - matchSize + i)) {
                    input.uncheckInput(matchSize);
                    return false;
                }
            }
        }

        return true;
    }

    bool matchAssertionBOL(ByteTerm& term)
    {
        return (input.atStart(term.inputPosition)) || (pattern->m_multiline && testCharacterClass(pattern->newlineCharacterClass, input.readChecked(term.inputPosition - 1)));
    }

    bool matchAssertionEOL(ByteTerm& term)
    {
        if (term.inputPosition)
            return (input.atEnd(term.inputPosition)) || (pattern->m_multiline && testCharacterClass(pattern->newlineCharacterClass, input.readChecked(term.inputPosition)));
        else
            return (input.atEnd()) || (pattern->m_multiline && testCharacterClass(pattern->newlineCharacterClass, input.read()));
    }

    bool matchAssertionWordBoundary(ByteTerm& term)
    {
        bool prevIsWordchar = !input.atStart(term.inputPosition) && testCharacterClass(pattern->wordcharCharacterClass, input.readChecked(term.inputPosition - 1));
        bool readIsWordchar;
        if (term.inputPosition)
            readIsWordchar = !input.atEnd(term.inputPosition) && testCharacterClass(pattern->wordcharCharacterClass, input.readChecked(term.inputPosition));
        else
            readIsWordchar = !input.atEnd() && testCharacterClass(pattern->wordcharCharacterClass, input.read());

        bool wordBoundary = prevIsWordchar != readIsWordchar;
        return term.invert() ? !wordBoundary : wordBoundary;
    }

    bool backtrackPatternCharacter(ByteTerm& term, DisjunctionContext* context)
    {
        BackTrackInfoPatternCharacter* backTrack = reinterpret_cast<BackTrackInfoPatternCharacter*>(context->frame + term.frameLocation);

        switch (term.atom.quantityType) {
        case QuantifierFixedCount:
            break;

        case QuantifierGreedy:
            if (backTrack->matchAmount) {
                --backTrack->matchAmount;
                input.uncheckInput(1);
                return true;
            }
            break;

        case QuantifierNonGreedy:
            if ((backTrack->matchAmount < term.atom.quantityCount) && input.checkInput(1)) {
                ++backTrack->matchAmount;
                if (checkCharacter(term.atom.patternCharacter, term.inputPosition - 1))
                    return true;
            }
            input.uncheckInput(backTrack->matchAmount);
            break;
        }

        return false;
    }

    bool backtrackPatternCasedCharacter(ByteTerm& term, DisjunctionContext* context)
    {
        BackTrackInfoPatternCharacter* backTrack = reinterpret_cast<BackTrackInfoPatternCharacter*>(context->frame + term.frameLocation);

        switch (term.atom.quantityType) {
        case QuantifierFixedCount:
            break;

        case QuantifierGreedy:
            if (backTrack->matchAmount) {
                --backTrack->matchAmount;
                input.uncheckInput(1);
                return true;
            }
            break;

        case QuantifierNonGreedy:
            if ((backTrack->matchAmount < term.atom.quantityCount) && input.checkInput(1)) {
                ++backTrack->matchAmount;
                if (checkCasedCharacter(term.atom.casedCharacter.lo, term.atom.casedCharacter.hi, term.inputPosition - 1))
                    return true;
            }
            input.uncheckInput(backTrack->matchAmount);
            break;
        }

        return false;
    }

    bool matchCharacterClass(ByteTerm& term, DisjunctionContext* context)
    {
        ASSERT(term.type == ByteTerm::TypeCharacterClass);
        BackTrackInfoPatternCharacter* backTrack = reinterpret_cast<BackTrackInfoPatternCharacter*>(context->frame + term.frameLocation);

        switch (term.atom.quantityType) {
        case QuantifierFixedCount: {
            for (unsigned matchAmount = 0; matchAmount < term.atom.quantityCount; ++matchAmount) {
                if (!checkCharacterClass(term.atom.characterClass, term.invert(), term.inputPosition + matchAmount))
                    return false;
            }
            return true;
        }

        case QuantifierGreedy: {
            unsigned matchAmount = 0;
            while ((matchAmount < term.atom.quantityCount) && input.checkInput(1)) {
                if (!checkCharacterClass(term.atom.characterClass, term.invert(), term.inputPosition - 1)) {
                    input.uncheckInput(1);
                    break;
                }
                ++matchAmount;
            }
            backTrack->matchAmount = matchAmount;

            return true;
        }

        case QuantifierNonGreedy:
            backTrack->matchAmount = 0;
            return true;
        }

        ASSERT_NOT_REACHED();
        return false;
    }

    bool backtrackCharacterClass(ByteTerm& term, DisjunctionContext* context)
    {
        ASSERT(term.type == ByteTerm::TypeCharacterClass);
        BackTrackInfoPatternCharacter* backTrack = reinterpret_cast<BackTrackInfoPatternCharacter*>(context->frame + term.frameLocation);

        switch (term.atom.quantityType) {
        case QuantifierFixedCount:
            break;

        case QuantifierGreedy:
            if (backTrack->matchAmount) {
                --backTrack->matchAmount;
                input.uncheckInput(1);
                return true;
            }
            break;

        case QuantifierNonGreedy:
            if ((backTrack->matchAmount < term.atom.quantityCount) && input.checkInput(1)) {
                ++backTrack->matchAmount;
                if (checkCharacterClass(term.atom.characterClass, term.invert(), term.inputPosition - 1))
                    return true;
            }
            input.uncheckInput(backTrack->matchAmount);
            break;
        }

        return false;
    }

    bool matchBackReference(ByteTerm& term, DisjunctionContext* context)
    {
        ASSERT(term.type == ByteTerm::TypeBackReference);
        BackTrackInfoBackReference* backTrack = reinterpret_cast<BackTrackInfoBackReference*>(context->frame + term.frameLocation);

        int matchBegin = output[(term.atom.subpatternId << 1)];
        int matchEnd = output[(term.atom.subpatternId << 1) + 1];

        // If the end position of the referenced match hasn't set yet then the backreference in the same parentheses where it references to that.
        // In this case the result of match is empty string like when it references to a parentheses with zero-width match.
        // Eg.: /(a\1)/
        if (matchEnd == -1)
            return true;

        ASSERT((matchBegin == -1) == (matchEnd == -1));
        ASSERT(matchBegin <= matchEnd);

        if (matchBegin == matchEnd)
            return true;

        switch (term.atom.quantityType) {
        case QuantifierFixedCount: {
            backTrack->begin = input.getPos();
            for (unsigned matchAmount = 0; matchAmount < term.atom.quantityCount; ++matchAmount) {
                if (!tryConsumeBackReference(matchBegin, matchEnd, term.inputPosition)) {
                    input.setPos(backTrack->begin);
                    return false;
                }
            }
            return true;
        }

        case QuantifierGreedy: {
            unsigned matchAmount = 0;
            while ((matchAmount < term.atom.quantityCount) && tryConsumeBackReference(matchBegin, matchEnd, term.inputPosition))
                ++matchAmount;
            backTrack->matchAmount = matchAmount;
            return true;
        }

        case QuantifierNonGreedy:
            backTrack->begin = input.getPos();
            backTrack->matchAmount = 0;
            return true;
        }

        ASSERT_NOT_REACHED();
        return false;
    }

    bool backtrackBackReference(ByteTerm& term, DisjunctionContext* context)
    {
        ASSERT(term.type == ByteTerm::TypeBackReference);
        BackTrackInfoBackReference* backTrack = reinterpret_cast<BackTrackInfoBackReference*>(context->frame + term.frameLocation);

        int matchBegin = output[(term.atom.subpatternId << 1)];
        int matchEnd = output[(term.atom.subpatternId << 1) + 1];
        ASSERT((matchBegin == -1) == (matchEnd == -1));
        ASSERT(matchBegin <= matchEnd);

        if (matchBegin == matchEnd)
            return false;

        switch (term.atom.quantityType) {
        case QuantifierFixedCount:
            // for quantityCount == 1, could rewind.
            input.setPos(backTrack->begin);
            break;

        case QuantifierGreedy:
            if (backTrack->matchAmount) {
                --backTrack->matchAmount;
                input.rewind(matchEnd - matchBegin);
                return true;
            }
            break;

        case QuantifierNonGreedy:
            if ((backTrack->matchAmount < term.atom.quantityCount) && tryConsumeBackReference(matchBegin, matchEnd, term.inputPosition)) {
                ++backTrack->matchAmount;
                return true;
            } else
                input.setPos(backTrack->begin);
            break;
        }

        return false;
    }

    void recordParenthesesMatch(ByteTerm& term, ParenthesesDisjunctionContext* context)
    {
        if (term.capture()) {
            unsigned subpatternId = term.atom.subpatternId;
            output[(subpatternId << 1)] = context->getDisjunctionContext(term)->matchBegin + term.inputPosition;
            output[(subpatternId << 1) + 1] = context->getDisjunctionContext(term)->matchEnd + term.inputPosition;
        }
    }
    void resetMatches(ByteTerm& term, ParenthesesDisjunctionContext* context)
    {
        unsigned firstSubpatternId = term.atom.subpatternId;
        unsigned count = term.atom.parenthesesDisjunction->m_numSubpatterns;
        context->restoreOutput(output, firstSubpatternId, count);
    }
    JSRegExpResult parenthesesDoBacktrack(ByteTerm& term, BackTrackInfoParentheses* backTrack)
    {
        while (backTrack->matchAmount) {
            ParenthesesDisjunctionContext* context = backTrack->lastContext;

            JSRegExpResult result = matchDisjunction(term.atom.parenthesesDisjunction, context->getDisjunctionContext(term), true);
            if (result == JSRegExpMatch)
                return JSRegExpMatch;

            resetMatches(term, context);
            popParenthesesDisjunctionContext(backTrack);
            freeParenthesesDisjunctionContext(context);

            if (result != JSRegExpNoMatch)
                return result;
        }

        return JSRegExpNoMatch;
    }

    bool matchParenthesesOnceBegin(ByteTerm& term, DisjunctionContext* context)
    {
        ASSERT(term.type == ByteTerm::TypeParenthesesSubpatternOnceBegin);
        ASSERT(term.atom.quantityCount == 1);

        BackTrackInfoParenthesesOnce* backTrack = reinterpret_cast<BackTrackInfoParenthesesOnce*>(context->frame + term.frameLocation);

        switch (term.atom.quantityType) {
        case QuantifierGreedy: {
            // set this speculatively; if we get to the parens end this will be true.
            backTrack->begin = input.getPos();
            break;
        }
        case QuantifierNonGreedy: {
            backTrack->begin = notFound;
            context->term += term.atom.parenthesesWidth;
            return true;
        }
        case QuantifierFixedCount:
            break;
        }

        if (term.capture()) {
            unsigned subpatternId = term.atom.subpatternId;
            output[(subpatternId << 1)] = input.getPos() + term.inputPosition;
        }

        return true;
    }

    bool matchParenthesesOnceEnd(ByteTerm& term, DisjunctionContext* context)
    {
        ASSERT(term.type == ByteTerm::TypeParenthesesSubpatternOnceEnd);
        ASSERT(term.atom.quantityCount == 1);

        if (term.capture()) {
            unsigned subpatternId = term.atom.subpatternId;
            output[(subpatternId << 1) + 1] = input.getPos() + term.inputPosition;
        }

        if (term.atom.quantityType == QuantifierFixedCount)
            return true;

        BackTrackInfoParenthesesOnce* backTrack = reinterpret_cast<BackTrackInfoParenthesesOnce*>(context->frame + term.frameLocation);
        return backTrack->begin != input.getPos();
    }

    bool backtrackParenthesesOnceBegin(ByteTerm& term, DisjunctionContext* context)
    {
        ASSERT(term.type == ByteTerm::TypeParenthesesSubpatternOnceBegin);
        ASSERT(term.atom.quantityCount == 1);

        BackTrackInfoParenthesesOnce* backTrack = reinterpret_cast<BackTrackInfoParenthesesOnce*>(context->frame + term.frameLocation);

        if (term.capture()) {
            unsigned subpatternId = term.atom.subpatternId;
            output[(subpatternId << 1)] = -1;
            output[(subpatternId << 1) + 1] = -1;
        }

        switch (term.atom.quantityType) {
        case QuantifierGreedy:
            // if we backtrack to this point, there is another chance - try matching nothing.
            ASSERT(backTrack->begin != notFound);
            backTrack->begin = notFound;
            context->term += term.atom.parenthesesWidth;
            return true;
        case QuantifierNonGreedy:
            ASSERT(backTrack->begin != notFound);
        case QuantifierFixedCount:
            break;
        }

        return false;
    }

    bool backtrackParenthesesOnceEnd(ByteTerm& term, DisjunctionContext* context)
    {
        ASSERT(term.type == ByteTerm::TypeParenthesesSubpatternOnceEnd);
        ASSERT(term.atom.quantityCount == 1);

        BackTrackInfoParenthesesOnce* backTrack = reinterpret_cast<BackTrackInfoParenthesesOnce*>(context->frame + term.frameLocation);

        switch (term.atom.quantityType) {
        case QuantifierGreedy:
            if (backTrack->begin == notFound) {
                context->term -= term.atom.parenthesesWidth;
                return false;
            }
        case QuantifierNonGreedy:
            if (backTrack->begin == notFound) {
                backTrack->begin = input.getPos();
                if (term.capture()) {
                    // Technically this access to inputPosition should be accessing the begin term's
                    // inputPosition, but for repeats other than fixed these values should be
                    // the same anyway! (we don't pre-check for greedy or non-greedy matches.)
                    ASSERT((&term - term.atom.parenthesesWidth)->type == ByteTerm::TypeParenthesesSubpatternOnceBegin);
                    ASSERT((&term - term.atom.parenthesesWidth)->inputPosition == term.inputPosition);
                    unsigned subpatternId = term.atom.subpatternId;
                    output[subpatternId << 1] = input.getPos() + term.inputPosition;
                }
                context->term -= term.atom.parenthesesWidth;
                return true;
            }
        case QuantifierFixedCount:
            break;
        }

        return false;
    }

    bool matchParenthesesTerminalBegin(ByteTerm& term, DisjunctionContext* context)
    {
        ASSERT(term.type == ByteTerm::TypeParenthesesSubpatternTerminalBegin);
        ASSERT(term.atom.quantityType == QuantifierGreedy);
        ASSERT(term.atom.quantityCount == UINT_MAX);
        ASSERT(!term.capture());

        BackTrackInfoParenthesesTerminal* backTrack = reinterpret_cast<BackTrackInfoParenthesesTerminal*>(context->frame + term.frameLocation);
        backTrack->begin = input.getPos();
        return true;
    }

    bool matchParenthesesTerminalEnd(ByteTerm& term, DisjunctionContext* context)
    {
        ASSERT(term.type == ByteTerm::TypeParenthesesSubpatternTerminalEnd);

        BackTrackInfoParenthesesTerminal* backTrack = reinterpret_cast<BackTrackInfoParenthesesTerminal*>(context->frame + term.frameLocation);
        // Empty match is a failed match.
        if (backTrack->begin == input.getPos())
            return false;

        // Successful match! Okay, what's next? - loop around and try to match moar!
        context->term -= (term.atom.parenthesesWidth + 1);
        return true;
    }

    bool backtrackParenthesesTerminalBegin(ByteTerm& term, DisjunctionContext* context)
    {
        ASSERT(term.type == ByteTerm::TypeParenthesesSubpatternTerminalBegin);
        ASSERT(term.atom.quantityType == QuantifierGreedy);
        ASSERT(term.atom.quantityCount == UINT_MAX);
        ASSERT(!term.capture());

        // If we backtrack to this point, we have failed to match this iteration of the parens.
        // Since this is greedy / zero minimum a failed is also accepted as a match!
        context->term += term.atom.parenthesesWidth;
        return true;
    }

    bool backtrackParenthesesTerminalEnd(ByteTerm&, DisjunctionContext*)
    {
        // 'Terminal' parentheses are at the end of the regex, and as such a match past end
        // should always be returned as a successful match - we should never becktrack to here.
        ASSERT_NOT_REACHED();
        return false;
    }

    bool matchParentheticalAssertionBegin(ByteTerm& term, DisjunctionContext* context)
    {
        ASSERT(term.type == ByteTerm::TypeParentheticalAssertionBegin);
        ASSERT(term.atom.quantityCount == 1);

        BackTrackInfoParentheticalAssertion* backTrack = reinterpret_cast<BackTrackInfoParentheticalAssertion*>(context->frame + term.frameLocation);

        backTrack->begin = input.getPos();
        return true;
    }

    bool matchParentheticalAssertionEnd(ByteTerm& term, DisjunctionContext* context)
    {
        ASSERT(term.type == ByteTerm::TypeParentheticalAssertionEnd);
        ASSERT(term.atom.quantityCount == 1);

        BackTrackInfoParentheticalAssertion* backTrack = reinterpret_cast<BackTrackInfoParentheticalAssertion*>(context->frame + term.frameLocation);

        input.setPos(backTrack->begin);

        // We've reached the end of the parens; if they are inverted, this is failure.
        if (term.invert()) {
            context->term -= term.atom.parenthesesWidth;
            return false;
        }

        return true;
    }

    bool backtrackParentheticalAssertionBegin(ByteTerm& term, DisjunctionContext* context)
    {
        ASSERT(term.type == ByteTerm::TypeParentheticalAssertionBegin);
        ASSERT(term.atom.quantityCount == 1);

        // We've failed to match parens; if they are inverted, this is win!
        if (term.invert()) {
            context->term += term.atom.parenthesesWidth;
            return true;
        }

        return false;
    }

    bool backtrackParentheticalAssertionEnd(ByteTerm& term, DisjunctionContext* context)
    {
        ASSERT(term.type == ByteTerm::TypeParentheticalAssertionEnd);
        ASSERT(term.atom.quantityCount == 1);

        BackTrackInfoParentheticalAssertion* backTrack = reinterpret_cast<BackTrackInfoParentheticalAssertion*>(context->frame + term.frameLocation);

        input.setPos(backTrack->begin);

        context->term -= term.atom.parenthesesWidth;
        return false;
    }

    JSRegExpResult matchParentheses(ByteTerm& term, DisjunctionContext* context)
    {
        ASSERT(term.type == ByteTerm::TypeParenthesesSubpattern);

        BackTrackInfoParentheses* backTrack = reinterpret_cast<BackTrackInfoParentheses*>(context->frame + term.frameLocation);

        unsigned subpatternId = term.atom.subpatternId;
        ByteDisjunction* disjunctionBody = term.atom.parenthesesDisjunction;

        backTrack->prevBegin = output[(subpatternId << 1)];
        backTrack->prevEnd = output[(subpatternId << 1) + 1];

        backTrack->matchAmount = 0;
        backTrack->lastContext = 0;

        switch (term.atom.quantityType) {
        case QuantifierFixedCount: {
            // While we haven't yet reached our fixed limit,
            while (backTrack->matchAmount < term.atom.quantityCount) {
                // Try to do a match, and it it succeeds, add it to the list.
                ParenthesesDisjunctionContext* context = allocParenthesesDisjunctionContext(disjunctionBody, output, term);
                JSRegExpResult result = matchDisjunction(disjunctionBody, context->getDisjunctionContext(term));
                if (result == JSRegExpMatch)
                    appendParenthesesDisjunctionContext(backTrack, context);
                else {
                    // The match failed; try to find an alternate point to carry on from.
                    resetMatches(term, context);
                    freeParenthesesDisjunctionContext(context);

                    if (result == JSRegExpNoMatch) {
                        JSRegExpResult backtrackResult = parenthesesDoBacktrack(term, backTrack);
                        if (backtrackResult != JSRegExpMatch)
                            return backtrackResult;
                    } else
                        return result;
                }
            }

            ASSERT(backTrack->matchAmount == term.atom.quantityCount);
            ParenthesesDisjunctionContext* context = backTrack->lastContext;
            recordParenthesesMatch(term, context);
            return JSRegExpMatch;
        }

        case QuantifierGreedy: {
            while (backTrack->matchAmount < term.atom.quantityCount) {
                ParenthesesDisjunctionContext* context = allocParenthesesDisjunctionContext(disjunctionBody, output, term);
                JSRegExpResult result = matchNonZeroDisjunction(disjunctionBody, context->getDisjunctionContext(term));
                if (result == JSRegExpMatch)
                    appendParenthesesDisjunctionContext(backTrack, context);
                else {
                    resetMatches(term, context);
                    freeParenthesesDisjunctionContext(context);

                    if (result != JSRegExpNoMatch)
                        return result;

                    break;
                }
            }

            if (backTrack->matchAmount) {
                ParenthesesDisjunctionContext* context = backTrack->lastContext;
                recordParenthesesMatch(term, context);
            }
            return JSRegExpMatch;
        }

        case QuantifierNonGreedy:
            return JSRegExpMatch;
        }

        ASSERT_NOT_REACHED();
        return JSRegExpErrorNoMatch;
    }

    // Rules for backtracking differ depending on whether this is greedy or non-greedy.
    //
    // Greedy matches never should try just adding more - you should already have done
    // the 'more' cases.  Always backtrack, at least a leetle bit.  However cases where
    // you backtrack an item off the list needs checking, since we'll never have matched
    // the one less case.  Tracking forwards, still add as much as possible.
    //
    // Non-greedy, we've already done the one less case, so don't match on popping.
    // We haven't done the one more case, so always try to add that.
    //
    JSRegExpResult backtrackParentheses(ByteTerm& term, DisjunctionContext* context)
    {
        ASSERT(term.type == ByteTerm::TypeParenthesesSubpattern);

        BackTrackInfoParentheses* backTrack = reinterpret_cast<BackTrackInfoParentheses*>(context->frame + term.frameLocation);

        if (term.capture()) {
            unsigned subpatternId = term.atom.subpatternId;
            output[(subpatternId << 1)] = backTrack->prevBegin;
            output[(subpatternId << 1) + 1] = backTrack->prevEnd;
        }

        ByteDisjunction* disjunctionBody = term.atom.parenthesesDisjunction;

        switch (term.atom.quantityType) {
        case QuantifierFixedCount: {
            ASSERT(backTrack->matchAmount == term.atom.quantityCount);

            ParenthesesDisjunctionContext* context = 0;
            JSRegExpResult result = parenthesesDoBacktrack(term, backTrack);

            if (result != JSRegExpMatch)
                return result;

            // While we haven't yet reached our fixed limit,
            while (backTrack->matchAmount < term.atom.quantityCount) {
                // Try to do a match, and it it succeeds, add it to the list.
                context = allocParenthesesDisjunctionContext(disjunctionBody, output, term);
                result = matchDisjunction(disjunctionBody, context->getDisjunctionContext(term));

                if (result == JSRegExpMatch)
                    appendParenthesesDisjunctionContext(backTrack, context);
                else {
                    // The match failed; try to find an alternate point to carry on from.
                    resetMatches(term, context);
                    freeParenthesesDisjunctionContext(context);

                    if (result == JSRegExpNoMatch) {
                        JSRegExpResult backtrackResult = parenthesesDoBacktrack(term, backTrack);
                        if (backtrackResult != JSRegExpMatch)
                            return backtrackResult;
                    } else
                        return result;
                }
            }

            ASSERT(backTrack->matchAmount == term.atom.quantityCount);
            context = backTrack->lastContext;
            recordParenthesesMatch(term, context);
            return JSRegExpMatch;
        }

        case QuantifierGreedy: {
            if (!backTrack->matchAmount)
                return JSRegExpNoMatch;

            ParenthesesDisjunctionContext* context = backTrack->lastContext;
            JSRegExpResult result = matchNonZeroDisjunction(disjunctionBody, context->getDisjunctionContext(term), true);
            if (result == JSRegExpMatch) {
                while (backTrack->matchAmount < term.atom.quantityCount) {
                    ParenthesesDisjunctionContext* context = allocParenthesesDisjunctionContext(disjunctionBody, output, term);
                    JSRegExpResult parenthesesResult = matchNonZeroDisjunction(disjunctionBody, context->getDisjunctionContext(term));
                    if (parenthesesResult == JSRegExpMatch)
                        appendParenthesesDisjunctionContext(backTrack, context);
                    else {
                        resetMatches(term, context);
                        freeParenthesesDisjunctionContext(context);

                        if (parenthesesResult != JSRegExpNoMatch)
                            return parenthesesResult;

                        break;
                    }
                }
            } else {
                resetMatches(term, context);
                popParenthesesDisjunctionContext(backTrack);
                freeParenthesesDisjunctionContext(context);

                if (result != JSRegExpNoMatch)
                    return result;
            }

            if (backTrack->matchAmount) {
                ParenthesesDisjunctionContext* context = backTrack->lastContext;
                recordParenthesesMatch(term, context);
            }
            return JSRegExpMatch;
        }

        case QuantifierNonGreedy: {
            // If we've not reached the limit, try to add one more match.
            if (backTrack->matchAmount < term.atom.quantityCount) {
                ParenthesesDisjunctionContext* context = allocParenthesesDisjunctionContext(disjunctionBody, output, term);
                JSRegExpResult result = matchNonZeroDisjunction(disjunctionBody, context->getDisjunctionContext(term));
                if (result == JSRegExpMatch) {
                    appendParenthesesDisjunctionContext(backTrack, context);
                    recordParenthesesMatch(term, context);
                    return JSRegExpMatch;
                }

                resetMatches(term, context);
                freeParenthesesDisjunctionContext(context);

                if (result != JSRegExpNoMatch)
                    return result;
            }

            // Nope - okay backtrack looking for an alternative.
            while (backTrack->matchAmount) {
                ParenthesesDisjunctionContext* context = backTrack->lastContext;
                JSRegExpResult result = matchNonZeroDisjunction(disjunctionBody, context->getDisjunctionContext(term), true);
                if (result == JSRegExpMatch) {
                    // successful backtrack! we're back in the game!
                    if (backTrack->matchAmount) {
                        context = backTrack->lastContext;
                        recordParenthesesMatch(term, context);
                    }
                    return JSRegExpMatch;
                }

                // pop a match off the stack
                resetMatches(term, context);
                popParenthesesDisjunctionContext(backTrack);
                freeParenthesesDisjunctionContext(context);

                return result;
            }

            return JSRegExpNoMatch;
        }
        }

        ASSERT_NOT_REACHED();
        return JSRegExpErrorNoMatch;
    }

    void lookupForBeginChars()
    {
        int character;
        bool firstSingleCharFound;

        while (true) {
            if (input.isNotAvailableInput(2))
                return;

            firstSingleCharFound = false;

            character = input.readPair();

            for (unsigned i = 0; i < pattern->m_beginChars.size(); ++i) {
                BeginChar bc = pattern->m_beginChars[i];

                if (!firstSingleCharFound && bc.value <= 0xFFFF) {
                    firstSingleCharFound = true;
                    character &= 0xFFFF;
                }

                if ((character | bc.mask) == bc.value)
                    return;
            }

            input.next();
        }
    }

#define MATCH_NEXT() { ++context->term; goto matchAgain; }
#define BACKTRACK() { --context->term; goto backtrack; }
#define currentTerm() (disjunction->terms[context->term])
    JSRegExpResult matchDisjunction(ByteDisjunction* disjunction, DisjunctionContext* context, bool btrack = false, bool isBody = false)
    {
        if (!--remainingMatchCount)
            return JSRegExpErrorHitLimit;

        if (btrack)
            BACKTRACK();

        if (pattern->m_containsBeginChars && isBody)
            lookupForBeginChars();

        context->matchBegin = input.getPos();
        context->term = 0;

    matchAgain:
        ASSERT(context->term < static_cast<int>(disjunction->terms.size()));

        switch (currentTerm().type) {
        case ByteTerm::TypeSubpatternBegin:
            MATCH_NEXT();
        case ByteTerm::TypeSubpatternEnd:
            context->matchEnd = input.getPos();
            return JSRegExpMatch;

        case ByteTerm::TypeBodyAlternativeBegin:
            MATCH_NEXT();
        case ByteTerm::TypeBodyAlternativeDisjunction:
        case ByteTerm::TypeBodyAlternativeEnd:
            context->matchEnd = input.getPos();
            return JSRegExpMatch;

        case ByteTerm::TypeAlternativeBegin:
            MATCH_NEXT();
        case ByteTerm::TypeAlternativeDisjunction:
        case ByteTerm::TypeAlternativeEnd: {
            int offset = currentTerm().alternative.end;
            BackTrackInfoAlternative* backTrack = reinterpret_cast<BackTrackInfoAlternative*>(context->frame + currentTerm().frameLocation);
            backTrack->offset = offset;
            context->term += offset;
            MATCH_NEXT();
        }

        case ByteTerm::TypeAssertionBOL:
            if (matchAssertionBOL(currentTerm()))
                MATCH_NEXT();
            BACKTRACK();
        case ByteTerm::TypeAssertionEOL:
            if (matchAssertionEOL(currentTerm()))
                MATCH_NEXT();
            BACKTRACK();
        case ByteTerm::TypeAssertionWordBoundary:
            if (matchAssertionWordBoundary(currentTerm()))
                MATCH_NEXT();
            BACKTRACK();

        case ByteTerm::TypePatternCharacterOnce:
        case ByteTerm::TypePatternCharacterFixed: {
            for (unsigned matchAmount = 0; matchAmount < currentTerm().atom.quantityCount; ++matchAmount) {
                if (!checkCharacter(currentTerm().atom.patternCharacter, currentTerm().inputPosition + matchAmount))
                    BACKTRACK();
            }
            MATCH_NEXT();
        }
        case ByteTerm::TypePatternCharacterGreedy: {
            BackTrackInfoPatternCharacter* backTrack = reinterpret_cast<BackTrackInfoPatternCharacter*>(context->frame + currentTerm().frameLocation);
            unsigned matchAmount = 0;
            while ((matchAmount < currentTerm().atom.quantityCount) && input.checkInput(1)) {
                if (!checkCharacter(currentTerm().atom.patternCharacter, currentTerm().inputPosition - 1)) {
                    input.uncheckInput(1);
                    break;
                }
                ++matchAmount;
            }
            backTrack->matchAmount = matchAmount;

            MATCH_NEXT();
        }
        case ByteTerm::TypePatternCharacterNonGreedy: {
            BackTrackInfoPatternCharacter* backTrack = reinterpret_cast<BackTrackInfoPatternCharacter*>(context->frame + currentTerm().frameLocation);
            backTrack->matchAmount = 0;
            MATCH_NEXT();
        }

        case ByteTerm::TypePatternCasedCharacterOnce:
        case ByteTerm::TypePatternCasedCharacterFixed: {
            for (unsigned matchAmount = 0; matchAmount < currentTerm().atom.quantityCount; ++matchAmount) {
                if (!checkCasedCharacter(currentTerm().atom.casedCharacter.lo, currentTerm().atom.casedCharacter.hi, currentTerm().inputPosition + matchAmount))
                    BACKTRACK();
            }
            MATCH_NEXT();
        }
        case ByteTerm::TypePatternCasedCharacterGreedy: {
            BackTrackInfoPatternCharacter* backTrack = reinterpret_cast<BackTrackInfoPatternCharacter*>(context->frame + currentTerm().frameLocation);
            unsigned matchAmount = 0;
            while ((matchAmount < currentTerm().atom.quantityCount) && input.checkInput(1)) {
                if (!checkCasedCharacter(currentTerm().atom.casedCharacter.lo, currentTerm().atom.casedCharacter.hi, currentTerm().inputPosition - 1)) {
                    input.uncheckInput(1);
                    break;
                }
                ++matchAmount;
            }
            backTrack->matchAmount = matchAmount;

            MATCH_NEXT();
        }
        case ByteTerm::TypePatternCasedCharacterNonGreedy: {
            BackTrackInfoPatternCharacter* backTrack = reinterpret_cast<BackTrackInfoPatternCharacter*>(context->frame + currentTerm().frameLocation);
            backTrack->matchAmount = 0;
            MATCH_NEXT();
        }

        case ByteTerm::TypeCharacterClass:
            if (matchCharacterClass(currentTerm(), context))
                MATCH_NEXT();
            BACKTRACK();
        case ByteTerm::TypeBackReference:
            if (matchBackReference(currentTerm(), context))
                MATCH_NEXT();
            BACKTRACK();
        case ByteTerm::TypeParenthesesSubpattern: {
            JSRegExpResult result = matchParentheses(currentTerm(), context);

            if (result == JSRegExpMatch) {
                MATCH_NEXT();
            }  else if (result != JSRegExpNoMatch)
                return result;

            BACKTRACK();
        }
        case ByteTerm::TypeParenthesesSubpatternOnceBegin:
            if (matchParenthesesOnceBegin(currentTerm(), context))
                MATCH_NEXT();
            BACKTRACK();
        case ByteTerm::TypeParenthesesSubpatternOnceEnd:
            if (matchParenthesesOnceEnd(currentTerm(), context))
                MATCH_NEXT();
            BACKTRACK();
        case ByteTerm::TypeParenthesesSubpatternTerminalBegin:
            if (matchParenthesesTerminalBegin(currentTerm(), context))
                MATCH_NEXT();
            BACKTRACK();
        case ByteTerm::TypeParenthesesSubpatternTerminalEnd:
            if (matchParenthesesTerminalEnd(currentTerm(), context))
                MATCH_NEXT();
            BACKTRACK();
        case ByteTerm::TypeParentheticalAssertionBegin:
            if (matchParentheticalAssertionBegin(currentTerm(), context))
                MATCH_NEXT();
            BACKTRACK();
        case ByteTerm::TypeParentheticalAssertionEnd:
            if (matchParentheticalAssertionEnd(currentTerm(), context))
                MATCH_NEXT();
            BACKTRACK();

        case ByteTerm::TypeCheckInput:
            if (input.checkInput(currentTerm().checkInputCount))
                MATCH_NEXT();
            BACKTRACK();
        }

        // We should never fall-through to here.
        ASSERT_NOT_REACHED();

    backtrack:
        ASSERT(context->term < static_cast<int>(disjunction->terms.size()));

        switch (currentTerm().type) {
        case ByteTerm::TypeSubpatternBegin:
            return JSRegExpNoMatch;
        case ByteTerm::TypeSubpatternEnd:
            ASSERT_NOT_REACHED();

        case ByteTerm::TypeBodyAlternativeBegin:
        case ByteTerm::TypeBodyAlternativeDisjunction: {
            int offset = currentTerm().alternative.next;
            context->term += offset;
            if (offset > 0)
                MATCH_NEXT();

            if (input.atEnd())
                return JSRegExpNoMatch;

            input.next();

            if (pattern->m_containsBeginChars && isBody)
                lookupForBeginChars();

            context->matchBegin = input.getPos();

            if (currentTerm().alternative.onceThrough)
                context->term += currentTerm().alternative.next;

            MATCH_NEXT();
        }
        case ByteTerm::TypeBodyAlternativeEnd:
            ASSERT_NOT_REACHED();

            case ByteTerm::TypeAlternativeBegin:
            case ByteTerm::TypeAlternativeDisjunction: {
                int offset = currentTerm().alternative.next;
                context->term += offset;
                if (offset > 0)
                    MATCH_NEXT();
                BACKTRACK();
            }
            case ByteTerm::TypeAlternativeEnd: {
                // We should never backtrack back into an alternative of the main body of the regex.
                BackTrackInfoAlternative* backTrack = reinterpret_cast<BackTrackInfoAlternative*>(context->frame + currentTerm().frameLocation);
                unsigned offset = backTrack->offset;
                context->term -= offset;
                BACKTRACK();
            }

            case ByteTerm::TypeAssertionBOL:
            case ByteTerm::TypeAssertionEOL:
            case ByteTerm::TypeAssertionWordBoundary:
                BACKTRACK();

            case ByteTerm::TypePatternCharacterOnce:
            case ByteTerm::TypePatternCharacterFixed:
            case ByteTerm::TypePatternCharacterGreedy:
            case ByteTerm::TypePatternCharacterNonGreedy:
                if (backtrackPatternCharacter(currentTerm(), context))
                    MATCH_NEXT();
                BACKTRACK();
            case ByteTerm::TypePatternCasedCharacterOnce:
            case ByteTerm::TypePatternCasedCharacterFixed:
            case ByteTerm::TypePatternCasedCharacterGreedy:
            case ByteTerm::TypePatternCasedCharacterNonGreedy:
                if (backtrackPatternCasedCharacter(currentTerm(), context))
                    MATCH_NEXT();
                BACKTRACK();
            case ByteTerm::TypeCharacterClass:
                if (backtrackCharacterClass(currentTerm(), context))
                    MATCH_NEXT();
                BACKTRACK();
            case ByteTerm::TypeBackReference:
                if (backtrackBackReference(currentTerm(), context))
                    MATCH_NEXT();
                BACKTRACK();
            case ByteTerm::TypeParenthesesSubpattern: {
                JSRegExpResult result = backtrackParentheses(currentTerm(), context);

                if (result == JSRegExpMatch) {
                    MATCH_NEXT();
                } else if (result != JSRegExpNoMatch)
                    return result;

                BACKTRACK();
            }
            case ByteTerm::TypeParenthesesSubpatternOnceBegin:
                if (backtrackParenthesesOnceBegin(currentTerm(), context))
                    MATCH_NEXT();
                BACKTRACK();
            case ByteTerm::TypeParenthesesSubpatternOnceEnd:
                if (backtrackParenthesesOnceEnd(currentTerm(), context))
                    MATCH_NEXT();
                BACKTRACK();
            case ByteTerm::TypeParenthesesSubpatternTerminalBegin:
                if (backtrackParenthesesTerminalBegin(currentTerm(), context))
                    MATCH_NEXT();
                BACKTRACK();
            case ByteTerm::TypeParenthesesSubpatternTerminalEnd:
                if (backtrackParenthesesTerminalEnd(currentTerm(), context))
                    MATCH_NEXT();
                BACKTRACK();
            case ByteTerm::TypeParentheticalAssertionBegin:
                if (backtrackParentheticalAssertionBegin(currentTerm(), context))
                    MATCH_NEXT();
                BACKTRACK();
            case ByteTerm::TypeParentheticalAssertionEnd:
                if (backtrackParentheticalAssertionEnd(currentTerm(), context))
                    MATCH_NEXT();
                BACKTRACK();

            case ByteTerm::TypeCheckInput:
                input.uncheckInput(currentTerm().checkInputCount);
                BACKTRACK();
        }

        ASSERT_NOT_REACHED();
        return JSRegExpErrorNoMatch;
    }

    JSRegExpResult matchNonZeroDisjunction(ByteDisjunction* disjunction, DisjunctionContext* context, bool btrack = false)
    {
        JSRegExpResult result = matchDisjunction(disjunction, context, btrack);

        if (result == JSRegExpMatch) {
            while (context->matchBegin == context->matchEnd) {
                result = matchDisjunction(disjunction, context, true);
                if (result != JSRegExpMatch)
                    return result;
            }
            return JSRegExpMatch;
        }

        return result;
    }

    int interpret()
    {
        allocatorPool = pattern->m_allocator->startAllocator();
        if (!allocatorPool)
            CRASH();

        for (unsigned i = 0; i < ((pattern->m_body->m_numSubpatterns + 1) << 1); ++i)
            output[i] = -1;

        DisjunctionContext* context = allocDisjunctionContext(pattern->m_body.get());

        JSRegExpResult result = matchDisjunction(pattern->m_body.get(), context, false, true);
        if (result == JSRegExpMatch) {
            output[0] = context->matchBegin;
            output[1] = context->matchEnd;
        }

        freeDisjunctionContext(context);

        pattern->m_allocator->stopAllocator();

        // RegExp.cpp currently expects all error to be converted to -1.
        ASSERT((result == JSRegExpMatch) == (output[0] != -1));
        return output[0];
    }

    Interpreter(BytecodePattern* pattern, int* output, const UChar* inputChar, unsigned start, unsigned length)
        : pattern(pattern)
        , output(output)
        , input(inputChar, start, length)
        , allocatorPool(0)
        , remainingMatchCount(matchLimit)
    {
    }

private:
    BytecodePattern *pattern;
    int* output;
    InputStream input;
    BumpPointerPool* allocatorPool;
    unsigned remainingMatchCount;
};



class ByteCompiler {
    struct ParenthesesStackEntry {
        unsigned beginTerm;
        unsigned savedAlternativeIndex;
        ParenthesesStackEntry(unsigned beginTerm, unsigned savedAlternativeIndex/*, unsigned subpatternId, bool capture = false*/)
            : beginTerm(beginTerm)
            , savedAlternativeIndex(savedAlternativeIndex)
        {
        }
    };

public:
    ByteCompiler(RegexPattern& pattern)
        : m_pattern(pattern)
    {
        m_currentAlternativeIndex = 0;
    }

    PassOwnPtr<BytecodePattern> compile(BumpPointerAllocator* allocator)
    {
        regexBegin(m_pattern.m_numSubpatterns, m_pattern.m_body->m_callFrameSize, m_pattern.m_body->m_alternatives[0]->onceThrough());
        emitDisjunction(m_pattern.m_body);
        regexEnd();

        return adoptPtr(new BytecodePattern(m_bodyDisjunction.release(), m_allParenthesesInfo, m_pattern, allocator));
    }

    void checkInput(unsigned count)
    {
        m_bodyDisjunction->terms.append(ByteTerm::CheckInput(count));
    }

    void assertionBOL(int inputPosition)
    {
        m_bodyDisjunction->terms.append(ByteTerm::BOL(inputPosition));
    }

    void assertionEOL(int inputPosition)
    {
        m_bodyDisjunction->terms.append(ByteTerm::EOL(inputPosition));
    }

    void assertionWordBoundary(bool invert, int inputPosition)
    {
        m_bodyDisjunction->terms.append(ByteTerm::WordBoundary(invert, inputPosition));
    }

    void atomPatternCharacter(UChar ch, int inputPosition, unsigned frameLocation, unsigned quantityCount, QuantifierType quantityType)
    {
        if (m_pattern.m_ignoreCase) {
            UChar lo = Unicode::toLower(ch);
            UChar hi = Unicode::toUpper(ch);

            if (lo != hi) {
                m_bodyDisjunction->terms.append(ByteTerm(lo, hi, inputPosition, frameLocation, quantityCount, quantityType));
                return;
            }
        }

        m_bodyDisjunction->terms.append(ByteTerm(ch, inputPosition, frameLocation, quantityCount, quantityType));
    }

    void atomCharacterClass(CharacterClass* characterClass, bool invert, int inputPosition, unsigned frameLocation, unsigned quantityCount, QuantifierType quantityType)
    {
        m_bodyDisjunction->terms.append(ByteTerm(characterClass, invert, inputPosition));

        m_bodyDisjunction->terms[m_bodyDisjunction->terms.size() - 1].atom.quantityCount = quantityCount;
        m_bodyDisjunction->terms[m_bodyDisjunction->terms.size() - 1].atom.quantityType = quantityType;
        m_bodyDisjunction->terms[m_bodyDisjunction->terms.size() - 1].frameLocation = frameLocation;
    }

    void atomBackReference(unsigned subpatternId, int inputPosition, unsigned frameLocation, unsigned quantityCount, QuantifierType quantityType)
    {
        ASSERT(subpatternId);

        m_bodyDisjunction->terms.append(ByteTerm::BackReference(subpatternId, inputPosition));

        m_bodyDisjunction->terms[m_bodyDisjunction->terms.size() - 1].atom.quantityCount = quantityCount;
        m_bodyDisjunction->terms[m_bodyDisjunction->terms.size() - 1].atom.quantityType = quantityType;
        m_bodyDisjunction->terms[m_bodyDisjunction->terms.size() - 1].frameLocation = frameLocation;
    }

    void atomParenthesesOnceBegin(unsigned subpatternId, bool capture, int inputPosition, unsigned frameLocation, unsigned alternativeFrameLocation)
    {
        int beginTerm = m_bodyDisjunction->terms.size();

        m_bodyDisjunction->terms.append(ByteTerm(ByteTerm::TypeParenthesesSubpatternOnceBegin, subpatternId, capture, inputPosition));
        m_bodyDisjunction->terms[m_bodyDisjunction->terms.size() - 1].frameLocation = frameLocation;
        m_bodyDisjunction->terms.append(ByteTerm::AlternativeBegin());
        m_bodyDisjunction->terms[m_bodyDisjunction->terms.size() - 1].frameLocation = alternativeFrameLocation;

        m_parenthesesStack.append(ParenthesesStackEntry(beginTerm, m_currentAlternativeIndex));
        m_currentAlternativeIndex = beginTerm + 1;
    }

    void atomParenthesesTerminalBegin(unsigned subpatternId, bool capture, int inputPosition, unsigned frameLocation, unsigned alternativeFrameLocation)
    {
        int beginTerm = m_bodyDisjunction->terms.size();

        m_bodyDisjunction->terms.append(ByteTerm(ByteTerm::TypeParenthesesSubpatternTerminalBegin, subpatternId, capture, inputPosition));
        m_bodyDisjunction->terms[m_bodyDisjunction->terms.size() - 1].frameLocation = frameLocation;
        m_bodyDisjunction->terms.append(ByteTerm::AlternativeBegin());
        m_bodyDisjunction->terms[m_bodyDisjunction->terms.size() - 1].frameLocation = alternativeFrameLocation;

        m_parenthesesStack.append(ParenthesesStackEntry(beginTerm, m_currentAlternativeIndex));
        m_currentAlternativeIndex = beginTerm + 1;
    }

    void atomParenthesesSubpatternBegin(unsigned subpatternId, bool capture, int inputPosition, unsigned frameLocation, unsigned alternativeFrameLocation)
    {
        // Errrk! - this is a little crazy, we initially generate as a TypeParenthesesSubpatternOnceBegin,
        // then fix this up at the end! - simplifying this should make it much clearer.
        // https://bugs.webkit.org/show_bug.cgi?id=50136

        int beginTerm = m_bodyDisjunction->terms.size();

        m_bodyDisjunction->terms.append(ByteTerm(ByteTerm::TypeParenthesesSubpatternOnceBegin, subpatternId, capture, inputPosition));
        m_bodyDisjunction->terms[m_bodyDisjunction->terms.size() - 1].frameLocation = frameLocation;
        m_bodyDisjunction->terms.append(ByteTerm::AlternativeBegin());
        m_bodyDisjunction->terms[m_bodyDisjunction->terms.size() - 1].frameLocation = alternativeFrameLocation;

        m_parenthesesStack.append(ParenthesesStackEntry(beginTerm, m_currentAlternativeIndex));
        m_currentAlternativeIndex = beginTerm + 1;
    }

    void atomParentheticalAssertionBegin(unsigned subpatternId, bool invert, unsigned frameLocation, unsigned alternativeFrameLocation)
    {
        int beginTerm = m_bodyDisjunction->terms.size();

        m_bodyDisjunction->terms.append(ByteTerm(ByteTerm::TypeParentheticalAssertionBegin, subpatternId, invert, 0));
        m_bodyDisjunction->terms[m_bodyDisjunction->terms.size() - 1].frameLocation = frameLocation;
        m_bodyDisjunction->terms.append(ByteTerm::AlternativeBegin());
        m_bodyDisjunction->terms[m_bodyDisjunction->terms.size() - 1].frameLocation = alternativeFrameLocation;

        m_parenthesesStack.append(ParenthesesStackEntry(beginTerm, m_currentAlternativeIndex));
        m_currentAlternativeIndex = beginTerm + 1;
    }

    void atomParentheticalAssertionEnd(int inputPosition, unsigned frameLocation, unsigned quantityCount, QuantifierType quantityType)
    {
        unsigned beginTerm = popParenthesesStack();
        closeAlternative(beginTerm + 1);
        unsigned endTerm = m_bodyDisjunction->terms.size();

        ASSERT(m_bodyDisjunction->terms[beginTerm].type == ByteTerm::TypeParentheticalAssertionBegin);

        bool invertOrCapture = m_bodyDisjunction->terms[beginTerm].invertOrCapture;
        unsigned subpatternId = m_bodyDisjunction->terms[beginTerm].atom.subpatternId;

        m_bodyDisjunction->terms.append(ByteTerm(ByteTerm::TypeParentheticalAssertionEnd, subpatternId, invertOrCapture, inputPosition));
        m_bodyDisjunction->terms[beginTerm].atom.parenthesesWidth = endTerm - beginTerm;
        m_bodyDisjunction->terms[endTerm].atom.parenthesesWidth = endTerm - beginTerm;
        m_bodyDisjunction->terms[endTerm].frameLocation = frameLocation;

        m_bodyDisjunction->terms[beginTerm].atom.quantityCount = quantityCount;
        m_bodyDisjunction->terms[beginTerm].atom.quantityType = quantityType;
        m_bodyDisjunction->terms[endTerm].atom.quantityCount = quantityCount;
        m_bodyDisjunction->terms[endTerm].atom.quantityType = quantityType;
    }

    unsigned popParenthesesStack()
    {
        ASSERT(m_parenthesesStack.size());
        int stackEnd = m_parenthesesStack.size() - 1;
        unsigned beginTerm = m_parenthesesStack[stackEnd].beginTerm;
        m_currentAlternativeIndex = m_parenthesesStack[stackEnd].savedAlternativeIndex;
        m_parenthesesStack.shrink(stackEnd);

        ASSERT(beginTerm < m_bodyDisjunction->terms.size());
        ASSERT(m_currentAlternativeIndex < m_bodyDisjunction->terms.size());

        return beginTerm;
    }

#ifndef NDEBUG
    void dumpDisjunction(ByteDisjunction* disjunction)
    {
        printf("ByteDisjunction(%p):\n\t", disjunction);
        for (unsigned i = 0; i < disjunction->terms.size(); ++i)
            printf("{ %d } ", disjunction->terms[i].type);
        printf("\n");
    }
#endif

    void closeAlternative(int beginTerm)
    {
        int origBeginTerm = beginTerm;
        ASSERT(m_bodyDisjunction->terms[beginTerm].type == ByteTerm::TypeAlternativeBegin);
        int endIndex = m_bodyDisjunction->terms.size();

        unsigned frameLocation = m_bodyDisjunction->terms[beginTerm].frameLocation;

        if (!m_bodyDisjunction->terms[beginTerm].alternative.next)
            m_bodyDisjunction->terms.remove(beginTerm);
        else {
            while (m_bodyDisjunction->terms[beginTerm].alternative.next) {
                beginTerm += m_bodyDisjunction->terms[beginTerm].alternative.next;
                ASSERT(m_bodyDisjunction->terms[beginTerm].type == ByteTerm::TypeAlternativeDisjunction);
                m_bodyDisjunction->terms[beginTerm].alternative.end = endIndex - beginTerm;
                m_bodyDisjunction->terms[beginTerm].frameLocation = frameLocation;
            }

            m_bodyDisjunction->terms[beginTerm].alternative.next = origBeginTerm - beginTerm;

            m_bodyDisjunction->terms.append(ByteTerm::AlternativeEnd());
            m_bodyDisjunction->terms[endIndex].frameLocation = frameLocation;
        }
    }

    void closeBodyAlternative()
    {
        int beginTerm = 0;
        int origBeginTerm = 0;
        ASSERT(m_bodyDisjunction->terms[beginTerm].type == ByteTerm::TypeBodyAlternativeBegin);
        int endIndex = m_bodyDisjunction->terms.size();

        unsigned frameLocation = m_bodyDisjunction->terms[beginTerm].frameLocation;

        while (m_bodyDisjunction->terms[beginTerm].alternative.next) {
            beginTerm += m_bodyDisjunction->terms[beginTerm].alternative.next;
            ASSERT(m_bodyDisjunction->terms[beginTerm].type == ByteTerm::TypeBodyAlternativeDisjunction);
            m_bodyDisjunction->terms[beginTerm].alternative.end = endIndex - beginTerm;
            m_bodyDisjunction->terms[beginTerm].frameLocation = frameLocation;
        }

        m_bodyDisjunction->terms[beginTerm].alternative.next = origBeginTerm - beginTerm;

        m_bodyDisjunction->terms.append(ByteTerm::BodyAlternativeEnd());
        m_bodyDisjunction->terms[endIndex].frameLocation = frameLocation;
    }

    void atomParenthesesSubpatternEnd(unsigned lastSubpatternId, int inputPosition, unsigned frameLocation, unsigned quantityCount, QuantifierType quantityType, unsigned callFrameSize = 0)
    {
        unsigned beginTerm = popParenthesesStack();
        closeAlternative(beginTerm + 1);
        unsigned endTerm = m_bodyDisjunction->terms.size();

        ASSERT(m_bodyDisjunction->terms[beginTerm].type == ByteTerm::TypeParenthesesSubpatternOnceBegin);

        ByteTerm& parenthesesBegin = m_bodyDisjunction->terms[beginTerm];

        bool invertOrCapture = parenthesesBegin.invertOrCapture;
        unsigned subpatternId = parenthesesBegin.atom.subpatternId;

        unsigned numSubpatterns = lastSubpatternId - subpatternId + 1;
        ByteDisjunction* parenthesesDisjunction = new ByteDisjunction(numSubpatterns, callFrameSize);

        parenthesesDisjunction->terms.append(ByteTerm::SubpatternBegin());
        for (unsigned termInParentheses = beginTerm + 1; termInParentheses < endTerm; ++termInParentheses)
            parenthesesDisjunction->terms.append(m_bodyDisjunction->terms[termInParentheses]);
        parenthesesDisjunction->terms.append(ByteTerm::SubpatternEnd());

        m_bodyDisjunction->terms.shrink(beginTerm);

        m_allParenthesesInfo.append(parenthesesDisjunction);
        m_bodyDisjunction->terms.append(ByteTerm(ByteTerm::TypeParenthesesSubpattern, subpatternId, parenthesesDisjunction, invertOrCapture, inputPosition));

        m_bodyDisjunction->terms[beginTerm].atom.quantityCount = quantityCount;
        m_bodyDisjunction->terms[beginTerm].atom.quantityType = quantityType;
        m_bodyDisjunction->terms[beginTerm].frameLocation = frameLocation;
    }

    void atomParenthesesOnceEnd(int inputPosition, unsigned frameLocation, unsigned quantityCount, QuantifierType quantityType)
    {
        unsigned beginTerm = popParenthesesStack();
        closeAlternative(beginTerm + 1);
        unsigned endTerm = m_bodyDisjunction->terms.size();

        ASSERT(m_bodyDisjunction->terms[beginTerm].type == ByteTerm::TypeParenthesesSubpatternOnceBegin);

        bool invertOrCapture = m_bodyDisjunction->terms[beginTerm].invertOrCapture;
        unsigned subpatternId = m_bodyDisjunction->terms[beginTerm].atom.subpatternId;

        m_bodyDisjunction->terms.append(ByteTerm(ByteTerm::TypeParenthesesSubpatternOnceEnd, subpatternId, invertOrCapture, inputPosition));
        m_bodyDisjunction->terms[beginTerm].atom.parenthesesWidth = endTerm - beginTerm;
        m_bodyDisjunction->terms[endTerm].atom.parenthesesWidth = endTerm - beginTerm;
        m_bodyDisjunction->terms[endTerm].frameLocation = frameLocation;

        m_bodyDisjunction->terms[beginTerm].atom.quantityCount = quantityCount;
        m_bodyDisjunction->terms[beginTerm].atom.quantityType = quantityType;
        m_bodyDisjunction->terms[endTerm].atom.quantityCount = quantityCount;
        m_bodyDisjunction->terms[endTerm].atom.quantityType = quantityType;
    }

    void atomParenthesesTerminalEnd(int inputPosition, unsigned frameLocation, unsigned quantityCount, QuantifierType quantityType)
    {
        unsigned beginTerm = popParenthesesStack();
        closeAlternative(beginTerm + 1);
        unsigned endTerm = m_bodyDisjunction->terms.size();

        ASSERT(m_bodyDisjunction->terms[beginTerm].type == ByteTerm::TypeParenthesesSubpatternTerminalBegin);

        bool invertOrCapture = m_bodyDisjunction->terms[beginTerm].invertOrCapture;
        unsigned subpatternId = m_bodyDisjunction->terms[beginTerm].atom.subpatternId;

        m_bodyDisjunction->terms.append(ByteTerm(ByteTerm::TypeParenthesesSubpatternTerminalEnd, subpatternId, invertOrCapture, inputPosition));
        m_bodyDisjunction->terms[beginTerm].atom.parenthesesWidth = endTerm - beginTerm;
        m_bodyDisjunction->terms[endTerm].atom.parenthesesWidth = endTerm - beginTerm;
        m_bodyDisjunction->terms[endTerm].frameLocation = frameLocation;

        m_bodyDisjunction->terms[beginTerm].atom.quantityCount = quantityCount;
        m_bodyDisjunction->terms[beginTerm].atom.quantityType = quantityType;
        m_bodyDisjunction->terms[endTerm].atom.quantityCount = quantityCount;
        m_bodyDisjunction->terms[endTerm].atom.quantityType = quantityType;
    }

    void regexBegin(unsigned numSubpatterns, unsigned callFrameSize, bool onceThrough)
    {
        m_bodyDisjunction = adoptPtr(new ByteDisjunction(numSubpatterns, callFrameSize));
        m_bodyDisjunction->terms.append(ByteTerm::BodyAlternativeBegin(onceThrough));
        m_bodyDisjunction->terms[0].frameLocation = 0;
        m_currentAlternativeIndex = 0;
    }

    void regexEnd()
    {
        closeBodyAlternative();
    }

    void alternativeBodyDisjunction(bool onceThrough)
    {
        int newAlternativeIndex = m_bodyDisjunction->terms.size();
        m_bodyDisjunction->terms[m_currentAlternativeIndex].alternative.next = newAlternativeIndex - m_currentAlternativeIndex;
        m_bodyDisjunction->terms.append(ByteTerm::BodyAlternativeDisjunction(onceThrough));

        m_currentAlternativeIndex = newAlternativeIndex;
    }

    void alternativeDisjunction()
    {
        int newAlternativeIndex = m_bodyDisjunction->terms.size();
        m_bodyDisjunction->terms[m_currentAlternativeIndex].alternative.next = newAlternativeIndex - m_currentAlternativeIndex;
        m_bodyDisjunction->terms.append(ByteTerm::AlternativeDisjunction());

        m_currentAlternativeIndex = newAlternativeIndex;
    }

    void emitDisjunction(PatternDisjunction* disjunction, unsigned inputCountAlreadyChecked = 0, unsigned parenthesesInputCountAlreadyChecked = 0, bool isParentheticalAssertion = false)
    {
        for (unsigned alt = 0; alt < disjunction->m_alternatives.size(); ++alt) {
            unsigned currentCountAlreadyChecked = inputCountAlreadyChecked;

            PatternAlternative* alternative = disjunction->m_alternatives[alt];

            if (alt) {
                if (disjunction == m_pattern.m_body)
                    alternativeBodyDisjunction(alternative->onceThrough());
                else
                    alternativeDisjunction();
            }

            unsigned minimumSize = alternative->m_minimumSize;
            int countToCheck;

            if (isParentheticalAssertion && parenthesesInputCountAlreadyChecked > minimumSize)
                countToCheck = 0;
            else
                countToCheck = minimumSize - parenthesesInputCountAlreadyChecked;

            ASSERT(countToCheck >= 0);
            if (countToCheck) {
                checkInput(countToCheck);
                currentCountAlreadyChecked += countToCheck;
            }

            for (unsigned i = 0; i < alternative->m_terms.size(); ++i) {
                PatternTerm& term = alternative->m_terms[i];

                switch (term.type) {
                case PatternTerm::TypeAssertionBOL:
                    assertionBOL(term.inputPosition - currentCountAlreadyChecked);
                    break;

                case PatternTerm::TypeAssertionEOL:
                    assertionEOL(term.inputPosition - currentCountAlreadyChecked);
                    break;

                case PatternTerm::TypeAssertionWordBoundary:
                    assertionWordBoundary(term.invertOrCapture, term.inputPosition - currentCountAlreadyChecked);
                    break;

                case PatternTerm::TypePatternCharacter:
                    atomPatternCharacter(term.patternCharacter, term.inputPosition - currentCountAlreadyChecked, term.frameLocation, term.quantityCount, term.quantityType);
                    break;

                case PatternTerm::TypeCharacterClass:
                    atomCharacterClass(term.characterClass, term.invertOrCapture, term.inputPosition - currentCountAlreadyChecked, term.frameLocation, term.quantityCount, term.quantityType);
                    break;

                case PatternTerm::TypeBackReference:
                    atomBackReference(term.subpatternId, term.inputPosition - currentCountAlreadyChecked, term.frameLocation, term.quantityCount, term.quantityType);
                        break;

                case PatternTerm::TypeForwardReference:
                    break;

                case PatternTerm::TypeParenthesesSubpattern: {
                    unsigned disjunctionAlreadyCheckedCount = 0;
                    if (term.quantityCount == 1 && !term.parentheses.isCopy) {
                        unsigned alternativeFrameLocation = term.frameLocation;
                        // For QuantifierFixedCount we pre-check the minimum size; for greedy/non-greedy we reserve a slot in the frame.
                        if (term.quantityType == QuantifierFixedCount)
                            disjunctionAlreadyCheckedCount = term.parentheses.disjunction->m_minimumSize;
                        else
                            alternativeFrameLocation += RegexStackSpaceForBackTrackInfoParenthesesOnce;
                        unsigned delegateEndInputOffset = term.inputPosition - currentCountAlreadyChecked;
                        atomParenthesesOnceBegin(term.parentheses.subpatternId, term.invertOrCapture, delegateEndInputOffset - disjunctionAlreadyCheckedCount, term.frameLocation, alternativeFrameLocation);
                        emitDisjunction(term.parentheses.disjunction, currentCountAlreadyChecked, disjunctionAlreadyCheckedCount);
                        atomParenthesesOnceEnd(delegateEndInputOffset, term.frameLocation, term.quantityCount, term.quantityType);
                    } else if (term.parentheses.isTerminal) {
                        unsigned delegateEndInputOffset = term.inputPosition - currentCountAlreadyChecked;
                        atomParenthesesTerminalBegin(term.parentheses.subpatternId, term.invertOrCapture, delegateEndInputOffset - disjunctionAlreadyCheckedCount, term.frameLocation, term.frameLocation + RegexStackSpaceForBackTrackInfoParenthesesOnce);
                        emitDisjunction(term.parentheses.disjunction, currentCountAlreadyChecked, disjunctionAlreadyCheckedCount);
                        atomParenthesesTerminalEnd(delegateEndInputOffset, term.frameLocation, term.quantityCount, term.quantityType);
                    } else {
                        unsigned delegateEndInputOffset = term.inputPosition - currentCountAlreadyChecked;
                        atomParenthesesSubpatternBegin(term.parentheses.subpatternId, term.invertOrCapture, delegateEndInputOffset - disjunctionAlreadyCheckedCount, term.frameLocation, 0);
                        emitDisjunction(term.parentheses.disjunction, currentCountAlreadyChecked, 0);
                        atomParenthesesSubpatternEnd(term.parentheses.lastSubpatternId, delegateEndInputOffset, term.frameLocation, term.quantityCount, term.quantityType, term.parentheses.disjunction->m_callFrameSize);
                    }
                    break;
                }

                case PatternTerm::TypeParentheticalAssertion: {
                    unsigned alternativeFrameLocation = term.frameLocation + RegexStackSpaceForBackTrackInfoParentheticalAssertion;

                    ASSERT(currentCountAlreadyChecked >= (unsigned)term.inputPosition);
                    int positiveInputOffset = currentCountAlreadyChecked - term.inputPosition;

                    atomParentheticalAssertionBegin(term.parentheses.subpatternId, term.invertOrCapture, term.frameLocation, alternativeFrameLocation);
                    emitDisjunction(term.parentheses.disjunction, currentCountAlreadyChecked, positiveInputOffset, true);
                    atomParentheticalAssertionEnd(0, term.frameLocation, term.quantityCount, term.quantityType);
                    break;
                }
                }
            }
        }
    }

private:
    RegexPattern& m_pattern;
    OwnPtr<ByteDisjunction> m_bodyDisjunction;
    unsigned m_currentAlternativeIndex;
    Vector<ParenthesesStackEntry> m_parenthesesStack;
    Vector<ByteDisjunction*> m_allParenthesesInfo;
};


PassOwnPtr<BytecodePattern> byteCompileRegex(const UString& patternString, unsigned& numSubpatterns, const char*& error, BumpPointerAllocator* allocator, bool ignoreCase, bool multiline)
{
    RegexPattern pattern(ignoreCase, multiline);

    if ((error = compileRegex(patternString, pattern)))
        return PassOwnPtr<BytecodePattern>();

    numSubpatterns = pattern.m_numSubpatterns;

    return ByteCompiler(pattern).compile(allocator);
}

PassOwnPtr<BytecodePattern> byteCompileRegex(RegexPattern& pattern, BumpPointerAllocator* allocator)
{
    return ByteCompiler(pattern).compile(allocator);
}

int interpretRegex(BytecodePattern* regex, const UChar* input, unsigned start, unsigned length, int* output)
{
    return Interpreter(regex, output, input, start, length).interpret();
}


COMPILE_ASSERT(sizeof(Interpreter::BackTrackInfoPatternCharacter) == (RegexStackSpaceForBackTrackInfoPatternCharacter * sizeof(uintptr_t)), CheckRegexStackSpaceForBackTrackInfoPatternCharacter);
COMPILE_ASSERT(sizeof(Interpreter::BackTrackInfoCharacterClass) == (RegexStackSpaceForBackTrackInfoCharacterClass * sizeof(uintptr_t)), CheckRegexStackSpaceForBackTrackInfoCharacterClass);
COMPILE_ASSERT(sizeof(Interpreter::BackTrackInfoBackReference) == (RegexStackSpaceForBackTrackInfoBackReference * sizeof(uintptr_t)), CheckRegexStackSpaceForBackTrackInfoBackReference);
COMPILE_ASSERT(sizeof(Interpreter::BackTrackInfoAlternative) == (RegexStackSpaceForBackTrackInfoAlternative * sizeof(uintptr_t)), CheckRegexStackSpaceForBackTrackInfoAlternative);
COMPILE_ASSERT(sizeof(Interpreter::BackTrackInfoParentheticalAssertion) == (RegexStackSpaceForBackTrackInfoParentheticalAssertion * sizeof(uintptr_t)), CheckRegexStackSpaceForBackTrackInfoParentheticalAssertion);
COMPILE_ASSERT(sizeof(Interpreter::BackTrackInfoParenthesesOnce) == (RegexStackSpaceForBackTrackInfoParenthesesOnce * sizeof(uintptr_t)), CheckRegexStackSpaceForBackTrackInfoParenthesesOnce);
COMPILE_ASSERT(sizeof(Interpreter::BackTrackInfoParentheses) == (RegexStackSpaceForBackTrackInfoParentheses * sizeof(uintptr_t)), CheckRegexStackSpaceForBackTrackInfoParentheses);


} }
