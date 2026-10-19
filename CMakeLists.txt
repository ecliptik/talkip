cmake_minimum_required(VERSION 3.10)
project(talkip VERSION 0.1.0 LANGUAGES C)

# 68000 only (512Ke / Plus), small code; the stack is a static library linked into each app.
set(CMAKE_C_STANDARD 11)
add_compile_options(-m68000 -Os -Wall -ffunction-sections)

add_library(talkip STATIC
    src/ddp.c
    src/macip.c
    src/ip.c
    src/udp.c
    src/dns.c
    src/tcp.c
    src/tcpshim.c
    src/fmt.c
    src/appkit.c
    src/url.c
    src/http.c
    src/html.c
)
# Ported apps (Geomys/Flynn/Shiori Lite) see only include/ (talkip.h, MacTCP.h): their own tcp.h and dns.h
# would collide with talkip's internal headers in src/.
target_include_directories(talkip PUBLIC include PRIVATE src)

# Single segment: the socket listener (in ddp.c) runs at interrupt time and must never be unloaded.
function(talkip_app target)
    add_application(${target} ${ARGN})
    target_link_libraries(${target} talkip)
    target_include_directories(${target} PRIVATE src)
    set_target_properties(${target} PROPERTIES LINK_FLAGS "-Wl,-gc-sections -Wl,--mac-single")
endfunction()

# The tools, only when talkip is the top-level project (Flynn/Geomys/Shiori add_subdirectory it for the library)
if(CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    talkip_app(TalkIPStatus src/status_main.c resources/status.r)
    talkip_app(TalkIPPing src/ping_main.c resources/ping.r)
    talkip_app(TalkIPLookup src/lookup_main.c resources/lookup.r)
    talkip_app(TalkIPFetch src/fetch_main.c resources/fetch.r)
    talkip_app(TalkIPWeb src/web_main.c resources/web.r)
endif()

# M8 prototype: talkip as a MacTCP-compatible .ipp driver, installed at boot by an INIT (flat code in the system heap)
if(CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    set(IPP_SYSZ 40000 CACHE STRING "system heap bytes the TalkIP Driver INIT asks for ('sysz')")
    add_executable(TalkIPDriver
        src/ipp/ipp.c
        src/ddp.c src/macip.c src/ip.c src/udp.c src/tcp.c src/tcpshim.c src/fmt.c)
    target_compile_definitions(TalkIPDriver PRIVATE TALKIP_RESIDENT)
    target_include_directories(TalkIPDriver PRIVATE include src)
    set_target_properties(TalkIPDriver PROPERTIES OUTPUT_NAME TalkIPDriver.flt
        LINK_FLAGS "-Wl,--mac-flat -Wl,-gc-sections")
    add_custom_command(
        OUTPUT TalkIPDriver.bin TalkIPDriver.dsk
        COMMAND ${REZ} -I${REZ_INCLUDE_PATH} -DSYSZ_BYTES=${IPP_SYSZ}
                ${CMAKE_CURRENT_SOURCE_DIR}/resources/ipp.r
                -o TalkIPDriver.bin --cc TalkIPDriver.dsk -t INIT -c tkip
        DEPENDS TalkIPDriver resources/ipp.r)
    add_custom_target(TalkIPDriver_INIT ALL DEPENDS TalkIPDriver.bin)
endif()

# AppleTalk On: an INIT that turns AppleTalk on at every startup, for Macs without a PRAM battery (flat code, no globals)
if(CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    add_executable(AppleTalkOn src/atalkon/atalkon.c)
    set_target_properties(AppleTalkOn PROPERTIES OUTPUT_NAME AppleTalkOn.flt
        LINK_FLAGS "-Wl,--mac-flat -Wl,-gc-sections")
    add_custom_command(
        OUTPUT AppleTalkOn.bin AppleTalkOn.dsk
        COMMAND ${REZ} -I${REZ_INCLUDE_PATH}
                ${CMAKE_CURRENT_SOURCE_DIR}/resources/atalkon.r
                -o AppleTalkOn.bin --cc AppleTalkOn.dsk -t INIT -c tkip
        DEPENDS AppleTalkOn resources/atalkon.r)
    add_custom_target(AppleTalkOn_INIT ALL DEPENDS AppleTalkOn.bin)
endif()

# M8: a plain MacTCP client (no talkip code) to check the .ipp driver
if(CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    add_application(IPPProbe tests/m8/ipp_probe.c)
    target_include_directories(IPPProbe PRIVATE include)
endif()
if(CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    add_application(M8Hello tests/m8/hello.c)
endif()
