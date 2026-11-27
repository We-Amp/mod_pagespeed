// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

export type { ApiTransport, Unsubscribe } from './transport.js';
export type * from './types.js';
export { DirectTransport, ApiError } from './direct-transport.js';
export type { DirectTransportOptions } from './direct-transport.js';
export { VsCodeTransport } from './vscode-transport.js';
export type { VsCodeApi } from './vscode-transport.js';
export { ConnectionManager } from './connection.js';
export type {
  ConnectionState,
  ConnectionManagerOptions,
} from './connection.js';
export {
  createCacheClient,
  createCaptureClient,
} from './client.js';
export {
  ImageFormat,
  ViewportClass,
  PixelDensity,
  SaveData,
  TransferEncoding,
  encodeMask,
  decodeMask,
  DEVICE_PRESETS,
  formatImageFormat,
  formatViewport,
  formatDensity,
  formatSaveData,
  formatEncoding,
  formatMask,
  formatBytes,
  formatUptime,
} from './presets.js';
export type { MaskComponents, DevicePreset } from './presets.js';
