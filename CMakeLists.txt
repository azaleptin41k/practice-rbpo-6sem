cmake_minimum_required(VERSION 3.20)

if(NOT WIN32)
  message(FATAL_ERROR "RbpoTrayZad2 builds only on Windows (MIDL + service).")
endif()

project(RbpoTrayZad2 VERSION 1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ---------------------------------------------------------------------------
# Convert logo.png → src/logo.ico at configure time (Windows + PowerShell)
# ---------------------------------------------------------------------------
set(ICON_PNG "${CMAKE_SOURCE_DIR}/logo.png")
set(ICON_ICO "${CMAKE_SOURCE_DIR}/src/logo.ico")

if(EXISTS "${ICON_PNG}" AND NOT EXISTS "${ICON_ICO}")
  message(STATUS "Converting logo.png -> src/logo.ico ...")
  execute_process(
    COMMAND powershell -ExecutionPolicy Bypass
            -File "${CMAKE_SOURCE_DIR}/scripts/convert_icon.ps1"
            -InputFile "${ICON_PNG}"
            -OutputFile "${ICON_ICO}"
    RESULT_VARIABLE ICON_RESULT
  )
  if(NOT ICON_RESULT EQUAL 0)
    message(WARNING "Icon conversion failed (code ${ICON_RESULT}). "
                    "The app will use the default system icon.")
  endif()
endif()

# ---------------------------------------------------------------------------
# Checked-in RPC stubs
# ---------------------------------------------------------------------------
set(RPC_DIR "${CMAKE_SOURCE_DIR}/src/rpc")
set(RPC_HEADER "${RPC_DIR}/TrayRpc_h.h")
set(RPC_CLIENT "${RPC_DIR}/TrayRpc_c.c")
set(RPC_SERVER "${RPC_DIR}/TrayRpc_s.c")

# ---------------------------------------------------------------------------
# Tray GUI — rbpo-app.exe (как zad-1)
# ---------------------------------------------------------------------------
add_executable(rbpo-app WIN32
  src/main.cpp
  src/app.rc
  "${RPC_CLIENT}"
)

target_include_directories(rbpo-app PRIVATE
  ${CMAKE_SOURCE_DIR}/src
  "${RPC_DIR}"
  ${CMAKE_SOURCE_DIR}/include
)

target_compile_definitions(rbpo-app PRIVATE UNICODE _UNICODE)

target_compile_options(rbpo-app PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)

set_source_files_properties("${RPC_CLIENT}" PROPERTIES LANGUAGE C)

target_link_libraries(rbpo-app PRIVATE
  shell32
  user32
  gdi32
  advapi32
  rpcrt4
)

# ---------------------------------------------------------------------------
# Windows service — TraySvc.exe
# ---------------------------------------------------------------------------
add_executable(TraySvc
  src/service_main.cpp
  "${RPC_SERVER}"
)

target_include_directories(TraySvc PRIVATE
  "${RPC_DIR}"
  ${CMAKE_SOURCE_DIR}/include
)

target_compile_definitions(TraySvc PRIVATE UNICODE _UNICODE)

target_compile_options(TraySvc PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)

set_source_files_properties("${RPC_SERVER}" PROPERTIES LANGUAGE C)

target_link_libraries(TraySvc PRIVATE
  advapi32
  wtsapi32
  userenv
  rpcrt4
  user32
)

set_target_properties(TraySvc PROPERTIES WIN32_EXECUTABLE FALSE)

if(MSVC)
  set_property(TARGET rbpo-app PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
  set_property(TARGET TraySvc PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()
