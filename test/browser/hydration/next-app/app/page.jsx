// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 We-Amp B.V.

import Link from 'next/link';
import Counter from './counter';

const STYLE = '\n.box {\n  color: red;\n  padding: 4px 8px;\n}\n.t   { margin: 0 }\n';

export default function Page() {
  return (
    <div className="box" id="probe">
      <h1>Hello</h1>
      <p>Text  with   two spaces</p>
      <style>{STYLE}</style>
      <Counter />
      <img src="/i.png" alt="pic" width="1" height="1" />
      <iframe src="/f.html" title="f" width="1" height="1" />
      <Link id="nav" href="/two/">two</Link>
    </div>
  );
}
