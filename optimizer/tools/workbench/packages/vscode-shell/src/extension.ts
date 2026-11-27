// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

import * as vscode from 'vscode';
import type { HealthResponse, CachePurgeResponse, CacheReprocessResponse } from '@pagespeed/api-client';
import { WebviewBridge } from './bridge.js';
import { ConnectionViewProvider } from './connection-view.js';
import { UrlTreeViewProvider } from './url-tree-view.js';

// ---------------------------------------------------------------------------
// Shared extension state
// ---------------------------------------------------------------------------

const TOKEN_KEY = 'pagespeed.apiToken';

let currentBridge: WebviewBridge | undefined;
let dashboardPanel: vscode.WebviewPanel | undefined;
let waterfallPanel: vscode.WebviewPanel | undefined;
let diffPanel: vscode.WebviewPanel | undefined;
let configPanel: vscode.WebviewPanel | undefined;
let connectionView: ConnectionViewProvider;
let urlTreeProvider: UrlTreeViewProvider;

// ---------------------------------------------------------------------------
// Activation
// ---------------------------------------------------------------------------

export function activate(context: vscode.ExtensionContext): void {
  // -- Status bar -----------------------------------------------------------

  const statusBar = vscode.window.createStatusBarItem(
    vscode.StatusBarAlignment.Right,
    100,
  );
  statusBar.text = '$(circle-slash) PageSpeed';
  statusBar.tooltip = 'PageSpeed: Disconnected';
  statusBar.command = 'pagespeed.connect';
  statusBar.show();
  context.subscriptions.push(statusBar);

  // -- Connection tree view -------------------------------------------------

  connectionView = new ConnectionViewProvider();
  const treeView = vscode.window.createTreeView('pagespeed.connection', {
    treeDataProvider: connectionView,
  });
  context.subscriptions.push(treeView);

  const config = vscode.workspace.getConfiguration('pagespeed');
  connectionView.update({ apiUrl: getApiUrl(config) });

  // -- URL tree view --------------------------------------------------------

  urlTreeProvider = new UrlTreeViewProvider(
    getApiUrl(config),
    () => context.secrets.get(TOKEN_KEY),
  );
  const urlTreeView = vscode.window.createTreeView('pagespeed.urls', {
    treeDataProvider: urlTreeProvider,
  });
  context.subscriptions.push(urlTreeView);

  // -- Commands -------------------------------------------------------------

  context.subscriptions.push(
    vscode.commands.registerCommand('pagespeed.connect', () =>
      connectCommand(context, statusBar),
    ),
    vscode.commands.registerCommand('pagespeed.disconnect', () =>
      disconnectCommand(statusBar),
    ),
    vscode.commands.registerCommand('pagespeed.setToken', () =>
      setTokenCommand(context),
    ),
    vscode.commands.registerCommand('pagespeed.showDashboard', () =>
      showDashboardCommand(context),
    ),
    vscode.commands.registerCommand('pagespeed.showWaterfall', () =>
      showWaterfallCommand(context),
    ),
    vscode.commands.registerCommand('pagespeed.showDiff', () =>
      showDiffCommand(context),
    ),
    vscode.commands.registerCommand('pagespeed.showConfig', () =>
      showConfigCommand(context),
    ),
    vscode.commands.registerCommand('pagespeed.inspectUrl', () =>
      inspectUrlCommand(),
    ),
    vscode.commands.registerCommand('pagespeed.refreshUrls', () =>
      urlTreeProvider.refresh(),
    ),
    vscode.commands.registerCommand(
      'pagespeed.purgeUrlFromTree',
      (item?: { urlEntry?: { url: string; hostname: string; site?: string } }) =>
        purgeUrlFromTreeCommand(context, item),
    ),
    vscode.commands.registerCommand(
      'pagespeed.reprocessUrlFromTree',
      (item?: { urlEntry?: { url: string; hostname: string; site?: string } }) =>
        reprocessUrlFromTreeCommand(context, item),
    ),
  );

  // -- Initial context for conditional menus --------------------------------

  void vscode.commands.executeCommand(
    'setContext',
    'pagespeed.connected',
    false,
  );

  // -- Auto-connect ---------------------------------------------------------

  if (config.get<boolean>('autoConnect', true)) {
    void vscode.commands.executeCommand('pagespeed.connect');
  }
}

export function deactivate(): void {
  currentBridge?.dispose();
  currentBridge = undefined;
  dashboardPanel?.dispose();
  dashboardPanel = undefined;
  waterfallPanel?.dispose();
  waterfallPanel = undefined;
  diffPanel?.dispose();
  diffPanel = undefined;
  configPanel?.dispose();
  configPanel = undefined;
}

// ---------------------------------------------------------------------------
// Command: connect
// ---------------------------------------------------------------------------

async function connectCommand(
  context: vscode.ExtensionContext,
  statusBar: vscode.StatusBarItem,
): Promise<void> {
  const config = vscode.workspace.getConfiguration('pagespeed');
  const apiUrl = getApiUrl(config);
  const token = await context.secrets.get(TOKEN_KEY);

  statusBar.text = '$(loading~spin) PageSpeed';
  statusBar.tooltip = `PageSpeed: Connecting to ${apiUrl}...`;

  try {
    const url = new URL('/v1/health', apiUrl);
    const headers: Record<string, string> = {};
    if (token) {
      headers['Authorization'] = `Bearer ${token}`;
    }

    const res = await fetch(url.toString(), { headers });
    if (!res.ok) {
      throw new Error(`HTTP ${res.status} ${res.statusText}`);
    }

    const health = (await res.json()) as HealthResponse;

    statusBar.text = '$(circle-filled) PageSpeed';
    statusBar.tooltip = `PageSpeed: Connected (${health.status})`;
    statusBar.command = 'pagespeed.showDashboard';

    void vscode.commands.executeCommand(
      'setContext',
      'pagespeed.connected',
      true,
    );

    connectionView.update({
      connected: true,
      apiUrl,
      health,
      error: undefined,
    });

    urlTreeProvider.refresh();

    void vscode.window.showInformationMessage(
      `Connected to PageSpeed worker at ${apiUrl}`,
    );
  } catch (err) {
    const msg = err instanceof Error ? err.message : String(err);

    statusBar.text = '$(circle-slash) PageSpeed';
    statusBar.tooltip = `PageSpeed: Connection failed - ${msg}`;
    statusBar.command = 'pagespeed.connect';

    void vscode.commands.executeCommand(
      'setContext',
      'pagespeed.connected',
      false,
    );

    connectionView.update({
      connected: false,
      apiUrl,
      health: undefined,
      error: msg,
    });

    urlTreeProvider.refresh();

    void vscode.window.showWarningMessage(
      `PageSpeed connection failed: ${msg}`,
    );
  }
}

// ---------------------------------------------------------------------------
// Command: disconnect
// ---------------------------------------------------------------------------

function disconnectCommand(statusBar: vscode.StatusBarItem): void {
  currentBridge?.dispose();
  currentBridge = undefined;

  dashboardPanel?.dispose();
  dashboardPanel = undefined;
  waterfallPanel?.dispose();
  waterfallPanel = undefined;
  diffPanel?.dispose();
  diffPanel = undefined;
  configPanel?.dispose();
  configPanel = undefined;

  statusBar.text = '$(circle-slash) PageSpeed';
  statusBar.tooltip = 'PageSpeed: Disconnected';
  statusBar.command = 'pagespeed.connect';

  void vscode.commands.executeCommand(
    'setContext',
    'pagespeed.connected',
    false,
  );

  const config = vscode.workspace.getConfiguration('pagespeed');
  connectionView.update({
    connected: false,
    apiUrl: getApiUrl(config),
    health: undefined,
    error: undefined,
  });

  urlTreeProvider.refresh();
}

// ---------------------------------------------------------------------------
// Command: setToken
// ---------------------------------------------------------------------------

async function setTokenCommand(
  context: vscode.ExtensionContext,
): Promise<void> {
  const token = await vscode.window.showInputBox({
    prompt: 'Enter your PageSpeed API token',
    placeHolder: 'Token (stored securely in OS keychain)',
    password: true,
    ignoreFocusOut: true,
  });

  if (token === undefined) return; // User cancelled.

  if (token === '') {
    // Clear the stored token.
    await context.secrets.delete(TOKEN_KEY);
    void vscode.window.showInformationMessage('PageSpeed API token cleared.');
  } else {
    await context.secrets.store(TOKEN_KEY, token);
    void vscode.window.showInformationMessage(
      'PageSpeed API token stored securely.',
    );
  }
}

// ---------------------------------------------------------------------------
// Command: showDashboard
// ---------------------------------------------------------------------------

function showDashboardCommand(
  context: vscode.ExtensionContext,
): void {
  // Reveal existing panel if it exists.
  if (dashboardPanel) {
    dashboardPanel.reveal(vscode.ViewColumn.One);
    return;
  }

  const config = vscode.workspace.getConfiguration('pagespeed');
  const apiUrl = getApiUrl(config);

  dashboardPanel = vscode.window.createWebviewPanel(
    'pagespeed.dashboard',
    'PageSpeed Dashboard',
    vscode.ViewColumn.One,
    {
      enableScripts: true,
      retainContextWhenHidden: true,
    },
  );

  // Set up the bridge for this webview.
  const bridge = new WebviewBridge(
    dashboardPanel.webview,
    apiUrl,
    () => context.secrets.get(TOKEN_KEY),
  );
  currentBridge = bridge;

  dashboardPanel.webview.html = getDashboardHtml(dashboardPanel.webview);

  dashboardPanel.onDidDispose(() => {
    bridge.dispose();
    if (currentBridge === bridge) {
      currentBridge = undefined;
    }
    dashboardPanel = undefined;
  });
}

// ---------------------------------------------------------------------------
// Command: inspectUrl
// ---------------------------------------------------------------------------

async function inspectUrlCommand(): Promise<void> {
  const url = await vscode.window.showInputBox({
    prompt: 'Enter a URL to inspect in the PageSpeed cache',
    placeHolder: 'https://example.com/page',
    ignoreFocusOut: true,
    validateInput: (value) => {
      if (!value) return 'URL is required';
      try {
        new URL(value);
        return undefined;
      } catch {
        return 'Please enter a valid URL';
      }
    },
  });

  if (!url) return;

  void vscode.window.showInformationMessage(
    `URL inspection for "${url}" will be available in a future update.`,
  );
}

// ---------------------------------------------------------------------------
// Command: showWaterfall
// ---------------------------------------------------------------------------

function showWaterfallCommand(context: vscode.ExtensionContext): void {
  if (waterfallPanel) {
    waterfallPanel.reveal(vscode.ViewColumn.One);
    return;
  }

  const config = vscode.workspace.getConfiguration('pagespeed');
  const apiUrl = getApiUrl(config);

  waterfallPanel = vscode.window.createWebviewPanel(
    'pagespeed.waterfall',
    'PageSpeed Waterfall',
    vscode.ViewColumn.One,
    {
      enableScripts: true,
      retainContextWhenHidden: true,
    },
  );

  const bridge = new WebviewBridge(
    waterfallPanel.webview,
    apiUrl,
    () => context.secrets.get(TOKEN_KEY),
  );

  waterfallPanel.webview.html = getFeatureHtml(
    waterfallPanel.webview,
    'Waterfall Viewer',
    'Capture and compare network waterfalls for any URL. ' +
      'Use side-by-side mode to see the impact of PageSpeed optimizations.',
  );

  waterfallPanel.onDidDispose(() => {
    bridge.dispose();
    waterfallPanel = undefined;
  });
}

// ---------------------------------------------------------------------------
// Command: showDiff
// ---------------------------------------------------------------------------

function showDiffCommand(context: vscode.ExtensionContext): void {
  if (diffPanel) {
    diffPanel.reveal(vscode.ViewColumn.One);
    return;
  }

  const config = vscode.workspace.getConfiguration('pagespeed');
  const apiUrl = getApiUrl(config);

  diffPanel = vscode.window.createWebviewPanel(
    'pagespeed.diff',
    'PageSpeed Visual Diff',
    vscode.ViewColumn.One,
    {
      enableScripts: true,
      retainContextWhenHidden: true,
    },
  );

  const bridge = new WebviewBridge(
    diffPanel.webview,
    apiUrl,
    () => context.secrets.get(TOKEN_KEY),
  );

  diffPanel.webview.html = getFeatureHtml(
    diffPanel.webview,
    'Visual Diff',
    'Compare original and optimized page screenshots with ' +
      'slider, blink, and pixel-diff modes.',
  );

  diffPanel.onDidDispose(() => {
    bridge.dispose();
    diffPanel = undefined;
  });
}

// ---------------------------------------------------------------------------
// Command: showConfig
// ---------------------------------------------------------------------------

function showConfigCommand(context: vscode.ExtensionContext): void {
  if (configPanel) {
    configPanel.reveal(vscode.ViewColumn.One);
    return;
  }

  const config = vscode.workspace.getConfiguration('pagespeed');
  const apiUrl = getApiUrl(config);

  configPanel = vscode.window.createWebviewPanel(
    'pagespeed.config',
    'PageSpeed Configuration',
    vscode.ViewColumn.One,
    {
      enableScripts: true,
      retainContextWhenHidden: true,
    },
  );

  const bridge = new WebviewBridge(
    configPanel.webview,
    apiUrl,
    () => context.secrets.get(TOKEN_KEY),
  );

  configPanel.webview.html = getFeatureHtml(
    configPanel.webview,
    'Configuration',
    'Tune worker settings with presets, quality sliders, and feature toggles. ' +
      'Export as .pagespeed.json or CLI flags.',
  );

  configPanel.onDidDispose(() => {
    bridge.dispose();
    configPanel = undefined;
  });
}

// ---------------------------------------------------------------------------
// Command: purgeUrlFromTree
// ---------------------------------------------------------------------------

async function purgeUrlFromTreeCommand(
  context: vscode.ExtensionContext,
  item?: { urlEntry?: { url: string; hostname: string; site?: string } },
): Promise<void> {
  const entry = item?.urlEntry;
  if (!entry) return;

  const confirm = await vscode.window.showWarningMessage(
    `Purge all cached variants for ${entry.url}?`,
    { modal: true },
    'Purge',
  );
  if (confirm !== 'Purge') return;

  try {
    const config = vscode.workspace.getConfiguration('pagespeed');
    const apiUrl = getApiUrl(config);
    const token = await context.secrets.get(TOKEN_KEY);

    const url = new URL('/v1/cache/purge', apiUrl);
    const headers: Record<string, string> = {
      'Content-Type': 'application/json',
      // CSRF: keep parity with the web SPA's DirectTransport so the worker's
      // CSRF check on mutating /v1/* requests (and any future tightening)
      // accepts requests from the VS Code extension too.
      'X-Requested-With': 'XMLHttpRequest',
    };
    if (token) {
      headers['Authorization'] = `Bearer ${token}`;
    }

    const res = await fetch(url.toString(), {
      method: 'POST',
      headers,
      body: JSON.stringify({
        url: entry.url,
        hostname: entry.hostname,
        ...(entry.site ? { site: entry.site } : {}),
      }),
    });

    if (!res.ok) {
      throw new Error(`HTTP ${res.status} ${res.statusText}`);
    }

    const data = (await res.json()) as CachePurgeResponse;
    void vscode.window.showInformationMessage(
      `Purged ${data.deleted} alternate(s) for ${entry.url}`,
    );
    urlTreeProvider.refresh();
  } catch (err) {
    const msg = err instanceof Error ? err.message : String(err);
    void vscode.window.showErrorMessage(`Purge failed: ${msg}`);
  }
}

// ---------------------------------------------------------------------------
// Command: reprocessUrlFromTree
// ---------------------------------------------------------------------------

async function reprocessUrlFromTreeCommand(
  context: vscode.ExtensionContext,
  item?: { urlEntry?: { url: string; hostname: string; site?: string } },
): Promise<void> {
  const entry = item?.urlEntry;
  if (!entry) return;

  const confirm = await vscode.window.showWarningMessage(
    `Purge and reprocess ${entry.url}?`,
    { modal: true },
    'Reprocess',
  );
  if (confirm !== 'Reprocess') return;

  try {
    const config = vscode.workspace.getConfiguration('pagespeed');
    const apiUrl = getApiUrl(config);
    const token = await context.secrets.get(TOKEN_KEY);

    const url = new URL('/v1/cache/reprocess', apiUrl);
    const headers: Record<string, string> = {
      'Content-Type': 'application/json',
      // CSRF: see purge handler above.
      'X-Requested-With': 'XMLHttpRequest',
    };
    if (token) {
      headers['Authorization'] = `Bearer ${token}`;
    }

    const res = await fetch(url.toString(), {
      method: 'POST',
      headers,
      body: JSON.stringify({
        url: entry.url,
        hostname: entry.hostname,
        ...(entry.site ? { site: entry.site } : {}),
      }),
    });

    if (!res.ok) {
      throw new Error(`HTTP ${res.status} ${res.statusText}`);
    }

    const data = (await res.json()) as CacheReprocessResponse;
    const msg = data.reprocess_enqueued
      ? `Reprocessing enqueued for ${entry.url}`
      : `Reprocess request sent for ${entry.url}`;
    void vscode.window.showInformationMessage(msg);
    urlTreeProvider.refresh();
  } catch (err) {
    const msg = err instanceof Error ? err.message : String(err);
    void vscode.window.showErrorMessage(`Reprocess failed: ${msg}`);
  }
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

function getApiUrl(config: vscode.WorkspaceConfiguration): string {
  return config.get<string>('apiUrl', 'http://127.0.0.1:9880');
}

/**
 * Returns placeholder HTML for the dashboard webview.  The full Svelte-based
 * dashboard will be loaded here once the shared UI components are built.
 */
function getDashboardHtml(webview: vscode.Webview): string {
  // Content Security Policy: only allow scripts from the extension itself.
  const nonce = getNonce();

  return `<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="UTF-8" />
  <meta
    http-equiv="Content-Security-Policy"
    content="default-src 'none'; style-src ${webview.cspSource} 'nonce-${nonce}'; script-src 'nonce-${nonce}';"
  />
  <meta name="viewport" content="width=device-width, initial-scale=1.0" />
  <title>PageSpeed Dashboard</title>
  <style nonce="${nonce}">
    body {
      font-family: var(--vscode-font-family);
      color: var(--vscode-foreground);
      background-color: var(--vscode-editor-background);
      padding: 16px;
      margin: 0;
    }
    h1 {
      font-size: 1.4em;
      font-weight: 600;
      margin: 0 0 12px;
    }
    p {
      color: var(--vscode-descriptionForeground);
      line-height: 1.5;
    }
    .placeholder {
      display: flex;
      flex-direction: column;
      align-items: center;
      justify-content: center;
      min-height: 200px;
      text-align: center;
    }
    .icon {
      font-size: 3em;
      margin-bottom: 12px;
      opacity: 0.5;
    }
  </style>
</head>
<body>
  <div class="placeholder">
    <div class="icon">&#9889;</div>
    <h1>PageSpeed Dashboard</h1>
    <p>
      The dashboard will display real-time stats, cache entries, and
      optimization metrics once the shared UI components are connected.
    </p>
    <p>
      Connected to the PageSpeed worker via the extension host bridge.
    </p>
  </div>
</body>
</html>`;
}

/**
 * Returns placeholder HTML for a feature webview panel.  Used for waterfall,
 * visual diff, and configuration panels until shared Svelte rendering is
 * connected.
 */
function getFeatureHtml(
  webview: vscode.Webview,
  title: string,
  description: string,
): string {
  const nonce = getNonce();

  return `<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="UTF-8" />
  <meta
    http-equiv="Content-Security-Policy"
    content="default-src 'none'; style-src ${webview.cspSource} 'nonce-${nonce}'; script-src 'nonce-${nonce}';"
  />
  <meta name="viewport" content="width=device-width, initial-scale=1.0" />
  <title>${title}</title>
  <style nonce="${nonce}">
    body {
      font-family: var(--vscode-font-family);
      color: var(--vscode-foreground);
      background-color: var(--vscode-editor-background);
      padding: 16px;
      margin: 0;
    }
    h1 {
      font-size: 1.4em;
      font-weight: 600;
      margin: 0 0 12px;
    }
    p {
      color: var(--vscode-descriptionForeground);
      line-height: 1.5;
    }
    .placeholder {
      display: flex;
      flex-direction: column;
      align-items: center;
      justify-content: center;
      min-height: 200px;
      text-align: center;
    }
    .icon {
      font-size: 3em;
      margin-bottom: 12px;
      opacity: 0.5;
    }
  </style>
</head>
<body>
  <div class="placeholder">
    <div class="icon">&#9889;</div>
    <h1>${title}</h1>
    <p>${description}</p>
    <p>
      Connected to the PageSpeed worker via the extension host bridge.
    </p>
  </div>
</body>
</html>`;
}

function getNonce(): string {
  return crypto.randomUUID();
}
