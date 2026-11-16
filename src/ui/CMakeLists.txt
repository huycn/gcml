find_package(Qt6 6.5 REQUIRED COMPONENTS Widgets Svg)
file(GLOB_RECURSE ui_sources CONFIGURE_DEPENDS
     "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp" "${CMAKE_CURRENT_SOURCE_DIR}/*.h" "${CMAKE_CURRENT_SOURCE_DIR}/*.qrc")
add_executable(git-credential-manager-ui ${ui_sources})
target_include_directories(git-credential-manager-ui PRIVATE "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(git-credential-manager-ui PRIVATE gcmcore Qt6::Widgets Qt6::Svg)
install(TARGETS git-credential-manager-ui RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
