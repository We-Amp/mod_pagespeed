// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

std::wstring s2ws(const std::string& s);
std::string ws2s(const std::wstring& s);
// UTF-8, whatever the system's ANSI code page is: ws2s converts through that
// code page and turns every character it cannot represent into '?'.
std::string ws2utf8(const std::wstring& s);
