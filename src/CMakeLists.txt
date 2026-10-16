FILE(GLOB_RECURSE app_sources ${CMAKE_SOURCE_DIR}/src/*.cpp)
FILE(GLOB webp_sources ${CMAKE_SOURCE_DIR}/third_party/libwebp/src/dec/*.c
    ${CMAKE_SOURCE_DIR}/third_party/libwebp/src/dsp/*.c
    ${CMAKE_SOURCE_DIR}/third_party/libwebp/src/utils/*.c)
idf_component_register(SRCS ${app_sources} ${webp_sources} "../third_party/pngle/pngle.c"
    INCLUDE_DIRS "../include" PRIV_INCLUDE_DIRS "../third_party/libwebp")
set_source_files_properties(${webp_sources} PROPERTIES COMPILE_DEFINITIONS HAVE_CONFIG_H)
set_source_files_properties("../third_party/pngle/pngle.c" PROPERTIES COMPILE_DEFINITIONS PNGLE_NO_GAMMA_CORRECTION)
