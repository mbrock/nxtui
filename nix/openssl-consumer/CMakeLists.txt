cmake_minimum_required(VERSION 3.20)
project(uwp_crypto_consumer LANGUAGES C)

# No LIB_EAY/OPENSSL_CRYPTO_LIBRARY override: exercise FindOpenSSL's normal
# Windows search with GNU-syntax Clang and the installed archive aliases.
find_package(OpenSSL 3 REQUIRED COMPONENTS Crypto)
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/consumer.c"
  "#include <openssl/crypto.h>\nint main(void) { return OpenSSL_version_num() == 0; }\n")
add_executable(crypto-consumer "${CMAKE_CURRENT_BINARY_DIR}/consumer.c")
target_link_libraries(crypto-consumer PRIVATE OpenSSL::Crypto WindowsApp)
