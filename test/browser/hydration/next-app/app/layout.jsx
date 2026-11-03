// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 We-Amp B.V.

import './globals.css';

export default function Layout({ children }) {
  return (
    <html lang="en">
      <head>
        <link rel="stylesheet" href="/a.css" precedence="default" />
        <script src="/async.js" async />
      </head>
      <body>{children}</body>
    </html>
  );
}
