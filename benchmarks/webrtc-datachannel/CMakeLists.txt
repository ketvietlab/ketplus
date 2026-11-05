# WebRTC DataChannel transport spike (ADR 0024). A standalone project: the product build
# neither includes nor links it, and nothing here ships.
#
#   cmake -S benchmarks/webrtc-datachannel -B build/webrtc-spike -DCMAKE_BUILD_TYPE=Release
#   cmake --build build/webrtc-spike && ctest --test-dir build/webrtc-spike --output-on-failure
#
# A second build (for example with KETPLUS_SPIKE_USE_NICE) may reuse a source checkout through
# FETCHCONTENT_SOURCE_DIR_KETPLUS_LIBDATACHANNEL; never a shared FETCHCONTENT_BASE_DIR, which
# would also share the dependency's build tree between the two configurations.
cmake_minimum_required(VERSION 3.24)
project(ketplus_webrtc_spike LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(OpenSSL 3.0 REQUIRED COMPONENTS Crypto SSL)

include(FetchContent)

# libdatachannel 0.24.6 (MPL-2.0) with usrsctp (BSD-3-Clause) and plog (MIT). Media and
# libSRTP are left out: KetPlus needs data channels only. The ICE backend is libjuice (MPL-2.0),
# which reaches TURN over UDP only; KETPLUS_SPIKE_USE_NICE switches to libnice (LGPL-2.1 or
# MPL-1.1, needs GLib and pkg-config) for TURN over TCP.
option(KETPLUS_SPIKE_USE_NICE "Use libnice instead of libjuice for ICE" OFF)
set(NO_MEDIA ON CACHE BOOL "" FORCE)
set(NO_EXAMPLES ON CACHE BOOL "" FORCE)
set(NO_TESTS ON CACHE BOOL "" FORCE)
set(USE_NICE ${KETPLUS_SPIKE_USE_NICE} CACHE BOOL "" FORCE)
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

add_executable(ketplus_webrtc_spike WebRtcSpike.cpp)
target_link_libraries(ketplus_webrtc_spike PRIVATE LibDataChannel::LibDataChannelStatic OpenSSL::Crypto)

enable_testing()
add_test(NAME webrtc_p2p COMMAND ketplus_webrtc_spike p2p --bulk-mib 64)
# Relay-only paths need a TURN server: KETPLUS_SPIKE_TURN_URL=turn:user:password@host:3478.
if(DEFINED ENV{KETPLUS_SPIKE_TURN_URL})
    add_test(NAME webrtc_turn_udp
        COMMAND ketplus_webrtc_spike relay "$ENV{KETPLUS_SPIKE_TURN_URL}" --bulk-mib 64)
    if(KETPLUS_SPIKE_USE_NICE)
        add_test(NAME webrtc_turn_tcp
            COMMAND ketplus_webrtc_spike relay "$ENV{KETPLUS_SPIKE_TURN_URL}?transport=tcp" --bulk-mib 64)
    else()
        # libjuice cannot reach a TURN server over TCP; the relay-only connection must not form.
        add_test(NAME webrtc_turn_tcp_unsupported
            COMMAND ketplus_webrtc_spike relay "$ENV{KETPLUS_SPIKE_TURN_URL}?transport=tcp"
                    --expect-no-connection --connect-seconds 8)
    endif()
endif()
