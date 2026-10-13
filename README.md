![Spotify Status](/assets/card.svg)

# ReZeroTrueType

A lightwieght TrueType font library for pasring, rasterizetion, and atlas packing.Built for learning purposes.

## Requirements

- A C++23 compiler (MSVC, Clang or GCC)
- CMake 3.21 or newer
- On Windows the examples need `opengl32` and `gdi32`. CMake handles this.

## Build

```bash
cmake --preset default          # Ninja + cl, Debug
cmake --build --preset default

# or without presets
cmake -S . -B build
cmake --build build
```

For a release build, use `release` instead of `default`.
