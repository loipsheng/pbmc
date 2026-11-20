// Implementation of the high-precision integer arithmetic required by EDPLLSim.
#include "big_int.h"

Int::Int() { mpz_init(a); }

Int::Int(int v) {
  mpz_init(a);
  mpz_set_si(a, v);
}

Int::Int(const Int &b) {
  mpz_init(a);
  mpz_set(a, b.a);
}

Int::Int(const string &b) {
  mpz_init(a);
  mpz_set_str(a, b.c_str(), 10);
}

Int::~Int() { mpz_clear(a); }

Int &Int::operator=(const Int &b) {
  mpz_set(a, b.a);
  return *this;
}
Int &Int::operator=(const int b) {
  mpz_set_si(a, b);
  return *this;
}
Int &Int::operator=(const string &b) {
  mpz_set_str(a, b.c_str(), 10);
  return *this;
}

Int &Int::operator+=(const Int &b) {
  mpz_add(a, a, b.a);
  return *this;
}

Int &Int::operator-=(const Int &b) {
  mpz_sub(a, a, b.a);
  return *this;
}

Int &Int::operator*=(const Int &b) {
  mpz_mul(a, a, b.a);
  return *this;
}

Int &Int::operator/=(const Int &b) {
  mpz_tdiv_q(a, a, b.a);
  return *this;
}

Int Int::operator-() const {
  Int t;
  mpz_neg(t.a, a);
  return t;
}

Int Int::operator+(const Int &b) const {
  Int t;
  mpz_add(t.a, a, b.a);
  return t;
}
Int Int::operator+(const int b) const {
  Int t;
  if (b > 0)
    mpz_add_ui(t.a, a, b);
  else
    mpz_sub_ui(t.a, a, -b);
  return t;
}
Int operator+(const int a, const Int &b) {
  Int t;
  if (a > 0)
    mpz_add_ui(t.a, b.a, a);
  else
    mpz_sub_ui(t.a, b.a, -a);
  return t;
}

Int Int::operator-(const Int &b) const {
  Int t;
  mpz_sub(t.a, a, b.a);
  return t;
}
Int Int::operator-(const int b) const {
  Int t;
  if (b > 0)
    mpz_sub_ui(t.a, a, b);
  else
    mpz_add_ui(t.a, a, -b);
  return t;
}
Int operator-(const int a, const Int &b) {
  Int t;
  if (a > 0)
    mpz_ui_sub(t.a, a, b.a);
  else {
    mpz_set_si(t.a, a);
    mpz_sub(t.a, t.a, b.a);
  }
  return t;
}

Int Int::operator*(const Int &b) const {
  Int t;
  mpz_mul(t.a, a, b.a);
  return t;
}
Int Int::operator*(const int b) const {
  Int t;
  mpz_mul_si(t.a, a, b);
  return t;
}
Int operator*(const int a, const Int &b) {
  Int t;
  mpz_mul_si(t.a, b.a, a);
  return t;
}

Int Int::operator/(const Int &b) const {
  Int t;
  mpz_tdiv_q(t.a, a, b.a);
  return t;
}

int Int::operator%(int b) const {
  Int t;
  assert(b);
  if (b < 0) b = -b;
  return mpz_mod_ui(t.a, a, b);
}

Int Int::operator%(const Int &b) const {
  Int t;
  mpz_mod(t.a, a, b.a);
  return t;
}

Int Int::operator++(int) {
  mpz_add_ui(a, a, 1);
  return *this;
}

Int Int::operator--(int) {
  mpz_sub_ui(a, a, 1);
  return *this;
}

int Int::cmp(const Int &b) const { return mpz_cmp(a, b.a); }

bool Int::operator!=(const Int &b) const { return cmp(b); }

bool Int::operator==(const Int &b) const { return !cmp(b); }

bool Int::operator<(const Int &b) const { return cmp(b) < 0; }

istream &operator>>(istream &input, Int &b) {
  string s;
  input >> s;
  mpz_set_str(b.a, s.c_str(), 10);
  return input;
}

ostream &operator<<(ostream &output, const Int &b) {
  char *s = 0;
  output << string(mpz_get_str(s, 10, b.a));
  return output;
}

void Int::gcd(Int &res, const Int &a, const Int &b) {
  mpz_gcd(res.a, a.a, b.a);
}

double Int::to_double() const {
  return mpz_get_d(a);
}