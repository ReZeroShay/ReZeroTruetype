#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <optional>
#include <ReZeroTruetype/ReZeroTrueType.h>
#include "tigr.h"

namespace {
    class CAllocator {
      public:
        void *allocate(std::size_t size, [[maybe_unused]] std::size_t alignment) noexcept { return malloc(size); }
        void deallocate(void *ptr) noexcept { free(ptr); }
    };
    static_assert(ReZeroTrueType::DeallocatingAllocator<CAllocator>);

    static auto loadFile(const char *path) -> std::vector<std::uint8_t> {
        std::vector<std::uint8_t> data;
        FILE *f = std::fopen(path, "rb");
        if (!f)
            return data;
        std::fseek(f, 0, SEEK_END);
        long n = std::ftell(f);
        std::fseek(f, 0, SEEK_SET);
        if (n > 0) {
            data.resize(static_cast<std::size_t>(n));
            if (std::fread(data.data(), 1, static_cast<std::size_t>(n), f) != static_cast<std::size_t>(n))
                data.clear();
        }
        std::fclose(f);
        return data;
    }

    auto nextCodepoint(std::u8string_view text, std::size_t &offset) -> std::optional<uint32_t> {
        const ReZeroTrueType::u8StringView view(text.data(), text.size());
        auto cp = ReZeroTrueType::unicode::decodeUtf8(view, offset);
        if (!cp)
            return std::nullopt;
        return *cp;
    }

    auto drawGlyphFromPackedAtlas(Tigr *dst, ReZeroTrueType::PackedAtlas &atlas, const ReZeroTrueType::GlyphMetric &g,
                                  int penX, int baselineY, TPixel color) {
        if (g.blank || g.w == 0 || g.h == 0)
            return;

        const int dx = penX + g.ox;
        const int dy = baselineY + g.oy;

        const int x0 = std::max(dx, 0);
        const int y0 = std::max(dy, 0);
        const int x1 = std::min(dx + static_cast<int>(g.w), dst->w);
        const int y1 = std::min(dy + static_cast<int>(g.h), dst->h);

        if (x0 >= x1 || y0 >= y1)
            return;

        for (int y = y0; y < y1; ++y) {
            const uint32_t atlasY = g.y + static_cast<uint32_t>(y - dy);

            for (int x = x0; x < x1; ++x) {
                const uint32_t atlasX = g.x + static_cast<uint32_t>(x - dx);
                const uint8_t coverage = atlas.grid[atlasX, atlasY].a;
                if (coverage == 0)
                    continue;

                // 用库里的线性光混合，别在这儿再手写一遍 sRGB 直插的版本。
                TPixel &pixel = dst->pix[static_cast<std::size_t>(y) * dst->w + x];
                ReZeroTrueType::compositePixel(pixel.r, pixel.g, pixel.b, pixel.a,
                                               ReZeroTrueType::Pixel(color.r, color.g, color.b, color.a), coverage);
            }
        }
    }

    auto lerpColor(TPixel from, TPixel to, float t) -> TPixel {
        auto lerp = [t](unsigned char a, unsigned char b) { return static_cast<unsigned char>(a + (b - a) * t); };
        return tigrRGB(lerp(from.r, to.r), lerp(from.g, to.g), lerp(from.b, to.b));
    }

    auto drawTextLine(Tigr *screen, ReZeroTrueType::PackedAtlas &atlas, std::u8string_view text, size_t textNum,
                      uint32_t baselineY, uint32_t penX) {
        size_t offset = 0;

        for (auto i = 0u; i < textNum; i += 1) {
            auto cp = nextCodepoint(text, offset);
            if (!cp)
                break;
            if (const auto *g = atlas.find(*cp); g) {
                const float t = textNum > 1 ? static_cast<float>(i) / static_cast<float>(textNum - 1) : 0.f;
                drawGlyphFromPackedAtlas(screen, atlas, *g, penX, baselineY,
                                         lerpColor(tigrRGB(0x7f, 0xd6, 0xff), tigrRGB(0xff, 0xc4, 0x6b), t));
                penX += static_cast<uint32_t>(g->advance);
            }
        }
    }

} // namespace

int main([[maybe_unused]] int argc, [[maybe_unused]] char *argv[]) {
    CAllocator allocator;
    ReZeroTrueType::AllocatorView allocatorView(allocator);
    std::string fontPath = REZERO_EXAMPLE_FONT;
    auto content = loadFile(fontPath.c_str());
    if (content.empty()) {
        std::fprintf(stderr, "cannot read font: %s\n", fontPath.c_str());
        return 1;
    }
    auto font = ReZeroTrueType::TrueType::loadTrueTyperomMemory(allocatorView, {content.data(), content.size()});
    if (!font) {
        std::fprintf(stderr, "cannot parse font: %s\n", fontPath.c_str());
        return 1;
    }

    ReZeroTrueType::FontPackerParams params;
    params.pixelSize = 18.f;
    ReZeroTrueType::FontPacker packer(allocatorView, *font, params);
    packer.includeChars(u8"你好");

    auto atlasRes = packer.pack();
    if (!atlasRes) {
        fprintf(stderr, "pack failed: %d\n", static_cast<int>(atlasRes.error()));
        return 1;
    }
    auto &atlas = *atlasRes;
    printf("atlas: %ux%u, glyphs: %zu\n", atlas.grid.width, atlas.grid.height, atlas.glyphs.size());

    Tigr *canvas = tigrBitmap(static_cast<int>(atlas.grid.width), static_cast<int>(atlas.grid.height));
    if (!canvas)
        return 1;
    ::memcpy(canvas->pix, atlas.grid.data(), static_cast<std::size_t>(atlas.grid.width) * atlas.grid.height * 4);

    Tigr *screen = tigrWindow(600, 600, "ReZeroRenderText Example", 0);
    if (!screen) {
        std::fprintf(stderr, "cannot create window\n");
        tigrFree(canvas);
        return 1;
    }

    while (!tigrClosed(screen)) {
        tigrClear(screen, tigrRGB(30, 30, 30));
        float pixelSize = 24.f;

        const std::u8string title = u8"Hello from ReZeroTrueType.";
        uint32_t baselineY = static_cast<uint32_t>(font->lineHeight(pixelSize));
        uint32_t penX = 24;
        drawTextLine(screen, atlas, title, title.size(), baselineY, penX);
        baselineY += static_cast<uint32_t>(font->lineHeight(pixelSize));
        const std::u8string Line1 = u8"Simple, fast, and lightweight";
        drawTextLine(screen, atlas, Line1, Line1.size(), baselineY, penX);
        baselineY += static_cast<uint32_t>(font->lineHeight(pixelSize));
        const std::u8string Line2 = u8"Make with C++";
        drawTextLine(screen, atlas, Line2, Line2.size(), baselineY, penX);
        baselineY += static_cast<uint32_t>(font->lineHeight(pixelSize)) * 3;
        const std::u8string Line3 = u8"ABCDEFGHIJKLMNOPQRSTUVWXYZ";
        drawTextLine(screen, atlas, Line3, Line3.size(), baselineY, penX);
        baselineY += static_cast<uint32_t>(font->lineHeight(pixelSize));
        const std::u8string Line4 = u8"abcdefghijklmnopqrstuvwxyz 0123456789 !?.,;:";
        drawTextLine(screen, atlas, Line4, Line4.size(), baselineY, penX);
        baselineY += static_cast<uint32_t>(font->lineHeight(pixelSize));
        const std::u8string Line5 = u8"你好";
        drawTextLine(screen, atlas, Line5, 2, baselineY, penX);

        tigrUpdate(screen);
    }

    tigrFree(canvas);
    tigrFree(screen);

    return 0;
}
