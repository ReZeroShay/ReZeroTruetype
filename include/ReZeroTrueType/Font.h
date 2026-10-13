#pragma once
#include <cstdint>
#include <cmath>
#include <ReZeroTrueType/Error.h>
#include <ReZeroTrueType/ReZeroLib.h>

namespace ReZeroTrueType {
    constexpr uint32_t tagHead = 0x68656164; // 'head'
    constexpr uint32_t tagHhea = 0x68686561; // 'hhea'
    constexpr uint32_t tagMaxp = 0x6D617870; // 'maxp'
    constexpr uint32_t tagCmap = 0x636D6170; // 'cmap'
    constexpr uint32_t tagLoca = 0x6C6F6361; // 'loca'
    constexpr uint32_t tagGlyf = 0x676C7966; // 'glyf'
    constexpr uint32_t tagHmtx = 0x686D7478; // 'hmtx'

    constexpr uint32_t versionTrueType = 0x00010000;
    constexpr uint32_t versionAppleTrue = 0x74727565; // 'true'

    class TableView {
      public:
        template <typename T> using Result = Result<T, ErrorCode>;

        static constexpr std::size_t Npos = static_cast<std::size_t>(-1);

        TableView() = default;

        TableView(Span<uint8_t> bytes, std::size_t offset = 0, std::size_t length = Npos)
            : bytes(bytes), position(offset), end(length == Npos ? bytes.size() : min(bytes.size(), offset + length)) {}

        [[nodiscard]] auto ensure(std::size_t count) const noexcept -> bool {
            return position <= end && count <= end - position;
        }

        auto readU8() -> Result<uint8_t> {
            if (!ensure(1))
                return Error{ErrorCode::UnexpectedEOF};
            return static_cast<uint8_t>(bytes[position++]);
        }

        auto readI8() -> Result<int8_t> {
            REZERO_TRY(value, readU8());
            return static_cast<int8_t>(value);
        }

        auto readU16() -> Result<uint16_t> {
            if (!ensure(2))
                return Error{ErrorCode::UnexpectedEOF};
            const uint16_t value = misc::loadBE16(bytes.data() + position);
            position += 2;
            return value;
        }

        auto readI16() -> Result<int16_t> {
            REZERO_TRY(value, readU16());
            return static_cast<int16_t>(value);
        }

        auto readU32() -> Result<uint32_t> {
            if (!ensure(4))
                return Error{ErrorCode::UnexpectedEOF};
            const uint32_t value = misc::loadBE32(bytes.data() + position);
            position += 4;
            return value;
        }

        auto skip(std::size_t count) -> Result<void> {
            if (!ensure(count))
                return Error{ErrorCode::UnexpectedEOF};
            position += count;
            return {};
        }

        void skipUnchecked(std::size_t count) noexcept {
            if (ensure(count))
                position += count;
        }

        auto readU8At(std::size_t absolute) const -> Result<uint8_t> {
            if (absolute >= end)
                return Error{ErrorCode::UnexpectedEOF};
            return static_cast<uint8_t>(bytes[absolute]);
        }

        auto readU16At(std::size_t absolute) const -> Result<uint16_t> {
            if (absolute > end || end - absolute < 2)
                return Error{ErrorCode::UnexpectedEOF};
            return misc::loadBE16(bytes.data() + absolute);
        }

        auto readU32At(std::size_t absolute) const -> Result<uint32_t> {
            if (absolute > end || end - absolute < 4)
                return Error{ErrorCode::UnexpectedEOF};
            return misc::loadBE32(bytes.data() + absolute);
        }

        Span<uint8_t> bytes;
        std::size_t position = 0;
        std::size_t end = 0;
    };
    /// 轮廓上的一个点。坐标是**字体单位**
    struct OutlinePoint {
        float x = 0;
        float y = 0;
        /// true = 曲线上的点（ON），false = 二次贝塞尔的控制点（OFF）
        bool onCurve = true;
    };

    /// 一个字形（或复合字形拼出来的整体）的轮廓。
    ///
    /// 点存在一个平坦数组里，`contourEnds[i]` 是第 i 条闭合轮廓**最后一个点**的下标。
    /// 于是第 i 条轮廓覆盖 `[contourEnds[i-1] + 1, contourEnds[i]]`（第一条从 0 开始）。
    struct GlyphOutline {
        explicit GlyphOutline(AllocatorView allocator) : points(allocator), contourEnds(allocator) {}

        [[nodiscard]] auto empty() const noexcept -> bool { return points.empty(); }

        void clear() noexcept {
            points.clear();
            contourEnds.clear();
        }

        ArrayList<OutlinePoint> points;
        ArrayList<uint32_t> contourEnds;
    };

    /// 字形在字体单位下的包围盒，直接来自 glyf 表头的 8 个字节。
    struct GlyphBounds {
        int32_t xMin = 0;
        int32_t yMin = 0;
        int32_t xMax = 0;
        int32_t yMax = 0;

        [[nodiscard]] auto empty() const noexcept -> bool { return xMax <= xMin || yMax <= yMin; }
    };

    struct GlyphScratch {
        explicit GlyphScratch(AllocatorView allocator) noexcept
            : normPoints(allocator), newEnds(allocator), endPts(allocator), flags(allocator), xs(allocator),
              ys(allocator) {}

        // 归一化的输出缓冲，最后 swap 给 Outline。
        ArrayList<OutlinePoint> normPoints;
        ArrayList<uint32_t> newEnds;

        ArrayList<uint16_t> endPts;
        ArrayList<uint8_t> flags;
        ArrayList<int32_t> xs;
        ArrayList<int32_t> ys;
    };

    /// 把轮廓整理成便于光栅化的 ON / OFF 点序列。
    ///
    /// TrueType 的曲线段永远是 "ON 起点 --(若干个 OFF 控制点)-- ON 终点"。
    /// 如果两个 ON 点之间有连续多个 OFF 点，那么每对相邻 OFF 点之间隐含一个
    /// ON 点，位置取两者中点：
    ///
    ///     mid = (p + q) / 2
    ///
    /// 例如 `ON -> OFF -> OFF -> ON` 会变成 `ON -> OFF -> mid -> OFF -> ON`。
    /// 这样光栅化时每段都是标准的二次曲线（起点、一个控制点、终点）。
    ///
    /// 另外轮廓的第一个点不一定是 ON 点，所以每条轮廓都从第一个 ON 点开始走，
    /// 转一圈回到它为止 —— 保证每段都能从 ON 出发。
    ///
    /// Example:
    ///     ON -> OFF -> OFF -> ON
    ///
    /// becomes:
    ///     ON -> OFF -> MID -> OFF -> ON
    inline auto normalizeContours(GlyphOutline &out, GlyphScratch &scratch) -> void {
        ArrayList<OutlinePoint> &norm = scratch.normPoints;
        ArrayList<uint32_t> &newEnds = scratch.newEnds;
        norm.clear();
        newEnds.clear();

        uint32_t contourStart = 0;

        for (const uint32_t end : out.contourEnds) {
            // contourEnds 保存轮廓最后一个点的索引。
            //
            // 例如：
            //
            //     contourStart = 5
            //     end          = 9
            //
            //     表示当前轮廓包含 points[5] 到 points[9]。
            //     因此一共有：9 - 5 + 1 = 5 个点。
            const uint32_t count = end - contourStart + 1;

            // 找一个 ON 点作为当前轮廓的遍历起点。
            //
            // TrueType 轮廓的第一个点不一定是 ON 点，
            // 可能是 OFF 点（控制点）。
            //
            // 由于轮廓是闭合的，我们可以从任意一个 ON 点开始遍历。
            // 这样每一段轮廓都可以按照 ON → ON 的方式处理，
            // 中间的 OFF 点作为二次贝塞尔曲线的控制点。
            uint32_t firstOn = 0;
            while (firstOn < count && !out.points[contourStart + firstOn].onCurve)
                ++firstOn;

            // 整条轮廓都是 OFF 点：TrueType 规定这时 ON 点取相邻两个 OFF 点的
            // 中点，于是整条轮廓是一圈二次曲线：
            //
            //     OFF p0 -> MID(p0,p1) -> OFF p1 -> MID(p1,p2) -> ... -> OFF p_{n-1}
            //     最后一个 OFF 点再绕回开头的 MID(p_{n-1},p0)。
            //
            // 起点因此取「末点与首点的中点」，然后依次输出每个 OFF 点，并在相邻
            // 两个 OFF 点之间补一个中点。这样输出序列仍然是 ON/OFF 交替，后面
            // 的展平逻辑不用特殊照顾。
            if (firstOn == count) {
                const OutlinePoint &first = out.points[contourStart];
                const OutlinePoint &last = out.points[contourStart + count - 1];
                norm.push_back({(last.x + first.x) / 2, (last.y + first.y) / 2, true});

                for (uint32_t k = 0; k < count; ++k) {
                    const OutlinePoint &current = out.points[contourStart + k];
                    norm.push_back(current);

                    // 最后一个 OFF 点后面接的是开头的那个 ON 点，不用再补。
                    if (k + 1 < count) {
                        const OutlinePoint &next = out.points[contourStart + k + 1];
                        norm.push_back({(current.x + next.x) / 2, (current.y + next.y) / 2, true});
                    }
                }

                newEnds.push_back(static_cast<uint32_t>(norm.size() - 1));
                contourStart = end + 1;
                continue;
            }

            // 从第一个 ON 点开始遍历整个轮廓。
            //
            // i：当前 ON 点
            // j：下一个 ON 点
            // k：i 和 j 之间当前正在处理的点
            uint32_t i = firstOn;

            /*
             * 轮廓是闭合的，所以一直处理到重新回到 firstOn。
             *
             * 每处理完一个 ON 点，就向后寻找下一个 ON 点。
             * 如果中间有 OFF 点，则把这些 OFF 点和它们之间的隐式 ON 点加入 scratch。
             *
             * 例如：
             *
             *     ON A -> OFF B -> OFF C -> ON D
             *     ^                         |
             *     +-------------------------+
             *
             * 处理后变成：
             *
             *     ON A -> OFF B -> MID -> OFF C -> ON D
             *
             * 其中 MID 是 B 和 C 的中点，是 TrueType 定义的隐式 ON 点。
             */
            do {
                // 当前 ON 点，作为一段或多段二次曲线的起点。
                norm.push_back(out.points[contourStart + i]);

                /*
                 * 向后寻找下一个 ON 点。
                 *
                 * i 和 j 之间的所有点都是 OFF 点。
                 *
                 * 例如：
                 *
                 *     i                         j
                 *     ON -> OFF -> OFF -> OFF -> ON
                 *           <--------------->
                 *             OFF 点
                 */
                uint32_t j = (i + 1) % count;

                while (!out.points[contourStart + j].onCurve)
                    j = (j + 1) % count;

                /*
                 * 加入 i 和 j 之间的所有 OFF 点。
                 *
                 * 通常一段二次贝塞尔曲线是：
                 *
                 *     ON A -> OFF B -> ON D
                 *
                 * 但 TrueType 允许连续出现多个 OFF 点：
                 *
                 *     ON A -> OFF B -> OFF C -> ON D
                 *
                 * 连续两个 OFF 点 B、C 之间没有实际存储的 ON 点。
                 * TrueType 将它们的中点视为一个隐式 ON 点：
                 *
                 *     ON A -> OFF B -> MID -> OFF C -> ON D
                 */
                uint32_t k = (i + 1) % count;

                while (k != j) {
                    // Current off-curve point.
                    const OutlinePoint &P = out.points[contourStart + k];
                    // Look at the next point in the contour.
                    uint32_t next = (k + 1) % count;
                    // Keep the original off-curve point.
                    norm.push_back(P);

                    /*
                     * 如果下一个点也是 OFF 点，
                     * 则在当前 OFF 点和下一个 OFF 点之间
                     * 插入一个隐式 ON 点。
                     *
                     * 隐式 ON 点就是两个 OFF 点的中点：
                     *
                     *     P                 Q
                     *     ●-----------------●
                     *              |
                     *              ↓
                     *              ●
                     *             MID
                     *
                     *     MID = (P + Q) / 2
                     */
                    if (!out.points[contourStart + next].onCurve) {
                        const OutlinePoint &Q = out.points[contourStart + next];

                        norm.push_back({(P.x + Q.x) / 2, (P.y + Q.y) / 2, true});
                    }
                    k = next;
                }
                /*
                 * j 是下一个 ON 点。
                 *
                 * 下一轮从这个 ON 点继续处理。
                 */
                i = j;
            } while (i != firstOn);
            /*
             * norm 是规范化后的轮廓。
             *
             * 由于连续 OFF 点之间可能插入了隐式 ON 点，
             * 所以规范化后的点数量可能比原来的轮廓更多。
             *
             * 因此，这里记录规范化轮廓的最后一个点的索引。
             */
            newEnds.push_back(static_cast<uint32_t>(norm.size() - 1));

            contourStart = end + 1;
        }

        // 用 swap 而不是 move：把 out 原来那块缓冲换回来继续用，
        // 否则每次调用都会释放掉旧缓冲、下次再重新分配。
        std::swap(out.points, norm);
        std::swap(out.contourEnds, newEnds);
    }

    /// TrueType is a non-owning view of the font data.
    /// The lifetime of TrueType must not exceed the lifetime of Font data.
    class TrueType {
        template <typename T> using Result = Result<T, ErrorCode>;

      public:
        struct TableEntry {
            uint32_t tag = 0;
            uint32_t offset = 0;
            uint32_t length = 0;
        };

        TrueType(AllocatorView allocator, Span<uint8_t> bytes)
            : allocator(allocator), advanceWidths(allocator), bytes(bytes) {}

        /// Converts a value from font units to pixels.
        ///
        /// Font metrics stored in a TTF font are expressed in font units.
        /// unitsPerEm defines how many font units make up one em.
        ///
        /// @param fontUnits Value expressed in font units.
        /// @param pixelSize Target size in pixels.
        /// @return The converted value in pixels.
        auto unitsToPixel(float units, float pixelSize) const noexcept -> float {
            REZERO_ASSERT(unitsPerEm != 0);
            return units * pixelSize / static_cast<float>(unitsPerEm);
        }
        /// Returns the recommended distance between two text baselines.
        ///
        /// The line height is calculated from the font's vertical metrics:
        ///
        ///     lineHeight(em) = ascent - descent + lineGap
        ///
        /// descent is normally negative, so subtracting it includes the
        /// distance below the baseline.
        ///
        /// The result is converted from font units to pixels.
        auto lineHeight(const float pixelSize) const noexcept -> float {
            REZERO_ASSERT(unitsPerEm != 0);
            const int16_t units = ascent - descent + lineGap;
            return unitsToPixel(static_cast<float>(units), pixelSize);
        }

        static auto loadTrueTyperomMemory(AllocatorView Allocator, Span<uint8_t> bytes) -> Result<TrueType> {
            if (bytes.size() < 12)
                return Error{ErrorCode::FileTooSmall};

            const uint32_t version = misc::loadBE32(bytes.data());
            if (version != versionTrueType && version != versionAppleTrue)
                return Error{ErrorCode::InvalidVersion};

            TrueType loaded(Allocator, bytes);
            REZERO_CHECK(loaded.loadDirectory());
            REZERO_CHECK(loaded.loadHead());
            REZERO_CHECK(loaded.loadMaxp());
            REZERO_CHECK(loaded.loadHhea());
            REZERO_CHECK(loaded.loadHmtx());
            REZERO_CHECK(loaded.loadCmap());
            return std::move(loaded);
        }

        auto loadDirectory() -> Result<void> {
            TableView view(bytes);
            view.skipUnchecked(4); // sfntVersion
            REZERO_TRY(tableCount, view.readU16());
            view.skipUnchecked(6); // searchRange / entrySelector / rangeShift

            ArrayList<TableEntry> tables(allocator);
            for (uint16_t i = 0; i < tableCount; ++i) {
                REZERO_TRY(tag, view.readU32());
                REZERO_TRY(checksum, view.readU32());
                REZERO_TRY(offset, view.readU32());
                REZERO_TRY(length, view.readU32());
                (void)checksum; // 不校验，只占位跳过
                tables.push_back({tag, offset, length});
            }

            for (const TableEntry &entry : tables) {
                switch (entry.tag) {
                case tagHead:
                    headOffset = entry.offset;
                    break;
                case tagHhea:
                    hheaOffset = entry.offset;
                    break;
                case tagMaxp:
                    maxpOffset = entry.offset;
                    break;
                case tagCmap:
                    cmapOffset = entry.offset;
                    break;
                case tagLoca:
                    locaOffset = entry.offset;
                    break;
                case tagGlyf:
                    glyfOffset = entry.offset;
                    break;
                case tagHmtx:
                    hmtxOffset = entry.offset;
                    break;
                default:
                    break;
                }
            }

            if (headOffset == 0 || hheaOffset == 0 || maxpOffset == 0 || cmapOffset == 0 || hmtxOffset == 0)
                return Error{ErrorCode::MissingTable};
            return {};
        }

        // Only need indexToLocFormat and unitsPerEm
        auto loadHead() -> Result<void> {
            TableView view(bytes, headOffset);
            view.skipUnchecked(4); // version
            view.skipUnchecked(4); // fontRevision
            view.skipUnchecked(4); // checkSumAdjustment
            view.skipUnchecked(4); // magicNumber
            view.skipUnchecked(2); // flags
            REZERO_TRY(upem, view.readU16());
            unitsPerEm = upem;
            view.skipUnchecked(16); // created / modified
            view.skipUnchecked(8);  // xMin..yMax
            view.skipUnchecked(2);  // macStyle
            view.skipUnchecked(2);  // lowestRecPPEM
            view.skipUnchecked(2);  // fontDirectionHint
            REZERO_TRY(locFormat, view.readI16());
            indexToLocFormat = locFormat;
            return {};
        }

        // read numGlyphs
        auto loadMaxp() -> Result<void> {
            TableView view(bytes, maxpOffset);
            REZERO_TRY(version, view.readU32());
            (void)version;
            REZERO_TRY(count, view.readU16());
            numGlyphs = count;
            return {};
        }

        auto loadHhea() -> Result<void> {
            TableView view(bytes, hheaOffset);
            view.skipUnchecked(4); // version
            REZERO_TRY(ascender, view.readI16());
            REZERO_TRY(descender, view.readI16());
            REZERO_TRY(gap, view.readI16());
            ascent = ascender;
            descent = descender;
            lineGap = gap;

            view.skipUnchecked(14); // advanceWidthMax .. caretOffset
            view.skipUnchecked(8);  // reserved
            view.skipUnchecked(2);  // metricDataFormat
            REZERO_TRY(metricCount, view.readU16());
            numberOfHMetrics = metricCount;
            return {};
        }

        // Only need advanceWidth
        auto loadHmtx() -> Result<void> {
            TableView view(bytes, hmtxOffset);
            advanceWidths.reserve(numberOfHMetrics);
            for (uint16_t i = 0; i < numberOfHMetrics; ++i) {
                REZERO_TRY(advance, view.readU16());
                REZERO_TRY(bearing, view.readI16());
                (void)bearing;
                advanceWidths.push_back(advance);
            }
            return {};
        }

        // read a supported cmap subtable.
        auto loadCmap() -> Result<void> {
            TableView view(bytes, cmapOffset);
            REZERO_TRY(version, view.readU16());
            (void)version;
            REZERO_TRY(subtableCount, view.readU16());

            struct Candidate {
                uint16_t platformId;
                uint16_t encodingId;
                uint32_t offset;
            };

            ArrayList<Candidate> candidates(allocator);

            for (uint16_t i = 0; i < subtableCount; ++i) {
                REZERO_TRY(platform, view.readU16());
                REZERO_TRY(encoding, view.readU16());
                REZERO_TRY(offset, view.readU32());
                candidates.push_back({platform, encoding, offset});
            }

            // Platform and encoding IDs used to select the preferred cmap subtable.
            constexpr uint16_t priority[][2] = {{3, 10}, {0, 6}, {0, 4}, {3, 1}, {0, 3},
                                                {0, 2},  {0, 1}, {0, 0}, {3, 0}};

            for (const auto &pair : priority) {
                for (const Candidate &item : candidates) {
                    if (item.platformId != pair[0] || item.encodingId != pair[1])
                        continue;

                    const uint32_t absolute = cmapOffset + item.offset;
                    TableView subtable(bytes, absolute);
                    REZERO_TRY(format, subtable.readU16());

                    if (format == 0 || format == 4 || format == 6 || format == 12) {
                        cmapFormat = format;
                        cmapSubtableOffset = absolute;
                        return {};
                    }
                }
            }

            return Error{ErrorCode::InvalidData};
        }

        auto advanceWidth(uint16_t GlyphID) const -> uint16_t {
            if (GlyphID < numberOfHMetrics)
                return advanceWidths[GlyphID];
            return advanceWidths.back();
        }

        /// 码点 -> 字形号。查不到返回 0（即 .notdef）。
        auto glyphIndexForCodepoint(uint32_t codepoint) const -> Result<uint16_t> {
            switch (cmapFormat) {
            case 0:
                return lookupCmapFormat0(codepoint);
            case 4:
                return lookupCmapFormat4(codepoint);
            case 6:
                return lookupCmapFormat6(codepoint);
            case 12:
                return lookupCmapFormat12(codepoint);
            default:
                return Error{ErrorCode::Unsupported};
            }
        }

      private:
        /// cmap format 4（unicode BMP）。子表布局：
        ///
        ///     偏移  0  uint16 format = 4
        ///     偏移  2  uint16 length
        ///     偏移  4  uint16 language
        ///     偏移  6  uint16 segCountX2          段数 × 2
        ///     偏移  8  uint16 searchRange
        ///     偏移 10  uint16 entrySelector
        ///     偏移 12  uint16 rangeShift
        ///     偏移 14  uint16 endCode[segCount]       每段的结束码点
        ///              uint16 reservedPad
        ///              uint16 startCode[segCount]     每段的起始码点
        ///              int16  idDelta[segCount]
        ///              uint16 idRangeOffset[segCount]
        ///              uint16 glyphIdArray[]
        ///
        /// 先二分找第一个 `endCode >= codepoint` 的段，再看它是否落在 startCode 之后：
        ///
        ///     if (idRangeOffset[i] == 0)
        ///         glyph = (codepoint + idDelta[i]) mod 65536
        ///     else
        ///         glyph = idDelta[i] + (uint16 read at
        ///                 &idRangeOffset[i] + idRangeOffset[i] + 2 * (codepoint - startCode[i]))
        ///
        /// 关键点：**idRangeOffset 是相对它自己那 2 字节单元格的字节偏移**，
        /// 所以真正的地址是 `&idRangeOffset[i] + idRangeOffset[i] + 2 * 段内偏移`。
        [[nodiscard]] auto lookupCmapFormat4(uint32_t codepoint) const -> Result<uint16_t> {
            TableView view(bytes, cmapSubtableOffset);
            view.skipUnchecked(6); // format / length / language

            REZERO_TRY(segCountX2, view.readU16());
            const uint32_t segCount = segCountX2 / 2;

            // 四个数组各自的起始位置（相对文件开头）。
            const uint32_t afterHeader = cmapSubtableOffset + 14;
            const uint32_t endCodeAt = afterHeader;
            const uint32_t startCodeAt = afterHeader + segCountX2 + 2; // +2 跳过 reservedPad
            const uint32_t idDeltaAt = startCodeAt + segCountX2;
            const uint32_t idRangeOffsetAt = idDeltaAt + segCountX2;

            // 二分：找第一个 endCode >= codepoint 的段。
            uint32_t low = 0;
            uint32_t high = segCount;
            while (low < high) {
                const uint32_t mid = (low + high) / 2;
                REZERO_TRY(endCode, view.readU16At(endCodeAt + mid * 2));
                if (endCode >= codepoint)
                    high = mid;
                else
                    low = mid + 1;
            }
            if (low >= segCount)
                return 0;

            const uint32_t segment = low;
            REZERO_TRY(startCode, view.readU16At(startCodeAt + segment * 2));
            REZERO_TRY(endCode, view.readU16At(endCodeAt + segment * 2));
            REZERO_TRY(idDelta, view.readU16At(idDeltaAt + segment * 2));
            REZERO_TRY(idRangeOffset, view.readU16At(idRangeOffsetAt + segment * 2));

            if (codepoint < startCode || codepoint > endCode)
                return 0;

            const int32_t delta = static_cast<int16_t>(idDelta);
            if (idRangeOffset == 0)
                return static_cast<uint16_t>((codepoint + delta) & 0xFFFFu);

            const uint32_t glyphAt = idRangeOffsetAt + segment * 2 + idRangeOffset + 2 * (codepoint - startCode);
            REZERO_TRY(glyph, view.readU16At(glyphAt));
            if (glyph == 0)
                return 0; // 这一格没有字形
            return static_cast<uint16_t>((glyph + delta) & 0xFFFFu);
        }

        /// cmap format 12：一组组连续区间，每组 12 字节：
        ///
        ///     uint32 startCharCode
        ///     uint32 endCharCode
        ///     uint32 startGlyphId
        ///
        /// 区间内字形号连续，所以 `glyph = startGlyphId + (codepoint - startCharCode)`。
        [[nodiscard]] auto lookupCmapFormat12(uint32_t codepoint) const -> Result<uint16_t> {
            TableView view(bytes, cmapSubtableOffset);
            view.skipUnchecked(4); // format / reserved
            REZERO_TRY(length, view.readU32());
            (void)length;
            view.skipUnchecked(4); // language
            REZERO_TRY(groupCount, view.readU32());
            const uint32_t groupsAt = static_cast<uint32_t>(view.position);

            // 二分：找最后一个 startCharCode <= codepoint 的区间。
            uint32_t low = 0;
            uint32_t high = groupCount;
            while (low < high) {
                const uint32_t mid = (low + high) / 2;
                REZERO_TRY(start, view.readU32At(groupsAt + mid * 12));
                if (start <= codepoint)
                    low = mid + 1;
                else
                    high = mid;
            }
            if (low == 0)
                return 0;

            const uint32_t group = low - 1;
            REZERO_TRY(start, view.readU32At(groupsAt + group * 12));
            REZERO_TRY(end, view.readU32At(groupsAt + group * 12 + 4));
            REZERO_TRY(startGlyph, view.readU32At(groupsAt + group * 12 + 8));

            if (codepoint < start || codepoint > end)
                return 0;
            return static_cast<uint16_t>(startGlyph + (codepoint - start));
        }

        /// cmap format 0：256 字节的直接映射表，只覆盖 0..255。
        [[nodiscard]] auto lookupCmapFormat0(uint32_t codepoint) const -> Result<uint16_t> {
            if (codepoint >= 256)
                return 0;
            TableView view(bytes, cmapSubtableOffset);
            REZERO_TRY(glyph, view.readU8At(cmapSubtableOffset + 6 + codepoint));
            return glyph;
        }

        /// cmap format 6：一段连续码点：
        ///
        ///     uint16 firstCode
        ///     uint16 entryCount
        ///     uint16 glyphIdArray[entryCount]
        [[nodiscard]] auto lookupCmapFormat6(uint32_t codepoint) const -> Result<uint16_t> {
            TableView view(bytes, cmapSubtableOffset);
            view.skipUnchecked(6); // format / length / language
            REZERO_TRY(firstCode, view.readU16());
            REZERO_TRY(entryCount, view.readU16());

            if (codepoint < firstCode || codepoint >= static_cast<uint32_t>(firstCode) + entryCount)
                return 0;
            REZERO_TRY(glyph, view.readU16At(cmapSubtableOffset + 10 + 2 * (codepoint - firstCode)));
            return glyph;
        }

      public:
        AllocatorView allocator;

        uint32_t headOffset = 0;
        uint32_t hheaOffset = 0;
        uint32_t maxpOffset = 0;
        uint32_t cmapOffset = 0;
        uint32_t locaOffset = 0;
        uint32_t glyfOffset = 0;
        uint32_t hmtxOffset = 0;

        // lineHeight(Em) = ascent - descent + lineGap
        int16_t ascent = 0;
        int16_t descent = 0;
        int16_t lineGap = 0;

        // Font scale = fontSize / unitsPerEm.
        // pixel = fontUnit * fontSize / unitsPerEm.
        uint16_t unitsPerEm = 0;
        // 0 = short offsets, 1 = long offsets.
        int16_t indexToLocFormat = 0;

        // Number of glyphs: glyph index range = [0, numGlyphs).
        uint16_t numGlyphs = 0;

        // Full metrics: [0, numberOfHMetrics).
        // Remaining glyphs use the last advanceWidth.
        uint16_t numberOfHMetrics = 0;

        // Format of the selected cmap subtable.
        uint16_t cmapFormat = 0;
        // Offset of the selected cmap subtable.
        uint32_t cmapSubtableOffset = 0;

        // Distance to the next glyph.
        ArrayList<uint16_t> advanceWidths;

        Span<uint8_t> bytes;
    };

    class Outlinedecoder {
        template <typename T> using Result = Result<T, ErrorCode>;

      public:
        explicit Outlinedecoder(AllocatorView source) noexcept : Allocator(source), loca(source) {}

        auto open(TrueType &font) -> Result<void> {
            return open(font.bytes, font.glyfOffset, font.locaOffset, font.indexToLocFormat, font.numGlyphs);
        }
        /// 解析 loca 表。幂等：已经打开过就直接返回。
        ///
        /// loca 存每个字形在 glyf 表里的**字节偏移**，共 numGlyphs + 1 项，
        /// 相邻两项之差就是该字形的长度。两种编码：
        ///
        ///     indexToLocFormat == 0   每项 uint16，真实偏移 = 值 * 2
        ///     indexToLocFormat == 1   每项 uint32，值即偏移
        auto open(Span<uint8_t> inBytes, uint32_t inGlyfOffset, uint32_t inLocaOffset, int16_t inIndexToLocFormat,
                  uint16_t inNumGlyphs) -> Result<void> {
            if (opened)
                return {};

            bytes = inBytes;
            glyfOffset = inGlyfOffset;
            indexToLocFormat = inIndexToLocFormat;
            numGlyphs = inNumGlyphs;

            // 位图字体没有 glyf/loca：每个字形都是空轮廓。
            if (glyfOffset == 0 || inLocaOffset == 0) {
                opened = true;
                return {};
            }

            loca.clear();
            loca.reserve(static_cast<std::size_t>(numGlyphs) + 1);

            TableView view(bytes, inLocaOffset);
            for (uint32_t i = 0; i <= numGlyphs; ++i) {
                if (indexToLocFormat == 0) {
                    REZERO_TRY(half, view.readU16());
                    loca.push_back(static_cast<uint32_t>(half) * 2);
                } else {
                    REZERO_TRY(offset, view.readU32());
                    loca.push_back(offset);
                }
            }

            opened = true;
            return {};
        }

        /// 只读 glyf 表头里的包围盒，不解析轮廓、不展平曲线。
        ///
        /// glyf 表头是 10 个字节：int16 numberOfContours，然后 xMin/yMin/xMax/yMax。
        /// 注意它是轮廓**所有控制点**的包围盒：二次贝塞尔曲线段一定落在控制点的
        /// 凸包内，所以这个盒子必然是真实墨迹的超集 —— 拿它做装箱预留一定够用。
        [[nodiscard]] auto bounds(uint16_t glyphId) const -> Result<GlyphBounds> {
            if (glyphId >= numGlyphs)
                return Error{ErrorCode::OutOfBounds};

            const uint32_t start = loca[glyphId];
            const uint32_t end = loca[glyphId + 1];
            if (end <= start)
                return GlyphBounds{}; // 空字形，没有表头

            TableView view(bytes, glyfOffset + start, end - start);
            view.skipUnchecked(2); // numberOfContours

            REZERO_TRY(xMin, view.readI16());
            REZERO_TRY(yMin, view.readI16());
            REZERO_TRY(xMax, view.readI16());
            REZERO_TRY(yMax, view.readI16());
            return GlyphBounds{xMin, yMin, xMax, yMax};
        }

        /// 取一个字形在字体单位下的轮廓。
        ///
        /// `out` 会被清空后重建；`scratch` 是临时缓冲。两者都能跨字形复用。
        auto outline(uint16_t glyphId, GlyphScratch &scratch, GlyphOutline &out) const -> Result<void> {
            out.clear();
            return parseGlyph(glyphId, scratch, out);
        }

      private:
        auto parseGlyph(uint16_t glyphId, GlyphScratch &scratch, GlyphOutline &out) const -> Result<void> {
            if (glyphId >= numGlyphs)
                return Error{ErrorCode::OutOfBounds};

            const uint32_t start = loca[glyphId];
            const uint32_t end = loca[glyphId + 1];
            if (end <= start)
                return {}; // 空字形

            TableView view(bytes, glyfOffset + start, end - start);

            REZERO_TRY(contourCount, view.readI16());
            view.skipUnchecked(8); // xMin, yMin, xMax, yMax

            if (contourCount > 0)
                REZERO_CHECK(loadSimpleGlyph(view, static_cast<uint32_t>(contourCount), scratch, out));
            else if (contourCount < 0)
                REZERO_CHECK(loadCompositeGlyph(view, scratch, out));

            normalizeContours(out, scratch);
            return {};
        }

        /// 简单字形：点表存在自己的数据里。分三段：
        ///
        ///     1. uint16 endPtsOfContours[numberOfContours]   每条轮廓最后一个点的下标
        ///     2. uint16 instructionLength + 指令字节（TrueType 字节码，本库跳过）
        ///     3. 标志位数组 + x 增量数组 + y 增量数组
        auto loadSimpleGlyph(TableView &view, uint32_t contourCount, GlyphScratch &scratch, GlyphOutline &out) const
            -> Result<void> {
            ArrayList<uint16_t> &endPts = scratch.endPts;
            ArrayList<uint8_t> &flags = scratch.flags;
            ArrayList<int32_t> &xs = scratch.xs;
            ArrayList<int32_t> &ys = scratch.ys;
            endPts.clear();
            flags.clear();
            xs.clear();
            ys.clear();

            // 点的总数 = 最后一条轮廓的末点下标 + 1。
            uint32_t pointCount = 0;
            for (uint32_t i = 0; i < contourCount; ++i) {
                REZERO_TRY(endPoint, view.readU16());
                endPts.push_back(endPoint);
                pointCount = static_cast<uint32_t>(endPoint) + 1;
            }

            REZERO_TRY(instructionLength, view.readU16());
            REZERO_CHECK(view.skip(instructionLength));

            // ---- 标志位 ----
            //
            // 每个点一个字节，位的含义：
            //
            //     0x01 ON_CURVE_POINT          该点在曲线上
            //     0x02 X_SHORT_VECTOR          x 增量占 1 字节
            //     0x04 Y_SHORT_VECTOR          y 增量占 1 字节
            //     0x08 REPEAT_FLAG             后随一个字节，表示这个标志再重复 n 次
            //     0x10 X_IS_SAME_OR_POSITIVE   见下面的坐标解码
            //     0x20 Y_IS_SAME_OR_POSITIVE   见下面的坐标解码
            flags.resize(pointCount);
            uint32_t flagIndex = 0;
            while (flagIndex < pointCount) {
                REZERO_TRY(flag, view.readU8());
                flags[flagIndex++] = flag;

                if (flag & 0x08) {
                    REZERO_TRY(repeat, view.readU8());
                    for (uint32_t i = 0; i < repeat; ++i)
                        flags[flagIndex++] = flag;
                }
            }

            // ---- 坐标 ----
            //
            // 坐标是**增量**编码：从头开始逐个累加，得到每个点的绝对坐标。
            // 增量的读法和含义由标志位决定：
            //
            //   x_SHORT (0x02) 置位：增量绝对值是 1 字节；
            //                        0x10 置位取正号，否则取负号。
            //   x_SHORT 清零：0x10 置位表示增量恰好是 0（不读字节）；
            //                 否则增量是一个 int16。
            //   y 方向同理，用 0x04 和 0x20。
            xs.resize(pointCount);
            ys.resize(pointCount);

            int32_t x = 0;
            for (uint32_t i = 0; i < pointCount; ++i) {
                const uint8_t flag = flags[i];
                if (flag & 0x02) {
                    REZERO_TRY(delta, view.readU8());
                    x += (flag & 0x10) ? static_cast<int32_t>(delta) : -static_cast<int32_t>(delta);
                } else if (!(flag & 0x10)) {
                    REZERO_TRY(delta, view.readI16());
                    x += delta;
                }
                xs[i] = x;
            }

            int32_t y = 0;
            for (uint32_t i = 0; i < pointCount; ++i) {
                const uint8_t flag = flags[i];
                if (flag & 0x04) {
                    REZERO_TRY(delta, view.readU8());
                    y += (flag & 0x20) ? static_cast<int32_t>(delta) : -static_cast<int32_t>(delta);
                } else if (!(flag & 0x20)) {
                    REZERO_TRY(delta, view.readI16());
                    y += delta;
                }
                ys[i] = y;
            }

            // 按轮廓切开，记录每条轮廓的末点下标。
            uint32_t pointIndex = 0;
            for (uint32_t c = 0; c < contourCount; ++c) {
                const uint32_t endPoint = endPts[c];
                for (; pointIndex <= endPoint; ++pointIndex) {
                    out.points.push_back({static_cast<float>(xs[pointIndex]), static_cast<float>(ys[pointIndex]),
                                          (flags[pointIndex] & 0x01) != 0});
                }
                out.contourEnds.push_back(static_cast<uint32_t>(out.points.size() - 1));
            }
            return {};
        }

        /// 复合字形：由若干"部件字形 + 仿射变换"拼成。
        ///
        /// 每个部件是一条记录（后三项视标志位可选）：
        ///
        ///     uint16 flags
        ///     uint16 glyphIndex
        ///     int16 或 int8 arg1, arg2      由 0x0001 决定宽度
        ///     变换参数                       见 WE_HAVE_* 标志
        ///
        /// 用到的标志位：
        ///
        ///     0x0001 ARG_1_AND_2_ARE_WORDS     参数是 int16（否则 int8）
        ///     0x0002 ARGS_ARE_XY_VALUES        参数是 xy 偏移（否则点配对，本库不支持）
        ///     0x0004 ROUND_XY_TO_GRID          偏移取整到网格
        ///     0x0008 WE_HAVE_A_SCALE           一个 F2Dot14，x/y 共用
        ///     0x0020 MORE_COMPONENTS           后面还有部件
        ///     0x0040 WE_HAVE_AN_X_AND_Y_SCALE  两个 F2Dot14，x/y 各一个
        ///     0x0080 WE_HAVE_A_TWO_BY_TWO      四个 F2Dot14，完整 2x2 矩阵
        ///     0x0100 WE_HAVE_INSTRUCTIONS      尾部还有指令字节（跳过）
        ///     0x0800 SCALED_COMPONENT_OFFSET   偏移要跟着矩阵缩放
        ///     0x1000 UNSCALED_COMPONENT_OFFSET 偏移不缩放（优先于 0x0800）
        ///
        /// F2Dot14 是定点数：值 = 原始 int16 / 16384。
        auto loadCompositeGlyph(TableView &view, GlyphScratch &scratch, GlyphOutline &out) const -> Result<void> {
            uint16_t flags = 0;

            for (;;) {
                REZERO_TRY(inFlags, view.readU16());
                REZERO_TRY(inGlyphId, view.readU16());
                flags = inFlags;
                const uint16_t glyphId = inGlyphId;

                int32_t arg1 = 0;
                int32_t arg2 = 0;
                if (flags & 0x0001) {
                    REZERO_TRY(rawX, view.readI16());
                    REZERO_TRY(rawY, view.readI16());
                    arg1 = rawX;
                    arg2 = rawY;
                } else {
                    REZERO_TRY(rawX, view.readI8());
                    REZERO_TRY(rawY, view.readI8());
                    arg1 = rawX;
                    arg2 = rawY;
                }

                if (!(flags & 0x0002))
                    return Error{ErrorCode::CompositeGlyph_UnsupportedPointMatching};

                // 2x2 变换矩阵，默认单位矩阵：
                //     x' = a * x + b * y
                //     y' = c * x + d * y
                float a = 1.0f;
                float b = 0.0f;
                float c = 0.0f;
                float d = 1.0f;

                if (flags & 0x0008) {
                    REZERO_TRY(scale, view.readI16());
                    a = d = static_cast<float>(scale) / 16384.0f;
                } else if (flags & 0x0040) {
                    REZERO_TRY(scaleX, view.readI16());
                    REZERO_TRY(scaleY, view.readI16());
                    a = static_cast<float>(scaleX) / 16384.0f;
                    d = static_cast<float>(scaleY) / 16384.0f;
                } else if (flags & 0x0080) {
                    REZERO_TRY(m00, view.readI16());
                    REZERO_TRY(m01, view.readI16());
                    REZERO_TRY(m10, view.readI16());
                    REZERO_TRY(m11, view.readI16());
                    a = static_cast<float>(m00) / 16384.0f;
                    b = static_cast<float>(m01) / 16384.0f;
                    c = static_cast<float>(m10) / 16384.0f;
                    d = static_cast<float>(m11) / 16384.0f;
                }

                float dx = static_cast<float>(arg1);
                float dy = static_cast<float>(arg2);

                // SCALED_COMPONENT_OFFSET：偏移量本身也要过一遍上面的矩阵。
                if ((flags & 0x0800) != 0 && (flags & 0x1000) == 0) {
                    const float scaledX = a * dx + b * dy;
                    const float scaledY = c * dx + d * dy;
                    dx = scaledX;
                    dy = scaledY;
                }

                if (flags & 0x0004) {
                    dx = roundf(dx);
                    dy = roundf(dy);
                }

                // 部件自己也可能又是复合字形，所以这里递归。
                // 用局部 Outline：嵌套时共用一个可复用缓冲会互相覆盖。
                GlyphOutline component(Allocator);
                REZERO_CHECK(parseGlyph(glyphId, scratch, component));

                // 把部件的点变换后接到当前轮廓后面，并平移它的轮廓末点下标。
                const uint32_t basePoint = static_cast<uint32_t>(out.points.size());
                for (const OutlinePoint &p : component.points)
                    out.points.push_back({a * p.x + b * p.y + dx, c * p.x + d * p.y + dy, p.onCurve});
                for (const uint32_t componentEnd : component.contourEnds)
                    out.contourEnds.push_back(componentEnd + basePoint);

                if (!(flags & 0x0020))
                    break;
            }

            if (flags & 0x0100) {
                REZERO_TRY(length, view.readU16());
                REZERO_CHECK(view.skip(length));
            }
            return {};
        }

        AllocatorView Allocator;
        Span<uint8_t> bytes;
        ArrayList<uint32_t> loca;
        uint32_t glyfOffset = 0;
        int16_t indexToLocFormat = 0;
        uint16_t numGlyphs = 0;
        bool opened = false;
    };

} // namespace ReZeroTrueType
