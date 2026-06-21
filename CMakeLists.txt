cmake_minimum_required(VERSION 3.10)

project(InstyLSP)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_BUILD_TYPE Release)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -ggdb -O0")

# Produce a self-contained Windows executable when cross-compiling: statically
# link the C++ runtime so insty-lsp.exe does not need libc++.dll/libunwind.dll.
if(CMAKE_CROSSCOMPILING AND WIN32)
    set(CMAKE_EXE_LINKER_FLAGS
        "${CMAKE_EXE_LINKER_FLAGS} -static -static-libgcc -static-libstdc++")
endif()

# The LSP server reuses the compiler's frontend (lexer, parser, shared
# utilities). It builds those sources directly from the Compiler project so
# there is a single source of truth. Point INSTY_COMPILER_DIR elsewhere if
# the Compiler checkout is not a sibling directory.
set(INSTY_COMPILER_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../Compiler" CACHE PATH
    "Path to the Insty Compiler project (provides frontend sources)")

if(NOT EXISTS "${INSTY_COMPILER_DIR}/include/lexer/lexer.hpp")
    message(FATAL_ERROR
        "Insty Compiler sources not found at '${INSTY_COMPILER_DIR}'. "
        "Set -DINSTY_COMPILER_DIR=<path> to the Compiler project directory.")
endif()

set(INSTY_FRONTEND_SRC "${INSTY_COMPILER_DIR}/src")

# Compiler frontend: lexer + parser + the LLVM-free subset of utilities.
# (linker.cpp and target.cpp are the only LLVM-dependent utilities and the
# LSP never references them, which keeps this project free of LLVM.)
add_library(InstyFrontend STATIC
    ${INSTY_FRONTEND_SRC}/lexer/lexer.cpp
    ${INSTY_FRONTEND_SRC}/lexer/stringify.cpp

    ${INSTY_FRONTEND_SRC}/parser/parser.cpp
    ${INSTY_FRONTEND_SRC}/parser/parserUtils.cpp
    ${INSTY_FRONTEND_SRC}/parser/symbolTable.cpp
    ${INSTY_FRONTEND_SRC}/parser/expressions/add.cpp
    ${INSTY_FRONTEND_SRC}/parser/expressions/args.cpp
    ${INSTY_FRONTEND_SRC}/parser/expressions/array.cpp
    ${INSTY_FRONTEND_SRC}/parser/expressions/assign.cpp
    ${INSTY_FRONTEND_SRC}/parser/expressions/call.cpp
    ${INSTY_FRONTEND_SRC}/parser/expressions/dsn.cpp
    ${INSTY_FRONTEND_SRC}/parser/expressions/expr.cpp
    ${INSTY_FRONTEND_SRC}/parser/expressions/logical.cpp
    ${INSTY_FRONTEND_SRC}/parser/expressions/member.cpp
    ${INSTY_FRONTEND_SRC}/parser/expressions/memberCall.cpp
    ${INSTY_FRONTEND_SRC}/parser/expressions/mul.cpp
    ${INSTY_FRONTEND_SRC}/parser/expressions/object.cpp
    ${INSTY_FRONTEND_SRC}/parser/expressions/primary.cpp
    ${INSTY_FRONTEND_SRC}/parser/expressions/shift.cpp
    ${INSTY_FRONTEND_SRC}/parser/expressions/unary.cpp
    ${INSTY_FRONTEND_SRC}/parser/expressions/literals.cpp
    ${INSTY_FRONTEND_SRC}/parser/expressions/operators.cpp

    ${INSTY_FRONTEND_SRC}/parser/statements/fn.cpp
    ${INSTY_FRONTEND_SRC}/parser/statements/if.cpp
    ${INSTY_FRONTEND_SRC}/parser/statements/stmt.cpp
    ${INSTY_FRONTEND_SRC}/parser/statements/struct.cpp
    ${INSTY_FRONTEND_SRC}/parser/statements/class.cpp
    ${INSTY_FRONTEND_SRC}/parser/statements/enum.cpp
    ${INSTY_FRONTEND_SRC}/parser/statements/compileTimeIf.cpp
    ${INSTY_FRONTEND_SRC}/parser/statements/vars-fn.cpp
    ${INSTY_FRONTEND_SRC}/parser/statements/when.cpp
    ${INSTY_FRONTEND_SRC}/parser/statements/switch.cpp
    ${INSTY_FRONTEND_SRC}/parser/statements/while.cpp
    ${INSTY_FRONTEND_SRC}/parser/statements/loop.cpp
    ${INSTY_FRONTEND_SRC}/parser/statements/declarations.cpp
    ${INSTY_FRONTEND_SRC}/parser/statements/control_flow.cpp

    ${INSTY_FRONTEND_SRC}/utilities/config.cpp
    ${INSTY_FRONTEND_SRC}/utilities/utils.cpp
    ${INSTY_FRONTEND_SRC}/utilities/values.cpp
    ${INSTY_FRONTEND_SRC}/utilities/errors.cpp
    ${INSTY_FRONTEND_SRC}/utilities/type_system.cpp
    ${INSTY_FRONTEND_SRC}/utilities/logger.cpp
)

target_include_directories(InstyFrontend PUBLIC
    ${INSTY_COMPILER_DIR}/include
)

# LSP Server (with exceptions enabled for error handling)
add_library(InstyLSP STATIC
    src/lsp_server.cpp
    src/transport.cpp
    src/workspace.cpp
    src/analysis.cpp
    src/features.cpp
    src/json.cpp
    include/lsp/lsp_server.hpp
    include/lsp/lsp_internal.hpp
    include/lsp/json.hpp
)

target_include_directories(InstyLSP PUBLIC include)
target_compile_options(InstyLSP PRIVATE -fexceptions)
target_link_libraries(InstyLSP PUBLIC InstyFrontend)

add_executable(insty-lsp src/main.cpp)
target_compile_options(insty-lsp PRIVATE -fexceptions)
target_link_libraries(insty-lsp PRIVATE InstyLSP)
