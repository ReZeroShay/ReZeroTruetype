cmake_minimum_required(VERSION 3.21.0)
project(ReZeroTrueType
    VERSION 0.1.0
    DESCRIPTION "A lightweight TrueType font library for parsing, rasterization, and atlas packing"
    LANGUAGES CXX)

# ----------------------------------------------------------------------
# Options
# ----------------------------------------------------------------------

message(STATUS "Build the ReZeroTrueType examples ${REZEROTRUETYPE_BUILD_EXAMPLES}")

# ----------------------------------------------------------------------
# Compiler flags
# ----------------------------------------------------------------------

if (CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    set(REZEROTRUETYPE_NO_EXCEPTIONS -fno-exceptions -fno-unwind-tables)
    set(REZEROTRUETYPE_NO_RTTI -fno-rtti)
    set(REZEROTRUETYPE_WARNING_OPTIONS -Wall -Wextra)
elseif(MSVC OR CMAKE_CXX_SIMULATE_ID STREQUAL "MSVC")
    set(REZEROTRUETYPE_NO_EXCEPTIONS /EHs- /EHc- /D_HAS_EXCEPTIONS=0)
    set(REZEROTRUETYPE_NO_RTTI /GR-)
    set(REZEROTRUETYPE_WARNING_OPTIONS /W4 /utf-8)
endif()

# ----------------------------------------------------------------------
# Library
# ----------------------------------------------------------------------

add_library(ReZeroTrueType INTERFACE)
add_library(ReZero::ReZeroTrueType ALIAS ReZeroTrueType)

target_include_directories(ReZeroTrueType
    INTERFACE
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_compile_features(ReZeroTrueType INTERFACE cxx_std_23)

target_compile_options(ReZeroTrueType INTERFACE
    ${REZEROTRUETYPE_NO_EXCEPTIONS}
    ${REZEROTRUETYPE_NO_RTTI}
    ${REZEROTRUETYPE_WARNING_OPTIONS}
)

if(REZEROTRUETYPE_BUILD_EXAMPLES)
    include(CTest)
    add_subdirectory(examples)
endif()
