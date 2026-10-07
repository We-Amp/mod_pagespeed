// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

import { describe, expect, it } from "vitest";
import { sha256Hex } from "./sha256";

describe("sha256Hex", () => {
  it("matches the standard test vectors", () => {
    expect(sha256Hex("")).toBe("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    expect(sha256Hex("abc")).toBe("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    expect(sha256Hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq")).toBe(
      "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1",
    );
  });

  it("hashes the UTF-8 bytes, across the padding boundaries", () => {
    expect(sha256Hex("Größe ü")).toBe("a8b6d0a057b5522396977aeca343e639d96c26e01be45b2dd0495541d1c61ae0");
    expect(sha256Hex("x".repeat(55))).toBe("d5e285683cd4efc02d021a5c62014694958901005d6f71e89e0989fac77e4072");
    expect(sha256Hex("x".repeat(56))).toBe("04c26261370ee7541549d16dee320c723e3fd14671e66a099afe0a377c16888e");
  });

  it("handles a long input", () => {
    expect(sha256Hex("a".repeat(1_000_000))).toBe("cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
  });
});
