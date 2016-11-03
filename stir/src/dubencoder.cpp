/* -*- mode: C; c-basic-offset: 4; intent-tabs-mode: nil -*-
 *
 * STIR -- Sifteo Tiled Image Reducer
 * Micah Elizabeth Scott <micah@misc.name>
 *
 * Copyright <c> 2012 Sifteo, Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 * 
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 * 
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include <map>
#include <assert.h>
#include "dubencoder.h"
#include "logger.h"
using namespace Stir;

// Seems to be the sweet spot, as far as powers-of-two go.
const unsigned DUBEncoder::BLOCK_SIZE = 8;


void DUBEncoder::encodeTiles(std::vector<uint16_t> &tiles)
{
    BitBuffer bits;

    // Deduplicate blocks. If we ever get two of the same, give them the
    // same address in the index.
    typedef std::map<std::vector<uint16_t>, uint16_t> dedupeMemo_t;
    dedupeMemo_t dedupeMemo;

    // Encode blocks, and store an index with 16-bit addresses since the
    // beginning of the block data.

    for (unsigned f = 0; f < mFrames; f++)
        for (unsigned y = 0; y < mHeight; y += BLOCK_SIZE)
            for (unsigned x = 0; x < mWidth; x += BLOCK_SIZE) {
                unsigned w = std::min(BLOCK_SIZE, mWidth - x);
                unsigned h = std::min(BLOCK_SIZE, mHeight - y);

                std::vector<uint16_t> blockData;
                encodeBlock(&tiles[x + y*mWidth + f*mWidth*mHeight], w, h, blockData);

                dedupeMemo_t::iterator I = dedupeMemo.find(blockData);
                if (I == dedupeMemo.end()) {
                    // Unique block
                    uint16_t addr = blockResult.size();
                    indexResult.push_back(addr);
                    dedupeMemo[blockData] = addr;
                    blockResult.insert(blockResult.end(),
                        blockData.begin(), blockData.end());

                } else {
                    // Duplicated block
                    indexResult.push_back(I->second);
                }
            }

    // Now scan the index and see if it would fit in 8 bits. If not,
    // we'll use a 16-bit index.

    mIndex16 = false;
    for (unsigned I = 0, E = indexResult.size(); I != E; ++I) {
        if (packIndex(I) >= 0x100) {
            mIndex16 = true;
            break;
        }
    }
}

unsigned DUBEncoder::getNumBlocks() const
{
    return ((mWidth + BLOCK_SIZE - 1) / BLOCK_SIZE) *
           ((mHeight + BLOCK_SIZE - 1) / BLOCK_SIZE) *
           mFrames;
}

unsigned DUBEncoder::packIndex(unsigned i) const
{
    // Index stores a word offset from the _next_ word after the
    // current one in the index.

    unsigned nextWord = mIndex16 ? (i + 1) : (i + 2)/2;
    return getIndexSize() + indexResult[i] - nextWord;
}

unsigned DUBEncoder::getIndexSize() const
{
    // Size of the index, in words
    unsigned s = indexResult.size();
    if (!mIndex16)
        s = (s + 1) / 2;
    return s;
}

bool DUBEncoder::isTooLarge() const
{
    return (getIndexSize() + blockResult.size()) >= 0x10000;
}

bool DUBEncoder::isIndex16() const
{
    return mIndex16;
}

void DUBEncoder::getResult(std::vector<uint16_t> &result) const
{
    // Relocate and pack the index

    if (mIndex16) {
        for (unsigned I = 0, E = indexResult.size(); I != E; ++I)
            result.push_back(packIndex(I));

    } else {
        std::vector<uint8_t> index8;
        for (unsigned I = 0, E = indexResult.size(); I != E; ++I)
            index8.push_back(packIndex(I));
        if (index8.size() & 1)
            index8.push_back(0);

        // Pack into little-endian 16-bit words
        for (unsigned I = 0, E = index8.size(); I != E; I += 2)
            result.push_back(index8[I] | (index8[I+1] << 8));
    }

    assert(result.size() == getIndexSize());

    // Insert block data as-is
    result.insert(result.end(), blockResult.begin(), blockResult.end());
}

unsigned DUBEncoder::getTileCount() const
{
    return mWidth * mHeight * mFrames;
}

unsigned DUBEncoder::getCompressedWords() const
{
    return getIndexSize() + blockResult.size();
}

float DUBEncoder::getRatio() const
{
    return 100.0 - getCompressedWords() * 100.0 / getTileCount();
}

void DUBEncoder::logStats(const std::string &name, Logger &log)
{
    log.infoLineWithLabel(name.c_str(),
        "%4d tiles, %4d words, % 5.01f%% compression",
        getTileCount(), getCompressedWords(), getRatio());
}

void DUBEncoder::encodeBlock(uint16_t *pTopLeft,
    unsigned width, unsigned height, std::vector<uint16_t> &data)
{
    BitBuffer bits;
    std::vector<uint16_t> dict;
    Code prevCode = { Code::INVALID };
    unsigned repeatCount = 0;
    bool repeating = false;

    for (unsigned y = 0; y < height; y++)
        for (unsigned x = 0; x < width; x++) {
            uint16_t tile = pTopLeft[x + y*mWidth];

            // Find the best code for this tile, and update the dictionary
            Code code = findBestCode(dict, tile);
            dict.push_back(tile);

            // If we ever output two identical codes in a row, that counts
            // as a run. The next code *must* be a REPEAT code.
            bool sameCode = (code.type == prevCode.type && code.value == prevCode.value);
            prevCode = code;

            if (repeating) {
                if (sameCode) {
                    // Extending an existing run
                    repeatCount++;
                    continue;
                } else {
                    // Break an existing run
                    Code rep = { Code::REPEAT, static_cast<int>(repeatCount) };
                    debugCode(x, y, rep, tile);
                    packCode(rep, bits);
                    bits.flush(data);
                    repeating = false;
                }
            } else if (sameCode) {
                // Beginning a run. The next code will be a REPEAT.
                repeating = true;
                repeatCount = 0;
            }

            debugCode(x, y, code, tile);
            packCode(code, bits);
            bits.flush(data);
        }

    if (repeating) {
        // Flush any final REPEAT code we have stowed away.
        Code rep = { Code::REPEAT, static_cast<int>(repeatCount) };
        debugCode(-1, -1, rep, -1);
        packCode(rep, bits);
    }

    // Flush all remaining data, padding to a word boundary.
    bits.flush(data, true);
}

void DUBEncoder::debugCode(int x, int y, DUBEncoder::Code c, int tile) const
{
#ifdef DEBUG_DUB
    printf("DUB: (%d, %d) - {%d,%d} = %04x\n", x, y, c.type, c.value, tile);
#endif
}

DUBEncoder::Code DUBEncoder::findBestCode(const std::vector<uint16_t> &dict, uint16_t tile)
{
    Code code;
    unsigned bestLength;

    if (dict.empty()) {
        // Dictionary is empty. This is a special case in which DELTA
        // codes are literal. In effect, the nonexistant last entry in
        // the dict is treated as zero.

        code.type = Code::DELTA;
        code.value = tile;
        bestLength = codeLen(code);

    } else {
        // Try a DELTA code based on the most recent dictionary entry.

        int delta = (int)tile - (int)dict.back();
        code.type = Code::DELTA;
        code.value = delta;
        bestLength = codeLen(code);
    }

    // Now see if we can do better by scanning for an identical tile
    // in our history, and emitting a REF code. In the event of a tie,
    // always prefer a REF code.

    for (unsigned i = 0; i < dict.size(); i++)
        if (tile == dict[dict.size() - 1 - i]) {
            Code candidate = { Code::REF, static_cast<int>(i) };
            unsigned len = codeLen(candidate);
            if (len <= bestLength) {
                code = candidate;
                bestLength = len;
                
                // We won't do better by increasing 'i' at this point.
                break;
            }
        }

    return code;
}

void DUBEncoder::packCode(Code code, BitBuffer &bits) const
{
    // Experimentally determined sweet-spot
    const unsigned chunk = 3;

    switch (code.type) {

    case Code::DELTA:
        // Type bit, sign bit, delta
        bits.append(0, 1);
        if (code.value < 0) {
            bits.append(1, 1);
            bits.appendVar(-code.value, chunk);
        } else {
            bits.append(0, 1);
            bits.appendVar(code.value, chunk);
        }
        break;

    case Code::REF:
        // Type bit, backref index
        bits.append(1, 1);
        bits.appendVar(code.value, chunk);
        break;

    case Code::REPEAT:
        // Repeat count only, no header.
        // This code only appears after two repeated codes.
        bits.appendVar(code.value, chunk);
        break;

    case Code::INVALID:
        assert(0);
        break;
    }
}

unsigned DUBEncoder::codeLen(Code code) const
{
    BitBuffer bits;
    packCode(code, bits);
    return bits.getCount();
}
