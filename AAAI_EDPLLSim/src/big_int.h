#ifndef __BIG_INT_H__
#define __BIG_INT_H__

#include <assert.h>
#include <gmp.h>

#include <string>

using namespace std;

class Int {
 public:
  mpz_t a;

  Int();
  Int(int v);
  Int(const Int& b);
  Int(const string& b);
  ~Int();

  Int& operator=(const Int& b);
  Int& operator=(const int b);
  Int& operator=(const string& b);

  Int& operator+=(const Int& b);
  Int& operator-=(const Int& b);
  Int& operator*=(const Int& b);
  Int& operator/=(const Int& b);

  Int operator-() const;

  Int operator+(const Int& b) const;
  Int operator+(const int b) const;
  friend Int operator+(const int a, const Int& b);

  Int operator-(const Int& b) const;
  Int operator-(const int b) const;
  friend Int operator-(const int a, const Int& b);

  Int operator*(const Int& b) const;
  Int operator*(const int b) const;
  friend Int operator*(const int a, const Int& b);

  Int operator/(const Int& b) const;

  int operator%(int b) const;
  Int operator%(const Int& b) const;

  Int operator++(int);
  Int operator--(int);

  int cmp(const Int& v) const;
  bool operator!=(const Int& b) const;
  bool operator==(const Int& b) const;
  bool operator<(const Int& b) const;

  friend istream& operator>>(istream& input, Int& b);
  friend ostream& operator<<(ostream& output, const Int& b);

  static void gcd(Int& res, const Int& a, const Int& b);
  double to_double() const;
};

#endif