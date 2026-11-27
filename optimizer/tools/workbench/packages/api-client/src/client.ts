// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// ---------------------------------------------------------------------------
// Typed cache client wrapping the shell-agnostic ApiTransport.
// ---------------------------------------------------------------------------

import type { ApiTransport } from './transport.js';
import type {
  CacheUrlsResponse,
  CacheAlternatesResponse,
  CacheSelectResponse,
  CachePurgeResponse,
  CachePurgeAllResponse,
  CacheReprocessResponse,
  CooldownsResponse,
  CaptureWaterfallRequest,
  CaptureWaterfallResponse,
  CaptureScreenshotRequest,
  CaptureScreenshotResponse,
} from './types.js';

/** Drop an empty `site` so unsited requests stay byte-identical to before. */
function withSite<T extends { site?: string }>(body: T): T {
  if (body.site) return body;
  const { site: _site, ...rest } = body;
  return rest as T;
}

/**
 * Create a cache client that provides typed methods for every cache endpoint.
 *
 * Usage:
 * ```ts
 * const cache = createCacheClient(transport);
 * const urls = await cache.getUrls({ offset: 0, limit: 50 });
 * ```
 */
export function createCacheClient(transport: ApiTransport) {
  return {
    /**
     * GET /v1/cache/urls — list cached URLs with pagination.
     */
    getUrls(
      params?: { offset?: number; limit?: number; hostname?: string },
    ): Promise<CacheUrlsResponse> {
      const query: Record<string, string> = {};
      if (params?.offset !== undefined) query.offset = String(params.offset);
      if (params?.limit !== undefined) query.limit = String(params.limit);
      if (params?.hostname !== undefined) query.hostname = params.hostname;
      return transport.get<CacheUrlsResponse>(
        '/v1/cache/urls',
        Object.keys(query).length > 0 ? query : undefined,
      );
    },

    /**
     * GET /v1/cache/alternates — list alternates for a given URL+hostname.
     */
    getAlternates(params: {
      url: string;
      hostname: string;
      scheme?: string;
      site?: string;
    }): Promise<CacheAlternatesResponse> {
      const query: Record<string, string> = {
        url: params.url,
        hostname: params.hostname,
      };
      if (params.scheme) query.scheme = params.scheme;
      if (params.site) query.site = params.site;
      return transport.get<CacheAlternatesResponse>('/v1/cache/alternates', query);
    },

    /**
     * GET /v1/cache/select — select the best alternate for a capability mask.
     */
    selectAlternate(params: {
      url: string;
      hostname: string;
      mask: number;
      scheme?: string;
      site?: string;
    }): Promise<CacheSelectResponse> {
      const query: Record<string, string> = {
        url: params.url,
        hostname: params.hostname,
        mask: String(params.mask),
      };
      if (params.scheme) query.scheme = params.scheme;
      if (params.site) query.site = params.site;
      return transport.get<CacheSelectResponse>('/v1/cache/select', query);
    },

    /**
     * POST /v1/cache/purge — purge all alternates for a URL+hostname.
     */
    purgeUrl(body: {
      url: string;
      hostname: string;
      scheme?: string;
      site?: string;
    }): Promise<CachePurgeResponse> {
      return transport.post<CachePurgeResponse>('/v1/cache/purge', withSite(body));
    },

    /**
     * POST /v1/cache/purge — purge all cached content.
     */
    purgeAll(): Promise<CachePurgeAllResponse> {
      return transport.post<CachePurgeAllResponse>('/v1/cache/purge', {
        scope: 'all',
        confirm: 'purge-all',
      });
    },

    /**
     * POST /v1/cache/reprocess — purge and re-queue processing for a URL.
     */
    reprocessUrl(body: {
      url: string;
      hostname: string;
      scheme?: string;
      site?: string;
    }): Promise<CacheReprocessResponse> {
      return transport.post<CacheReprocessResponse>(
        '/v1/cache/reprocess',
        withSite(body),
      );
    },

    /**
     * GET /v1/cache/content — fetch raw alternate content as a Response.
     * Returns the raw Response so callers can create blob URLs or read text.
     * Requires transport.getBlob support (DirectTransport only).
     */
    getContent(params: {
      url: string;
      hostname: string;
      alternate_id: number;
      scheme?: string;
      site?: string;
    }): Promise<Response> {
      if (!transport.getBlob) {
        return Promise.reject(
          new Error('getContent requires a transport with getBlob support'),
        );
      }
      const query: Record<string, string> = {
        url: params.url,
        hostname: params.hostname,
        alternate_id: String(params.alternate_id),
      };
      if (params.scheme) query.scheme = params.scheme;
      if (params.site) query.site = params.site;
      return transport.getBlob('/v1/cache/content', query);
    },

    /**
     * GET /v1/cache/cooldowns — list active HTML processing cooldowns.
     */
    getCooldowns(
      params?: { hostname?: string },
    ): Promise<CooldownsResponse> {
      const query: Record<string, string> = {};
      if (params?.hostname !== undefined) query.hostname = params.hostname;
      return transport.get<CooldownsResponse>(
        '/v1/cache/cooldowns',
        Object.keys(query).length > 0 ? query : undefined,
      );
    },
  };
}

/**
 * Create a capture client for waterfall and screenshot endpoints.
 *
 * Usage:
 * ```ts
 * const capture = createCaptureClient(transport);
 * const wf = await capture.captureWaterfall({ url: 'https://example.com' });
 * ```
 */
export function createCaptureClient(transport: ApiTransport) {
  return {
    /**
     * POST /v1/capture/waterfall — capture a network waterfall for a URL.
     */
    captureWaterfall(
      body: CaptureWaterfallRequest,
    ): Promise<CaptureWaterfallResponse> {
      return transport.post<CaptureWaterfallResponse>(
        '/v1/capture/waterfall',
        body,
      );
    },

    /**
     * POST /v1/capture/screenshot — capture a screenshot for a URL.
     */
    captureScreenshot(
      body: CaptureScreenshotRequest,
    ): Promise<CaptureScreenshotResponse> {
      return transport.post<CaptureScreenshotResponse>(
        '/v1/capture/screenshot',
        body,
      );
    },
  };
}
