cmake_minimum_required(VERSION 3.16)
project(cs2620-pset4 CXX)

option(SAN "Enable AddressSanitizer+UBSanitizer" OFF)
option(ASAN "Enable AddressSanitizer" OFF)
option(UBSAN "Enable UBSanitizer" OFF)
option(TSAN "Enable ThreadSanitizer" OFF)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/..
    ${CMAKE_CURRENT_SOURCE_DIR}/../bencode
    ${CMAKE_CURRENT_SOURCE_DIR}/../cotamer
)
add_compile_options(-g -O3 -W -Wall)
add_link_options(-g -O3 -W -Wall)

if((SAN AND NOT TSAN) OR UBSAN)
    add_compile_options(-fsanitize=undefined)
    add_link_options(-fsanitize=undefined)
endif()
if((SAN AND NOT TSAN) OR ASAN)
    add_compile_options(-fsanitize=address)
    add_link_options(-fsanitize=address)
endif()
if(TSAN)
    add_compile_options(-fsanitize=thread)
    add_link_options(-fsanitize=thread)
endif()

# from examples dir
# Generate config header
# Find xxhash
find_path(XXHASH_INCLUDE_DIR xxhash.h HINTS /opt/homebrew/include)
find_library(XXHASH_LIBRARY xxhash HINTS /opt/homebrew/lib)

if(NOT XXHASH_INCLUDE_DIR OR NOT XXHASH_LIBRARY)
    message(FATAL_ERROR "xxhash not found. Install with: brew install xxhash")
endif()

# Find llhttp
set(COTAMER_HAVE_LLHTTP 0)
if(NOT COTAMER_LLHTTP STREQUAL "OFF")
    find_path(LLHTTP_INCLUDE_DIR llhttp.h HINTS /opt/homebrew/include)
    find_library(LLHTTP_LIBRARY llhttp HINTS /opt/homebrew/lib)
    if(LLHTTP_INCLUDE_DIR AND LLHTTP_LIBRARY)
        set(COTAMER_HAVE_LLHTTP 1)
        message(STATUS "llhttp integration: enabled")
    elseif(COTAMER_LLHTTP STREQUAL "ON")
        message(FATAL_ERROR "COTAMER_LLHTTP=ON but llhttp was not found")
    else()
        message(STATUS "llhttp integration: disabled (llhttp not found)")
    endif()
else()
    message(STATUS "llhttp integration: disabled (COTAMER_LLHTTP=OFF)")
endif()

# Find libcurl
set(COTAMER_HAVE_CURL 0)
if(NOT COTAMER_CURL STREQUAL "OFF")
    find_package(CURL QUIET)
    if(CURL_FOUND)
        set(COTAMER_HAVE_CURL 1)
        message(STATUS "libcurl integration: enabled (${CURL_VERSION_STRING})")
    elseif(COTAMER_CURL STREQUAL "ON")
        message(FATAL_ERROR "COTAMER_CURL=ON but libcurl was not found")
    else()
        message(STATUS "libcurl integration: disabled (libcurl not found)")
    endif()
endif()

# Find nlohmann::json (header-only)
set(COTAMER_HAVE_NLOHMANN_JSON 0)
if(NOT COTAMER_NLOHMANN_JSON STREQUAL "OFF")
    find_package(nlohmann_json QUIET)
    if(nlohmann_json_FOUND)
        set(COTAMER_HAVE_NLOHMANN_JSON 1)
        message(STATUS "nlohmann::json integration: enabled (${nlohmann_json_VERSION})")
        # Propagate the include path globally so cotamer/http.hh's forward-
        # declaration of nlohmann::json compiles for every consumer.
        get_target_property(_njson_inc nlohmann_json::nlohmann_json
                            INTERFACE_INCLUDE_DIRECTORIES)
        if(_njson_inc)
            include_directories(${_njson_inc})
        endif()
    elseif(COTAMER_NLOHMANN_JSON STREQUAL "ON")
        message(FATAL_ERROR "COTAMER_NLOHMANN_JSON=ON but nlohmann_json was not found")
    else()
        message(STATUS "nlohmann::json integration: disabled (nlohmann_json not found)")
    endif()
endif()

# Find zlib
set(COTAMER_HAVE_ZLIB 0)
if(NOT COTAMER_ZLIB STREQUAL "OFF")
    find_package(ZLIB QUIET)
    if(ZLIB_FOUND)
        set(COTAMER_HAVE_ZLIB 1)
        message(STATUS "zlib integration: enabled (${ZLIB_VERSION_STRING})")
    elseif(COTAMER_ZLIB STREQUAL "ON")
        message(FATAL_ERROR "COTAMER_ZLIB=ON but zlib was not found")
    else()
        message(STATUS "zlib integration: disabled (zlib not found)")
    endif()
endif()

# Generate config header
configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/../cotamer/cotamer/config.hh.in
    ${CMAKE_CURRENT_BINARY_DIR}/cotamer/config.hh
    @ONLY
)
include_directories(${CMAKE_CURRENT_BINARY_DIR})

if(COTAMER_HAVE_LLHTTP)
    add_library(CotamerHttp OBJECT
        ../cotamer/cotamer/http.cc
    )
    target_include_directories(CotamerHttp PUBLIC ${LLHTTP_INCLUDE_DIR})
endif()



add_library(Cotamer OBJECT
    ../cotamer/cotamer/cotamer.cc
    ../cotamer/cotamer/io.cc
)

set_source_files_properties(
    ../bencode/bencode.c
    PROPERTIES
        LANGUAGE CXX
        COMPILE_OPTIONS "-x;c++"
)
add_library(Bencode OBJECT
    ../bencode/bencode.c
)
target_include_directories(Bencode PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/../bencode
)

add_library(Pancy OBJECT
    pancydb.cc
    client_model.cc
    lockseq_model.cc
    utils.cc
)

add_executable(pt-paxos
    pt-paxos.cc
    $<TARGET_OBJECTS:Cotamer>
    $<TARGET_OBJECTS:Pancy>
)

add_executable(server
    tracker.cc
    tracker/http.cc
    tracker/messages.cc
    tracker/state.cc
    $<TARGET_OBJECTS:Cotamer>
    $<TARGET_OBJECTS:CotamerHttp>
    $<TARGET_OBJECTS:Bencode>
)

add_executable(client
    tracker.cc
    tracker/http.cc
    tracker/messages.cc
    tracker/state.cc
    $<TARGET_OBJECTS:Cotamer>
    $<TARGET_OBJECTS:CotamerHttp>
    $<TARGET_OBJECTS:Bencode>
)

if(COTAMER_HAVE_LLHTTP)
    target_include_directories(server PRIVATE ${LLHTTP_INCLUDE_DIR})
    target_include_directories(client PRIVATE ${LLHTTP_INCLUDE_DIR})
    target_link_libraries(server PRIVATE ${LLHTTP_LIBRARY})
    target_link_libraries(client PRIVATE ${LLHTTP_LIBRARY})
endif()
