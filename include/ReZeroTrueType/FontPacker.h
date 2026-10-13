// pack() 的流程：
//
//   pack()
//     ├─ planGlyphs()            第一遍：只算每个字形要预留多大的盒子，不碰像素
//     │    └─ computeBox()
//     ├─ sortByHeight()          高到矮排序，装出来的图集更紧凑
//     ├─ initialSide()           按总面积估一个初始边长
//     ├─ tryPack()               把盒子摆进图集（装不下就边长翻倍重试）
//     └─ renderGlyphs()          第二遍：解析 + 光栅化，直接写进图集的最终位置
//
// 两遍式的意义：第一遍完全不产生"每个字形一份小位图"，所以峰值内存里
// 只剩下最终的图集本身；装箱重试也只是重跑一遍数学，不涉及任何像素拷贝。

#pragma once
#include <cmath>
#include <ReZeroTrueType/Error.h>
#include <ReZeroTrueType/Font.h>
#include <ReZeroTrueType/Image.h>
#include <ReZeroTrueType/Rasterize.h>
#include <ReZeroTrueType/ReZeroLib.h>

namespace ReZeroTrueType {

    /// 直接从图集画字所需的一切信息。
    struct GlyphMetric {
        uint32_t cp = 0;
        /// 在图集里的位置和尺寸。空白字形是 1×1。
        uint32_t x = 0;
        uint32_t y = 0;
        uint32_t w = 1;
        uint32_t h = 1;
        /// 归一化的 UV，等于 (x, y, x + w, y + h) / 图集边长。GPU 上传纹理时直接用。
        float u0 = 0.0f;
        float v0 = 0.0f;
        float u1 = 0.0f;
        float v1 = 0.0f;
        /// 相对笔位置 / 基线的偏移（设备像素），绘制时加上即可。
        int32_t ox = 0;
        int32_t oy = 0;
        /// 前进量（整数设备像素）。
        ///
        /// 整数定位模型下笔只能按整数步进，所以这里在打包时就四舍五入定死 —— 交给消费者
        /// 各自 static_cast 截断的话，每个字形会少走 0..1px，而且是**同向**累积的
        /// （advance 10.4 每次都走成 10），几十个字符后字距就能看出被挤紧。
        int32_t advance = 0;
        /// 空白字形（空格之类），没有墨迹，不要画。
        bool blank = true;
    };

    struct PackedAtlas {
        // RGBA8
        Image grid;
        ArrayList<GlyphMetric> glyphs;
        explicit PackedAtlas(AllocatorView source) noexcept : grid(source), glyphs(source) {}
        [[nodiscard]] auto find(uint32_t codepoint) const noexcept -> const GlyphMetric * {
            std::size_t low = 0;
            std::size_t high = glyphs.size();
            while (low < high) {
                const std::size_t mid = (low + high) / 2;
                if (glyphs[mid].cp < codepoint)
                    low = mid + 1;
                else
                    high = mid;
            }
            if (low < glyphs.size() && glyphs[low].cp == codepoint)
                return &glyphs[low];
            return nullptr;
        }
    };

    struct FontPackerParams {
        float pixelSize = 18.f;
        std::uint32_t padding = 2;
        bool includeAscii = true;
        std::uint32_t maxSide = 4096;
        RasterizeParams rasterize = {};
    };

    class FontPacker {
        template <typename T> using Result = Result<T, ErrorCode>;

      private:
        struct GlyphPlan {
            uint32_t cp = 0;
            uint16_t glyphId = 0;
            /// 设备像素空间里预留的盒子（y 已经翻转到图像坐标）。
            int32_t boxX = 0;
            int32_t boxY = 0;
            uint32_t boxW = 1;
            uint32_t boxH = 1;
            /// 装箱结果：盒子左上角在图集里的位置。
            uint32_t atlasX = 0;
            uint32_t atlasY = 0;
            float advance = 0.f;
            bool blank = true;
        };

        /// 货架：图集里的一横行。块从左往右摆，摆满就封层。
        struct Shelf {
            uint32_t y = 0;
            /// 这一层下一个空位的 x。
            uint32_t cursorX = 0;
            /// 这一层的高度 = 放进来过的最高块（含 padding）。
            uint32_t height = 0;
        };

      public:
        FontPacker(AllocatorView allocator, TrueType &font, const FontPackerParams params)
            : allocator(allocator), params(params), font(font), rasterizer(allocator), outlineDecoder(allocator),
              scratch(allocator), shape(allocator), coverage(allocator), codepoints(allocator) {
            if (params.includeAscii)
                includeRange(0x20, 0x7E);
        }
        void includeCodepoint(std::uint32_t cp) {
            for (const uint32_t existing : codepoints)
                if (existing == cp)
                    return;
            codepoints.push_back(cp);
        }
        void includeRange(std::uint32_t first, std::uint32_t last) {
            for (auto cp = first; cp <= last; cp += 1) {
                includeCodepoint(cp);
            }
        }

        void includeChars(u8StringView chars) {
            std::size_t offset = 0;
            auto cp = unicode::decodeUtf8(chars, offset);
            while (cp) {
                includeCodepoint(*cp);
                cp = unicode::decodeUtf8(chars, offset);
            }
        }
        auto pack() -> Result<PackedAtlas> {
            REZERO_CHECK(outlineDecoder.open(font));
            ArrayList<GlyphPlan> plans(allocator);

            planGlyphs(plans);
            sortByHeight(plans);

            uint32_t side = initialSide(plans);
            for (;;) {
                if (tryPack(plans, side))
                    return renderGlyphs(plans, side);
                if (side >= params.maxSide)
                    return Error{ErrorCode::Packer_AtlasTooLarge};
                side *= 2;
            }
        }

      private:
        /// 第一遍：算出每个字形要预留多大的盒子。全程不产生像素。
        void planGlyphs(ArrayList<GlyphPlan> &plans) {
            const float scale = params.pixelSize / static_cast<float>(font.unitsPerEm);

            for (const uint32_t cp : codepoints) {
                GlyphPlan plan;
                plan.cp = cp;

                const auto glyphId = font.glyphIndexForCodepoint(cp);
                if (!glyphId || *glyphId == 0) {
                    plans.push_back(plan); // 字体里没有这个码点 -> 空白
                    continue;
                }
                plan.glyphId = *glyphId;
                // advance 在这里就要算好：空白字形（空格）也必须能推进笔。
                plan.advance = static_cast<float>(font.advanceWidth(plan.glyphId)) * scale;

                if (!computeBox(plan, scale)) {
                    plans.push_back(plan); // 空轮廓 -> 空白
                    continue;
                }
                plan.blank = false;
                plans.push_back(plan);
            }
        }

        /// 用 glyf 表头里的 bbox 算出设备空间的整数盒子。返回 false 表示这个字形没有墨迹。
        ///
        /// 换算过程：
        ///
        ///     字体空间（y 向上）            设备空间（y 向下）
        ///     xMin * scale        ->       boxX = floor(xMin * scale)
        ///     xMax * scale        ->       boxX + boxW = ceil(xMax * scale)
        ///     yMin * scale        ->       boxY + boxH = ceil(-yMin * scale)
        ///     yMax * scale        ->       boxY = floor(-yMax * scale)
        ///
        /// 注意 y 的上下边是**互换**的：字体里的最高点（yMax）翻转后变成设备空间里
        /// 的最小 y，也就是盒子的上边。floor/ceil 是为了对齐像素网格。
        ///
        /// glyf 表头存的是轮廓所有控制点的 bbox，而二次曲线一定落在控制点的凸包内，
        /// 所以这个盒子必然是真实墨迹的超集 —— 预留一定够用，不必再解析一遍轮廓。
        auto computeBox(GlyphPlan &plan, float scale) -> bool {
            const auto bounds = outlineDecoder.bounds(plan.glyphId);
            if (!bounds || bounds->empty())
                return false;

            const float minX = static_cast<float>(bounds->xMin) * scale;
            const float maxX = static_cast<float>(bounds->xMax) * scale;
            const float minY = -static_cast<float>(bounds->yMax) * scale;
            const float maxY = -static_cast<float>(bounds->yMin) * scale;

            plan.boxX = static_cast<int32_t>(floorf(minX));
            plan.boxY = static_cast<int32_t>(floorf(minY));
            int32_t right = static_cast<int32_t>(ceilf(maxX));
            int32_t bottom = static_cast<int32_t>(ceilf(maxY));

            // 退化盒子（宽度或高度算出来是 0）也至少给 1 像素。
            if (right <= plan.boxX)
                right = plan.boxX + 1;
            if (bottom <= plan.boxY)
                bottom = plan.boxY + 1;

            plan.boxW = static_cast<uint32_t>(right - plan.boxX);
            plan.boxH = static_cast<uint32_t>(bottom - plan.boxY);
            return true;
        }
        /// 高到矮、宽度次之、码点最后。先摆高的块，让每层的高度尽快定型；
        /// 矮块当"填空砖"，哪里剩得多往哪里塞。
        static void sortByHeight(ArrayList<GlyphPlan> &plans) {
            insertionSort(plans.span(), [](const GlyphPlan &a, const GlyphPlan &b) {
                if (a.boxH != b.boxH)
                    return a.boxH > b.boxH;
                if (a.boxW != b.boxW)
                    return a.boxW > b.boxW;
                return a.cp < b.cp;
            });
        }
        /// 初始边长：所有块（含 padding）面积之和乘 1.5 当余量，再向上取到 2 的幂，
        /// 最小从 128 开始。装不下时 pack() 会翻倍重试，所以估小了也没关系。
        [[nodiscard]] auto initialSide(const ArrayList<GlyphPlan> &plans) const noexcept -> uint32_t {
            std::size_t area = 0;
            for (const GlyphPlan &plan : plans) {
                if (plan.blank)
                    continue; // 空白字形不进图集
                area += static_cast<std::size_t>(plan.boxW + params.padding) * (plan.boxH + params.padding);
            }

            uint32_t side = 128;
            while (static_cast<std::size_t>(side) * side < area * 3 / 2)
                side *= 2;

            if (side > params.maxSide)
                side = params.maxSide;
            return side;
        }

        /// 把盒子摆进 side×side 的图集。成功时每个非空白字形都写好了 atlasX/atlasY。
        ///
        /// 每一层（Shelf）从下往上开：bottom 是已用区域的下边界，新层的 y 就是它。
        /// 摆一个块时先在已有层里找"塞得下且行尾剩得最少"的那层（best fit），
        /// 找不到就在最底部开新层。
        auto tryPack(ArrayList<GlyphPlan> &plans, uint32_t side) -> bool {
            ArrayList<Shelf> shelves(allocator);
            uint32_t bottom = params.padding;

            for (GlyphPlan &plan : plans) {
                plan.atlasX = 0;
                plan.atlasY = 0;
                if (plan.blank)
                    continue; // 空白字形没有墨迹，不占图集

                const uint32_t cellW = plan.boxW + params.padding;
                const uint32_t cellH = plan.boxH + params.padding;

                // 在已有层里挑塞下之后行尾剩得最少的那一层。
                int32_t best = -1;
                int32_t bestLeft = -1;
                for (std::size_t i = 0; i < shelves.size(); ++i) {
                    const Shelf &item = shelves[i];
                    if (item.height >= cellH && item.cursorX + cellW <= side) {
                        const int32_t left = static_cast<int32_t>(side - (item.cursorX + cellW));
                        if (best < 0 || left < bestLeft) {
                            best = static_cast<int32_t>(i);
                            bestLeft = left;
                        }
                    }
                }

                if (best >= 0) {
                    Shelf &item = shelves[static_cast<std::size_t>(best)];
                    plan.atlasX = item.cursorX;
                    plan.atlasY = item.y;
                    item.cursorX += cellW;
                    continue;
                }

                // 都塞不下：在最底部开一层。
                if (cellW > side || bottom + cellH > side + params.padding)
                    return false;
                shelves.push_back({bottom, params.padding + cellW, cellH});
                plan.atlasX = params.padding;
                plan.atlasY = bottom;
                bottom += cellH;
            }
            return true;
        }

        /// 第二遍：解析 + 光栅化，把覆盖率**直接写进图集的最终位置**。
        auto renderGlyphs(ArrayList<GlyphPlan> &plans, uint32_t side) -> Result<PackedAtlas> {
            PackedAtlas result(allocator);
            result.grid.allocate(side, side);
            result.glyphs.reserve(plans.size());

            const float scale = params.pixelSize / static_cast<float>(font.unitsPerEm);

            for (const GlyphPlan &plan : plans) {
                GlyphMetric metric;
                metric.cp = plan.cp;
                // 取整只发生在这里：plan 里保留精确的浮点值，暴露给消费者的度量是整数。
                metric.advance = static_cast<int32_t>(std::lround(plan.advance));
                metric.blank = plan.blank;
                metric.x = plan.atlasX;
                metric.y = plan.atlasY;

                if (!plan.blank) {
                    REZERO_CHECK(outlineDecoder.outline(plan.glyphId, scratch, shape));

                    // 就地变换到设备空间（y 轴翻转）。归一化是仿射无关的，
                    // 所以先归一化再变换和先变换再归一化等价。
                    for (OutlinePoint &point : shape.points)
                        point = {point.x * scale, -point.y * scale, point.onCurve};

                    REZERO_CHECK(rasterizer.rasterize(shape, coverage, params.rasterize));
                    if (coverage.width == 0 || coverage.height == 0) {
                        metric.blank = true; // 规划说有墨、实际没有：按空白处理
                    } else {
                        // 精确蒙版落在第一遍预留的盒子里，所以只需把"蒙版原点相对盒子原点"
                        // 的偏移加到盒子的图集坐标上。
                        //
                        //     dx = 蒙版原点 x - 盒子原点 x     （>= 0）
                        //     atlasX = 盒子在图集里的 x + dx
                        const int32_t dx = coverage.originX - plan.boxX;
                        const int32_t dy = coverage.originY - plan.boxY;
                        REZERO_ASSERT(dx >= 0 && dy >= 0);
                        REZERO_ASSERT(dx + static_cast<int32_t>(coverage.width) <= static_cast<int32_t>(plan.boxW));
                        REZERO_ASSERT(dy + static_cast<int32_t>(coverage.height) <= static_cast<int32_t>(plan.boxH));

                        metric.x = plan.atlasX + static_cast<uint32_t>(dx);
                        metric.y = plan.atlasY + static_cast<uint32_t>(dy);
                        metric.w = coverage.width;
                        metric.h = coverage.height;
                        metric.ox = coverage.originX;
                        metric.oy = coverage.originY;
                        blitMask(result.grid, metric.x, metric.y, coverage);
                    }
                }

                const float invSide = 1.0f / static_cast<float>(side);
                metric.u0 = static_cast<float>(metric.x) * invSide;
                metric.v0 = static_cast<float>(metric.y) * invSide;
                metric.u1 = static_cast<float>(metric.x + metric.w) * invSide;
                metric.v1 = static_cast<float>(metric.y + metric.h) * invSide;
                result.glyphs.push_back(metric);
            }

            insertionSort(result.glyphs.span(), [](const GlyphMetric &a, const GlyphMetric &b) { return a.cp < b.cp; });
            return std::move(result);
        }

        static void blitMask(Image &grid, uint32_t x0, uint32_t y0, const Mask &coverage) {
            for (uint32_t y = 0; y < coverage.height; ++y) {
                const uint8_t *source = coverage.alpha.data() + static_cast<std::size_t>(y) * coverage.width;
                Pixel *destination = grid.data() + static_cast<std::size_t>(y0 + y) * grid.width + x0;

                for (uint32_t x = 0; x < coverage.width; ++x) {
                    const uint8_t alpha = source[x];
                    if (alpha == 0)
                        continue; // 底本来就是全透明，零覆盖率不用写
                    destination[x] = Pixel(255, 255, 255, alpha);
                }
            }
        }

      private:
        AllocatorView allocator;
        FontPackerParams params;
        TrueType &font;
        Rasterizer rasterizer;
        Outlinedecoder outlineDecoder;
        GlyphScratch scratch;
        GlyphOutline shape;
        Mask coverage;
        ArrayList<uint32_t> codepoints;
    };

} // namespace ReZeroTrueType
