// rasterize()
//   ├─ makeLinesFromOutline()      → 二次曲线 → 线段
//   ├─ calculateBBox()
//   └─ supersample()         → 多采样 + nonzero winding
#pragma once
#include <cmath>
#include <ReZeroTrueType/Error.h>
#include <ReZeroTrueType/Font.h>
#include <ReZeroTrueType/ReZeroLib.h>

namespace ReZeroTrueType {

    struct RasterizeParams {
        /// 每个像素每边的采样点数 = 1 << subpixelBits。
        ///
        /// 2 -> 4×4 = 16 个采样，覆盖率有 17 个等级（0..16 步进）。
        /// 提高它会变慢且更费内存，但边缘更平滑。
        uint32_t subpixelBits = 4;
    };

    struct Mask {
        explicit Mask(AllocatorView allocator) : alpha(allocator) {}

        void clear() noexcept {
            width = 0;
            height = 0;
            originX = 0;
            originY = 0;
            alpha.clear();
        }

        uint32_t width = 0;
        uint32_t height = 0;
        int32_t originX = 0;
        int32_t originY = 0;
        ArrayList<uint8_t> alpha;
    };

    ///     rasterize()
    ///       ├─ flattenOutline()   二次曲线 -> 线段
    ///       ├─ boundingBox()      线段端点 -> 整数矩形
    ///       └─ superSample()      逐像素多采样 + 非零环绕规则 -> 覆盖率
    class Rasterizer {
        template <typename T> using Result = Result<T, ErrorCode>;

      public:
        explicit Rasterizer(AllocatorView allocator) : segments(allocator) {}

        auto rasterize(const GlyphOutline &shape, Mask &out, const RasterizeParams &params = {}) -> Result<void> {
            out.clear();

            if (shape.empty())
                return {};

            // 每个像素最多使用 4 × 4 个采样点。
            const uint32_t samples = 1u << params.subpixelBits;
            // 采样越多，曲线需要越精确。
            const float tolerance = 0.25f / static_cast<float>(samples);

            // 轮廓转换成直线。
            flattenOutline(shape, tolerance);

            if (segments.empty())
                return {};

            const Box area = boundingBox();
            if (area.empty())
                return {};

            out.width = area.width();
            out.height = area.height();
            out.originX = area.minX;
            out.originY = area.minY;

            out.alpha.reserve(static_cast<std::size_t>(out.width) * out.height);
            superSample(area, samples, out.alpha);
            return {};
        }

      private:
        struct Segment {
            float x0;
            float y0;
            float x1;
            float y1;
        };
        struct Box {
            int32_t minX = 0;
            int32_t minY = 0;
            int32_t maxX = 0;
            int32_t maxY = 0;

            [[nodiscard]] auto empty() const noexcept -> bool { return maxX <= minX || maxY <= minY; }
            [[nodiscard]] auto width() const noexcept -> uint32_t { return static_cast<uint32_t>(maxX - minX); }
            [[nodiscard]] auto height() const noexcept -> uint32_t { return static_cast<uint32_t>(maxY - minY); }
        };

        // 第一步：曲线展平成线段
        //
        /// 把一条轮廓展平。
        ///
        /// 归一化之后点序列形如 ON, OFF, ON, OFF, ...。固定按"当前点 a、下一个点 b"
        /// 处理：
        ///
        ///     b 是 ON  -> 直线段 a→b
        ///     b 是 OFF -> 二次曲线 a→b→c，其中 c 是 b 之后那个点（一定是 ON）
        ///
        /// 下标全部取模 count，轮廓是闭合的，所以末尾自然绕回起点，不用特殊收尾。
        void flattenContour(const GlyphOutline &shape, uint32_t start, uint32_t count, float tolerance) {
            for (uint32_t i = 0; i < count; i += 1) {
                const OutlinePoint &a = shape.points[start + i];
                const OutlinePoint &b = shape.points[start + (i + 1) % count];
                // ON -> ON，直接连接
                if (b.onCurve) {
                    segments.push_back({a.x, a.y, b.x, b.y});
                    continue;
                }
                // ON → OFF → ON 二次曲线
                const OutlinePoint &c = shape.points[start + (i + 2) % count];
                flattenQuadratic(a.x, a.y, b.x, b.y, c.x, c.y, tolerance);

                i += 1; // 跳过控制点
            }
        }
        void flattenOutline(const GlyphOutline &shape, float tolerance) {
            segments.clear();

            uint32_t contourStart = 0;
            for (const uint32_t end : shape.contourEnds) {
                flattenContour(shape, contourStart, end - contourStart + 1, tolerance);
                contourStart = end + 1;
            }
        }
        /// 把一条二次贝塞尔曲线递归切成线段。
        ///
        /// 曲线的参数式：
        ///
        ///     b(t) = (1-t)² p0 + 2(1-t) t c + t² p1,   t ∈ [0, 1]
        ///
        /// 在 t = 0.5 处切开（De Casteljau），得到两条新的二次曲线：
        ///
        ///     m = (p0 + c) / 2        左边那条的控制点
        ///     n = (c + p1) / 2        右边那条的控制点
        ///     q = (m + n) / 2         切分点，两条曲线共用的端点
        ///
        ///     左边: p0, m, q          右边: q, n, p1
        ///
        /// **什么时候可以停**：曲线离它的弦最远的地方在 t = 0.5，
        ///
        ///     b(0.5)  = (p0 + 2c + p1) / 4
        ///     弦中点   = (p0 + p1) / 2
        ///     最大偏差 = |b(0.5) - 弦中点| = |2c - p0 - p1| / 4 = |c - 弦中点| / 2
        ///
        /// 所以"控制点到弦中点的距离 <= 2 * tolerance"就能保证偏差不超过 tolerance。
        /// 下面直接拿这个距离和 tolerance 比，等于把阈值收紧了一半：多切一刀，
        /// 但省掉一次乘 2。每切一次偏差大约降到 1/4，所以一定收敛，
        /// 不需要额外的递归深度上限。
        void flattenQuadratic(float x0, float y0, float cx, float cy, float x1, float y1, float tolerance) {
            const float midX = (x0 + x1) * 0.5f;
            const float midY = (y0 + y1) * 0.5f;

            // 控制点到弦中点的距离，和上面的偏差公式差一个常数因子 2。
            const float dx = cx - midX;
            const float dy = cy - midY;

            if (dx * dx + dy * dy <= tolerance * tolerance) {
                segments.push_back({x0, y0, x1, y1});
                return;
            }

            // De Casteljau 切分,将曲线分成两段。
            const float mx = (x0 + cx) * 0.5f;
            const float my = (y0 + cy) * 0.5f;
            const float nx = (cx + x1) * 0.5f;
            const float ny = (cy + y1) * 0.5f;
            const float qx = (mx + nx) * 0.5f;
            const float qy = (my + ny) * 0.5f;

            flattenQuadratic(x0, y0, mx, my, qx, qy, tolerance);
            flattenQuadratic(qx, qy, nx, ny, x1, y1, tolerance);
        }

        /// 蒙版范围 = 所有线段端点的包围盒，左边向下取整、右边向上取整：
        ///
        ///     minX = floor(min(x0, x1))    maxX = ceil(max(x0, x1))
        ///     minY = floor(min(y0, y1))    maxY = ceil(max(y0, y1))
        ///
        /// 展平后的端点都落在轮廓控制点的凸包内，所以这个矩形不会漏掉任何墨迹。
        /// 取整是为了让蒙版边界对齐像素网格。
        [[nodiscard]] auto boundingBox() const noexcept -> Box {
            float minX = segments[0].x0;
            float minY = segments[0].y0;
            float maxX = segments[0].x0;
            float maxY = segments[0].y0;

            for (const Segment &item : segments) {
                minX = min(minX, min(item.x0, item.x1));
                maxX = max(maxX, max(item.x0, item.x1));
                minY = min(minY, min(item.y0, item.y1));
                maxY = max(maxY, max(item.y0, item.y1));
            }

            Box area;
            area.minX = static_cast<int32_t>(floorf(minX));
            area.minY = static_cast<int32_t>(floorf(minY));
            area.maxX = static_cast<int32_t>(ceilf(maxX));
            area.maxY = static_cast<int32_t>(ceilf(maxY));
            return area;
        }

        /// 点是否在轮廓内部 —— TrueType 规定的**非零环绕规则**。
        ///
        /// 做法：从被测点向右发一条水平射线，看它穿过每条边的方向，
        /// 把顺时针穿越记 +1、逆时针记 -1，累加得到环绕数 winding。
        ///
        ///     if ((y0 > y) == (y1 > y))  跳过        边的两端在射线同一侧
        ///     t      = (y - y0) / (y1 - y0)          交点在边上的参数
        ///     crossX = x0 + t * (x1 - x0)            交点的 x 坐标
        ///     if (x < crossX)  winding += sign(y1 - y0)
        ///
        /// `(y0 > y) == (y1 > y)` 是半开区间 [min, max) 的写法，作用是：射线正好
        /// 穿过顶点时，相邻两条边里只有一条算穿过，避免重复计数。
        ///
        /// 最后 `winding != 0` 即在内部。用奇偶规则的话，笔画互相重叠的汉字
        /// 内部会出空洞，所以必须用非零规则。
        [[nodiscard]] auto pointInside(float x, float y) const noexcept -> bool {
            int32_t winding = 0;

            for (const Segment &item : segments) {
                if ((item.y0 > y) == (item.y1 > y))
                    continue;
                // 求出y水平线上于该线段的交点x
                const float t = (y - item.y0) / (item.y1 - item.y0);
                const float crossX = item.x0 + t * (item.x1 - item.x0);
                // 判断该点向右发射的射线是否可以命中这条线段
                if (x < crossX)
                    winding += (item.y1 > item.y0) ? 1 : -1; // 边的方向决定符号
            }

            return winding != 0; // 非零环绕规则 (nonzero winding)
        }

        /// 逐像素算覆盖率。
        ///
        /// 每个像素切成 samples × samples 个子格，采样点取每个子格的**中心**：
        ///
        ///     sampleX = x + (subX + 0.5) / samples
        ///     sampleY = y + (subY + 0.5) / samples
        ///
        /// 取中心而不是角点，是为了让采样点不会正好落在像素边界上。
        /// 覆盖率 = 落在内部的采样点数 / 总采样数，换算到 0..255：
        ///
        ///     alpha = inside * 255 / total
        ///
        /// 复杂度是 o(宽 × 高 × samples² × 线段数)：每个采样点都要重扫一遍线段表。
        /// 这是本实现最贵的一步，换来的是没有边表、没有排序、代码一眼看完 ——
        /// 对这个项目的定位来说，这个取舍是划算的。
        void superSample(const Box &area, uint32_t samples, ArrayList<uint8_t> &alpha) const {
            const uint32_t total = samples * samples;
            const float invSamples = 1.0f / static_cast<float>(samples);

            for (int32_t y = area.minY; y < area.maxY; ++y) {
                for (int32_t x = area.minX; x < area.maxX; ++x) {
                    uint32_t inside = 0;

                    for (uint32_t subY = 0; subY < samples; ++subY) {
                        const float sampleY = static_cast<float>(y) + (static_cast<float>(subY) + 0.5f) * invSamples;

                        for (uint32_t subX = 0; subX < samples; ++subX) {
                            const float sampleX =
                                static_cast<float>(x) + (static_cast<float>(subX) + 0.5f) * invSamples;
                            if (pointInside(sampleX, sampleY))
                                ++inside;
                        }
                    }

                    alpha.push_back(static_cast<uint8_t>((inside * 255u + total / 2) / total));
                }
            }
        }

        ArrayList<Segment> segments;
    };

    struct ScanlineRasterizer {
        // TODO
    };
} // namespace ReZeroTrueType
