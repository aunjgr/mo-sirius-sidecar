cmake_minimum_required(VERSION 3.20)

project(mo_sidecar LANGUAGES CXX)

find_package(Arrow REQUIRED CONFIG)
find_package(ArrowFlight REQUIRED CONFIG)
find_package(OpenSSL REQUIRED)

set(EXTENSION_SOURCES
    src/config.cpp
    src/native_result.cpp
    src/protocol.cpp
    src/stream_input.cpp
    src/tae_read_resolver.cpp
    src/flight_runtime.cpp
    src/mo_sidecar_extension.cpp)

build_static_extension(mo_sidecar ${EXTENSION_SOURCES})
build_loadable_extension(mo_sidecar " " ${EXTENSION_SOURCES})

foreach(_target mo_sidecar_extension mo_sidecar_loadable_extension)
  set_target_properties(
    ${_target}
    PROPERTIES CXX_STANDARD 20
               CXX_STANDARD_REQUIRED ON
               NO_SYSTEM_FROM_IMPORTED ON)
  target_compile_definitions(${_target} PRIVATE CPPHTTPLIB_OPENSSL_SUPPORT)
  target_include_directories(
    ${_target}
    PRIVATE include
            ${CMAKE_CURRENT_SOURCE_DIR}/../tae-scanner/include
            ${CMAKE_SOURCE_DIR}/third_party/httplib
            ${CMAKE_CURRENT_SOURCE_DIR}/../sirius/src/include
            ${CMAKE_CURRENT_SOURCE_DIR}/../sirius/substrait/src/include
            ${CMAKE_CURRENT_SOURCE_DIR}/../sirius/substrait/third_party
            ${CMAKE_CURRENT_SOURCE_DIR}/../sirius/substrait/third_party/substrait)
  target_link_libraries(
    ${_target}
    Arrow::arrow_shared
    ArrowFlight::arrow_flight_shared
    OpenSSL::SSL
    OpenSSL::Crypto
    sirius_extension)
  add_dependencies(${_target} duckdb_static)
endforeach()

install(
  TARGETS mo_sidecar_extension
  EXPORT "${DUCKDB_EXPORT_SET}"
  LIBRARY DESTINATION "${INSTALL_LIB_DIR}"
  ARCHIVE DESTINATION "${INSTALL_LIB_DIR}")

if(BUILD_UNITTESTS)
  add_executable(
    mo_sidecar_contract_unittest
    test/unittest.cpp
    test/test_protocol.cpp
    src/config.cpp
    src/protocol.cpp)
  target_include_directories(
    mo_sidecar_contract_unittest
    PRIVATE include
            ${CMAKE_CURRENT_SOURCE_DIR}/../sirius/src/include
            $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/third_party/catch>)
  target_link_libraries(mo_sidecar_contract_unittest OpenSSL::Crypto)
  set_target_properties(
    mo_sidecar_contract_unittest
    PROPERTIES CXX_STANDARD 20
               CXX_STANDARD_REQUIRED ON
               RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/test")

  add_executable(
    mo_sidecar_stream_unittest
    test/unittest.cpp
    test/test_stream_input.cpp
    src/native_result.cpp
    src/protocol.cpp
    src/stream_input.cpp)
  target_include_directories(
    mo_sidecar_stream_unittest
    PRIVATE include
            ${CMAKE_CURRENT_SOURCE_DIR}/../tae-scanner/include
            ${CMAKE_CURRENT_SOURCE_DIR}/../sirius/src/include
            ${CMAKE_CURRENT_SOURCE_DIR}/../sirius/substrait/src/include
            ${CMAKE_CURRENT_SOURCE_DIR}/../sirius/substrait/third_party
            ${CMAKE_CURRENT_SOURCE_DIR}/../sirius/substrait/third_party/substrait
            $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/third_party/catch>)
  target_link_libraries(
    mo_sidecar_stream_unittest
    Arrow::arrow_shared
    ArrowFlight::arrow_flight_shared
    OpenSSL::Crypto
    duckdb_static
    sirius_extension
    substrait_extension
    dummy_static_extension_loader)
  set_target_properties(
    mo_sidecar_stream_unittest
    PROPERTIES CXX_STANDARD 20
               CXX_STANDARD_REQUIRED ON
               RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/test")
endif()
