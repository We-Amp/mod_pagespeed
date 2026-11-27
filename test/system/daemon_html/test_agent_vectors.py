#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 We-Amp B.V.

"""Checks the shared agent vector file against a reference implementation
of its matching rule over the tokens its header lists.

When KNOWN_AGENT_TABLE names a copy of the agent table JSON (a list of
objects with a "token" field), the header tokens are also checked against
it; otherwise that cross-check is skipped with a note."""

import json
import os
import string
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
VECTORS = os.path.join(HERE, "agent_vectors.txt")
BOUNDARY = frozenset(" ();/")
EMPTY = "(empty)"


def load(path=VECTORS):
    tokens, vectors = [], []
    with open(path, encoding="utf-8") as f:
        for lineno, raw in enumerate(f, 1):
            line = raw.rstrip("\n")
            if line.startswith("# token: "):
                tokens.append(line[len("# token: "):])
                continue
            if not line or line.startswith("#"):
                continue
            cls, sep, ua = line.partition("\t")
            if not sep:
                raise ValueError("%s:%d: no TAB" % (path, lineno))
            vectors.append((lineno, int(cls), "" if ua == EMPTY else ua))
    return tokens, vectors


# ASCII-only case folding, as the rule says: str.lower() would also fold
# non-ASCII letters (U+212A KELVIN SIGN to "k") and change lengths.
ASCII_LOWER = str.maketrans(string.ascii_uppercase, string.ascii_lowercase)


def matches(user_agent, token):
    ua, t = user_agent.translate(ASCII_LOWER), token.translate(ASCII_LOWER)
    i = ua.find(t)
    while i != -1:
        j = i + len(t)
        if ((i == 0 or ua[i - 1] in BOUNDARY) and
                (j == len(ua) or ua[j] in BOUNDARY)):
            return True
        i = ua.find(t, i + 1)
    return False


def classify(user_agent, tokens):
    return 3 if any(matches(user_agent, t) for t in tokens) else 0


class AgentVectorsTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tokens, cls.vectors = load()

    def test_header_tokens(self):
        self.assertGreater(len(self.tokens), 0)
        self.assertEqual(len(self.tokens), len(set(self.tokens)))
        for t in self.tokens:
            self.assertEqual(t, t.strip())
            self.assertNotIn("  ", t)
            self.assertNotIn("\t", t)

    def test_every_vector(self):
        self.assertGreater(len(self.vectors), 100)
        for lineno, cls, ua in self.vectors:
            self.assertIn(cls, (0, 3), "line %d" % lineno)
            self.assertEqual(classify(ua, self.tokens), cls,
                             "line %d: %r" % (lineno, ua))

    def test_coverage(self):
        uas = [ua for _, _, ua in self.vectors]
        self.assertIn("", uas)
        classes = {cls for _, cls, _ in self.vectors}
        self.assertEqual(classes, {0, 3})
        # Every multi-word token is exercised as a phrase match.
        for t in self.tokens:
            if " " in t:
                self.assertTrue(
                    any(c == 3 and matches(ua, t) and
                        classify(ua, [x for x in self.tokens if x != t]) == 0
                        for _, c, ua in self.vectors),
                    "no phrase vector for %r" % t)

    def test_reference_rule(self):
        self.assertTrue(matches("a (GPTBot/1) b", "gptbot"))
        self.assertFalse(matches("GPTBotX", "GPTBot"))
        self.assertFalse(matches("ChatGPT  Agent", "ChatGPT Agent"))
        self.assertTrue(matches("x;ChatGPT Agent/2", "ChatGPT Agent"))
        self.assertFalse(matches("bigsurxai", "bigsur.ai"))
        self.assertFalse(matches("\u212aimiBot/1.0", "KimiBot"))
        self.assertTrue(matches("kIMIbOT/1.0", "KimiBot"))

    def test_table_cross_check(self):
        path = os.environ.get("KNOWN_AGENT_TABLE")
        if not path or not os.path.exists(path):
            print("note: KNOWN_AGENT_TABLE not set; skipping the "
                  "cross-check against the agent table")
            self.skipTest("agent table not reachable")
        with open(path, encoding="utf-8") as f:
            table = [e["token"] for e in json.load(f)]
        self.assertEqual(sorted(self.tokens), sorted(table))


if __name__ == "__main__":
    unittest.main()
