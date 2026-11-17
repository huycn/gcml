# Azure Repos provider and the Entra ID client. OpenSSL (libcrypto) signs the client assertions of service principals
# with certificates and reads .NET's PKCS#12 certificate store.
find_package(OpenSSL REQUIRED COMPONENTS Crypto)
gcm_object_library(gcmazrepos_obj "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(gcmazrepos_obj PUBLIC OpenSSL::Crypto)
add_library(gcmazrepos STATIC $<TARGET_OBJECTS:gcmazrepos_obj>)
target_include_directories(gcmazrepos PUBLIC "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(gcmazrepos PUBLIC gcmcore OpenSSL::Crypto)
