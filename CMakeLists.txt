cmake_minimum_required(VERSION 3.16)
project(nexodion VERSION 1.0.0 LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Strict warnings
if(MSVC)
    add_compile_options(/W4 /WX)
else()
    add_compile_options(-Wall -Wextra -Wpedantic)
endif()

# Include directories
include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/external
)

# Core library
add_library(nexodion_core STATIC
    src/daemon_client.cpp
    src/game_metadata.cpp
    src/param_sfo.cpp
    src/package_identity.cpp
    src/file_transport.cpp
    src/scanner.cpp
    src/index_cache.cpp
    src/input.cpp
    src/views.cpp
    src/ui_shell.cpp
    src/installer.cpp
    src/inspector.cpp
    src/startup_diagnostics.cpp
    src/source_manager.cpp
    src/source_workflow.cpp
    src/shadowmount_api.cpp
    src/native_services.cpp
    src/game_workflow.cpp
    src/navigation.cpp
    src/native_painter.cpp
    src/library_workflow.cpp
    src/library_state.cpp
    src/event_log.cpp
    src/recovery_workflow.cpp
)

target_include_directories(nexodion_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/external
)

find_package(Threads REQUIRED)
target_link_libraries(nexodion_core PUBLIC
    Threads::Threads
)

# Native network file access: vendored libsmb2/libnfs (static). Builds are
# optional so the tree stays buildable when the vendored trees are absent.
option(NEXODION_ENABLE_SMB "Build the native SMB transport (vendored libsmb2)" ON)
option(NEXODION_ENABLE_NFS "Build the native NFS transport (vendored libnfs)" ON)
if(NEXODION_ENABLE_SMB AND EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/external/libsmb2/CMakeLists.txt)
    set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
    set(ENABLE_TESTS OFF CACHE BOOL "" FORCE)
    set(ENABLE_EXAMPLES OFF CACHE BOOL "" FORCE)
    add_subdirectory(external/libsmb2 ${CMAKE_CURRENT_BINARY_DIR}/libsmb2 EXCLUDE_FROM_ALL)
    add_library(nexodion_smb_transport STATIC src/file_transport_smb.cpp)
    target_include_directories(nexodion_smb_transport PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/external/libsmb2/include)
    target_link_libraries(nexodion_smb_transport PUBLIC nexodion_core smb2)
    target_link_libraries(nexodion_core INTERFACE nexodion_smb_transport)
endif()
if(NEXODION_ENABLE_NFS AND EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/external/libnfs/CMakeLists.txt)
    set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
    set(ENABLE_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(ENABLE_TESTS OFF CACHE BOOL "" FORCE)
    set(ENABLE_UTILS OFF CACHE BOOL "" FORCE)
    add_subdirectory(external/libnfs ${CMAKE_CURRENT_BINARY_DIR}/libnfs EXCLUDE_FROM_ALL)
    add_library(nexodion_nfs_transport STATIC src/file_transport_nfs.cpp)
    target_include_directories(nexodion_nfs_transport PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/external/libnfs/include)
    target_link_libraries(nexodion_nfs_transport PUBLIC nexodion_core nfs)
    target_link_libraries(nexodion_core INTERFACE nexodion_nfs_transport)
endif()

# Dedicated PS5 Application & Payload Target
option(NEXODION_NATIVE_UI "Build the SDL native display and controller backend" ${PS5})
if(NEXODION_NATIVE_UI)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(NATIVE_SDL REQUIRED IMPORTED_TARGET sdl2 SDL2_image SDL2_gfx)
    add_library(nexodion_runtime STATIC src/native_runtime_sdl.cpp)
    target_link_libraries(nexodion_runtime PUBLIC nexodion_core PkgConfig::NATIVE_SDL)
else()
    add_library(nexodion_runtime STATIC src/native_runtime_stub.cpp)
    target_link_libraries(nexodion_runtime PUBLIC nexodion_core)
endif()
if(PROSPERO OR PS5)
    # Core sources contain PS5-specific installer/diagnostic paths as well as main.
    target_compile_definitions(nexodion_core PUBLIC __PROSPERO__=1 __PS5__=1)
endif()
add_executable(nexodion_elf src/main.cpp)
set_target_properties(nexodion_elf PROPERTIES 
    OUTPUT_NAME "nexodion.elf"
)
target_link_libraries(nexodion_elf PRIVATE 
    nexodion_runtime
    Threads::Threads
)

# PS5 SDK stubs when cross-compiling via prospero.cmake / pacbrew
if(PROSPERO OR PS5)
    target_compile_definitions(nexodion_elf PRIVATE __PROSPERO__=1 __PS5__=1)
    target_link_libraries(nexodion_elf PRIVATE
        -lkernel
        -lSceSystemService
        -lSceNotification
        -lSceNet
        -lSceUserService
    )
    add_executable(nexodion_pkg_installer external/ps5-react/nexodion_pkg_installer.c)
    set_target_properties(nexodion_pkg_installer PROPERTIES OUTPUT_NAME "nexodion_pkg_installer.elf")
    target_link_libraries(nexodion_pkg_installer PRIVATE -lkernel -lSceUserService)
    add_executable(nexodion_launcher external/ps5-react/nexodion_launcher.c)
    set_target_properties(nexodion_launcher PROPERTIES OUTPUT_NAME "nexodion_launcher.elf")
    target_link_libraries(nexodion_launcher PRIVATE -lkernel -lSceUserService -lSceSystemService)
    add_dependencies(nexodion_elf nexodion_pkg_installer nexodion_launcher)
endif()

# Testing
enable_testing()
if(NOT CMAKE_CROSSCOMPILING)
    add_executable(native_services_test tests/native_services_test.cpp)
    target_link_libraries(native_services_test PRIVATE nexodion_core)
    add_test(NAME native_services_test COMMAND native_services_test)
endif()
add_executable(source_workflow_test tests/source_workflow_test.cpp)
target_link_libraries(source_workflow_test PRIVATE nexodion_core)
add_test(NAME source_workflow_test COMMAND source_workflow_test)

# Milestone 6 Tests: native network file access
add_executable(file_transport_test tests/file_transport_test.cpp)
target_link_libraries(file_transport_test PRIVATE nexodion_core)
add_test(NAME file_transport_test COMMAND file_transport_test)

# Milestone 1 Tests
add_executable(daemon_client_test tests/daemon_client_test.cpp)
target_link_libraries(daemon_client_test PRIVATE nexodion_core Threads::Threads)
add_test(NAME daemon_client_test COMMAND daemon_client_test)

# Milestone 2 Tests
add_executable(scanner_cache_test tests/scanner_cache_test.cpp)
target_link_libraries(scanner_cache_test PRIVATE nexodion_core Threads::Threads)
add_test(NAME scanner_cache_test COMMAND scanner_cache_test)

# Milestone 3 Tests
add_executable(ui_shell_test tests/ui_shell_test.cpp)
target_link_libraries(ui_shell_test PRIVATE nexodion_core Threads::Threads)
add_test(NAME ui_shell_test COMMAND ui_shell_test)

# Milestone 4 Tests
add_executable(inspector_installer_test tests/inspector_installer_test.cpp)
target_link_libraries(inspector_installer_test PRIVATE nexodion_core Threads::Threads)
add_test(NAME inspector_installer_test COMMAND inspector_installer_test)

# Milestone 4 Extensions Tests
add_executable(ps5_installer_persistence_test tests/ps5_installer_persistence_test.cpp)
target_link_libraries(ps5_installer_persistence_test PRIVATE nexodion_core Threads::Threads)
add_test(NAME ps5_installer_persistence_test COMMAND ps5_installer_persistence_test)

add_executable(install_retry_preflight_history_test tests/install_retry_preflight_history_test.cpp)
target_link_libraries(install_retry_preflight_history_test PRIVATE nexodion_core Threads::Threads)
add_test(NAME install_retry_preflight_history_test COMMAND install_retry_preflight_history_test)

# Startup Diagnostics Tests
add_executable(startup_diagnostics_test tests/startup_diagnostics_test.cpp)
target_link_libraries(startup_diagnostics_test PRIVATE nexodion_core Threads::Threads)
add_test(NAME startup_diagnostics_test COMMAND startup_diagnostics_test)

# Milestone 1 (Nexodion foundation) Tests
add_executable(foundation_test tests/foundation_test.cpp)
target_link_libraries(foundation_test PRIVATE nexodion_core Threads::Threads)
add_test(NAME foundation_test COMMAND foundation_test)

add_executable(native_phase2_test tests/native_phase2_test.cpp)
target_link_libraries(native_phase2_test PRIVATE nexodion_core)
add_test(NAME native_phase2_test COMMAND native_phase2_test)
if(NEXODION_NATIVE_UI AND NOT CMAKE_CROSSCOMPILING)
    add_executable(native_runtime_test tests/native_runtime_test.cpp)
    target_link_libraries(native_runtime_test PRIVATE nexodion_runtime)
    add_test(NAME native_runtime_test COMMAND native_runtime_test)
    set_tests_properties(native_runtime_test PROPERTIES ENVIRONMENT "SDL_VIDEODRIVER=dummy;SDL_AUDIODRIVER=dummy")
endif()

add_executable(package_identity_test tests/package_identity_test.cpp)
target_link_libraries(package_identity_test PRIVATE nexodion_core)
add_test(NAME package_identity_test COMMAND package_identity_test)

add_executable(library_features_test tests/library_features_test.cpp)
target_link_libraries(library_features_test PRIVATE nexodion_core Threads::Threads)
add_test(NAME library_features_test COMMAND library_features_test)

add_executable(recovery_features_test tests/recovery_features_test.cpp)
target_link_libraries(recovery_features_test PRIVATE nexodion_core Threads::Threads)
add_test(NAME recovery_features_test COMMAND recovery_features_test)
