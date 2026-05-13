# cmake/btstack.cmake
#
# Build BTstack as a static library, mirroring the source list of
# vendor/btstack/port/windows-winusb/CMakeLists.txt but WITHOUT the example
# executables and the Python3 GATT generator (we don't need any of that for
# our use case).
#
# License posture: BTstack is "personal/non-commercial free". The user has
# cloned vendor/btstack/ themselves into the working tree (it is NOT a
# submodule of our repo — see HANDOFF.md §4.2). We just compile against it
# in-place. Do NOT redistribute a built binary.

set(BTSTACK_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/vendor/btstack)
set(BTSTACK_PORT_DIR ${BTSTACK_ROOT}/port/windows-winusb)
set(BTSTACK_CONFIG_DIR ${CMAKE_CURRENT_SOURCE_DIR}/cmake/btstack_config)

if(NOT EXISTS ${BTSTACK_ROOT}/src/btstack.h)
    message(FATAL_ERROR
        "BTstack sources not found at ${BTSTACK_ROOT}.\n"
        "Per HANDOFF.md §4.2 this repo does NOT bundle BTstack. Run:\n"
        "  git clone https://github.com/bluekitchen/btstack.git vendor/btstack")
endif()

# Mirror the GLOB sets from the windows-winusb port's CMakeLists.txt.
file(GLOB BTSTACK_SRC          "${BTSTACK_ROOT}/src/*.c")
file(GLOB BTSTACK_CLASSIC      "${BTSTACK_ROOT}/src/classic/*.c")
file(GLOB BTSTACK_BLE          "${BTSTACK_ROOT}/src/ble/*.c")
file(GLOB BTSTACK_GATT         "${BTSTACK_ROOT}/src/ble/gatt-service/*.c")
file(GLOB BTSTACK_WINDOWS      "${BTSTACK_ROOT}/platform/windows/*.c")
file(GLOB BTSTACK_BLUEDROID    "${BTSTACK_ROOT}/3rd-party/bluedroid/encoder/srce/*.c"
                               "${BTSTACK_ROOT}/3rd-party/bluedroid/decoder/srce/*.c")
file(GLOB BTSTACK_HXCMOD       "${BTSTACK_ROOT}/3rd-party/hxcmod-player/*.c"
                               "${BTSTACK_ROOT}/3rd-party/hxcmod-player/mods/*.c")
file(GLOB BTSTACK_MD5          "${BTSTACK_ROOT}/3rd-party/md5/md5.c")
file(GLOB BTSTACK_RIJNDAEL     "${BTSTACK_ROOT}/3rd-party/rijndael/rijndael.c")
file(GLOB BTSTACK_UECC         "${BTSTACK_ROOT}/3rd-party/micro-ecc/uECC.c")
file(GLOB BTSTACK_YXML         "${BTSTACK_ROOT}/3rd-party/yxml/yxml.c")
file(GLOB BTSTACK_ZEPHYR       "${BTSTACK_ROOT}/chipset/zephyr/*.c")
file(GLOB BTSTACK_REALTEK      "${BTSTACK_ROOT}/chipset/realtek/*.c")

# The TLV-based LE device DB replaces the memory-only one; the windows port
# removes this file. Mirror that.
list(REMOVE_ITEM BTSTACK_BLE   "${BTSTACK_ROOT}/src/ble/le_device_db_memory.c")

add_library(btstack STATIC
    ${BTSTACK_SRC}
    ${BTSTACK_CLASSIC}
    ${BTSTACK_BLE}
    ${BTSTACK_GATT}
    ${BTSTACK_WINDOWS}
    ${BTSTACK_BLUEDROID}
    ${BTSTACK_HXCMOD}
    ${BTSTACK_MD5}
    ${BTSTACK_RIJNDAEL}
    ${BTSTACK_UECC}
    ${BTSTACK_YXML}
    ${BTSTACK_ZEPHYR}
    ${BTSTACK_REALTEK}
)

target_include_directories(btstack PUBLIC
    ${BTSTACK_ROOT}/src
    ${BTSTACK_ROOT}/platform/windows
    ${BTSTACK_ROOT}/platform/posix
    ${BTSTACK_ROOT}/platform/embedded
    ${BTSTACK_ROOT}/chipset/zephyr
    ${BTSTACK_ROOT}/chipset/realtek
    ${BTSTACK_ROOT}/3rd-party/bluedroid/encoder/include
    ${BTSTACK_ROOT}/3rd-party/bluedroid/decoder/include
    ${BTSTACK_ROOT}/3rd-party/micro-ecc
    ${BTSTACK_ROOT}/3rd-party/md5
    ${BTSTACK_ROOT}/3rd-party/rijndael
    ${BTSTACK_ROOT}/3rd-party/yxml
    ${BTSTACK_ROOT}/3rd-party/hxcmod-player
    ${BTSTACK_ROOT}/3rd-party/hxcmod-player/mod
    ${BTSTACK_ROOT}/3rd-party/lc3-google/include
    # btstack_config.h: use the windows-winusb port's config verbatim
    ${BTSTACK_PORT_DIR}
)

# Windows system libs for the WinUSB HCI transport (no external libusb needed).
target_link_libraries(btstack PUBLIC setupapi winusb)

if(MSVC)
    target_compile_options(btstack PRIVATE
        # BTstack triggers many benign warnings under MSVC /W3
        /wd4244 /wd4267 /wd4146 /wd4018 /wd4101 /wd4133
        /wd4005 /wd4334 /wd4324 /wd4116 /wd4127 /wd4477
    )
    # BTstack uses POSIX-style identifiers in places
    target_compile_definitions(btstack PUBLIC
        _CRT_SECURE_NO_WARNINGS
        _WINSOCK_DEPRECATED_NO_WARNINGS
        WIN32_LEAN_AND_MEAN
        NOMINMAX
    )
endif()
