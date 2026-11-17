find_package(Qt6 6.5 REQUIRED COMPONENTS Widgets Svg)

# The dialogs and commands as a library (linked by the helper and by tests/ui), main.cpp as the executable.
file(GLOB ui_sources CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp" "${CMAKE_CURRENT_SOURCE_DIR}/*.h")
list(REMOVE_ITEM ui_sources "${CMAKE_CURRENT_SOURCE_DIR}/main.cpp")
add_library(gcmui STATIC ${ui_sources})
target_include_directories(gcmui PUBLIC "${PROJECT_SOURCE_DIR}/src" "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(gcmui PUBLIC gcmcore Qt6::Widgets Qt6::Svg)

# Images as :/gcm/<file>. For a static library Qt builds the resources as an object library that is propagated
# to the executables, so they are not dropped by the linker.
file(GLOB ui_assets RELATIVE "${CMAKE_CURRENT_SOURCE_DIR}" CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/assets/*")
qt_add_resources(gcmui "gcmui_assets" PREFIX "/gcm" BASE assets FILES ${ui_assets})

add_executable(git-credential-manager-ui main.cpp)
target_link_libraries(git-credential-manager-ui PRIVATE gcmui)
install(TARGETS git-credential-manager-ui RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
