# ADR 0019: in-app updates for the macOS Desktop through Sparkle 2.
#
# A build offers updates only when both the appcast URL and the matching EdDSA public key are
# configured. Otherwise Sparkle is neither downloaded nor bundled, and Settings → Updates says
# that this build is not configured. The private signing key never enters this repository or
# the build: release tooling signs archives outside CMake (packaging/macos/publish-update.sh).
set(KETPLUS_UPDATE_FEED_URL "" CACHE STRING
    "HTTPS appcast URL compiled into KetPlus.app; empty disables in-app updates")
set(KETPLUS_UPDATE_PUBLIC_ED_KEY "" CACHE STRING
    "Base64 Ed25519 public key that verifies update archives; required with the feed URL")
set(KETPLUS_SPARKLE_ARCHIVE "" CACHE FILEPATH
    "Local copy of the pinned Sparkle release archive, for offline builds")

set(ketplus_sparkle_version "2.10.0")
set(ketplus_sparkle_sha256
    "c2bf58aa8387266ac179357b1415d6f2635f044da8be41042af32425dae6da0c")

set(KETPLUS_UPDATES_ENABLED OFF)
if(KETPLUS_UPDATE_FEED_URL STREQUAL "" AND KETPLUS_UPDATE_PUBLIC_ED_KEY STREQUAL "")
    message(STATUS "KetPlus in-app updates: disabled (no KETPLUS_UPDATE_FEED_URL)")
    return()
endif()
if(KETPLUS_UPDATE_FEED_URL STREQUAL "" OR KETPLUS_UPDATE_PUBLIC_ED_KEY STREQUAL "")
    message(FATAL_ERROR
        "KETPLUS_UPDATE_FEED_URL and KETPLUS_UPDATE_PUBLIC_ED_KEY must be set together")
endif()
# Info.plist embeds the URL verbatim, so it must not need XML escaping.
if(NOT KETPLUS_UPDATE_FEED_URL MATCHES "^https://[A-Za-z0-9.-]+(:[0-9]+)?(/[A-Za-z0-9._~%/-]*)?$")
    message(FATAL_ERROR
        "KETPLUS_UPDATE_FEED_URL must be an https:// URL without a query; "
        "got '${KETPLUS_UPDATE_FEED_URL}'")
endif()
# An Ed25519 public key is 32 bytes: 43 base64 characters plus one padding character. CMake
# regular expressions have no {n} repetition, so the length is checked separately.
string(LENGTH "${KETPLUS_UPDATE_PUBLIC_ED_KEY}" ketplus_update_key_length)
if(NOT ketplus_update_key_length EQUAL 44
   OR NOT KETPLUS_UPDATE_PUBLIC_ED_KEY MATCHES "^[A-Za-z0-9+/]+=$")
    message(FATAL_ERROR
        "KETPLUS_UPDATE_PUBLIC_ED_KEY must be the base64 public key printed by Sparkle's "
        "generate_keys, never the private key")
endif()
if(NOT APPLE)
    message(STATUS "KetPlus in-app updates: not available on this platform")
    return()
endif()

include(FetchContent)
if(KETPLUS_SPARKLE_ARCHIVE)
    set(ketplus_sparkle_url "${KETPLUS_SPARKLE_ARCHIVE}")
else()
    set(ketplus_sparkle_url
        "https://github.com/sparkle-project/Sparkle/releases/download/${ketplus_sparkle_version}/Sparkle-${ketplus_sparkle_version}.tar.xz")
endif()
# The release archive has no CMake project; SOURCE_SUBDIR only keeps MakeAvailable from
# looking for one.
FetchContent_Declare(ketplus_sparkle
    URL "${ketplus_sparkle_url}"
    URL_HASH "SHA256=${ketplus_sparkle_sha256}"
    SOURCE_SUBDIR "no-cmake-project"
)
FetchContent_MakeAvailable(ketplus_sparkle)

set(KETPLUS_SPARKLE_DIR "${ketplus_sparkle_SOURCE_DIR}")
set(KETPLUS_SPARKLE_FRAMEWORK "${KETPLUS_SPARKLE_DIR}/Sparkle.framework")
if(NOT EXISTS "${KETPLUS_SPARKLE_FRAMEWORK}/Versions/B/Sparkle")
    message(FATAL_ERROR "Sparkle ${ketplus_sparkle_version} archive has no Sparkle.framework")
endif()

add_library(ketplus_sparkle INTERFACE)
target_compile_options(ketplus_sparkle INTERFACE "-F${KETPLUS_SPARKLE_DIR}")
target_link_options(ketplus_sparkle INTERFACE "-F${KETPLUS_SPARKLE_DIR}")
target_link_libraries(ketplus_sparkle INTERFACE "-framework Sparkle")

# Build-tree executables (including tests) load the unpacked framework; installation replaces
# this with the bundle's Contents/Frameworks.
list(APPEND CMAKE_BUILD_RPATH "${KETPLUS_SPARKLE_DIR}")

set(KETPLUS_UPDATES_ENABLED ON)
message(STATUS
    "KetPlus in-app updates: Sparkle ${ketplus_sparkle_version}, feed ${KETPLUS_UPDATE_FEED_URL}")
