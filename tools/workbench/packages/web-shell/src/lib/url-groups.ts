// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// ---------------------------------------------------------------------------
// URL Group management — named collections of URLs for batch inspection.
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Types
// ---------------------------------------------------------------------------

export interface UrlEntry {
  url: string;
  hostname: string;
  /** Cache keyspace of the entry; absent for unsited entries. */
  site?: string;
}

export interface UrlGroup {
  /** Unique identifier (timestamp-based). */
  id: string;
  /** User-provided group name. */
  name: string;
  /** ISO-8601 timestamp when the group was created. */
  created_at: string;
  /** ISO-8601 timestamp of the last modification. */
  updated_at: string;
  /** URLs in this group. */
  urls: UrlEntry[];
}

/** Summary row for batch inspection results. */
export interface UrlGroupSummaryEntry {
  url: string;
  hostname: string;
  alternate_count: number;
  total_size: number;
  original_size: number;
  savings: number;
}

// ---------------------------------------------------------------------------
// localStorage persistence
// ---------------------------------------------------------------------------

const URL_GROUPS_STORAGE_KEY = 'pagespeed-url-groups';

/**
 * Load all URL groups from localStorage.
 */
export function loadUrlGroups(): UrlGroup[] {
  if (typeof window === 'undefined') return [];
  try {
    const raw = localStorage.getItem(URL_GROUPS_STORAGE_KEY);
    if (!raw) return [];
    const parsed = JSON.parse(raw);
    if (!Array.isArray(parsed)) return [];
    return parsed as UrlGroup[];
  } catch {
    return [];
  }
}

/**
 * Save all URL groups to localStorage.
 */
export function saveUrlGroups(groups: UrlGroup[]): void {
  if (typeof window === 'undefined') return;
  localStorage.setItem(URL_GROUPS_STORAGE_KEY, JSON.stringify(groups));
}

// ---------------------------------------------------------------------------
// CRUD operations
// ---------------------------------------------------------------------------

/**
 * Create a new empty URL group.
 */
export function createUrlGroup(name: string): UrlGroup {
  const now = new Date().toISOString();
  return {
    id: `grp-${Date.now()}-${Math.random().toString(36).slice(2, 8)}`,
    name,
    created_at: now,
    updated_at: now,
    urls: [],
  };
}

/**
 * Add a new URL group and persist. Returns the updated group list.
 */
export function addUrlGroup(name: string): UrlGroup[] {
  const groups = loadUrlGroups();
  const group = createUrlGroup(name);
  groups.unshift(group);
  saveUrlGroups(groups);
  return groups;
}

/**
 * Delete a URL group by ID and persist.
 */
export function deleteUrlGroup(id: string): UrlGroup[] {
  const groups = loadUrlGroups().filter((g) => g.id !== id);
  saveUrlGroups(groups);
  return groups;
}

/**
 * Rename a URL group and persist.
 */
export function renameUrlGroup(id: string, newName: string): UrlGroup[] {
  const groups = loadUrlGroups();
  const group = groups.find((g) => g.id === id);
  if (group) {
    group.name = newName;
    group.updated_at = new Date().toISOString();
  }
  saveUrlGroups(groups);
  return groups;
}

/**
 * Add a URL to a group. Deduplicates by url+hostname+site.
 */
export function addUrlToGroup(
  groupId: string,
  entry: UrlEntry,
): UrlGroup[] {
  const groups = loadUrlGroups();
  const group = groups.find((g) => g.id === groupId);
  if (group) {
    const exists = group.urls.some(
      (u) =>
        u.url === entry.url &&
        u.hostname === entry.hostname &&
        (u.site ?? '') === (entry.site ?? ''),
    );
    if (!exists) {
      group.urls.push({ ...entry });
      group.updated_at = new Date().toISOString();
    }
  }
  saveUrlGroups(groups);
  return groups;
}

/**
 * Remove a URL from a group by url+hostname.
 */
export function removeUrlFromGroup(
  groupId: string,
  url: string,
  hostname: string,
): UrlGroup[] {
  const groups = loadUrlGroups();
  const group = groups.find((g) => g.id === groupId);
  if (group) {
    group.urls = group.urls.filter(
      (u) => !(u.url === url && u.hostname === hostname),
    );
    group.updated_at = new Date().toISOString();
  }
  saveUrlGroups(groups);
  return groups;
}

/**
 * Add multiple URLs to a group at once.
 */
export function addUrlsToGroup(
  groupId: string,
  entries: UrlEntry[],
): UrlGroup[] {
  const groups = loadUrlGroups();
  const group = groups.find((g) => g.id === groupId);
  if (group) {
    let modified = false;
    for (const entry of entries) {
      const exists = group.urls.some(
        (u) =>
          u.url === entry.url &&
          u.hostname === entry.hostname &&
          (u.site ?? '') === (entry.site ?? ''),
      );
      if (!exists) {
        group.urls.push({ ...entry });
        modified = true;
      }
    }
    if (modified) {
      group.updated_at = new Date().toISOString();
    }
  }
  saveUrlGroups(groups);
  return groups;
}

/**
 * Get a single URL group by ID.
 */
export function getUrlGroup(id: string): UrlGroup | undefined {
  return loadUrlGroups().find((g) => g.id === id);
}
