// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// The analysis runs here, off the main thread. The worker receives File
// objects, reads them itself, and posts back progress and the aggregate. It
// never opens a network connection and never touches storage.
import { analyzeFiles } from '../../lib/crawler-cost/analyze';

type Inbound = { type: 'analyze'; files: File[] };
const scope = self as unknown as {
  postMessage(message: unknown): void;
  onmessage: ((event: MessageEvent<Inbound>) => void) | null;
};

scope.onmessage = async (event) => {
  const msg = event.data;
  if (!msg || msg.type !== 'analyze') return;
  try {
    const result = await analyzeFiles(msg.files, {
      onProgress: (bytesRead, bytesTotal) =>
        scope.postMessage({ type: 'progress', bytesRead, bytesTotal }),
      onFile: (report) => scope.postMessage({ type: 'file', report }),
    });
    scope.postMessage({ type: 'result', result });
  } catch (e) {
    scope.postMessage({
      type: 'error',
      message: e instanceof Error ? e.message : 'the analysis failed',
    });
  }
};

// Tells the page the worker's handler is installed and files can be posted.
scope.postMessage({ type: 'ready' });
