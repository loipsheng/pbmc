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

  add_executable(basic-test tests/inprocessing/basic_test.cc)
  target_link_libraries(basic-test
    pbmc roundingsat flowcutter z mpfr gmp gmpxx
  )
  add_test(NAME gate_2a_basic_unit COMMAND basic-test)

  add_executable(bicp-test tests/inprocessing/bicp_test.cc)
  target_link_libraries(bicp-test
    pbmc roundingsat flowcutter z mpfr gmp gmpxx
  )
  add_test(NAME gate_2b_bicp_unit COMMAND bicp-test)

  add_executable(reference-key-test tests/inprocessing/reference_key_test.cc)
  target_link_libraries(reference-key-test
    pbmc roundingsat flowcutter z mpfr gmp gmpxx
  )
  add_test(NAME gate_2e1_reference_key COMMAND reference-key-test)

  add_executable(cs-test tests/inprocessing/cs_test.cc)
  target_link_libraries(cs-test
    pbmc roundingsat flowcutter z mpfr gmp gmpxx
  )
  add_test(NAME gate_3a_cs_unit COMMAND cs-test)

  add_executable(gcd-test tests/inprocessing/gcd_test.cc)
  target_link_libraries(gcd-test
    pbmc roundingsat flowcutter z mpfr gmp gmpxx
  )
  add_test(NAME gate_3b_gcd_unit COMMAND gcd-test)

  add_executable(cardinality-test tests/inprocessing/cardinality_test.cc)
  target_link_libraries(cardinality-test
    pbmc roundingsat flowcutter z mpfr gmp gmpxx
  )
  add_test(NAME gate_3c_cardinality_unit COMMAND cardinality-test)

  add_executable(sc-test tests/inprocessing/sc_test.cc)
  target_link_libraries(sc-test
    pbmc roundingsat flowcutter z mpfr gmp gmpxx
  )
  add_test(NAME gate_3d_sc_committed_unit COMMAND sc-test)

  add_executable(overlay-test tests/inprocessing/overlay_test.cc)
  target_link_libraries(overlay-test
    pbmc roundingsat flowcutter z mpfr gmp gmpxx
  )
  add_test(NAME gate_3e_overlay_undo COMMAND overlay-test)

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
    add_test(
      NAME gate_2a_basic
      COMMAND ${PYTHON_EXECUTABLE}
              ${CMAKE_CURRENT_SOURCE_DIR}/tests/inprocessing/run_gate_2a.py
              --solver $<TARGET_FILE:pbmc-bin>
    )
    add_test(
      NAME gate_2b_bicp
      COMMAND ${PYTHON_EXECUTABLE}
              ${CMAKE_CURRENT_SOURCE_DIR}/tests/inprocessing/run_gate_2b.py
              --solver $<TARGET_FILE:pbmc-bin>
    )
    add_test(
      NAME gate_2d_cache_bypass
      COMMAND ${PYTHON_EXECUTABLE}
              ${CMAKE_CURRENT_SOURCE_DIR}/tests/inprocessing/run_gate_2d.py
              --solver $<TARGET_FILE:pbmc-bin>
    )
    add_test(
      NAME gate_2e_residual_cache
      COMMAND ${PYTHON_EXECUTABLE}
              ${CMAKE_CURRENT_SOURCE_DIR}/tests/inprocessing/run_gate_2e.py
              --solver $<TARGET_FILE:pbmc-bin>
    )
    add_test(
      NAME gate_3a_cs
      COMMAND ${PYTHON_EXECUTABLE}
              ${CMAKE_CURRENT_SOURCE_DIR}/tests/inprocessing/run_gate_3a.py
              --solver $<TARGET_FILE:pbmc-bin>
    )
    add_test(
      NAME gate_3b_gcd
      COMMAND ${PYTHON_EXECUTABLE}
              ${CMAKE_CURRENT_SOURCE_DIR}/tests/inprocessing/run_gate_3b.py
              --solver $<TARGET_FILE:pbmc-bin>
    )
    add_test(
      NAME gate_3c_cardinality
      COMMAND ${PYTHON_EXECUTABLE}
              ${CMAKE_CURRENT_SOURCE_DIR}/tests/inprocessing/run_gate_3c.py
              --solver $<TARGET_FILE:pbmc-bin>
    )
    add_test(
      NAME gate_3d_sc_committed
      COMMAND ${PYTHON_EXECUTABLE}
              ${CMAKE_CURRENT_SOURCE_DIR}/tests/inprocessing/run_gate_3d.py
              --solver $<TARGET_FILE:pbmc-bin>
    )
  endif()
endif()
