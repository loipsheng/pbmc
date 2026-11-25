cmake_minimum_required(VERSION 3.5)
project(pbmc CXX)
set(CMAKE_CXX_STANDARD 17)

option(PBMC_STATIC_BUILD "build pbmc as a static execution file" off)
option(RS_LOWER_PREC "Int256 instead of mpz_int in parts of roundingsat" ON)

link_directories(/usr/local/lib)
include_directories(${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/flow-cutter-pace17/src ${CMAKE_CURRENT_SOURCE_DIR}/roundingsat /usr/local/include)

IF(APPLE)
  # apple machines cannot static compile because lack of system files, linking default homebrew dir
  link_directories(/opt/homebrew/lib)
  include_directories(/opt/homebrew/include)
ENDIF()


add_compile_options(
  "$<$<CONFIG:Debug>:-O0>"
  "$<$<NOT:$<CONFIG:Debug>>:-O3>"
)
add_compile_options(-Wall -Wno-parentheses -Wno-format -Wno-sign-compare -Wno-unused-variable -Wno-unused-result)
add_compile_options("$<$<CXX_COMPILER_ID:GNU>:-Wno-literal-suffix;-Wno-class-memaccess>")
add_compile_options("$<$<CXX_COMPILER_ID:Clang>:-Wno-reserved-user-defined-literal;-Wno-gnu-array-member-paren-init>")
add_compile_options("$<$<CXX_COMPILER_ID:AppleClang>:-Wno-redundant-move;-Wno-undefined-var-template;-Wno-reserved-user-defined-literal;-Wno-gnu-array-member-paren-init;-std=c++17>")

# this uses lower precision for roundingsat internally, boost::multiprecision::int256_t instead of boost::multiprecision::mpz_int, disable if needed
IF(RS_LOWER_PREC)
  add_definitions("-DLOW_PREC")
ENDIF()

add_subdirectory(flow-cutter-pace17)
add_subdirectory(roundingsat)

add_library(pbmc STATIC 
	core/ComponentCache.cc core/ComponentManager.cc core/Counter.cc core/simplify.cc
	core/Config.cc core/Instance.cc ddnnf/DecisionTree.cc
	core/Solver.cc 
	utils/Options.cc utils/System.cc
	preprocessor/Preprocessor.cc preprocessor/TestSolver.cc 
	preprocessor/lib_sharpsat_td/subsumer.cpp
	preprocessor/TreeDecomposition.cc preprocessor/IFlowCutter.cc
	)
target_compile_definitions(pbmc 
  PRIVATE $<$<CONFIG:Debug>:DEBUG>
           __STDC_FORMAT_MACROS __STDC_LIMIT_MACROS
)

add_executable(pbmc-bin core/Main.cc)
set_target_properties(pbmc-bin PROPERTIES OUTPUT_NAME pbmc)

target_link_libraries(pbmc-bin pbmc roundingsat flowcutter z mpfr gmp gmpxx $<$<BOOL:${PBMC_STATIC_BUILD}>:-static>)

include(CTest)
if(BUILD_TESTING)
  add_executable(equality-metadata-test tests/inprocessing/equality_metadata_test.cc)
  target_link_libraries(equality-metadata-test gmp gmpxx)
  add_test(NAME gate_0e_equality_metadata COMMAND equality-metadata-test)

  add_executable(equality-gc-test tests/inprocessing/equality_gc_test.cc)
  target_link_libraries(equality-gc-test
    pbmc roundingsat flowcutter z mpfr gmp gmpxx
  )
  add_test(NAME gate_0e_equality_gc COMMAND equality-gc-test)

  add_executable(simplify-result-test tests/inprocessing/simplify_result_test.cc)
  add_test(NAME gate_1_simplify_result COMMAND simplify-result-test)

  find_package(PythonInterp 3 QUIET)
  if(PYTHONINTERP_FOUND)
    add_test(
      NAME gate_0b_exact_mc
      COMMAND ${PYTHON_EXECUTABLE}
              ${CMAKE_CURRENT_SOURCE_DIR}/tests/inprocessing/run_gate_0b.py
              --solver $<TARGET_FILE:pbmc-bin>
    )
    add_test(
      NAME gate_0e_operator_equality
      COMMAND ${PYTHON_EXECUTABLE}
              ${CMAKE_CURRENT_SOURCE_DIR}/tests/inprocessing/run_gate_0e.py
              --solver $<TARGET_FILE:pbmc-bin>
    )
    add_test(
      NAME gate_1_empty_simplifier
      COMMAND ${PYTHON_EXECUTABLE}
              ${CMAKE_CURRENT_SOURCE_DIR}/tests/inprocessing/run_gate_1.py
              --solver $<TARGET_FILE:pbmc-bin>
    )
  endif()
endif()
