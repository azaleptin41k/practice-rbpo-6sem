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
# MIDL — RPC stubs 
# ---------------------------------------------------------------------------
find_program(MIDL_COMPILER midl)
if(NOT MIDL_COMPILER)
  file(GLOB _SDK_BIN_DIRS
    "C:/Program Files (x86)/Windows Kits/10/bin/10.*/x64"
    "C:/Program Files (x86)/Windows Kits/10/bin/x64"
  )
  find_program(MIDL_COMPILER midl PATHS ${_SDK_BIN_DIRS})
endif()
if(NOT MIDL_COMPILER)
  message(FATAL_ERROR "MIDL compiler not found. Install the Windows SDK.")
endif()

set(IDL_FILE "${CMAKE_SOURCE_DIR}/idl/TrayRpc.idl")
set(ACF_FILE "${CMAKE_SOURCE_DIR}/idl/TrayRpc.acf")
set(RPC_GEN_DIR "${CMAKE_BINARY_DIR}/rpc_gen")
file(MAKE_DIRECTORY "${RPC_GEN_DIR}")

set(RPC_HEADER "${RPC_GEN_DIR}/TrayRpc_h.h")
set(RPC_CLIENT "${RPC_GEN_DIR}/TrayRpc_c.c")
set(RPC_SERVER "${RPC_GEN_DIR}/TrayRpc_s.c")

add_custom_command(
  OUTPUT "${RPC_HEADER}" "${RPC_CLIENT}" "${RPC_SERVER}"
  COMMAND "${MIDL_COMPILER}"
    /acf "${ACF_FILE}"
    /nologo
    /win64
    /W1
    /h TrayRpc_h.h
    /cstub TrayRpc_c.c
    /sstub TrayRpc_s.c
    "${IDL_FILE}"
  DEPENDS "${IDL_FILE}" "${ACF_FILE}"
  WORKING_DIRECTORY "${RPC_GEN_DIR}"
  COMMENT "Running MIDL compiler on TrayRpc.idl"
)

add_custom_target(rpc_stubs DEPENDS "${RPC_HEADER}" "${RPC_CLIENT}" "${RPC_SERVER}")

# ---------------------------------------------------------------------------
# RPC stub libraries (build generated MIDL sources as pure C)
# ---------------------------------------------------------------------------
add_library(trayrpc_client_stub STATIC "${RPC_CLIENT}")
add_library(trayrpc_server_stub STATIC "${RPC_SERVER}")

add_dependencies(trayrpc_client_stub rpc_stubs)
add_dependencies(trayrpc_server_stub rpc_stubs)

target_include_directories(trayrpc_client_stub PRIVATE "${RPC_GEN_DIR}")
target_include_directories(trayrpc_server_stub PRIVATE "${RPC_GEN_DIR}")

set_source_files_properties("${RPC_CLIENT}" PROPERTIES
  GENERATED TRUE
  LANGUAGE C
  COMPILE_FLAGS "/TC"
)
set_source_files_properties("${RPC_SERVER}" PROPERTIES
  GENERATED TRUE
  LANGUAGE C
  COMPILE_FLAGS "/TC"
)

# ---------------------------------------------------------------------------
# Tray GUI — rbpo-app.exe (как zad-1)
# ---------------------------------------------------------------------------
add_executable(rbpo-app WIN32
  src/main.cpp
  src/app.rc
)

add_dependencies(rbpo-app rpc_stubs)

target_include_directories(rbpo-app PRIVATE
  ${CMAKE_SOURCE_DIR}/src
  ${CMAKE_SOURCE_DIR}/include
  "${RPC_GEN_DIR}"
)

target_compile_definitions(rbpo-app PRIVATE UNICODE _UNICODE)

target_compile_options(rbpo-app PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)

target_link_libraries(rbpo-app PRIVATE
  shell32
  user32
  gdi32
  advapi32
  rpcrt4
  trayrpc_client_stub
)

# ---------------------------------------------------------------------------
# Windows service — TraySvc.exe
# ---------------------------------------------------------------------------
add_executable(TraySvc
  src/service_main.cpp
)

add_dependencies(TraySvc rpc_stubs)

target_include_directories(TraySvc PRIVATE
  ${CMAKE_SOURCE_DIR}/include
  "${RPC_GEN_DIR}"
)

target_compile_definitions(TraySvc PRIVATE UNICODE _UNICODE)

target_compile_options(TraySvc PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)

target_link_libraries(TraySvc PRIVATE
  advapi32
  wtsapi32
  userenv
  rpcrt4
  user32
  trayrpc_server_stub
)

set_target_properties(TraySvc PROPERTIES WIN32_EXECUTABLE FALSE)

if(MSVC)
  set_property(TARGET rbpo-app PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
  set_property(TARGET TraySvc PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()
