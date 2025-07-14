# this is the subdirectory cmake list for roundingsat

set(roundingsat_SOURCES
    ./Constr.cpp
    ./ConstrExp.cpp
    ./ConstrSimple.cpp
    ./LpSolver.cpp
    ./Solver.cpp
    ./SolverStructs.cpp
    ./parsing.cpp
    ./quit.cpp
    ./roundingsat.cpp
    ./run.cpp
    ./used_licenses/gpl_3_0.cpp
    ./used_licenses/zib_academic.cpp
    ./used_licenses/roundingsat.cpp
    ./used_licenses/MIT.cpp
    ./used_licenses/licenses.cpp
    ./used_licenses/boost.cpp
    ./used_licenses/lgpl_3_0.cpp
)

set(roundingsat_HEADERS
    ./Constr.hpp
    ./ConstrExp.hpp
    ./ConstrSimple.hpp
    ./SolverStructs.hpp
    ./Logger.hpp
    ./Stats.hpp
    ./LpSolver.hpp
    ./Options.hpp
    ./IntSet.hpp
    ./Solver.hpp
    ./typedefs.hpp
    ./auxiliary.hpp
    ./globals.hpp
    ./parsing.hpp
    ./quit.hpp
    ./run.hpp
    ./used_licenses/roundingsat.hpp
    ./used_licenses/lgpl_3_0.hpp
    ./used_licenses/boost.hpp
    ./used_licenses/MIT.hpp
    ./used_licenses/gpl_3_0.hpp
    ./used_licenses/zib_academic.hpp
    ./used_licenses/licenses.hpp
)


add_definitions("-DGIT_COMMIT_HASH=Placeholder")
add_definitions("-DGIT_BRANCH=Placeholder")


# add_compile_options("$<$<CONFIG:Debug>:-O0>")
add_compile_options(
  "$<$<CONFIG:Debug>:-O0>"
  "$<$<NOT:$<CONFIG:Debug>>:-O3>"
)

add_library(roundingsat ${roundingsat_SOURCES})

# target_compile_definitions(roundingsat 
#   PRIVATE $<$<CONFIG:Debug>:DEBUG>)

# using gmp
target_link_libraries(roundingsat gmp gmpxx)
target_compile_definitions(roundingsat PUBLIC WITHGMP)