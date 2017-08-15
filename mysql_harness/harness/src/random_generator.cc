/*
  Copyright (c) 2017, Oracle and/or its affiliates. All rights reserved.

  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation; version 2 of the License.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
*/

#include "random_generator.h"

#include <algorithm>
#include <assert.h> // <cassert> is flawed: assert() lands in global namespace on Ubuntu 14.04, not std::
#include <random>
#include <stdexcept>
#include <string>

namespace mysql_harness {

namespace {
const unsigned kMinPasswordLength = 8;

const std::string kAlphabetDigits = "0123456789";
const std::string kAlphabetLowercase = "abcdefghijklmnopqrstuvwxyz";
const std::string kAlphabetUppercase = "ABCDEFGHIJKLMNOPQRSTUVWXYZ";
const std::string kAlphabetSpecial = "~@#$^&*()-=+]}[{|;:.>,</?";

std::string get_alphabet(unsigned alphabet_mask) {
  std::string result;
  if (alphabet_mask & RandomGenerator::AlphabetDigits) result += kAlphabetDigits;
  if (alphabet_mask & RandomGenerator::AlphabetLowercase) result += kAlphabetLowercase;
  if (alphabet_mask & RandomGenerator::AlphabetUppercase) result += kAlphabetUppercase;
  if (alphabet_mask & RandomGenerator::AlphabetSpecial) result += kAlphabetSpecial;

  return result;
}
}

std::string RandomGenerator::generate_identifier(unsigned length, unsigned alphabet_mask) /*override*/ {
  std::string result;
  std::random_device rd;
  const std::string alphabet = get_alphabet(alphabet_mask);

  if (alphabet.length() < 1) {
    throw std::invalid_argument("Wrong alphabet mask provided for generate_identifier("
                                + std::to_string(alphabet_mask) + ")");
  }

  std::uniform_int_distribution<unsigned long> dist(0, alphabet.length() - 1);

  for (unsigned i = 0; i < length; i++)
    result += alphabet[dist(rd)];

  return result;
}

std::string RandomGenerator::generate_strong_password(unsigned length) /*override*/ {
  if (length < kMinPasswordLength) {
    throw std::invalid_argument("The password needs to be at least "
                                + std::to_string(kMinPasswordLength)
                                + " charactes long");
  }

  std::string result;
  result += generate_identifier(1, AlphabetDigits); // at least one digit
  result += generate_identifier(1, AlphabetLowercase); // at least one lowercase letter
  result += generate_identifier(1, AlphabetUppercase); // at least one upperrcase letter
  result += generate_identifier(1, AlphabetSpecial); // at least one special character

  // fill the rest with random data from the whole characters set
  length -= static_cast<unsigned>(result.length());
  result += generate_identifier(length, AlphabetAll);

  std::random_shuffle(result.begin(), result.end());

  return result;
}

// returns "012345678901234567890123...", truncated to length
std::string FakeRandomGenerator::generate_identifier(unsigned length, unsigned) /*override*/ {
  std::string pwd;
  for (unsigned i = 0; i < length; i++)
    pwd += static_cast<char>('0' + i % 10);
  return pwd;
}

// returns "012345678901234567890123...", truncated to length
std::string FakeRandomGenerator::generate_strong_password(unsigned length) /*override*/ {
  return generate_identifier(length, 0);
}

} // namespace mysql_harness
