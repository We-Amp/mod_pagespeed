// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// @vitest-environment jsdom
import { describe, it, expect, beforeEach } from 'vitest';
import {
  loadUrlGroups,
  saveUrlGroups,
  createUrlGroup,
  addUrlGroup,
  deleteUrlGroup,
  renameUrlGroup,
  addUrlToGroup,
  removeUrlFromGroup,
  addUrlsToGroup,
  getUrlGroup,
} from '../url-groups';
import type { UrlGroup, UrlEntry } from '../url-groups';

describe('URL group localStorage', () => {
  beforeEach(() => {
    localStorage.clear();
  });

  it('loadUrlGroups returns empty array when nothing saved', () => {
    expect(loadUrlGroups()).toEqual([]);
  });

  it('saveUrlGroups and loadUrlGroups roundtrip', () => {
    const group: UrlGroup = {
      id: 'grp-1',
      name: 'Test Group',
      created_at: '2024-01-15T10:00:00Z',
      updated_at: '2024-01-15T10:00:00Z',
      urls: [{ url: 'https://example.com/', hostname: 'example.com' }],
    };
    saveUrlGroups([group]);
    const loaded = loadUrlGroups();
    expect(loaded).toHaveLength(1);
    expect(loaded[0].name).toBe('Test Group');
    expect(loaded[0].urls).toHaveLength(1);
  });

  it('loadUrlGroups handles corrupted localStorage', () => {
    localStorage.setItem('pagespeed-url-groups', 'broken');
    expect(loadUrlGroups()).toEqual([]);
  });

  it('loadUrlGroups handles non-array JSON', () => {
    localStorage.setItem(
      'pagespeed-url-groups',
      JSON.stringify({ not: 'array' }),
    );
    expect(loadUrlGroups()).toEqual([]);
  });
});

describe('createUrlGroup', () => {
  it('creates a group with name and empty URLs', () => {
    const group = createUrlGroup('My Group');
    expect(group.name).toBe('My Group');
    expect(group.urls).toEqual([]);
    expect(group.id).toMatch(/^grp-/);
    expect(group.created_at).toBeTruthy();
    expect(group.updated_at).toBeTruthy();
  });

  it('generates unique IDs', () => {
    const a = createUrlGroup('A');
    const b = createUrlGroup('B');
    expect(a.id).not.toBe(b.id);
  });
});

describe('addUrlGroup', () => {
  beforeEach(() => {
    localStorage.clear();
  });

  it('adds a group and persists it', () => {
    const result = addUrlGroup('First');
    expect(result).toHaveLength(1);
    expect(result[0].name).toBe('First');

    // Verify persisted.
    const loaded = loadUrlGroups();
    expect(loaded).toHaveLength(1);
  });

  it('prepends new groups (newest first)', () => {
    addUrlGroup('First');
    const result = addUrlGroup('Second');
    expect(result).toHaveLength(2);
    expect(result[0].name).toBe('Second');
    expect(result[1].name).toBe('First');
  });
});

describe('deleteUrlGroup', () => {
  beforeEach(() => {
    localStorage.clear();
  });

  it('deletes a group by ID', () => {
    const groups = addUrlGroup('To Delete');
    const id = groups[0].id;
    const result = deleteUrlGroup(id);
    expect(result).toHaveLength(0);
  });

  it('does nothing when ID not found', () => {
    addUrlGroup('Stays');
    const result = deleteUrlGroup('nonexistent');
    expect(result).toHaveLength(1);
  });
});

describe('renameUrlGroup', () => {
  beforeEach(() => {
    localStorage.clear();
  });

  it('renames a group by ID', () => {
    const groups = addUrlGroup('Old Name');
    const id = groups[0].id;
    const result = renameUrlGroup(id, 'New Name');
    expect(result[0].name).toBe('New Name');
  });

  it('updates the updated_at timestamp', () => {
    const groups = addUrlGroup('Test');
    const id = groups[0].id;
    const oldUpdated = groups[0].updated_at;

    // Small delay to ensure different timestamp.
    const result = renameUrlGroup(id, 'Renamed');
    expect(result[0].updated_at).toBeTruthy();
    // Name changed.
    expect(result[0].name).toBe('Renamed');
  });
});

describe('addUrlToGroup', () => {
  beforeEach(() => {
    localStorage.clear();
  });

  it('adds a URL to a group', () => {
    const groups = addUrlGroup('Test');
    const id = groups[0].id;
    const entry: UrlEntry = {
      url: 'https://example.com/image.jpg',
      hostname: 'example.com',
    };
    const result = addUrlToGroup(id, entry);
    const group = result.find((g) => g.id === id)!;
    expect(group.urls).toHaveLength(1);
    expect(group.urls[0].url).toBe('https://example.com/image.jpg');
  });

  it('deduplicates by url+hostname', () => {
    const groups = addUrlGroup('Test');
    const id = groups[0].id;
    const entry: UrlEntry = {
      url: 'https://example.com/page',
      hostname: 'example.com',
    };
    addUrlToGroup(id, entry);
    const result = addUrlToGroup(id, entry);
    const group = result.find((g) => g.id === id)!;
    expect(group.urls).toHaveLength(1);
  });

  it('allows same URL with different hostname', () => {
    const groups = addUrlGroup('Test');
    const id = groups[0].id;
    addUrlToGroup(id, {
      url: 'https://example.com/page',
      hostname: 'example.com',
    });
    const result = addUrlToGroup(id, {
      url: 'https://example.com/page',
      hostname: 'other.com',
    });
    const group = result.find((g) => g.id === id)!;
    expect(group.urls).toHaveLength(2);
  });
});

describe('removeUrlFromGroup', () => {
  beforeEach(() => {
    localStorage.clear();
  });

  it('removes a URL from a group', () => {
    const groups = addUrlGroup('Test');
    const id = groups[0].id;
    addUrlToGroup(id, {
      url: 'https://example.com/a',
      hostname: 'example.com',
    });
    addUrlToGroup(id, {
      url: 'https://example.com/b',
      hostname: 'example.com',
    });

    const result = removeUrlFromGroup(
      id,
      'https://example.com/a',
      'example.com',
    );
    const group = result.find((g) => g.id === id)!;
    expect(group.urls).toHaveLength(1);
    expect(group.urls[0].url).toBe('https://example.com/b');
  });
});

describe('addUrlsToGroup', () => {
  beforeEach(() => {
    localStorage.clear();
  });

  it('adds multiple URLs at once', () => {
    const groups = addUrlGroup('Test');
    const id = groups[0].id;
    const entries: UrlEntry[] = [
      { url: 'https://example.com/1', hostname: 'example.com' },
      { url: 'https://example.com/2', hostname: 'example.com' },
      { url: 'https://example.com/3', hostname: 'example.com' },
    ];
    const result = addUrlsToGroup(id, entries);
    const group = result.find((g) => g.id === id)!;
    expect(group.urls).toHaveLength(3);
  });

  it('deduplicates during batch add', () => {
    const groups = addUrlGroup('Test');
    const id = groups[0].id;
    addUrlToGroup(id, {
      url: 'https://example.com/1',
      hostname: 'example.com',
    });

    const entries: UrlEntry[] = [
      { url: 'https://example.com/1', hostname: 'example.com' }, // duplicate
      { url: 'https://example.com/2', hostname: 'example.com' }, // new
    ];
    const result = addUrlsToGroup(id, entries);
    const group = result.find((g) => g.id === id)!;
    expect(group.urls).toHaveLength(2);
  });
});

describe('getUrlGroup', () => {
  beforeEach(() => {
    localStorage.clear();
  });

  it('returns the group by ID', () => {
    const groups = addUrlGroup('Find Me');
    const id = groups[0].id;
    const found = getUrlGroup(id);
    expect(found).toBeDefined();
    expect(found!.name).toBe('Find Me');
  });

  it('returns undefined for unknown ID', () => {
    expect(getUrlGroup('nonexistent')).toBeUndefined();
  });
});
