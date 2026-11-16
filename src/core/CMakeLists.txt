# The core library is compiled as one OBJECT library per ownership area and merged into `gcmcore`.
file(GLOB core_headers CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.h" "${CMAKE_CURRENT_SOURCE_DIR}/auth/*.h")

set(core_parts base gitcfg app net stores)
set(core_objects)
foreach(part IN LISTS core_parts)
  file(GLOB_RECURSE part_sources CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/${part}/*.cpp")
  if(part_sources)
    gcm_object_library(gcmcore_${part} "${CMAKE_CURRENT_SOURCE_DIR}/${part}")
    list(APPEND core_objects $<TARGET_OBJECTS:gcmcore_${part}>)
  endif()
endforeach()
if(TARGET gcmcore_stores)
  target_link_libraries(gcmcore_stores PUBLIC PkgConfig::LIBSECRET)
endif()

add_library(gcmcore STATIC ${core_objects} ${core_headers})
target_include_directories(gcmcore PUBLIC "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(gcmcore PUBLIC Qt6::Core Qt6::Network PkgConfig::LIBSECRET)
