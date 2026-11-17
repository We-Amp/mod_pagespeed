// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

import * as vscode from 'vscode';
import type {
  CacheUrlEntry,
  CacheUrlsResponse,
  AlternateInfo,
  CacheAlternatesResponse,
} from '@pagespeed/api-client';
import { formatBytes } from '@pagespeed/api-client';

// ---------------------------------------------------------------------------
// fetchApi — lightweight HTTP helper for the extension host
// ---------------------------------------------------------------------------

async function fetchApi<T>(
  path: string,
  baseUrl: string,
  token?: string,
): Promise<T> {
  const url = new URL(path, baseUrl);
  const headers: Record<string, string> = {
    Accept: 'application/json',
  };
  if (token) {
    headers['Authorization'] = `Bearer ${token}`;
  }
  const resp = await fetch(url.toString(), { headers });
  if (!resp.ok) {
    throw new Error(`API error: ${resp.status}`);
  }
  return resp.json() as Promise<T>;
}

// ---------------------------------------------------------------------------
// UrlTreeItem
// ---------------------------------------------------------------------------

class UrlTreeItem extends vscode.TreeItem {
  /** The URL entry (set only on URL-level items). */
  readonly urlEntry?: CacheUrlEntry;
  /** The alternate info (set only on alternate-level items). */
  readonly alternateInfo?: AlternateInfo;

  constructor(options: {
    label: string;
    description?: string;
    collapsibleState: vscode.TreeItemCollapsibleState;
    icon: string;
    contextValue: string;
    urlEntry?: CacheUrlEntry;
    alternateInfo?: AlternateInfo;
    tooltip?: string;
  }) {
    super(options.label, options.collapsibleState);
    this.description = options.description;
    this.iconPath = new vscode.ThemeIcon(options.icon);
    this.contextValue = options.contextValue;
    this.urlEntry = options.urlEntry;
    this.alternateInfo = options.alternateInfo;
    if (options.tooltip) {
      this.tooltip = options.tooltip;
    }
  }
}

// ---------------------------------------------------------------------------
// Icon selection for alternates
// ---------------------------------------------------------------------------

function alternateIcon(alt: AlternateInfo): string {
  const ct = (alt.content_type ?? '').toLowerCase();
  if (ct.startsWith('text/html')) return 'file-code';
  if (ct.startsWith('image/')) return 'file-media';
  if (ct.startsWith('text/css') || ct.includes('javascript')) {
    return 'file-code';
  }
  return 'file';
}

// ---------------------------------------------------------------------------
// UrlTreeViewProvider
// ---------------------------------------------------------------------------

/**
 * TreeDataProvider for the "pagespeed.urls" view.
 * Displays cached URLs at the top level and their alternates as children.
 */
export class UrlTreeViewProvider
  implements vscode.TreeDataProvider<UrlTreeItem>
{
  private readonly _onDidChangeTreeData =
    new vscode.EventEmitter<UrlTreeItem | undefined>();
  readonly onDidChangeTreeData = this._onDidChangeTreeData.event;

  constructor(
    private readonly apiUrl: string,
    private readonly getToken: () => Promise<string | undefined>,
  ) {}

  /** Force a full refresh of the tree. */
  refresh(): void {
    this._onDidChangeTreeData.fire(undefined);
  }

  getTreeItem(element: UrlTreeItem): UrlTreeItem {
    return element;
  }

  async getChildren(element?: UrlTreeItem): Promise<UrlTreeItem[]> {
    if (!element) {
      return this.getRootChildren();
    }
    if (element.urlEntry) {
      return this.getAlternateChildren(element.urlEntry);
    }
    return [];
  }

  // -- Root level: list cached URLs -----------------------------------------

  private async getRootChildren(): Promise<UrlTreeItem[]> {
    try {
      const token = await this.getToken();
      const data = await fetchApi<CacheUrlsResponse>(
        '/v1/cache/urls?limit=100',
        this.apiUrl,
        token,
      );
      return data.urls.map((entry) => {
        // Extract the path portion from the full URL for the label.
        let label: string;
        try {
          const parsed = new URL(entry.url);
          label = parsed.pathname + parsed.search;
        } catch {
          label = entry.url;
        }

        const altWord =
          entry.alternate_count === 1 ? 'alternate' : 'alternates';

        return new UrlTreeItem({
          label,
          description: `${entry.hostname} (${entry.alternate_count} ${altWord})`,
          collapsibleState:
            vscode.TreeItemCollapsibleState.Collapsed,
          icon: 'globe',
          contextValue: 'url',
          urlEntry: entry,
          tooltip: `${entry.url}\nHost: ${entry.hostname}\nAlternates: ${entry.alternate_count}`,
        });
      });
    } catch {
      // Not connected or API error — return empty tree without crashing.
      return [];
    }
  }

  // -- Second level: alternates for a URL -----------------------------------

  private async getAlternateChildren(
    entry: CacheUrlEntry,
  ): Promise<UrlTreeItem[]> {
    try {
      const token = await this.getToken();
      const params = new URLSearchParams({
        url: entry.url,
        hostname: entry.hostname,
      });
      if (entry.site) params.set('site', entry.site);
      const data = await fetchApi<CacheAlternatesResponse>(
        `/v1/cache/alternates?${params.toString()}`,
        this.apiUrl,
        token,
      );
      return data.alternates.map((alt) => {
        const parts: string[] = [];
        if (alt.format) parts.push(alt.format);
        if (alt.viewport) parts.push(alt.viewport);
        if (alt.density) parts.push(alt.density);
        if (alt.encoding && alt.encoding !== 'Identity') {
          parts.push(alt.encoding);
        }
        if (alt.save_data) parts.push('Save-Data');
        if (alt.is_sentinel) parts.push('[sentinel]');

        const label = parts.length > 0 ? parts.join(' / ') : `alt ${alt.alternate_id}`;
        const hitWord = alt.hit_count === 1 ? 'hit' : 'hits';
        const description = `${formatBytes(alt.size)} (${alt.hit_count} ${hitWord})`;

        return new UrlTreeItem({
          label,
          description,
          collapsibleState: vscode.TreeItemCollapsibleState.None,
          icon: alternateIcon(alt),
          contextValue: 'alternate',
          alternateInfo: alt,
          tooltip: [
            `Content-Type: ${alt.content_type}`,
            `Origin Content-Type: ${alt.origin_content_type}`,
            `Alternate ID: ${alt.alternate_id}`,
            `Size: ${formatBytes(alt.size)}`,
            `Hits: ${alt.hit_count}`,
          ].join('\n'),
        });
      });
    } catch {
      return [];
    }
  }
}
