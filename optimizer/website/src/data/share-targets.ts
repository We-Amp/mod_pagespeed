// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// Single source of truth for the social share-intent targets, consumed by the
// inline ShareButtons panel (blog/docs). Keeping the brand-icon path data +
// intent URLs in one place means share surfaces cannot drift apart. No
// third-party tracking JS — every target is a plain share-intent URL;
// Copy-link is handled separately.

export interface ShareTarget {
  /** Display name, used in aria-label / title. */
  name: string;
  /** umami `data-umami-event-network` discriminator. */
  slug: string;
  /** Pre-encoded share-intent URL. */
  href: string;
  /** 24×24 viewBox SVG path, rendered with fill="currentColor". */
  icon: string;
}

/** Build the ordered share targets for a given canonical URL + page title. */
export function buildShareTargets(shareUrl: string, title: string): ShareTarget[] {
  const u = encodeURIComponent(shareUrl);
  const t = encodeURIComponent(title);
  return [
    {
      name: 'X',
      slug: 'x',
      href: `https://twitter.com/intent/tweet?url=${u}&text=${t}`,
      icon: 'M18.244 2.25h3.308l-7.227 8.26 8.502 11.24h-6.66l-5.214-6.817L4.99 21.75H1.68l7.73-8.835L1.254 2.25H8.08l4.713 6.231zm-1.161 17.52h1.833L7.084 4.126H5.117z',
    },
    {
      name: 'LinkedIn',
      slug: 'linkedin',
      href: `https://www.linkedin.com/sharing/share-offsite/?url=${u}`,
      icon: 'M20.447 20.452h-3.554v-5.569c0-1.328-.027-3.037-1.852-3.037-1.853 0-2.136 1.445-2.136 2.939v5.667H9.351V9h3.414v1.561h.046c.477-.9 1.637-1.85 3.37-1.85 3.601 0 4.267 2.37 4.267 5.455v6.286zM5.337 7.433a2.062 2.062 0 01-2.063-2.065 2.064 2.064 0 112.063 2.065zm1.782 13.019H3.555V9h3.564v11.452zM22.225 0H1.771C.792 0 0 .774 0 1.729v20.542C0 23.227.792 24 1.771 24h20.451C23.2 24 24 23.227 24 22.271V1.729C24 .774 23.2 0 22.222 0h.003z',
    },
    {
      name: 'Hacker News',
      slug: 'hackernews',
      href: `https://news.ycombinator.com/submitlink?u=${u}&t=${t}`,
      icon: 'M0 24V0h24v24H0zM6.951 5.896l4.112 7.708v5.064h1.583v-4.972l4.148-7.799h-1.749l-2.457 4.875c-.372.745-.688 1.434-.688 1.434s-.297-.708-.651-1.434L8.882 5.896h-1.93z',
    },
    {
      name: 'Reddit',
      slug: 'reddit',
      href: `https://www.reddit.com/submit?url=${u}&title=${t}`,
      icon: 'M12 0A12 12 0 000 12a12 12 0 0012 12 12 12 0 0012-12A12 12 0 0012 0zm5.01 4.744c.688 0 1.25.561 1.25 1.249a1.25 1.25 0 01-2.498.056l-2.597-.547-.8 3.747c1.824.07 3.48.632 4.674 1.488.308-.309.73-.491 1.207-.491.968 0 1.754.786 1.754 1.754 0 .716-.435 1.333-1.01 1.614a3.111 3.111 0 01.042.52c0 2.694-3.13 4.87-7.004 4.87-3.874 0-7.004-2.176-7.004-4.87 0-.183.015-.366.043-.534A1.748 1.748 0 014.028 12c0-.968.786-1.754 1.754-1.754.463 0 .898.196 1.207.49 1.207-.883 2.878-1.43 4.744-1.487l.885-4.182a.342.342 0 01.14-.197.35.35 0 01.238-.042l2.906.617a1.214 1.214 0 011.108-.701zM9.25 12C8.561 12 8 12.562 8 13.25c0 .687.561 1.248 1.25 1.248.687 0 1.248-.561 1.248-1.249 0-.688-.561-1.249-1.249-1.249zm5.5 0c-.687 0-1.248.561-1.248 1.25 0 .687.561 1.248 1.249 1.248.688 0 1.249-.561 1.249-1.249 0-.687-.562-1.249-1.25-1.249zm-5.466 3.99a.327.327 0 00-.231.094.33.33 0 000 .463c.842.842 2.484.913 2.961.913.477 0 2.105-.071 2.961-.913a.361.361 0 00.029-.463.33.33 0 00-.464 0c-.547.533-1.684.73-2.512.73-.828 0-1.979-.196-2.512-.73a.326.326 0 00-.232-.095z',
    },
    {
      name: 'Bluesky',
      slug: 'bluesky',
      href: `https://bsky.app/intent/compose?text=${t}%20${u}`,
      icon: 'M5.769 3.262C8.32 5.18 11.064 9.07 12 11.157c.936-2.087 3.68-5.977 6.231-7.895C20.067 1.88 23 .57 23 3.915c0 .668-.382 5.61-.606 6.414-.778 2.792-3.625 3.504-6.156 3.073 4.425.752 5.55 3.246 3.118 5.74-4.62 4.737-6.638-1.19-7.156-2.709-.095-.279-.139-.409-.139-.298 0-.111-.044.02-.139.298-.518 1.519-2.536 7.446-7.156 2.709-2.432-2.494-1.307-4.988 3.118-5.74-2.531.431-5.378-.281-6.156-3.073C1.382 9.525 1 4.583 1 3.915 1 .57 3.933 1.88 5.769 3.262z',
    },
    {
      name: 'Email',
      slug: 'email',
      href: `mailto:?subject=${t}&body=${u}`,
      icon: 'M1.5 8.67v8.58a3 3 0 003 3h15a3 3 0 003-3V8.67l-8.928 5.493a3 3 0 01-3.144 0L1.5 8.67z M22.5 6.908V6.75a3 3 0 00-3-3h-15a3 3 0 00-3 3v.158l9.714 5.978a1.5 1.5 0 001.572 0L22.5 6.908z',
    },
  ];
}
