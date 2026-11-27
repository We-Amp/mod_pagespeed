// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

import { describe, it, expect, vi, beforeEach } from 'vitest';
import {
  createCacheClient,
  createCaptureClient,
} from '../client.js';
import type { ApiTransport } from '../transport.js';
import type {
  CacheUrlsResponse,
  CacheAlternatesResponse,
  CacheSelectResponse,
  CachePurgeResponse,
  CacheReprocessResponse,
  CooldownsResponse,
  CaptureWaterfallResponse,
  CaptureScreenshotResponse,
} from '../types.js';

// ---------------------------------------------------------------------------
// Mock transport
// ---------------------------------------------------------------------------

function createMockTransport(): ApiTransport {
  return {
    get: vi.fn(),
    patch: vi.fn(),
    post: vi.fn(),
    subscribe: vi.fn(() => () => {}),
    connected: { subscribe: vi.fn() } as never,
    error: { subscribe: vi.fn() } as never,
    connect: vi.fn(),
    disconnect: vi.fn(),
  };
}

let transport: ReturnType<typeof createMockTransport>;
let cache: ReturnType<typeof createCacheClient>;

beforeEach(() => {
  transport = createMockTransport();
  cache = createCacheClient(transport);
});

// ---------------------------------------------------------------------------
// getUrls
// ---------------------------------------------------------------------------

describe('getUrls', () => {
  it('calls GET /v1/cache/urls with no params', async () => {
    const response: CacheUrlsResponse = {
      total: 0,
      offset: 0,
      limit: 100,
      next_offset: 0,
      has_more: false,
      urls: [],
    };
    (transport.get as ReturnType<typeof vi.fn>).mockResolvedValueOnce(response);

    const result = await cache.getUrls();

    expect(transport.get).toHaveBeenCalledWith('/v1/cache/urls', undefined);
    expect(result).toEqual(response);
  });

  it('passes page and limit as string query params', async () => {
    const response: CacheUrlsResponse = {
      total: 200,
      offset: 2,
      limit: 50,
      next_offset: 52,
      has_more: true,
      urls: [],
    };
    (transport.get as ReturnType<typeof vi.fn>).mockResolvedValueOnce(response);

    await cache.getUrls({ offset: 2, limit: 50 });

    expect(transport.get).toHaveBeenCalledWith('/v1/cache/urls', {
      offset: '2',
      limit: '50',
    });
  });

  it('passes hostname as query param', async () => {
    const response: CacheUrlsResponse = {
      total: 10,
      offset: 0,
      limit: 100,
      next_offset: 10,
      has_more: false,
      urls: [],
    };
    (transport.get as ReturnType<typeof vi.fn>).mockResolvedValueOnce(response);

    await cache.getUrls({ hostname: 'example.com' });

    expect(transport.get).toHaveBeenCalledWith('/v1/cache/urls', {
      hostname: 'example.com',
    });
  });

  it('passes all params together', async () => {
    const response: CacheUrlsResponse = {
      total: 5,
      offset: 1,
      limit: 25,
      next_offset: 5,
      has_more: false,
      urls: [],
    };
    (transport.get as ReturnType<typeof vi.fn>).mockResolvedValueOnce(response);

    await cache.getUrls({ offset: 1, limit: 25, hostname: 'test.com' });

    expect(transport.get).toHaveBeenCalledWith('/v1/cache/urls', {
      offset: '1',
      limit: '25',
      hostname: 'test.com',
    });
  });
});

// ---------------------------------------------------------------------------
// getAlternates
// ---------------------------------------------------------------------------

describe('getAlternates', () => {
  it('calls GET /v1/cache/alternates with url and hostname', async () => {
    const response: CacheAlternatesResponse = {
      url: '/style.css',
      hostname: 'example.com',
      alternates: [],
      count: 0,
      chain_length: 0,
    };
    (transport.get as ReturnType<typeof vi.fn>).mockResolvedValueOnce(response);

    const result = await cache.getAlternates({
      url: '/style.css',
      hostname: 'example.com',
    });

    expect(transport.get).toHaveBeenCalledWith('/v1/cache/alternates', {
      url: '/style.css',
      hostname: 'example.com',
    });
    expect(result).toEqual(response);
  });
});

// ---------------------------------------------------------------------------
// selectAlternate
// ---------------------------------------------------------------------------

describe('selectAlternate', () => {
  it('calls GET /v1/cache/select with url, hostname, and mask as string', async () => {
    const response: CacheSelectResponse = {
      url: '/hero.jpg',
      hostname: 'example.com',
      client_mask: {
        raw: 10,
        format: 'avif',
        viewport: 'desktop',
        density: '1x',
        save_data: false,
        encoding: 'identity',
      },
      alternates: [
        {
          alternate_id: 10,
          is_sentinel: false,
          size: 42000,
          score: 1200,
          mask: {
            raw: 10,
            format: 'avif',
            viewport: 'desktop',
            density: '1x',
            save_data: false,
            encoding: 'identity',
          },
          content_type: 'image',
        },
      ],
      best_index: 0,
      best_score: 1200,
    };
    (transport.get as ReturnType<typeof vi.fn>).mockResolvedValueOnce(response);

    const result = await cache.selectAlternate({
      url: '/img.jpg',
      hostname: 'example.com',
      mask: 0x0a,
    });

    expect(transport.get).toHaveBeenCalledWith('/v1/cache/select', {
      url: '/img.jpg',
      hostname: 'example.com',
      mask: '10',
    });
    expect(result.best_score).toBe(1200);
    expect(result.best_index).toBe(0);
  });
});

// ---------------------------------------------------------------------------
// purgeUrl
// ---------------------------------------------------------------------------

describe('purgeUrl', () => {
  it('calls POST /v1/cache/purge with url and hostname body', async () => {
    const response: CachePurgeResponse = { url: '/index.html', hostname: 'example.com', deleted: 5 };
    (transport.post as ReturnType<typeof vi.fn>).mockResolvedValueOnce(
      response,
    );

    const result = await cache.purgeUrl({
      url: '/index.html',
      hostname: 'example.com',
    });

    expect(transport.post).toHaveBeenCalledWith('/v1/cache/purge', {
      url: '/index.html',
      hostname: 'example.com',
    });
    expect(result.deleted).toBe(5);
  });
});

// ---------------------------------------------------------------------------
// reprocessUrl
// ---------------------------------------------------------------------------

describe('reprocessUrl', () => {
  it('calls POST /v1/cache/reprocess with url and hostname body', async () => {
    const response: CacheReprocessResponse = {
      url: '/img.jpg',
      hostname: 'example.com',
      reprocess_enqueued: true,
    };
    (transport.post as ReturnType<typeof vi.fn>).mockResolvedValueOnce(
      response,
    );

    const result = await cache.reprocessUrl({
      url: '/img.jpg',
      hostname: 'example.com',
    });

    expect(transport.post).toHaveBeenCalledWith('/v1/cache/reprocess', {
      url: '/img.jpg',
      hostname: 'example.com',
    });
    expect(result.reprocess_enqueued).toBe(true);
  });
});

// ---------------------------------------------------------------------------
// getCooldowns
// ---------------------------------------------------------------------------

describe('getCooldowns', () => {
  it('calls GET /v1/cache/cooldowns with no params', async () => {
    const response: CooldownsResponse = {
      cooldowns: [],
      count: 0,
      enabled: true,
    };
    (transport.get as ReturnType<typeof vi.fn>).mockResolvedValueOnce(response);

    const result = await cache.getCooldowns();

    expect(transport.get).toHaveBeenCalledWith(
      '/v1/cache/cooldowns',
      undefined,
    );
    expect(result).toEqual(response);
  });

  it('passes hostname as query param', async () => {
    const response: CooldownsResponse = {
      cooldowns: [
        {
          url: '/page',
          hostname: 'example.com',
          reason: 'processing',
          remaining_seconds: 47,
          duration_seconds: 60,
        },
      ],
      count: 1,
      enabled: true,
    };
    (transport.get as ReturnType<typeof vi.fn>).mockResolvedValueOnce(response);

    await cache.getCooldowns({ hostname: 'example.com' });

    expect(transport.get).toHaveBeenCalledWith('/v1/cache/cooldowns', {
      hostname: 'example.com',
    });
  });
});

// ---------------------------------------------------------------------------
// Error propagation
// ---------------------------------------------------------------------------

describe('error propagation', () => {
  it('propagates transport errors from get methods', async () => {
    const err = new Error('Network failure');
    (transport.get as ReturnType<typeof vi.fn>).mockRejectedValueOnce(err);

    await expect(cache.getUrls()).rejects.toThrow('Network failure');
  });

  it('propagates transport errors from post methods', async () => {
    const err = new Error('Unauthorized');
    (transport.post as ReturnType<typeof vi.fn>).mockRejectedValueOnce(err);

    await expect(
      cache.purgeUrl({ url: '/x', hostname: 'h' }),
    ).rejects.toThrow('Unauthorized');
  });
});

// ===========================================================================
// createCaptureClient
// ===========================================================================

describe('createCaptureClient', () => {
  let captureTransport: ReturnType<typeof createMockTransport>;
  let capture: ReturnType<typeof createCaptureClient>;

  beforeEach(() => {
    captureTransport = createMockTransport();
    capture = createCaptureClient(captureTransport);
  });

  // -------------------------------------------------------------------------
  // captureWaterfall
  // -------------------------------------------------------------------------

  describe('captureWaterfall', () => {
    it('calls POST /v1/capture/waterfall with url body', async () => {
      const response: CaptureWaterfallResponse = {
        url: 'https://example.com/',
        viewport_width: 1280,
        entries: [],
        navigation_timing: {
          domContentLoaded: 100,
          loadEvent: 200,
          firstPaint: 50,
          firstContentfulPaint: 80,
          largestContentfulPaint: 150,
        },
        total_transfer_size: 0,
        total_decoded_size: 0,
      };
      (
        captureTransport.post as ReturnType<typeof vi.fn>
      ).mockResolvedValueOnce(response);

      const result = await capture.captureWaterfall({
        url: 'https://example.com/',
      });

      expect(captureTransport.post).toHaveBeenCalledWith(
        '/v1/capture/waterfall',
        { url: 'https://example.com/' },
      );
      expect(result).toEqual(response);
    });

    it('passes optional viewport_width and through_proxy', async () => {
      const response: CaptureWaterfallResponse = {
        url: 'https://example.com/',
        viewport_width: 480,
        entries: [],
        navigation_timing: {
          domContentLoaded: 0,
          loadEvent: 0,
          firstPaint: 0,
          firstContentfulPaint: 0,
          largestContentfulPaint: 0,
        },
        total_transfer_size: 0,
        total_decoded_size: 0,
      };
      (
        captureTransport.post as ReturnType<typeof vi.fn>
      ).mockResolvedValueOnce(response);

      await capture.captureWaterfall({
        url: 'https://example.com/',
        viewport_width: 480,
        through_proxy: true,
      });

      expect(captureTransport.post).toHaveBeenCalledWith(
        '/v1/capture/waterfall',
        {
          url: 'https://example.com/',
          viewport_width: 480,
          through_proxy: true,
        },
      );
    });

    it('propagates transport errors', async () => {
      const err = new Error('Chrome not available');
      (
        captureTransport.post as ReturnType<typeof vi.fn>
      ).mockRejectedValueOnce(err);

      await expect(
        capture.captureWaterfall({ url: 'https://example.com/' }),
      ).rejects.toThrow('Chrome not available');
    });
  });

  // -------------------------------------------------------------------------
  // captureScreenshot
  // -------------------------------------------------------------------------

  describe('captureScreenshot', () => {
    it('calls POST /v1/capture/screenshot with url body', async () => {
      const response: CaptureScreenshotResponse = {
        url: 'https://example.com/',
        viewport_width: 1280,
        viewport_height: 720,
        png_base64: 'iVBORw0KGgo=',
      };
      (
        captureTransport.post as ReturnType<typeof vi.fn>
      ).mockResolvedValueOnce(response);

      const result = await capture.captureScreenshot({
        url: 'https://example.com/',
      });

      expect(captureTransport.post).toHaveBeenCalledWith(
        '/v1/capture/screenshot',
        { url: 'https://example.com/' },
      );
      expect(result).toEqual(response);
    });

    it('passes optional parameters', async () => {
      const response: CaptureScreenshotResponse = {
        url: 'https://example.com/',
        viewport_width: 768,
        viewport_height: 1024,
        png_base64: 'abc=',
      };
      (
        captureTransport.post as ReturnType<typeof vi.fn>
      ).mockResolvedValueOnce(response);

      await capture.captureScreenshot({
        url: 'https://example.com/',
        viewport_width: 768,
        through_proxy: false,
        full_page: true,
      });

      expect(captureTransport.post).toHaveBeenCalledWith(
        '/v1/capture/screenshot',
        {
          url: 'https://example.com/',
          viewport_width: 768,
          through_proxy: false,
          full_page: true,
        },
      );
    });

    it('propagates transport errors', async () => {
      const err = new Error('Service Unavailable');
      (
        captureTransport.post as ReturnType<typeof vi.fn>
      ).mockRejectedValueOnce(err);

      await expect(
        capture.captureScreenshot({ url: 'https://example.com/' }),
      ).rejects.toThrow('Service Unavailable');
    });
  });
});

// ---------------------------------------------------------------------------
// site (per server block keyspace)
// ---------------------------------------------------------------------------

describe('site parameter', () => {
  const site = 'a'.repeat(64);

  it('getAlternates, selectAlternate and getContent send site when set', async () => {
    (transport.get as ReturnType<typeof vi.fn>).mockResolvedValue({});
    const getBlob = vi.fn().mockResolvedValue(new Response(''));
    (transport as { getBlob?: unknown }).getBlob = getBlob;

    await cache.getAlternates({ url: '/a', hostname: 'h', site });
    expect(transport.get).toHaveBeenLastCalledWith('/v1/cache/alternates', {
      url: '/a',
      hostname: 'h',
      site,
    });

    await cache.selectAlternate({ url: '/a', hostname: 'h', mask: 3, site });
    expect(transport.get).toHaveBeenLastCalledWith('/v1/cache/select', {
      url: '/a',
      hostname: 'h',
      mask: '3',
      site,
    });

    await cache.getContent({ url: '/a', hostname: 'h', alternate_id: 7, site });
    expect(getBlob).toHaveBeenLastCalledWith('/v1/cache/content', {
      url: '/a',
      hostname: 'h',
      alternate_id: '7',
      site,
    });
  });

  it('omits site when empty or absent', async () => {
    (transport.get as ReturnType<typeof vi.fn>).mockResolvedValue({});
    await cache.getAlternates({ url: '/a', hostname: 'h', site: '' });
    expect(transport.get).toHaveBeenLastCalledWith('/v1/cache/alternates', {
      url: '/a',
      hostname: 'h',
    });
    await cache.selectAlternate({ url: '/a', hostname: 'h', mask: 1 });
    expect(transport.get).toHaveBeenLastCalledWith('/v1/cache/select', {
      url: '/a',
      hostname: 'h',
      mask: '1',
    });
  });

  it('purgeUrl and reprocessUrl send site only when set', async () => {
    (transport.post as ReturnType<typeof vi.fn>).mockResolvedValue({});
    await cache.purgeUrl({ url: '/a', hostname: 'h', site });
    expect(transport.post).toHaveBeenLastCalledWith('/v1/cache/purge', {
      url: '/a',
      hostname: 'h',
      site,
    });
    await cache.reprocessUrl({ url: '/a', hostname: 'h', site: '' });
    expect(transport.post).toHaveBeenLastCalledWith('/v1/cache/reprocess', {
      url: '/a',
      hostname: 'h',
    });
  });
});
