// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 We-Amp B.V.

'use client';
import { useEffect, useState } from 'react';

export default function Counter() {
  const [n, setN] = useState(0);
  useEffect(() => {
    window.__hydrated = true;
  }, []);
  return (
    <p>
      <button id="inc" onClick={() => setN(n + 1)}>inc</button>
      <span id="count">{String(n)}</span>
    </p>
  );
}
