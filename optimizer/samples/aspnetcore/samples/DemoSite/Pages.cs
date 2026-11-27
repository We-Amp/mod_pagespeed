// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

/// <summary>
/// Inline HTML pages for the demo site. Each page is designed to
/// exercise specific PageSpeed middleware optimizations.
/// </summary>
static class Pages
{
    private const string Nav = """
        <nav>
            <a href="/">Home</a>
            <a href="/gallery">Gallery</a>
            <a href="/blog">Blog</a>
            <a href="/about">About</a>
            <a href="/api/status">API</a>
        </nav>
        """;

    private const string Footer = """
        <footer>
            <p>&copy; 2026 PageSpeed Demo &mdash;
            <a href="/health">Health Check</a></p>
        </footer>
        """;

    /// <summary>
    /// Home page: hero image (LCP preload + fetchpriority), CSS
    /// (Link preload header), below-fold images (lazy loading),
    /// third-party script (preconnect).
    /// </summary>
    public static readonly string Home = $$"""
        <!DOCTYPE html>
        <html lang="en">
        <head>
            <meta charset="utf-8">
            <meta name="viewport" content="width=device-width, initial-scale=1">
            <title>PageSpeed ASP.NET Core Demo</title>
            <link rel="stylesheet" href="/css/style.css">
            <script src="https://cdn.jsdelivr.net/npm/chart.js" defer></script>
        </head>
        <body>
            {{Nav}}

            <main>
                <section class="hero">
                    <img src="/img/hero.jpg" width="1200" height="600"
                         alt="Hero banner — optimized with fetchpriority">
                    <h1>mod_pagespeed + ASP.NET Core</h1>
                    <p>This page demonstrates the PageSpeed middleware
                       processing a real HTML response.</p>
                </section>

                <section class="features">
                    <h2>What gets optimized?</h2>

                    <div class="card-grid">
                        <div class="card">
                            <img src="/img/feature-css.jpg" width="400" height="250"
                                 alt="Critical CSS">
                            <h3>Critical CSS Extraction</h3>
                            <p>Above-the-fold CSS is inlined; render-blocking
                               stylesheets are deferred.</p>
                        </div>

                        <div class="card">
                            <img src="/img/feature-lazy.jpg" width="400" height="250"
                                 alt="Lazy loading">
                            <h3>Image Lazy Loading</h3>
                            <p>Below-fold images receive <code>loading="lazy"</code>
                               automatically.</p>
                        </div>

                        <div class="card">
                            <img src="/img/feature-preload.jpg" width="400" height="250"
                                 alt="LCP preload">
                            <h3>LCP Preload Hints</h3>
                            <p>The hero image gets a <code>Link</code> preload header
                               and <code>fetchpriority="high"</code>.</p>
                        </div>

                        <div class="card">
                            <img src="/img/feature-preconnect.jpg" width="400" height="250"
                                 alt="Preconnect">
                            <h3>Third-Party Preconnect</h3>
                            <p>Origins like <code>cdn.jsdelivr.net</code> receive
                               <code>Link</code> preconnect headers.</p>
                        </div>
                    </div>
                </section>

                <section class="cta">
                    <h2>Try it yourself</h2>
                    <p>Open DevTools &rarr; Network tab. Compare:</p>
                    <ul>
                        <li><a href="/">Optimized</a> — check Link response headers</li>
                        <li><a href="/?bypass=1">Bypassed</a> — original HTML, no Link
                            headers</li>
                        <li><a href="/api/status">API endpoint</a> — JSON passes through
                            (excluded path)</li>
                    </ul>
                </section>
            </main>

            {{Footer}}
        </body>
        </html>
        """;

    /// <summary>
    /// Gallery page: many images to demonstrate lazy loading.
    /// First image gets fetchpriority="high", the rest get loading="lazy".
    /// </summary>
    public static readonly string Gallery = $$"""
        <!DOCTYPE html>
        <html lang="en">
        <head>
            <meta charset="utf-8">
            <meta name="viewport" content="width=device-width, initial-scale=1">
            <title>Gallery — PageSpeed Demo</title>
            <link rel="stylesheet" href="/css/style.css">
        </head>
        <body>
            {{Nav}}

            <main>
                <h1>Image Gallery</h1>
                <p>The first image receives <code>fetchpriority="high"</code>.
                   All others receive <code>loading="lazy"</code>.</p>
                <p><a href="/gallery?bypass=1">View without optimization</a></p>

                <div class="gallery-grid">
                    <figure>
                        <img src="/img/photo-1.jpg" width="600" height="400"
                             alt="Mountain landscape">
                        <figcaption>Mountain Landscape</figcaption>
                    </figure>
                    <figure>
                        <img src="/img/photo-2.jpg" width="600" height="400"
                             alt="Ocean sunset">
                        <figcaption>Ocean Sunset</figcaption>
                    </figure>
                    <figure>
                        <img src="/img/photo-3.jpg" width="600" height="400"
                             alt="Forest path">
                        <figcaption>Forest Path</figcaption>
                    </figure>
                    <figure>
                        <img src="/img/photo-4.jpg" width="600" height="400"
                             alt="City skyline">
                        <figcaption>City Skyline</figcaption>
                    </figure>
                    <figure>
                        <img src="/img/photo-5.jpg" width="600" height="400"
                             alt="Desert dunes">
                        <figcaption>Desert Dunes</figcaption>
                    </figure>
                    <figure>
                        <img src="/img/photo-6.jpg" width="600" height="400"
                             alt="Northern lights">
                        <figcaption>Northern Lights</figcaption>
                    </figure>
                </div>
            </main>

            {{Footer}}
        </body>
        </html>
        """;

    /// <summary>
    /// Blog page: third-party embeds to demonstrate preconnect headers.
    /// YouTube iframe and external widget scripts trigger preconnect hints
    /// for their respective origins.
    /// </summary>
    public static readonly string Blog = $$"""
        <!DOCTYPE html>
        <html lang="en">
        <head>
            <meta charset="utf-8">
            <meta name="viewport" content="width=device-width, initial-scale=1">
            <title>Blog — PageSpeed Demo</title>
            <link rel="stylesheet" href="/css/style.css">
            <link rel="stylesheet"
                  href="https://fonts.googleapis.com/css2?family=Inter:wght@400;700&display=swap">
        </head>
        <body>
            {{Nav}}

            <main class="blog-post">
                <article>
                    <header>
                        <h1>How PageSpeed Optimizes Your HTML</h1>
                        <p class="meta">
                            <img src="/img/author.jpg" width="40" height="40"
                                 alt="Author avatar" class="avatar">
                            <span>By the PageSpeed Team &mdash; February 2026</span>
                        </p>
                    </header>

                    <p>mod_pagespeed intercepts HTML responses at the middleware
                       level and applies a suite of optimizations. This blog post
                       includes third-party resources that trigger
                       <code>preconnect</code> hints.</p>

                    <h2>Embedded Video</h2>
                    <p>The YouTube iframe below causes a <code>Link: preconnect</code>
                       header for <code>www.youtube.com</code>.</p>
                    <div class="embed-container">
                        <iframe width="560" height="315"
                                src="https://www.youtube.com/embed/dQw4w9WgXcQ"
                                title="Demo video"
                                allow="accelerometer; autoplay; clipboard-write;
                                       encrypted-media; gyroscope"
                                allowfullscreen></iframe>
                    </div>

                    <h2>External Fonts</h2>
                    <p>The Google Fonts stylesheet in <code>&lt;head&gt;</code>
                       triggers preconnect to <code>fonts.googleapis.com</code>
                       and a preload header for the CSS.</p>

                    <h2>Content Images</h2>
                    <img src="/img/blog-diagram.jpg" width="800" height="400"
                         alt="Architecture diagram">
                    <p>Images below the fold receive <code>loading="lazy"</code>
                       while the first visible image gets
                       <code>fetchpriority="high"</code>.</p>

                    <h2>Inline Code Snippet</h2>
                    <pre><code>builder.Services.AddPageSpeed(options =&gt;
                    {
                        options.Cache.VolumePath = "/var/cache/pagespeed/volume.dat";
                        options.Html.EnableLazyLoad = true;
                        options.Html.EnablePreconnect = true;
                    });

                    app.UsePageSpeed();</code></pre>

                    <p><a href="/blog?bypass=1">View this page without optimization</a>
                       to compare Link headers in DevTools.</p>
                </article>
            </main>

            {{Footer}}
        </body>
        </html>
        """;

    /// <summary>
    /// About page: simple text-heavy page with minimal images.
    /// Shows that the middleware processes all HTML, not just
    /// image-heavy pages.
    /// </summary>
    public static readonly string About = $$"""
        <!DOCTYPE html>
        <html lang="en">
        <head>
            <meta charset="utf-8">
            <meta name="viewport" content="width=device-width, initial-scale=1">
            <title>About — PageSpeed Demo</title>
            <link rel="stylesheet" href="/css/style.css">
        </head>
        <body>
            {{Nav}}

            <main>
                <h1>About This Demo</h1>

                <p>This site demonstrates the <strong>WeAmp.PageSpeed.AspNetCore</strong>
                   middleware integrated into a standard ASP.NET Core application.</p>

                <h2>Architecture</h2>
                <p>The middleware sits in the ASP.NET Core pipeline between exception
                   handling and static files. It buffers HTML responses, passes them
                   through <code>libpagespeed</code> via P/Invoke, and writes the
                   optimized result back to the response stream.</p>

                <h2>Features Demonstrated</h2>
                <dl>
                    <dt>Critical CSS</dt>
                    <dd>Extracts above-the-fold CSS and inlines it in
                        <code>&lt;head&gt;</code>.</dd>

                    <dt>Lazy Loading</dt>
                    <dd>Adds <code>loading="lazy"</code> to below-fold images.</dd>

                    <dt>LCP Preload</dt>
                    <dd>Identifies the hero/LCP image and emits a
                        <code>Link: preload</code> response header.</dd>

                    <dt>Preconnect</dt>
                    <dd>Discovers third-party origins and emits
                        <code>Link: preconnect</code> headers.</dd>

                    <dt>Image Dimensions</dt>
                    <dd>Ensures <code>width</code> and <code>height</code> attributes
                        are present to prevent layout shift.</dd>
                </dl>

                <h2>Bypass Mode</h2>
                <p>Append <code>?bypass=1</code> to any page URL to skip the
                   middleware and see the original HTML. Compare response headers
                   and source in DevTools.</p>

                <h2>Configuration</h2>
                <p>All features are configurable via <code>appsettings.json</code>
                   under the <code>"PageSpeed"</code> section, or via the
                   <code>AddPageSpeed()</code> options callback. Changes are
                   hot-reloadable.</p>
            </main>

            {{Footer}}
        </body>
        </html>
        """;
}
