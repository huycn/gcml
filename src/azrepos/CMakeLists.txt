gcm_object_library(gcmazrepos_obj "${CMAKE_CURRENT_SOURCE_DIR}")
add_library(gcmazrepos STATIC $<TARGET_OBJECTS:gcmazrepos_obj>)
target_include_directories(gcmazrepos PUBLIC "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(gcmazrepos PUBLIC gcmcore)
