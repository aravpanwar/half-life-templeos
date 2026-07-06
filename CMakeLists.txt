cmake_minimum_required(VERSION 3.15)
project(templeos_hl1 C CXX)

# GoldSrc client.dll is 32-bit. Configure with:  -A Win32
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
    message(WARNING "GoldSrc requires a 32-bit build. Configure with -A Win32.")
endif()

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 14)

if(MSVC)
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
endif()

# ---- standalone pipeline probe (no HL, no MinHook needed) ---------------
add_executable(rfb_probe tools/rfb_probe.c src/rfb/rfb_client.c)
target_include_directories(rfb_probe PRIVATE src/rfb)
target_link_libraries(rfb_probe PRIVATE ws2_32)

# ---- MinHook (git submodule under third_party/minhook) ------------------
# git submodule add https://github.com/TsudaKageyu/minhook third_party/minhook
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/third_party/minhook/CMakeLists.txt)
    add_subdirectory(third_party/minhook)

    # ---- core mod sources ------------------------------------------------
    set(TOSHL_SRC
        src/rfb/rfb_client.c
        src/glhook/gl_hook.c
        src/vmproc/vm_launcher.c
        src/client/terminal_mode.cpp
    )

    # The full mod builds INTO the HL SDK's client library. Standalone here
    # we expose the orchestrator as a static lib you link against cl_dll.
    add_library(toshl_core STATIC ${TOSHL_SRC})
    target_include_directories(toshl_core PUBLIC
        src/rfb src/glhook src/vmproc src/client
        third_party/minhook/include
    )
    target_link_libraries(toshl_core PUBLIC minhook ws2_32 opengl32)
else()
    message(STATUS "MinHook submodule missing; building rfb_probe only. "
                   "Run: git submodule update --init")
endif()

# NOTE: to produce the final client.dll, add the Half-Life SDK's cl_dll
# sources to a SHARED target and link toshl_core into it, then call the
# TOSHL_* entry points from the SDK WIRING splice points documented in
# src/client/terminal_mode.cpp.
