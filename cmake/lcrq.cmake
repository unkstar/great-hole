# lcrq (RaptorQ RFC 6330) build integration.
# Upstream is autotools-only (git.sr.ht/~librecast/lcrq) with no CMakeLists.txt,
# so we wrap configure+make via ExternalProject and expose an IMPORTED target.
# Recovered from ali:/root deployment copy (was never committed before 2026-08-05).
cmake_minimum_required(VERSION 3.10)
project(lcrq LANGUAGES C)

include(ExternalProject)

set(LCRQ_PREFIX ${CMAKE_CURRENT_BINARY_DIR})
set(LCRQ_INSTALL_DIR ${LCRQ_PREFIX}/install)

# Create include dir early so INTERFACE_INCLUDE_DIRECTORIES resolves
file(MAKE_DIRECTORY ${LCRQ_INSTALL_DIR}/include)

ExternalProject_Add(lcrq_ext
    SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/libs/lcrq
    PREFIX ${LCRQ_PREFIX}
    CONFIGURE_COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/libs/lcrq/configure --prefix=${LCRQ_INSTALL_DIR} --disable-shared --enable-static
    BUILD_COMMAND $(MAKE)
    INSTALL_COMMAND $(MAKE) install
    BUILD_BYPRODUCTS ${LCRQ_INSTALL_DIR}/lib/liblcrq.a
)

add_library(lcrq STATIC IMPORTED GLOBAL)
set_target_properties(lcrq PROPERTIES IMPORTED_LOCATION ${LCRQ_INSTALL_DIR}/lib/liblcrq.a)
target_include_directories(lcrq INTERFACE ${LCRQ_INSTALL_DIR}/include)
add_dependencies(lcrq lcrq_ext)
