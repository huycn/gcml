cmake_minimum_required(VERSION 3.21)

file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" GCM_VERSION LIMIT_COUNT 1)
project(git-credential-manager VERSION ${GCM_VERSION} LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
  message(FATAL_ERROR "This port of Git Credential Manager supports Linux only.")
endif()

option(GCM_BUILD_TESTS "Build the unit tests" ON)
option(GCM_BUILD_UI "Build the graphical helper (git-credential-manager-ui)" ON)
set(gcm_all_providers github gitlab bitbucket azrepos)
set(GCM_PROVIDERS "${gcm_all_providers}" CACHE STRING
    "Host providers to build, a ;-separated list of github, gitlab, bitbucket, azrepos (the generic provider is always built)")
foreach(provider IN LISTS GCM_PROVIDERS)
  if(NOT provider IN_LIST gcm_all_providers)
    message(FATAL_ERROR "Unknown host provider '${provider}' in GCM_PROVIDERS (known: ${gcm_all_providers})")
  endif()
endforeach()

include(GNUInstallDirs)
find_package(Qt6 6.5 REQUIRED COMPONENTS Core Network)
find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBSECRET REQUIRED IMPORTED_TARGET libsecret-1)

# Informational version, like .NET's: "<version>+<commit>" when building from a Git checkout.
set(GCM_VERSION_INFORMATIONAL "${PROJECT_VERSION}")
find_program(GIT_EXECUTABLE git)
if(GIT_EXECUTABLE)
  execute_process(COMMAND "${GIT_EXECUTABLE}" rev-parse HEAD WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
                  OUTPUT_VARIABLE gcm_commit OUTPUT_STRIP_TRAILING_WHITESPACE RESULT_VARIABLE gcm_commit_result ERROR_QUIET)
  if(gcm_commit_result EQUAL 0 AND gcm_commit)
    set(GCM_VERSION_INFORMATIONAL "${PROJECT_VERSION}+${gcm_commit}")
  endif()
endif()

add_compile_options(-Wall -Wextra -Wpedantic)

# Release and MinSizeRel builds: link-time optimisation, unused functions and data removed, binaries stripped. Debug and
# RelWithDebInfo keep everything for debugging. Set CMAKE_INTERPROCEDURAL_OPTIMIZATION_<CONFIG>=OFF to build without
# link-time optimisation.
set(gcm_small_configs "$<CONFIG:Release,MinSizeRel>")
include(CheckIPOSupported)
check_ipo_supported(RESULT gcm_ipo_supported OUTPUT gcm_ipo_output LANGUAGES CXX)
foreach(config RELEASE MINSIZEREL)
  if(NOT DEFINED CMAKE_INTERPROCEDURAL_OPTIMIZATION_${config})
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_${config} ${gcm_ipo_supported})
  endif()
endforeach()
if(NOT gcm_ipo_supported)
  message(STATUS "Link-time optimisation not supported by this toolchain: ${gcm_ipo_output}")
endif()
add_compile_options("$<${gcm_small_configs}:-ffunction-sections;-fdata-sections>")
add_link_options("$<${gcm_small_configs}:-Wl,--gc-sections;-s>")
add_compile_definitions(QT_NO_CAST_FROM_ASCII QT_NO_CAST_TO_ASCII QT_NO_FOREACH QT_NO_KEYWORDS
                        GCM_VERSION_STRING="${PROJECT_VERSION}"
                        GCM_VERSION_INFORMATIONAL="${GCM_VERSION_INFORMATIONAL}")

# gcm_module(<target> <dir>...): a static library from every *.cpp/*.h found (recursively) in the given
# directories. Sources are globbed with CONFIGURE_DEPENDS so adding a file needs no CMake edit.
function(gcm_object_library target)
  set(globs)
  foreach(dir IN LISTS ARGN)
    list(APPEND globs "${dir}/*.cpp" "${dir}/*.h")
  endforeach()
  file(GLOB_RECURSE sources CONFIGURE_DEPENDS ${globs})
  add_library(${target} OBJECT ${sources})
  target_include_directories(${target} PUBLIC "${PROJECT_SOURCE_DIR}/src")
  target_link_libraries(${target} PUBLIC Qt6::Core Qt6::Network)
endfunction()

add_subdirectory(src/core)

# Modules are added once they have sources (the port is built up module by module).
function(gcm_add_module dir)
  file(GLOB_RECURSE module_sources CONFIGURE_DEPENDS "${PROJECT_SOURCE_DIR}/${dir}/*.cpp")
  if(module_sources)
    add_subdirectory(${dir})
  endif()
endfunction()
# The executable links and registers the providers whose library target exists (src/app/CMakeLists.txt), and the
# tests of a provider are skipped when its library is not built.
foreach(provider IN LISTS gcm_all_providers)
  if(provider IN_LIST GCM_PROVIDERS)
    gcm_add_module(src/${provider})
  endif()
endforeach()
gcm_add_module(src/app)
if(GCM_BUILD_UI)
  gcm_add_module(src/ui)
endif()

if(GCM_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
