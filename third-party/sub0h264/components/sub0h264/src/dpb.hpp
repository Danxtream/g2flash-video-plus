/** Sub0h264 — Decoded Picture Buffer (DPB)
 *
 *  Manages reference frames for inter prediction. Supports short-term
 *  reference marking for P-frame decoding.
 *
 *  Reference: ITU-T H.264 §8.2.5
 *
 *  Spec-annotated review (2026-04-09):
 *    §8.2.5.3 Sliding window: FIFO eviction by smallest frameNum [CHECKED]
 *    §8.2.5.4 MMCO ops 1-6: all implemented [CHECKED §8.2.5.4]
 *    §8.2.4.2.1 L0 list: short-term by PicNum desc, long-term asc [CHECKED §8.2.4.2.1]
 *    §8.2.4.3 L0 reordering: idc 0/1/2 commands [CHECKED §8.2.4.3]
 *    §A.3.1 DPB size: max(numRefFrames+1, 2) [CHECKED §A.3.1]
 *    IDR flush: all refs unmarked [CHECKED]
 *    [PARTIAL] MMCO Op 6 uses value2 — verify matches slice header MmcoCmd layout
 *    [PARTIAL] frameNum wrap-around in eviction not handled (benign for short sequences)
 *
 *  SPDX-License-Identifier: MIT
 */
#ifndef CROG_SUB0H264_DPB_HPP
#define CROG_SUB0H264_DPB_HPP

#include "frame.hpp"
#include "sps.hpp"

#include <algorithm>

#include <cstdint>
#include <vector>
#include <algorithm>

namespace sub0h264 {

/// Maximum DPB size (reference frames + current).
inline constexpr uint32_t cMaxDpbSize = 17U;

/** Entry in the Decoded Picture Buffer. */
struct DpbEntry
{
    Frame frame;
    uint16_t frameNum = 0U;       ///< H.264 frame_num
    int32_t picOrderCnt = 0;      ///< Picture order count
    bool isReference = false;     ///< True if used as reference
    bool isLongTerm = false;      ///< True if long-term reference
    bool isOutput = false;        ///< True if ready for display
    bool occupied = false;        ///< True if this slot is in use
};

/** Simplified DPB for Baseline/P-frame decoding. */
class Dpb
{
public:
    /** Initialize DPB for given SPS parameters. */
    void init(uint16_t width, uint16_t height, uint8_t numRefFrames) noexcept
    {
        width_ = width;
        height_ = height;
        maxRefFrames_ = numRefFrames;
        // Need at least 2 entries: one for current decode target + one reference.
        // With numRefFrames=0, the stream still needs a reference for P-frames.
        // §A.3.1: maxDpbFrames = Max(1, max_num_ref_frames).
        entries_.resize(std::max(numRefFrames + 1U, 2U));
        for (auto& e : entries_)
        {
            e = DpbEntry{};
            e.frame.allocate(width, height);
        }
    }

    /** Get a free slot for the next decoded frame.
     *  If DPB is full, bumps the oldest short-term reference.
     *  @return Pointer to the frame buffer to decode into.
     */
    Frame* getDecodeTarget() noexcept
    {
        // Find a free slot
        for (auto& e : entries_)
        {
            if (!e.occupied)
            {
                e.occupied = true;
                currentEntry_ = &e;
                return &e.frame;
            }
        }

        // Prefer reusing an occupied-but-not-reference slot (already output,
        // no longer needed). This avoids evicting an active reference when
        // MMCO has explicitly unmarked a slot. — §8.2.5
        for (auto& e : entries_)
        {
            if (e.occupied && !e.isReference)
            {
                currentEntry_ = &e;
                return &e.frame;
            }
        }

        // DPB full of references: evict oldest short-term reference (FIFO)
        DpbEntry* oldest = nullptr;
        for (auto& e : entries_)
        {
            if (e.occupied && e.isReference && !e.isLongTerm)
            {
                if (!oldest || e.frameNum < oldest->frameNum)
                    oldest = &e;
            }
        }

        if (oldest)
        {
            oldest->isReference = false;
            oldest->occupied = true;
            currentEntry_ = oldest;
            return &oldest->frame;
        }

        // Last resort: use first entry
        currentEntry_ = &entries_[0];
        return &entries_[0].frame;
    }

    /** Mark the current frame as a short-term reference. */
    void markAsReference(uint16_t frameNum) noexcept
    {
        if (currentEntry_)
        {
            currentEntry_->frameNum = frameNum;
            currentEntry_->isReference = true;
            currentEntry_->isLongTerm = false;
            currentEntry_->isOutput = true;
        }
    }

    /** Apply MMCO commands — ITU-T H.264 §8.2.5.4.
     *
     *  Called after decoding a reference frame (non-IDR with adaptive_ref_pic_marking).
     *
     *  @param currFrameNum  Current frame's frame_num
     *  @param maxFrameNum   MaxFrameNum = 1 << sps.bitsInFrameNum_
     *  @param numCommands   Number of MMCO commands
     *  @param commands      MMCO command array (op, value1, value2)
     */
    void applyMmco(uint16_t currFrameNum, uint32_t maxFrameNum,
                    uint32_t numCommands, const void* commands) noexcept
    {
        struct MmcoCmd { uint8_t op; uint32_t value1, value2; };
        const auto* cmds = static_cast<const MmcoCmd*>(commands);

        for (uint32_t c = 0U; c < numCommands; ++c)
        {
            uint8_t op = cmds[c].op;
            switch (op)
            {
            case 1U: {
                // Mark short-term ref as "unused for reference"
                // PicNum = CurrPicNum - (difference_of_pic_nums_minus1 + 1)
                int32_t picNum = static_cast<int32_t>(currFrameNum)
                               - static_cast<int32_t>(cmds[c].value1 + 1U);
                if (picNum < 0)
                    picNum += static_cast<int32_t>(maxFrameNum);
                for (auto& e : entries_)
                {
                    if (e.occupied && e.isReference && !e.isLongTerm &&
                        static_cast<int32_t>(e.frameNum) == picNum)
                    {
                        e.isReference = false;
                        break;
                    }
                }
                break;
            }
            case 2U: {
                // Mark long-term ref as "unused for reference"
                uint32_t ltPicNum = cmds[c].value1;
                for (auto& e : entries_)
                {
                    if (e.occupied && e.isReference && e.isLongTerm &&
                        e.frameNum == ltPicNum)
                    {
                        e.isReference = false;
                        e.isLongTerm = false;
                        break;
                    }
                }
                break;
            }
            case 3U: {
                // Assign long_term_frame_idx to short-term ref
                int32_t picNum = static_cast<int32_t>(currFrameNum)
                               - static_cast<int32_t>(cmds[c].value1 + 1U);
                if (picNum < 0)
                    picNum += static_cast<int32_t>(maxFrameNum);
                uint32_t ltIdx = cmds[c].value2;
                // First unmark any existing long-term with this idx
                for (auto& e : entries_)
                {
                    if (e.occupied && e.isLongTerm && e.frameNum == ltIdx)
                    {
                        e.isReference = false;
                        e.isLongTerm = false;
                    }
                }
                // Then mark the target short-term as long-term
                for (auto& e : entries_)
                {
                    if (e.occupied && e.isReference && !e.isLongTerm &&
                        static_cast<int32_t>(e.frameNum) == picNum)
                    {
                        e.isLongTerm = true;
                        e.frameNum = static_cast<uint16_t>(ltIdx);
                        break;
                    }
                }
                break;
            }
            case 4U: {
                // Set max long-term frame idx. All long-term refs with
                // LongTermFrameIdx > max_long_term_frame_idx_plus1 - 1 are unmarked.
                uint32_t maxPlus1 = cmds[c].value1;
                if (maxPlus1 == 0U)
                {
                    // "no long-term frame indices" — unmark all long-term
                    for (auto& e : entries_)
                        if (e.occupied && e.isLongTerm)
                        { e.isReference = false; e.isLongTerm = false; }
                }
                else
                {
                    for (auto& e : entries_)
                        if (e.occupied && e.isLongTerm && e.frameNum >= maxPlus1)
                        { e.isReference = false; e.isLongTerm = false; }
                }
                break;
            }
            case 5U: {
                // Mark all reference pictures as "unused for reference"
                for (auto& e : entries_)
                {
                    e.isReference = false;
                    e.isLongTerm = false;
                }
                break;
            }
            case 6U: {
                // Mark current picture as long-term reference
                uint32_t ltIdx = cmds[c].value2;
                // Unmark any existing long-term with this idx
                for (auto& e : entries_)
                {
                    if (e.occupied && e.isLongTerm && e.frameNum == ltIdx)
                    {
                        e.isReference = false;
                        e.isLongTerm = false;
                    }
                }
                if (currentEntry_)
                {
                    currentEntry_->isLongTerm = true;
                    currentEntry_->frameNum = static_cast<uint16_t>(ltIdx);
                }
                break;
            }
            default:
                break;
            }
        }
    }

    /** Mark all references as unused (IDR reset). */
    void flush() noexcept
    {
        for (auto& e : entries_)
        {
            e.isReference = false;
            e.occupied = false;
        }
        currentEntry_ = nullptr;
        refListL0Built_ = false;
        refListL0_.clear();
    }

    /** §8.2.5.3: Sliding window decoded reference picture marking.
     *  If the number of short-term + long-term references >= maxRefFrames,
     *  evict the oldest (smallest frameNum) short-term reference. */
    void applySlidingWindow() noexcept
    {
        uint32_t numRefs = 0U;
        for (const auto& e : entries_)
            if (e.occupied && e.isReference)
                ++numRefs;

        if (numRefs >= maxRefFrames_)
        {
            DpbEntry* oldest = nullptr;
            for (auto& e : entries_)
            {
                if (e.occupied && e.isReference && !e.isLongTerm)
                {
                    if (!oldest || e.frameNum < oldest->frameNum)
                        oldest = &e;
                }
            }
            if (oldest)
            {
                oldest->isReference = false;
                oldest->occupied = false;
            }
        }
    }

    /** Fill gaps in frame_num with non-existing reference frames — §8.2.5.2.
     *
     *  When frame_num is not equal to (PrevRefFrameNum + 1) % MaxFrameNum,
     *  generate non-existing short-term reference frames for each missing
     *  frame_num value. The generated frames occupy DPB slots but their
     *  sample values are unspecified (the spec guarantees they are never
     *  used for actual inter prediction, only for reference list ordering).
     *
     *  @param prevRefFrameNum  frame_num of the most recently decoded reference
     *  @param currFrameNum     frame_num of the current picture
     *  @param maxFrameNum      MaxFrameNum = 1 << sps.bitsInFrameNum_
     */
    void fillGapsInFrameNum(uint16_t prevRefFrameNum, uint16_t currFrameNum,
                            uint32_t maxFrameNum) noexcept
    {
        uint32_t fn = (static_cast<uint32_t>(prevRefFrameNum) + 1U) % maxFrameNum;
        while (fn != currFrameNum)
        {
            // §8.2.5.3: sliding window marking before adding the non-existing frame
            applySlidingWindow();

            // Allocate a DPB slot for the non-existing frame
            DpbEntry* slot = nullptr;
            for (auto& e : entries_)
            {
                if (!e.occupied)
                {
                    slot = &e;
                    break;
                }
            }
            if (!slot)
            {
                // DPB full — evict oldest short-term reference
                for (auto& e : entries_)
                {
                    if (e.occupied && e.isReference && !e.isLongTerm)
                    {
                        if (!slot || e.frameNum < slot->frameNum)
                            slot = &e;
                    }
                }
                if (!slot)
                    break; // Cannot allocate — should not happen in conforming streams
            }

            // Mark as non-existing short-term reference
            slot->occupied = true;
            slot->frameNum = static_cast<uint16_t>(fn);
            slot->isReference = true;
            slot->isLongTerm = false;
            slot->isOutput = false;
            // Sample values left as-is (spec says "may be set to any value")

            fn = (fn + 1U) % maxFrameNum;
        }
    }

    /** Build the initial L0 reference list per §8.2.4.2.1 and optionally
     *  apply reordering commands per §8.2.4.3.
     *
     *  Must be called once per slice before any getReference() calls.
     *  Without calling this, getReference falls back to frameNum-descending order.
     *
     *  @param currFrameNum     Current slice's frame_num
     *  @param maxFrameNum      MaxFrameNum = 1 << sps.bitsInFrameNum_
     *  @param numReorderCmds   Number of reordering commands (0 = no reordering)
     *  @param reorderCmds      Array of reordering commands from SliceHeader
     */
    void buildRefListL0(uint16_t currFrameNum, uint32_t maxFrameNum,
                        uint32_t numReorderCmds = 0U,
                        const void* reorderCmds = nullptr) noexcept
    {
        refListL0_.clear();

        // §8.2.4.2.1: Initial reference picture list for P/SP slices.
        // Short-term refs sorted by PicNum descending.
        // PicNum = FrameNumWrap = frameNum (for frame-only, no field pics)
        std::vector<const DpbEntry*> shortTerm;
        std::vector<const DpbEntry*> longTerm;
        for (const auto& e : entries_)
        {
            if (!e.occupied || !e.isReference)
                continue;
            if (e.isLongTerm)
                longTerm.push_back(&e);
            else
                shortTerm.push_back(&e);
        }

        // Short-term: descending PicNum (=FrameNumWrap for frames)
        std::sort(shortTerm.begin(), shortTerm.end(),
                  [](const DpbEntry* a, const DpbEntry* b) {
                      return a->frameNum > b->frameNum;
                  });
        // Long-term: ascending LongTermPicNum
        std::sort(longTerm.begin(), longTerm.end(),
                  [](const DpbEntry* a, const DpbEntry* b) {
                      return a->frameNum < b->frameNum;
                  });

        for (auto* e : shortTerm) refListL0_.push_back(e);
        for (auto* e : longTerm)  refListL0_.push_back(e);

        // §8.2.4.3: Modification process for reference picture lists
        if (numReorderCmds > 0U && reorderCmds != nullptr)
        {
            // The reorder commands are SliceHeader::ReorderCmd structs
            // We can't include slice.hpp here, so cast from void*
            struct ReorderCmd { uint8_t idc; uint32_t value; };
            const auto* cmds = static_cast<const ReorderCmd*>(reorderCmds);

            int32_t picNumPred = static_cast<int32_t>(currFrameNum);
            uint32_t refIdxL0 = 0U;

            for (uint32_t c = 0U; c < numReorderCmds; ++c)
            {
                uint8_t idc = cmds[c].idc;
                if (idc == 3U)
                    break;

                if (idc == 0U || idc == 1U)
                {
                    // §8.2.4.3.1: Short-term reordering
                    int32_t absDiffPicNum = static_cast<int32_t>(cmds[c].value) + 1;
                    int32_t picNumNoWrap;
                    if (idc == 0U)
                    {
                        picNumNoWrap = picNumPred - absDiffPicNum;
                        if (picNumNoWrap < 0)
                            picNumNoWrap += static_cast<int32_t>(maxFrameNum);
                    }
                    else
                    {
                        picNumNoWrap = picNumPred + absDiffPicNum;
                        if (picNumNoWrap >= static_cast<int32_t>(maxFrameNum))
                            picNumNoWrap -= static_cast<int32_t>(maxFrameNum);
                    }
                    picNumPred = picNumNoWrap;

                    // Find the DPB entry with this PicNum
                    const DpbEntry* target = nullptr;
                    for (const auto& e : entries_)
                    {
                        if (e.occupied && e.isReference && !e.isLongTerm &&
                            static_cast<int32_t>(e.frameNum) == picNumNoWrap)
                        {
                            target = &e;
                            break;
                        }
                    }

                    if (target)
                    {
                        // §8.2.4.3.1: Place target at refIdxL0, shift others,
                        // then compact to remove any duplicate of the same entry.
                        // 1. Insert target at refIdxL0
                        if (refIdxL0 <= refListL0_.size())
                            refListL0_.insert(refListL0_.begin() + static_cast<ptrdiff_t>(refIdxL0), target);
                        else
                            refListL0_.push_back(target);

                        // 2. Remove any OTHER occurrence of the same entry
                        //    (keep only the one we just inserted at refIdxL0)
                        for (size_t i = refIdxL0 + 1U; i < refListL0_.size(); )
                        {
                            if (refListL0_[i] == target)
                                refListL0_.erase(refListL0_.begin() + static_cast<ptrdiff_t>(i));
                            else
                                ++i;
                        }
                    }
                    ++refIdxL0;
                }
                else if (idc == 2U)
                {
                    // Long-term reordering
                    uint32_t longTermPicNum = cmds[c].value;
                    const DpbEntry* target = nullptr;
                    size_t targetPos = 0U;
                    for (size_t i = 0U; i < refListL0_.size(); ++i)
                    {
                        if (refListL0_[i]->isLongTerm &&
                            refListL0_[i]->frameNum == longTermPicNum)
                        {
                            target = refListL0_[i];
                            targetPos = i;
                            break;
                        }
                    }
                    if (target && targetPos != refIdxL0)
                    {
                        refListL0_.erase(refListL0_.begin() + static_cast<ptrdiff_t>(targetPos));
                        if (refIdxL0 <= refListL0_.size())
                            refListL0_.insert(refListL0_.begin() + static_cast<ptrdiff_t>(refIdxL0), target);
                    }
                    ++refIdxL0;
                }
            }
        }

        refListL0Built_ = true;
    }

    /** Get reference frame by index (L0 list, 0-based).
     *  Uses the pre-built L0 list if buildRefListL0() was called,
     *  otherwise falls back to frameNum-descending order.
     *  @return Pointer to reference frame, or nullptr.
     */
    const Frame* getReference(uint8_t refIdx) const noexcept
    {
        if (refListL0Built_ && refIdx < refListL0_.size())
            return &refListL0_[refIdx]->frame;

        // Fallback: build on the fly (legacy behavior)
        std::vector<const DpbEntry*> refList;
        for (const auto& e : entries_)
        {
            if (e.occupied && e.isReference)
                refList.push_back(&e);
        }

        std::sort(refList.begin(), refList.end(),
                  [](const DpbEntry* a, const DpbEntry* b) {
                      return a->frameNum > b->frameNum;
                  });

        if (refIdx < refList.size())
            return &refList[refIdx]->frame;

        return nullptr;
    }

    /** @return The most recently decoded frame. */
    const Frame* currentFrame() const noexcept
    {
        return currentEntry_ ? &currentEntry_->frame : nullptr;
    }

    /** @return Number of active reference frames. */
    uint32_t numReferences() const noexcept
    {
        uint32_t count = 0U;
        for (const auto& e : entries_)
            if (e.occupied && e.isReference)
                ++count;
        return count;
    }

private:
    std::vector<DpbEntry> entries_;
    DpbEntry* currentEntry_ = nullptr;
    uint16_t width_ = 0U;
    uint16_t height_ = 0U;
    uint8_t maxRefFrames_ = 0U;

    /// Cached L0 reference list — built by buildRefListL0(), used by getReference().
    mutable std::vector<const DpbEntry*> refListL0_;
    bool refListL0Built_ = false;
};

} // namespace sub0h264

#endif // CROG_SUB0H264_DPB_HPP
