#pragma once
#include <ReZeroTrueType/ReZeroLib.h>

namespace ReZeroTrueType {

    enum class ErrorCode : uint8_t {
        InvalidData,
        Unsupported,
        FileTooSmall,
        InvalidVersion,
        UnexpectedEOF,
        MissingTable,
        InvalidUtf8,
        InvalidSegmentCount,
        OutOfBounds,
        NotFoundCodepoint,
        UnitsPerEmIsZero,
        Glyph_TooDeep,
        Glyph_OutOfBounds,
        CompositeGlyph_UnsupportedPointMatching,
        DrawText_InvalidUtf8,
        Rasterize_InvalidWidthHeight,
        Packer_AtlasTooLarge,
    };

} // namespace ReZeroTrueType
