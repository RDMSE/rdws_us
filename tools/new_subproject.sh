#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 1 ]]; then
  echo "Uso: $0 <nome_subprojeto>"
  echo "Exemplo: $0 auth"
  exit 1
fi

NAME="$1"
ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SUB_DIR="${ROOT_DIR}/src/${NAME}"
TESTS_DIR="${SUB_DIR}/tests"
ROOT_CMAKE="${ROOT_DIR}/CMakeLists.txt"

if [[ ! "${NAME}" =~ ^[a-z][a-z0-9_]*$ ]]; then
  echo "Erro: nome invalido '${NAME}'. Use [a-z][a-z0-9_]*"
  exit 1
fi

if [[ -e "${SUB_DIR}" ]]; then
  echo "Erro: '${SUB_DIR}' ja existe."
  exit 1
fi

mkdir -p "${TESTS_DIR}"

cat > "${SUB_DIR}/CMakeLists.txt" <<EOF
# --- Biblioteca interna do subprojeto ---
add_library(${NAME}_core
    ${NAME}.cpp
)
target_include_directories(${NAME}_core PUBLIC \
    \${CMAKE_CURRENT_SOURCE_DIR}
)

# --- Executavel ---
add_executable(${NAME}_app main.cpp)
target_link_libraries(${NAME}_app PRIVATE ${NAME}_core)

# --- Testes unitarios ---
if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
EOF

cat > "${SUB_DIR}/${NAME}.hpp" <<EOF
#pragma once

#include <string>

namespace rdws_us::${NAME} {

std::string build_message(const std::string& input);

}  // namespace rdws_us::${NAME}
EOF

cat > "${SUB_DIR}/${NAME}.cpp" <<EOF
#include "${NAME}.hpp"

namespace rdws_us::${NAME} {

std::string build_message(const std::string& input) {
  if (input.empty()) {
    return "${NAME}://default";
  }

  return "${NAME}://" + input;
}

}  // namespace rdws_us::${NAME}
EOF

cat > "${SUB_DIR}/main.cpp" <<EOF
#include <iostream>

#include "${NAME}.hpp"

int main() {
  std::cout << rdws_us::${NAME}::build_message("health") << '\n';
  return 0;
}
EOF

cat > "${TESTS_DIR}/CMakeLists.txt" <<EOF
add_executable(${NAME}_test ${NAME}_test.cpp)
target_link_libraries(${NAME}_test PRIVATE ${NAME}_core)

if(TARGET GTest::gtest_main)
    target_link_libraries(${NAME}_test PRIVATE GTest::gtest_main)
elseif(TARGET gtest_main)
    target_link_libraries(${NAME}_test PRIVATE gtest_main)
else()
    message(FATAL_ERROR "GoogleTest target not found. Check FetchContent/find_package setup.")
endif()

include(GoogleTest)
gtest_discover_tests(${NAME}_test)
EOF

cat > "${TESTS_DIR}/${NAME}_test.cpp" <<EOF
#include <gtest/gtest.h>

#include "${NAME}.hpp"

TEST(${NAME^}Test, ReturnsDefaultWhenInputIsEmpty) {
  EXPECT_EQ(rdws_us::${NAME}::build_message(""), "${NAME}://default");
}

TEST(${NAME^}Test, PrefixesInputWithScheme) {
  EXPECT_EQ(rdws_us::${NAME}::build_message("users.csv"), "${NAME}://users.csv");
}
EOF

if ! grep -Fq "add_subdirectory(src/${NAME})" "${ROOT_CMAKE}"; then
  printf "\nadd_subdirectory(src/%s)\n" "${NAME}" >> "${ROOT_CMAKE}"
fi

echo "Subprojeto criado: src/${NAME}"
echo "Adicionado no root: add_subdirectory(src/${NAME})"

