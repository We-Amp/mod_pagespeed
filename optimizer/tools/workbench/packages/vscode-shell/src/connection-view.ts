// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

import * as vscode from 'vscode';
import { formatUptime } from '@pagespeed/api-client';
import type { HealthResponse } from '@pagespeed/api-client';

// ---------------------------------------------------------------------------
// ConnectionTreeItem
// ---------------------------------------------------------------------------

class ConnectionTreeItem extends vscode.TreeItem {
  constructor(
    label: string,
    description?: string,
    icon?: string,
  ) {
    super(label, vscode.TreeItemCollapsibleState.None);
    this.description = description;
    if (icon) {
      this.iconPath = new vscode.ThemeIcon(icon);
    }
  }
}

// ---------------------------------------------------------------------------
// ConnectionState — data backing the tree
// ---------------------------------------------------------------------------

export interface ConnectionState {
  connected: boolean;
  apiUrl: string;
  health?: HealthResponse;
  error?: string;
}

// ---------------------------------------------------------------------------
// ConnectionViewProvider
// ---------------------------------------------------------------------------

/**
 * TreeDataProvider for the "pagespeed.connection" view.
 * Displays the current connection status and basic health information
 * from the PageSpeed worker.
 */
export class ConnectionViewProvider
  implements vscode.TreeDataProvider<ConnectionTreeItem>
{
  private readonly _onDidChangeTreeData =
    new vscode.EventEmitter<ConnectionTreeItem | undefined>();
  readonly onDidChangeTreeData = this._onDidChangeTreeData.event;

  private state: ConnectionState = {
    connected: false,
    apiUrl: '',
  };

  /** Update the displayed state and refresh the tree. */
  update(state: Partial<ConnectionState>): void {
    Object.assign(this.state, state);
    this._onDidChangeTreeData.fire(undefined);
  }

  getTreeItem(element: ConnectionTreeItem): ConnectionTreeItem {
    return element;
  }

  getChildren(): ConnectionTreeItem[] {
    const items: ConnectionTreeItem[] = [];

    if (this.state.connected && this.state.health) {
      const h = this.state.health;
      items.push(
        new ConnectionTreeItem(
          'Status',
          h.status,
          'circle-filled',
        ),
      );
      items.push(
        new ConnectionTreeItem(
          'URL',
          this.state.apiUrl,
          'link',
        ),
      );
      items.push(
        new ConnectionTreeItem(
          'Uptime',
          formatUptime(h.uptime_seconds),
          'clock',
        ),
      );
      items.push(
        new ConnectionTreeItem(
          'Connections',
          `${h.connections.active} / ${h.connections.max}`,
          'plug',
        ),
      );
      items.push(
        new ConnectionTreeItem(
          'In-flight',
          String(h.inflight),
          'loading~spin',
        ),
      );
    } else if (this.state.error) {
      items.push(
        new ConnectionTreeItem(
          'Status',
          'Disconnected',
          'circle-slash',
        ),
      );
      items.push(
        new ConnectionTreeItem(
          'Error',
          this.state.error,
          'error',
        ),
      );
    } else {
      items.push(
        new ConnectionTreeItem(
          'Status',
          'Disconnected',
          'circle-slash',
        ),
      );
      if (this.state.apiUrl) {
        items.push(
          new ConnectionTreeItem(
            'URL',
            this.state.apiUrl,
            'link',
          ),
        );
      }
    }

    return items;
  }
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// formatUptime imported from @pagespeed/api-client
