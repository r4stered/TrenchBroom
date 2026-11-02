/*
 Copyright (C) 2026 Drew Williams

 This file is part of TrenchBroom.

 TrenchBroom is free software: you can redistribute it and/or modify
 it under the terms of the GNU General Public License as published by
 the Free Software Foundation, either version 3 of the License, or
 (at your option) any later version.

 TrenchBroom is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 GNU General Public License for more details.

 You should have received a copy of the GNU General Public License
 along with TrenchBroom. If not, see <http://www.gnu.org/licenses/>.
 */

#include "cli/Sha256.h"

#include <string>

#include <catch2/catch_test_macros.hpp>

namespace tb::cli
{

TEST_CASE("Sha256")
{
  SECTION("sha256Hex")
  {
    // reference digests from Python's hashlib
    CHECK(
      sha256Hex("")
      == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    CHECK(
      sha256Hex("abc")
      == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    // 55, 56 and 64 bytes cross the padding boundaries
    CHECK(
      sha256Hex(std::string(55, 'a'))
      == "9f4390f8d30c2dd92ec9f095b65e2b9ae9b0a925a5258e241c9f1e910f734318");
    CHECK(
      sha256Hex(std::string(56, 'a'))
      == "b35439a4ac6f0948b6d6f9e3c6af0f5f590ce20f1bde7090ef7970686ec6738a");
    CHECK(
      sha256Hex(std::string(64, 'a'))
      == "ffe054fe7ae0cb6dc65c3af9b61d5209f439851db43d0ba5997337df154668eb");
    CHECK(
      sha256Hex(std::string(1000, 'a'))
      == "41edece42d63e8d9bf515a9ba6932e1c20cbc9f5a5d134645adb5db1b9737ea3");
  }
}

} // namespace tb::cli
