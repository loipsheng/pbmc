/*
Copyright (c) 2025 Grabtaxi Holdings Pte Ltd (GRAB), All rights reserved.

Use of this source code is governed by an MIT-style license that can be found in the LICENSE file
*/



/***********************************************************************
Copyright (c) 2014-2020, Jan Elffers
Copyright (c) 2019-2021, Jo Devriendt
Copyright (c) 2020-2021, Stephan Gocht
Copyright (c) 2014-2021, Jakob Nordström

Parts of the code were copied or adapted from MiniSat.

MiniSat -- Copyright (c) 2003-2006, Niklas Een, Niklas Sorensson
           Copyright (c) 2007-2010  Niklas Sorensson

Permission is hereby granted, free of charge, to any person obtaining a
copy of this software and associated documentation files (the
"Software"), to deal in the Software without restriction, including
without limitation the rights to use, copy, modify, merge, publish,
distribute, sublicense, and/or sell copies of the Software, and to
permit persons to whom the Software is furnished to do so, subject to
the following conditions:

The above copyright notice and this permission notice shall be included
in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE
LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION
OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
***********************************************************************/

#pragma once

#include <cstddef>
#include <ostream>
#include "typedefs.hpp"

namespace rs {

struct CRef {
  uint32_t ofs;
  bool operator==(CRef const& o) const { return ofs == o.ofs; }
  bool operator!=(CRef const& o) const { return ofs != o.ofs; }
  bool operator<(CRef const& o) const { return ofs < o.ofs; }
  std::ostream& operator<<(std::ostream& os) { return os << ofs; }
};
const CRef CRef_Undef = {std::numeric_limits<uint32_t>::max()};

// TODO: make below methods part of a Solver object that's passed around
inline bool isTrue(const IntVecIt& level, Lit l) { return level[l] != INF; }
inline bool isFalse(const IntVecIt& level, Lit l) { return level[-l] != INF; }
inline bool isUnit(const IntVecIt& level, Lit l) { return level[l] == 0; }
inline bool isUnknown(const std::vector<int>& pos, Lit l) { return pos[toVar(l)] == INF; }
inline bool isDecided(const std::vector<CRef>& reasons, Lit l) { return reasons[toVar(l)] == CRef_Undef; }
inline bool isPropagated(const std::vector<CRef>& reasons, Lit l) { return !isDecided(reasons, l); }

struct Watch {
  CRef cref;
  int idx;
  /**
   * idx<0: blocked literal for clausal propagation
   * 0<=idx<INF: index of watched literal for cardinality propagation
   * INF<=idx: index of watched literal for watched/counting propagation
   **/
  Watch(CRef cr, int i) : cref(cr), idx(i){};
  bool operator==(const Watch& other) const { return other.cref == cref && other.idx == idx; }
};

// ---------------------------------------------------------------------
// Memory. Maximum supported size of learnt constraint database is 16GB

class OutOfMemoryException {};

struct ConstraintAllocator {
  uint32_t* memory = nullptr;  // TODO: why not uint64_t?
  uint32_t at = 0, cap = 0;
  uint32_t wasted = 0;  // for GC
  void capacity(uint32_t min_cap);
  static constexpr size_t alignmentWords() {
    return (alignof(std::max_align_t) + sizeof(uint32_t) - 1) /
           sizeof(uint32_t);
  }
  static size_t alignedWordSize(size_t words) {
    const size_t alignment = alignmentWords();
    return ((words + alignment - 1) / alignment) * alignment;
  }
  template <typename C>
  C* alloc(int nTerms) {
    uint32_t oldAt = at;
    const size_t words = alignedWordSize(C::getMemSize(nTerms));
    if (words > std::numeric_limits<uint32_t>::max() - at)
      throw OutOfMemoryException();
    at += static_cast<uint32_t>(words);
    capacity(at);
    assert(reinterpret_cast<uintptr_t>(memory + oldAt) % alignof(C) == 0);
    return (C*)(memory + oldAt);
  }
  Constr& operator[](CRef cr) { return (Constr&)*(memory + cr.ofs); }
  const Constr& operator[](CRef cr) const { return (Constr&)*(memory + cr.ofs); }
};

static inline void* xrealloc(void* ptr, size_t size) {
  void* mem = realloc(ptr, size);
  if (mem == NULL && errno == ENOMEM)
    throw OutOfMemoryException();
  else
    return mem;
}

// ---------------------------------------------------------------------
// Order heap

struct OrderHeap {  // segment tree (fast implementation of priority queue).
  std::vector<ActValV>& activity;
  int cap = 0;
  std::vector<Var> tree = {-1, -1};

  OrderHeap(std::vector<ActValV>& a) : activity(a) {}

  void resize(int newsize);
  void recalculate();
  void percolateUp(Var x);
  bool empty() const;
  bool inHeap(Var x) const;
  void insert(Var x);
  Var removeMax();
  // added for pb counter
  void clear();
};

}  // namespace rs
