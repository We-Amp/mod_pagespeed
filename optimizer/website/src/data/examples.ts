// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// Catalog for the live optimization-examples gallery (/examples/).
//
// Each entry maps to a hand-crafted demo page served by the module on its
// Apache demo host, demo-httpd-1.1.modpagespeed.com. The gallery frames that
// page twice:
//   before → <page>?PageSpeed=off                  (original, optimization off)
//   after  → <page>?PageSpeedFilters=<filters>     (only the listed filter(s))
//
// The `filters` string is the exact mod_pagespeed filter set the original
// open-source mod_pagespeed_example used to isolate each optimization, so the
// "after" frame shows that filter and (mostly) only that filter.
//
// Source of truth for the gallery. The build-time generator
// (tools/examples-gallery/generate.mjs) reads this catalog, fetches before/after
// from the live demo, and writes the measured impact + source diff into
// examples-data.json. Astro pages read both.

export const DEMO_ORIGIN = 'https://demo-httpd-1.1.modpagespeed.com';
export const DEMO_BASE = `${DEMO_ORIGIN}/mod_pagespeed_example`;

export type ExampleCategory = 'CSS' | 'JavaScript' | 'Images' | 'Caching' | 'HTML' | 'Resources';

export interface Example {
  /** URL slug: /examples/<slug>/ */
  slug: string;
  /** Display title. */
  title: string;
  /** Category for grouping on the index. */
  category: ExampleCategory;
  /** Demo page path relative to DEMO_BASE (no leading slash). */
  page: string;
  /** Exact PageSpeedFilters value applied for the "after" frame. */
  filters: string;
  /** One-line, plain-language explanation (brand voice: direct, honest). */
  blurb: string;
  /** Optional caveat shown on the detail page (e.g. depends on a third-party service). */
  note?: string;
  /** Release the example first shipped in (e.g. '1.15.0-r20'); drives a "New" badge in the gallery. */
  addedIn?: string;
  /**
   * Member filters of a compound name in `filters` that the demo visibly
   * exercises, checked against the measured output in examples-data.json
   * (e.g. a JPEG served as WebP). Lists the demo on those filters' topic pages.
   */
  shows?: string[];
}

/**
 * How an optimization manifests when it can't be shown as a server-side HTML
 * diff. Drives the detail-page note + a live-iframe caveat (see [slug].astro):
 *  - 'beacon': browser-gated — mod_pagespeed instruments the page, a real
 *    browser reports back, then it serves the optimized form and re-measures
 *    periodically (~5 min). No diff is capturable over HTTP, and the optimized
 *    frame can briefly revert to the original after a re-instrument.
 *  - 'header': works at the HTTP response-header / document-head hint level, so
 *    it leaves no change in the page body to diff.
 *  - 'browser': applies only to browsers mod_pagespeed recognizes as
 *    supporting it (a user-agent check), so a plain HTTP capture gets the
 *    original page back.
 *  - 'config': needs configuration or content the public demo doesn't provide
 *    (e.g. an Analytics account id, a recognized JS library).
 */
export type Mechanism = 'beacon' | 'header' | 'browser' | 'config';

export const MECHANISM: Record<string, Mechanism> = {
  lazyload_images: 'browser',
  defer_javascript: 'browser',
  resize_rendered_image_dimensions: 'beacon',
  inline_preview_images: 'beacon',
  prioritize_critical_css: 'beacon',
  hint_preload_subresources: 'header',
  insert_dns_prefetch: 'browser',
  prioritize_critical_images: 'beacon',
  canonicalize_javascript_libraries: 'config',
};

export const CATEGORY_ORDER: ExampleCategory[] = [
  'Images',
  'CSS',
  'JavaScript',
  'HTML',
  'Caching',
  'Resources',
];

export const CATEGORY_BLURB: Record<ExampleCategory, string> = {
  Images: 'Recompress, resize, inline, sprite, and lazy-load images — usually the largest win.',
  CSS: 'Minify, combine, inline, and reorder stylesheets to cut requests and unblock rendering.',
  JavaScript:
    'Minify, combine, inline, and defer scripts to send fewer bytes and unblock rendering.',
  HTML: 'Trim the markup itself — whitespace, comments, redundant attributes and quotes.',
  Caching: 'Make resources cacheable forever with content-hashed URLs.',
  Resources: 'Resource hints and URL rewriting that start fetches earlier and cut round-trips.',
};

export const EXAMPLES: Example[] = [
  // ---------------------------------------------------------------- Images
  {
    slug: 'rewrite_images',
    title: 'Optimize images',
    category: 'Images',
    page: 'rewrite_images.html',
    filters: 'rewrite_images,inline_images,resize_images,insert_image_dimensions',
    blurb: 'Recompresses images, resizes them to their display size, and inlines small ones.',
    shows: ['convert_jpeg_to_webp'],
  },
  {
    slug: 'convert_jpeg_to_avif',
    title: 'Convert JPEG to AVIF',
    category: 'Images',
    page: 'rewrite_images.html',
    filters: 'convert_jpeg_to_avif,rewrite_images',
    blurb: 'Re-encodes photographic JPEGs as AVIF for browsers that accept it.',
    addedIn: '1.15.0-r20',
  },
  {
    slug: 'convert_to_avif_animated',
    title: 'Convert animated GIF to AVIF',
    category: 'Images',
    page: 'convert_to_avif_animated.html',
    filters: 'convert_to_avif_animated',
    blurb: 'Re-encodes animated GIFs as animated AVIF for browsers that accept it.',
    addedIn: '1.15.0-r20',
  },
  {
    slug: 'prioritize_critical_images',
    title: 'Prioritize critical images',
    category: 'Images',
    page: 'prioritize_critical_images.html',
    filters: 'prioritize_critical_images',
    blurb:
      'Marks the largest above-the-fold image fetchpriority=high so the browser loads it first.',
    addedIn: '1.15.0-r20',
  },
  {
    slug: 'responsive_images',
    title: 'Responsive images',
    category: 'Images',
    page: 'responsive_images.html',
    filters:
      'responsive_images,responsive_images_zoom,rewrite_images,inline_images,resize_images,insert_image_dimensions',
    blurb: 'Serves a srcset so each device downloads an image sized for its screen.',
  },
  {
    slug: 'resize_mobile_images',
    title: 'Resize images for mobile',
    category: 'Images',
    page: 'resize_mobile_images.html',
    filters: 'resize_mobile_images,insert_image_dimensions',
    blurb: 'Serves smaller low-quality placeholders to mobile browsers, then the full image.',
  },
  {
    slug: 'resize_rendered_image_dimensions',
    title: 'Resize to rendered dimensions',
    category: 'Images',
    page: 'resize_rendered_dimensions/image_resize_using_rendered_dimensions.html',
    filters: 'resize_rendered_image_dimensions',
    blurb: 'Resizes an image to the dimensions it is actually rendered at on the page.',
  },
  {
    slug: 'inline_preview_images',
    title: 'Inline preview images',
    category: 'Images',
    page: 'inline_preview_images.html',
    filters: 'inline_preview_images,insert_image_dimensions',
    blurb: 'Shows an inlined low-quality placeholder until the full image loads.',
  },
  {
    slug: 'lazyload_images',
    title: 'Lazy-load images',
    category: 'Images',
    page: 'lazyload_images.html',
    filters: 'lazyload_images',
    blurb: 'Defers off-screen images until they scroll into the viewport.',
  },
  {
    slug: 'sprite_images',
    title: 'Sprite images',
    category: 'Images',
    page: 'sprite_images.html',
    filters: 'rewrite_css,sprite_images',
    blurb: 'Combines background images referenced in CSS into a single sprite.',
  },
  {
    slug: 'dedup_inlined_images',
    title: 'Deduplicate inlined images',
    category: 'Images',
    page: 'dedup_inlined_images.html',
    filters: 'inline_images,dedup_inlined_images',
    blurb: 'Replaces repeated inlined images with a reference to the first copy.',
  },

  // ------------------------------------------------------------------- CSS
  {
    slug: 'rewrite_css',
    title: 'Minify CSS',
    category: 'CSS',
    page: 'rewrite_css.html',
    filters: 'rewrite_css',
    blurb: 'Strips whitespace and comments, then rewrites CSS to the smallest equivalent form.',
  },
  {
    slug: 'combine_css',
    title: 'Combine CSS',
    category: 'CSS',
    page: 'combine_css.html',
    filters: 'combine_css',
    blurb: 'Combines multiple stylesheet files into one to cut HTTP requests.',
  },
  {
    slug: 'inline_css',
    title: 'Inline CSS',
    category: 'CSS',
    page: 'inline_css.html',
    filters: 'inline_css',
    blurb: 'Inlines small external stylesheets to remove a render-blocking request.',
  },
  {
    slug: 'outline_css',
    title: 'Outline CSS',
    category: 'CSS',
    page: 'outline_css.html',
    filters: 'outline_css',
    blurb: 'Moves large inline <style> blocks into external files so they can be cached.',
  },
  {
    slug: 'move_css_to_head',
    title: 'Move CSS to head',
    category: 'CSS',
    page: 'move_css_to_head.html',
    filters: 'move_css_to_head',
    blurb: 'Moves stylesheets into the <head> so the browser finds them sooner.',
  },
  {
    slug: 'move_css_above_scripts',
    title: 'Move CSS above scripts',
    category: 'CSS',
    page: 'move_css_above_scripts.html',
    filters: 'move_css_above_scripts',
    blurb: 'Reorders CSS ahead of scripts so styles are not blocked by JavaScript.',
  },
  {
    slug: 'flatten_css_imports',
    title: 'Flatten CSS @imports',
    category: 'CSS',
    page: 'flatten_css_imports.html',
    filters: 'rewrite_css,flatten_css_imports',
    blurb: 'Replaces @import rules with the imported CSS to avoid chained requests.',
  },
  {
    slug: 'inline_import_to_link',
    title: 'Convert @import to link',
    category: 'CSS',
    page: 'inline_import_to_link.html',
    filters: 'inline_import_to_link',
    blurb: 'Rewrites a <style> that only @imports into an equivalent <link>.',
  },
  {
    slug: 'inline_google_font_css',
    title: 'Inline Google Fonts CSS',
    category: 'CSS',
    page: 'inline_google_font_css.html',
    filters: 'inline_google_font_css',
    blurb: 'Inlines the small font-loading CSS that the Google Fonts API serves.',
    note: 'Fetches font CSS from the Google Fonts API to demonstrate.',
  },
  {
    slug: 'fallback_rewrite_css_urls',
    title: 'Fallback CSS URL rewriting',
    category: 'CSS',
    page: 'fallback_rewrite_css_urls.html',
    filters: 'fallback_rewrite_css_urls,rewrite_css,rewrite_images',
    blurb: 'Rewrites URLs inside CSS even when the stylesheet cannot be fully parsed.',
  },
  {
    slug: 'prioritize_critical_css',
    title: 'Prioritize critical CSS',
    category: 'CSS',
    page: 'prioritize_critical_css.html',
    filters: 'rewrite_css,flatten_css_imports,inline_import_to_link,prioritize_critical_css',
    blurb: 'Inlines above-the-fold CSS and loads the rest after first paint.',
  },
  {
    slug: 'rewrite_style_attributes',
    title: 'Rewrite style attributes',
    category: 'CSS',
    page: 'rewrite_style_attributes.html',
    filters: 'rewrite_style_attributes,rewrite_css,rewrite_images',
    blurb: 'Applies CSS rewriting to inline style="" attributes.',
  },
  {
    slug: 'rewrite_style_attributes_with_url',
    title: 'Rewrite style attributes with url()',
    category: 'CSS',
    page: 'rewrite_style_attributes.html',
    filters: 'rewrite_style_attributes_with_url,rewrite_css,rewrite_images',
    blurb: 'Rewrites inline style attributes only when they contain a url() reference.',
  },
  {
    slug: 'rewrite_css_extend_cache',
    title: 'Cache-extend images in CSS',
    category: 'CSS',
    page: 'rewrite_css_images.html',
    filters: 'rewrite_css,extend_cache',
    blurb: 'Rewrites image URLs inside CSS to content-hashed, cacheable URLs.',
  },
  {
    slug: 'rewrite_css_rewrite_images',
    title: 'Recompress images in CSS',
    category: 'CSS',
    page: 'rewrite_css_images.html',
    filters: 'rewrite_css,rewrite_images',
    blurb: 'Recompresses images referenced from inside stylesheets.',
  },

  // ------------------------------------------------------------ JavaScript
  {
    slug: 'rewrite_javascript',
    title: 'Minify JavaScript',
    category: 'JavaScript',
    page: 'rewrite_javascript.html',
    filters: 'rewrite_javascript',
    blurb: 'Strips comments and whitespace from JavaScript.',
    shows: ['rewrite_javascript_external', 'rewrite_javascript_inline'],
  },
  {
    slug: 'combine_javascript',
    title: 'Combine JavaScript',
    category: 'JavaScript',
    page: 'combine_javascript.html',
    filters: 'combine_javascript',
    blurb: 'Combines multiple script files into one to cut HTTP requests.',
  },
  {
    slug: 'inline_javascript',
    title: 'Inline JavaScript',
    category: 'JavaScript',
    page: 'inline_javascript.html',
    filters: 'inline_javascript',
    blurb: 'Inlines small external scripts to remove a request.',
  },
  {
    slug: 'outline_javascript',
    title: 'Outline JavaScript',
    category: 'JavaScript',
    page: 'outline_javascript.html',
    filters: 'outline_javascript',
    blurb: 'Moves large inline <script> blocks into external files so they can be cached.',
  },
  {
    slug: 'defer_javascript',
    title: 'Defer JavaScript',
    category: 'JavaScript',
    page: 'defer_javascript.html',
    filters: 'defer_javascript',
    blurb: 'Defers script execution until the page has loaded.',
  },
  {
    slug: 'canonicalize_javascript_libraries',
    title: 'Canonicalize JS libraries',
    category: 'JavaScript',
    page: 'canonicalize_javascript_libraries.html',
    filters: 'canonicalize_javascript_libraries',
    blurb: 'Redirects well-known library files to a shared, canonical, cacheable URL.',
  },
  {
    slug: 'make_show_ads_async',
    title: 'Async AdSense',
    category: 'JavaScript',
    page: 'make_show_ads_async.html',
    filters: 'make_show_ads_async',
    blurb: 'Rewrites synchronous Google AdSense tags to the asynchronous format.',
    note: 'Demonstrates rewriting the Google AdSense tag.',
  },

  // ------------------------------------------------------------------ HTML
  {
    slug: 'collapse_whitespace',
    title: 'Collapse whitespace',
    category: 'HTML',
    page: 'collapse_whitespace.html',
    filters: 'collapse_whitespace',
    blurb: 'Removes redundant whitespace from the HTML.',
  },
  {
    slug: 'remove_comments',
    title: 'Remove comments',
    category: 'HTML',
    page: 'remove_comments.html',
    filters: 'remove_comments',
    blurb: 'Strips HTML comments from the markup.',
  },
  {
    slug: 'remove_quotes',
    title: 'Remove quotes',
    category: 'HTML',
    page: 'remove_quotes.html',
    filters: 'remove_quotes',
    blurb: 'Removes quotes around HTML attribute values where they are not required.',
  },
  {
    slug: 'elide_attributes',
    title: 'Elide attributes',
    category: 'HTML',
    page: 'elide_attributes.html',
    filters: 'elide_attributes',
    blurb: 'Removes attributes whose value is the browser default.',
  },
  {
    slug: 'combine_heads',
    title: 'Combine heads',
    category: 'HTML',
    page: 'combine_heads.html',
    filters: 'combine_heads',
    blurb: 'Merges multiple <head> elements into one.',
  },
  {
    slug: 'pedantic',
    title: 'Pedantic',
    category: 'HTML',
    page: 'pedantic.html',
    filters: 'pedantic',
    blurb: 'Adds default type attributes to script and style tags that omit them.',
  },
  {
    slug: 'add_instrumentation',
    title: 'Add instrumentation',
    category: 'HTML',
    page: 'add_instrumentation.html',
    filters: 'add_instrumentation',
    blurb: 'Adds a small client-side beacon that reports real load timings.',
  },

  // --------------------------------------------------------------- Caching
  {
    slug: 'extend_cache',
    title: 'Extend cache',
    category: 'Caching',
    page: 'extend_cache.html',
    filters: 'extend_cache',
    blurb: 'Rewrites resource URLs to content-hashed names so they cache for a year.',
  },
  {
    slug: 'extend_cache_pdfs',
    title: 'Extend cache (PDFs)',
    category: 'Caching',
    page: 'extend_cache_pdfs.html',
    filters: 'extend_cache_pdfs',
    blurb: 'Applies cache extension to linked PDF files.',
  },
  {
    slug: 'local_storage_cache',
    title: 'Local storage cache',
    category: 'Caching',
    page: 'local_storage_cache.html',
    filters: 'local_storage_cache,inline_css,inline_images',
    blurb: "Stores inlined CSS and images in the browser's local storage for repeat visits.",
  },
  {
    slug: 'optimize_for_bandwidth',
    title: 'Optimize for bandwidth',
    category: 'Caching',
    page: 'optimize_for_bandwidth.html',
    filters: 'rewrite_css,rewrite_javascript,rewrite_images',
    blurb: 'Optimizes resources in place without changing their URLs — safe for any cache.',
    note: 'This demo enables the filters by name, so its optimized frame rewrites URLs; the OptimizeForBandwidth level itself keeps them as authored.',
  },

  // ------------------------------------------------------------- Resources
  {
    slug: 'insert_dns_prefetch',
    title: 'Insert DNS prefetch',
    category: 'Resources',
    page: 'insert_dns_prefetch.html',
    filters: 'insert_dns_prefetch',
    blurb: 'Injects <link rel="dns-prefetch"> hints so the browser resolves DNS early.',
  },
  {
    slug: 'hint_preload_subresources',
    title: 'Preload subresources',
    category: 'Resources',
    page: 'hint_preload_subresources.html',
    filters: 'hint_preload_subresources',
    blurb: 'Emits preload hints for CSS and JavaScript the page will need.',
  },
  {
    slug: 'insert_speculation_rules',
    title: 'Insert speculation rules',
    category: 'Resources',
    page: 'collapse_whitespace.html',
    filters: 'insert_speculation_rules',
    blurb:
      'Injects a speculation-rules script so supporting browsers prefetch same-origin links a visitor is likely to open next.',
    addedIn: '1.15.0-r20',
  },
  {
    slug: 'trim_urls',
    title: 'Trim URLs',
    category: 'Resources',
    page: 'trim_urls.html',
    filters: 'trim_urls',
    blurb: 'Shortens resource URLs to relative form where it is safe to do so.',
  },
  {
    slug: 'map_proxy_domain',
    title: 'Proxy external resources',
    category: 'Resources',
    page: 'proxy_external_resource.html',
    filters: '+rewrite_images,-inline_images',
    blurb: 'Proxies and optimizes trusted resources hosted on domains without PageSpeed.',
  },
];

/**
 * The examples that apply a filter: those whose `filters` value names it
 * (a leading `+` enables, a leading `-` disables and does not count) or whose
 * `shows` list does. Catalog order.
 */
export function examplesApplying(name: string): Example[] {
  return EXAMPLES.filter(
    (ex) =>
      ex.filters
        .split(',')
        .map((f) => f.trim().replace(/^\+/, ''))
        .includes(name) || !!ex.shows?.includes(name),
  );
}

/** before-frame URL (optimization off). */
export function beforeUrl(ex: Example): string {
  return `${DEMO_BASE}/${ex.page}?PageSpeed=off`;
}

/** after-frame URL (only the listed filter(s) applied). */
export function afterUrl(ex: Example): string {
  return `${DEMO_BASE}/${ex.page}?PageSpeedFilters=${encodeURIComponent(ex.filters)}`;
}
