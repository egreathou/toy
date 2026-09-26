cmake_minimum_required(VERSION 3.20)
project(toy CXX)

if(WIN32)
    add_compile_definitions(
		_WIN32_WINNT=0x0A00
		NOMINMAX
		WIN32_LEAN_AND_MEAN
	)
endif()

#if(WIN32)
    # Safely targets Windows 7, 8, 10, and 11 for standard Win32 APIs
#    add_compile_definitions(_WIN32_WINNT=0x0601)
#endif()

if(MSVC) # Windows 10 & 11
    add_compile_options(/utf-8)
endif()

set(CMAKE_CXX_STANDARD 20)

include(FetchContent)

# ── Fetch Asio first — before Crow ───────────────────────────────────────────
FetchContent_Declare(
    asio
    URL https://github.com/chriskohlhoff/asio/archive/refs/tags/asio-1-28-0.zip
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(asio)

# Point Crow at the fetched Asio headers directly
set(ASIO_INCLUDE_DIR ${asio_SOURCE_DIR}/asio/include CACHE STRING "" FORCE)

# ── OpenSSL root hint — ShiningLight prebuilt default install path ────────────
if(WIN32)
    set(OPENSSL_ROOT_DIR "C:/Program Files/OpenSSL-Win64"
        CACHE PATH "OpenSSL root directory (ShiningLight default)")
endif()

# ── OpenSSL — required for CROW TLS ──────────────────────────────────────────
find_package(OpenSSL REQUIRED)

# ── Crow ─────────────────────────────────────────────────────────────────────
set(CROW_USE_BOOST   OFF CACHE BOOL "" FORCE)
set(CROW_ENABLE_SSL  ON  CACHE BOOL "" FORCE)   # must be set before FetchContent_MakeAvailable

FetchContent_Declare(
    crow
    URL https://github.com/CrowCpp/Crow/archive/refs/tags/v1.3.0.zip
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(crow)

# ── nlohmann/json ─────────────────────────────────────────────────────────────
FetchContent_Declare(
    json
    URL https://github.com/nlohmann/json/archive/refs/tags/v3.11.3.zip
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(json)

# ── Core library — production (no static storage) ────────────────────────────
add_library(toy-core STATIC
    toy.cpp
    toy-storage.cpp
)

target_include_directories(toy-core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${ASIO_INCLUDE_DIR}
)

target_link_libraries(toy-core PUBLIC
    Crow::Crow
    nlohmann_json::nlohmann_json
    OpenSSL::SSL
    OpenSSL::Crypto
)

# ── Core library — test build (with static storage) ──────────────────────────
add_library(toy-core-test STATIC
    toy.cpp
    toy-storage.cpp
)

target_include_directories(toy-core-test PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${ASIO_INCLUDE_DIR}
)

target_link_libraries(toy-core-test PUBLIC
    Crow::Crow
    nlohmann_json::nlohmann_json
    OpenSSL::SSL
    OpenSSL::Crypto
)

# Apply TOY_STATIC_STORAGE to the test core library — affects toy.cpp compilation
target_compile_definitions(toy-core-test PRIVATE TOY_STATIC_STORAGE)

# ── Web service binary ───────────────────────────────────────────────────────
add_executable(toy-server
    toy-server.cpp
)

#if (MSVC) # enforce strict compiler settings for MSVC builds
#    target_compile_options(toy-server PRIVATE 
#        /W4          # Enables warning level 4 (Strict production code standard)
#        /permissive- # Enforces strict standards-conforming compiler behavior
#        /WX          # Treats all compiler warnings as errors
#    )
#endif()

target_link_libraries(toy-server PRIVATE toy-core)

# ── Copy OpenSSL DLLs next to toy-server on Windows ──────────────────────────
if(WIN32)
    # Dynamically extract the bin folder from your OpenSSL include directory
    get_filename_component(_OPENSSL_DIR "${OPENSSL_INCLUDE_DIR}" DIRECTORY)
    set(OPENSSL_BIN_DIR "${_OPENSSL_DIR}/bin")

    # Explicitly find which DLL pattern is actually present on your filesystem
    set(DLL_NAMES_TO_CHECK "libssl-3.dll" "libssl-3-x64.dll" "libssl.dll")
    set(FOUND_SSL_DLL "")
    set(FOUND_CRYPTO_DLL "")

    foreach(dll ${DLL_NAMES_TO_CHECK})
        if(EXISTS "${OPENSSL_BIN_DIR}/${dll}")
            set(FOUND_SSL_DLL "${OPENSSL_BIN_DIR}/${dll}")
            # Map the corresponding crypto file naming convention
            string(REPLACE "ssl" "crypto" crypto_dll ${dll})
            set(FOUND_CRYPTO_DLL "${OPENSSL_BIN_DIR}/${crypto_dll}")
            break()
        endif()
    endforeach()

    # Apply the copy command only if the exact target files were physically verified
    if(FOUND_SSL_DLL AND FOUND_CRYPTO_DLL)
        add_custom_command(TARGET toy-server POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${FOUND_SSL_DLL}"
                "${FOUND_CRYPTO_DLL}"
                "$<TARGET_FILE_DIR:toy-server>"
            COMMENT "Copying verified OpenSSL DLLs to output directory"
        )
    else()
        message(WARNING "Could not locate physical OpenSSL runtime DLLs inside ${OPENSSL_BIN_DIR}")
    endif()
endif()

# ── Test binary — links against test core ────────────────────────────────────
add_executable(toy-tests
    toy-tests.cpp
)
target_link_libraries(toy-tests PRIVATE toy-core-test)

# Platform specific
if(WIN32)
    target_link_libraries(toy-core       PUBLIC ws2_32 mswsock)
    target_link_libraries(toy-core-test  PUBLIC ws2_32 mswsock)
endif()

if(UNIX)
    target_link_libraries(toy-core       PUBLIC pthread)
    target_link_libraries(toy-core-test  PUBLIC pthread)
endif()

set(PWA_FILES
    "${CMAKE_CURRENT_SOURCE_DIR}/pwa/index.html"
    "${CMAKE_CURRENT_SOURCE_DIR}/pwa/app.js"
    "${CMAKE_CURRENT_SOURCE_DIR}/pwa/style.css"
    "${CMAKE_CURRENT_SOURCE_DIR}/pwa/manifest.json"
    "${CMAKE_CURRENT_SOURCE_DIR}/pwa/sw.js"
    "${CMAKE_CURRENT_SOURCE_DIR}/pwa/icon.png"
)

add_custom_command(
    OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/pwa_copied.stamp"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/pwa"
        "$<TARGET_FILE_DIR:toy-server>/www"
    COMMAND ${CMAKE_COMMAND} -E touch "${CMAKE_CURRENT_BINARY_DIR}/pwa_copied.stamp"
    DEPENDS ${PWA_FILES}
    COMMENT "Copying pwa/ -> www/ next to toy-server"
)
add_custom_target(copy_pwa_files DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/pwa_copied.stamp")
add_dependencies(toy-server copy_pwa_files)

# RE-ENABLE LATER IF NEEDED:
# add_custom_command(
#     OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/settings_seeded.stamp"
#     COMMAND ${CMAKE_COMMAND}
#         "-DSRC=${CMAKE_CURRENT_SOURCE_DIR}/settings.json"
#         "-DDEST_DIR=$<TARGET_FILE_DIR:toy-server>"
#         "-DDEST_NAME=settings.json"
#         -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/CopyIfMissing.cmake"
#     COMMAND ${CMAKE_COMMAND} -E touch "${CMAKE_CURRENT_BINARY_DIR}/settings_seeded.stamp"
#     DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/settings.json"
#     COMMENT "Seeding settings.json next to toy-server (only if not already present)"
# )
# add_custom_target(seed_settings DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/settings_seeded.stamp")
# add_dependencies(toy-server seed_settings)
