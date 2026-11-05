# Native daemon DataChannels (ADR 0025). Data only, pinned source, static linkage.
find_package(OpenSSL 3.0 REQUIRED COMPONENTS Crypto SSL)

include_guard(GLOBAL)
include(FetchContent)

# libdatachannel 0.24.6 (MPL-2.0) with usrsctp (BSD-3-Clause) and plog (MIT). Media and
# libSRTP are left out: KetPlus needs data channels only. The ICE backend is libjuice (MPL-2.0),
# which reaches TURN over UDP only.
set(NO_WEBSOCKET ON CACHE BOOL "" FORCE)
set(NO_MEDIA ON CACHE BOOL "" FORCE)
set(NO_EXAMPLES ON CACHE BOOL "" FORCE)
set(NO_TESTS ON CACHE BOOL "" FORCE)
set(USE_NICE OFF CACHE BOOL "" FORCE)
# libjuice 1.7.4, which libdatachannel 0.24.6 pins, answers ICE checks carrying a zero tiebreaker
# with 400 Bad Request; libwebrtc sends exactly that from its TURN-over-TCP ports, so an iOS
# client relayed over TCP never connects. libjuice cb52376 is 1.7.4 plus only that fix (#356).
# Drop the checkout once a libdatachannel release pins libjuice 1.7.5 or later.
find_package(Git REQUIRED)
FetchContent_Declare(ketplus_libdatachannel
    GIT_REPOSITORY https://github.com/paullouisageneau/libdatachannel.git
    GIT_TAG 6b1e2e620f1e37f0eafeee702eaea0043cb305fd # v0.24.6
    GIT_SUBMODULES deps/libjuice deps/usrsctp deps/plog
    GIT_SUBMODULES_RECURSE OFF
    PATCH_COMMAND "${GIT_EXECUTABLE}" -C deps/libjuice fetch --quiet origin
                  cb523763255f241442da1fbdd72c498f33a54b35
        COMMAND "${GIT_EXECUTABLE}" -C deps/libjuice checkout --quiet
                cb523763255f241442da1fbdd72c498f33a54b35
)
FetchContent_MakeAvailable(ketplus_libdatachannel)


# Product installers ship the linked daemon, not upstream development libraries or headers.
set_property(DIRECTORY "${ketplus_libdatachannel_SOURCE_DIR}" PROPERTY EXCLUDE_FROM_ALL TRUE)
