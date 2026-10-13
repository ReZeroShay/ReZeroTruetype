#pragma once

#include <cmath>
#include <ReZeroTrueType/ReZeroLib.h>

namespace ReZeroTrueType {
    struct Pixel {
        uint8_t r = 0;
        uint8_t g = 0;
        uint8_t b = 0;
        uint8_t a = 255;

        constexpr Pixel() = default;
        constexpr Pixel(uint8_t red, uint8_t green, uint8_t blue, uint8_t alpha = 255)
            : r(red), g(green), b(blue), a(alpha) {}
    };

    namespace detail {
        /// sRGB 分量 -> 线性光 [0, 1]。
        inline auto srgbToLinear(uint8_t value) noexcept -> float {
            const float encoded = static_cast<float>(value) / 255.0f;
            return encoded <= 0.04045f ? encoded / 12.92f : std::pow((encoded + 0.055f) / 1.055f, 2.4f);
        }

        /// 线性光 [0, 1] -> sRGB 分量。
        inline auto linearToSrgb(float value) noexcept -> uint8_t {
            const float clamped = value < 0.0f ? 0.0f : (value > 1.0f ? 1.0f : value);
            const float encoded =
                clamped <= 0.0031308f ? clamped * 12.92f : 1.055f * std::pow(clamped, 1.0f / 2.4f) - 0.055f;
            return static_cast<uint8_t>(encoded * 255.0f + 0.5f);
        }
    } // namespace detail

    /// 单个像素的 source-over 合成：把 `source` 按覆盖率 `coverage` 混到目标像素上。
    ///
    /// 混合在**线性光**空间里做。直接在 sRGB 空间插值（把编码值当成线性值用）会让反锯齿
    /// 的文字明显偏细偏淡：白字压黑底、覆盖率 50% 时那样算出来是 128，而正确值是 188 ——
    /// 这正是 DirectWrite 的 gamma 参数要修的东西。
    ///
    /// 目标像素用直通（非预乘）alpha，`coverage` 是 0..255 的灰度覆盖率，
    /// 源色的有效 alpha = `source.a * coverage / 255`。
    ///
    /// 拆成四个分量而不是收 `Pixel`，是为了让自绘后端的消费者（比如 tigr 的 TPixel）
    /// 也能共用同一份混合公式 —— 各写各的必然有人把 gamma 搞错。
    inline auto compositePixel(uint8_t &dstRed, uint8_t &dstGreen, uint8_t &dstBlue, uint8_t &dstAlpha,
                               const Pixel &source, uint8_t coverage) noexcept -> void {
        if (coverage == 0)
            return;

        const float sourceAlpha = (static_cast<float>(source.a) / 255.0f) * (static_cast<float>(coverage) / 255.0f);
        if (sourceAlpha <= 0.0f)
            return; // 源完全透明：source-over 的结果就是目标不变

        const float destinationAlpha = static_cast<float>(dstAlpha) / 255.0f;
        const float outAlpha = sourceAlpha + destinationAlpha * (1.0f - sourceAlpha);
        const float dstWeight = destinationAlpha * (1.0f - sourceAlpha);

        // 三个通道互相独立，先把目标色读出来，免得写回时覆盖掉还没用到的值。
        const uint8_t previousRed = dstRed;
        const uint8_t previousGreen = dstGreen;
        const uint8_t previousBlue = dstBlue;

        const float red =
            (detail::srgbToLinear(source.r) * sourceAlpha + detail::srgbToLinear(previousRed) * dstWeight) / outAlpha;
        const float green =
            (detail::srgbToLinear(source.g) * sourceAlpha + detail::srgbToLinear(previousGreen) * dstWeight) / outAlpha;
        const float blue =
            (detail::srgbToLinear(source.b) * sourceAlpha + detail::srgbToLinear(previousBlue) * dstWeight) / outAlpha;

        dstRed = detail::linearToSrgb(red);
        dstGreen = detail::linearToSrgb(green);
        dstBlue = detail::linearToSrgb(blue);
        dstAlpha = static_cast<uint8_t>(outAlpha * 255.0f + 0.5f);
    }

    /// `Pixel` 版本的 compositePixel()。
    inline auto compositePixel(Pixel &destination, const Pixel &source, uint8_t coverage) noexcept -> void {
        compositePixel(destination.r, destination.g, destination.b, destination.a, source, coverage);
    }

    struct Image {
        explicit Image(AllocatorView allocator) : allocator(allocator) {}

        ~Image() noexcept { release(); }

        Image(const Image &) = delete;
        Image &operator=(const Image &) = delete;

        Image(Image &&other) noexcept
            : allocator(other.allocator), pixels(other.pixels), width(other.width), height(other.height) {
            other.pixels = nullptr;
            other.width = 0;
            other.height = 0;
        }

        Image &operator=(Image &&other) noexcept {
            if (this == &other)
                return *this;

            release();
            allocator = other.allocator;
            pixels = other.pixels;
            width = other.width;
            height = other.height;

            other.pixels = nullptr;
            other.width = 0;
            other.height = 0;
            return *this;
        }

        void allocate(uint32_t inWidth, uint32_t inHeight) noexcept {
            release();

            width = inWidth;
            height = inHeight;

            const std::size_t count = static_cast<std::size_t>(inWidth) * static_cast<std::size_t>(inHeight);
            pixels = static_cast<Pixel *>(allocator.allocate(count * sizeof(Pixel), alignof(Pixel)));
            REZERO_ASSERT(pixels != nullptr);

            // 底色填"透明白"而不是透明黑。字形纹素是 (255,255,255,a)，如果底色是透明黑，
            // 双线性采样在字形边缘会把 RGB 从 255 插到 0 —— 一圈黑边。RGB 恒为 255 则
            // 怎么插值都不串色，预乘和非预乘两种采样方式都成立。
            fill(Pixel(255, 255, 255, 0));
        }
        void fill(Pixel value) noexcept {
            const std::size_t count = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
            for (std::size_t i = 0; i < count; ++i)
                pixels[i] = value;
        }

        /// 把一张 8 位覆盖率蒙版按 `color` 合成到本图像上，蒙版左上角对齐 (x0, y0)。
        ///
        /// 蒙版行优先、每行 `coverageWidth` 字节；超出图像的部分会被裁掉。
        /// 混合公式见 compositePixel()：线性光空间里的 source-over，所以浅色背景上的深色字
        /// 不会像在 sRGB 空间直接插值那样偏细偏淡。
        void compositeMask(uint32_t x0, uint32_t y0, const uint8_t *coverage, uint32_t coverageWidth,
                           uint32_t coverageHeight, Pixel color) noexcept {
            if (coverage == nullptr || coverageWidth == 0 || coverageHeight == 0)
                return;
            if (x0 >= width || y0 >= height)
                return;

            // 用减法而不是加法：x0/y0 已经被上面挡过一次，不会下溢。
            const uint32_t columns = min(coverageWidth, width - x0);
            const uint32_t rows = min(coverageHeight, height - y0);

            for (uint32_t y = 0; y < rows; ++y) {
                const uint8_t *source = coverage + static_cast<std::size_t>(y) * coverageWidth;
                Pixel *destination = pixels + static_cast<std::size_t>(y0 + y) * width + x0;

                for (uint32_t x = 0; x < columns; ++x)
                    compositePixel(destination[x], color, source[x]);
            }
        }

        Pixel &operator[](uint32_t x, uint32_t y) noexcept {
            REZERO_ASSERT(pixels);
            return pixels[static_cast<std::size_t>(y) * width + x];
        }
        const Pixel &operator[](uint32_t x, uint32_t y) const noexcept {
            REZERO_ASSERT(pixels);
            return pixels[static_cast<std::size_t>(y) * width + x];
        }

        auto data() -> Pixel * { return pixels; }
        auto data() const -> const Pixel * { return pixels; }

        void release() noexcept {
            if (pixels == nullptr)
                return;
            allocator.deallocate(pixels);
            pixels = nullptr;
            width = 0;
            height = 0;
        }

        AllocatorView allocator;
        Pixel *pixels{nullptr};
        uint32_t width = 0;
        uint32_t height = 0;
    };
} // namespace ReZeroTrueType
