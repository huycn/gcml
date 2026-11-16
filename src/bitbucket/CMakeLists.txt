gcm_object_library(gcmbitbucket_obj "${CMAKE_CURRENT_SOURCE_DIR}")
add_library(gcmbitbucket STATIC $<TARGET_OBJECTS:gcmbitbucket_obj>)
target_include_directories(gcmbitbucket PUBLIC "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(gcmbitbucket PUBLIC gcmcore)
