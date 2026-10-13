set(EXAMPLE_FONT "${CMAKE_SOURCE_DIR}/assets/MapleMono-NF-CN-Regular.ttf")
set(EXAMPLE2_FONT "${CMAKE_SOURCE_DIR}/assets/ProggyForever-Regular.ttf")

add_executable(ReZeroFontPacker ReZeroFontPacker.cpp tigr.cpp)
target_link_libraries(ReZeroFontPacker PRIVATE ReZeroTrueType)
target_compile_definitions(ReZeroFontPacker
  PRIVATE REZERO_EXAMPLE_FONT="${EXAMPLE2_FONT}")
target_compile_features(ReZeroFontPacker PRIVATE cxx_std_23)
 
if(MSVC)
    target_link_libraries(ReZeroFontPacker PRIVATE opengl32 gdi32)
endif()

add_executable(ReZeroRenderText ReZeroRenderText.cpp tigr.cpp)
target_link_libraries(ReZeroRenderText PRIVATE ReZeroTrueType)
target_compile_definitions(ReZeroRenderText
  PRIVATE REZERO_EXAMPLE_FONT="${EXAMPLE_FONT}")
target_compile_features(ReZeroRenderText PRIVATE cxx_std_23)
if(MSVC)
    target_link_libraries(ReZeroRenderText PRIVATE opengl32 gdi32)
endif()


if(MSVC)
    set_source_files_properties(tigr.cpp PROPERTIES 
        COMPILE_OPTIONS "/wd4244;/wd4267;/wd4310;/wd4611"
    )
endif()