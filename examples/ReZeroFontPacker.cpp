#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
#include <ReZeroTrueType/ReZeroTrueType.h>
#include "ReZeroTrueType/Font.h"
#include "ReZeroTrueType/FontPacker.h"
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
    params.pixelSize = 22.f;
    ReZeroTrueType::FontPacker packer(allocatorView, *font, params);
    auto atlasRes = packer.pack();
    if (!atlasRes) {
        std::fprintf(stderr, "pack failed: %d\n", static_cast<int>(atlasRes.error()));
        return 1;
    }
    auto &atlas = *atlasRes;

    // auto &image = atlas->atlas;
    // int atlasW = image.width;
    // int atlasH = image.height;
    // std::printf("atlas: %dx%d\n", atlasW, atlasH);

    Tigr *canvas = tigrBitmap(static_cast<int>(atlas.grid.width), static_cast<int>(atlas.grid.height));
    if (!canvas)
        return 1;
    for (uint32_t y = 0; y < atlas.grid.height; ++y) {
        for (uint32_t x = 0; x < atlas.grid.width; ++x) {
            const uint8_t coverage = atlas.grid[x, y].a;
            canvas->pix[static_cast<std::size_t>(y) * atlas.grid.width + x] =
                coverage ? tigrRGBA(255, 255, 255, coverage) : tigrRGBA(0, 0, 0, 0);
        }
    }
    Tigr *screen = tigrWindow(800, 600, "ReZeroFontPacker Example", 0);
    if (!screen) {
        std::fprintf(stderr, "cannot create window\n");
        tigrFree(canvas);
        return 1;
    }

    while (!tigrClosed(screen)) {
        tigrClear(screen, tigrRGB(30, 30, 30));
        tigrBlitTint(screen, canvas, 0, 0, 0, 0, canvas->w, canvas->h, tigrRGB(255, 255, 255));

        tigrUpdate(screen);
    }

    tigrFree(canvas);
    tigrFree(screen);

    return 0;
}
