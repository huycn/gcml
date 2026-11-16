gcm_object_library(gcmgitlab_obj "${CMAKE_CURRENT_SOURCE_DIR}")
add_library(gcmgitlab STATIC $<TARGET_OBJECTS:gcmgitlab_obj>)
target_include_directories(gcmgitlab PUBLIC "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(gcmgitlab PUBLIC gcmcore)
