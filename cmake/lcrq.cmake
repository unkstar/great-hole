# lcrq (RaptorQ RFC 6330) build integration.
# Upstream is autotools-only (git.sr.ht/~librecast/lcrq) with no CMakeLists.txt.
# Notes:
#  - lcrq's build system does NOT support out-of-source builds -> BUILD_IN_SOURCE 1
#  - lcrq's configure does not recognize --enable-static (ignored, only builds .so)
#    -> we package the .o files into liblcrq.a with ar afterwards
# Recovered from ali deploy copy (never committed before 2026-08-05), then fixed
# for in-source build + static archive.
cmake_minimum_required(VERSION 3.10)
project(lcrq LANGUAGES C)

include(ExternalProject)

set(LCRQ_PREFIX ${CMAKE_CURRENT_BINARY_DIR})
set(LCRQ_INSTALL_DIR ${LCRQ_PREFIX}/install)
set(LCRQ_SRC ${CMAKE_CURRENT_SOURCE_DIR}/libs/lcrq)

# Create include dir early so INTERFACE_INCLUDE_DIRECTORIES resolves
file(MAKE_DIRECTORY ${LCRQ_INSTALL_DIR}/include)

ExternalProject_Add(lcrq_ext
    SOURCE_DIR ${LCRQ_SRC}
    PREFIX ${LCRQ_PREFIX}
    BUILD_IN_SOURCE 1
    CONFIGURE_COMMAND ${LCRQ_SRC}/configure
    BUILD_COMMAND ${CMAKE_MAKE_PROGRAM} -C src
    INSTALL_COMMAND ${CMAKE_COMMAND} -E make_directory ${LCRQ_INSTALL_DIR}/lib
    INSTALL_COMMAND ${CMAKE_AR} rcs ${LCRQ_INSTALL_DIR}/lib/liblcrq.a src/*.o
    BUILD_BYPRODUCTS ${LCRQ_INSTALL_DIR}/lib/liblcrq.a
)

add_library(lcrq STATIC IMPORTED GLOBAL)
set_target_properties(lcrq PROPERTIES IMPORTED_LOCATION ${LCRQ_INSTALL_DIR}/lib/liblcrq.a)
target_include_directories(lcrq INTERFACE ${LCRQ_INSTALL_DIR}/include)
add_dependencies(lcrq lcrq_ext)
