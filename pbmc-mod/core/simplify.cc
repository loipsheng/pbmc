#include <gmpxx.h>

#include "mpfr/mpreal.h"
#include "core/Counter.h"

namespace GPMC {

template <typename T_data>
SimpResult Simplifier<T_data>::run(PBcounter<T_data>&) {
	return {};
}

template class Simplifier<mpz_class>;
template class Simplifier<mpfr::mpreal>;

} // namespace GPMC
