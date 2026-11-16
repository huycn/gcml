gcm_object_library(gcmgithub_obj "${CMAKE_CURRENT_SOURCE_DIR}")
add_library(gcmgithub STATIC $<TARGET_OBJECTS:gcmgithub_obj>)
target_include_directories(gcmgithub PUBLIC "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(gcmgithub PUBLIC gcmcore)
