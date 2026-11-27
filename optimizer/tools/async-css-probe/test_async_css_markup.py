# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

"""Markup lane: what the optimizer emits for a deferred stylesheet.

Hermetic and browser-free. Every assertion is about bytes and headers the
server produced, so a red run is a product change, never a rendering
difference. The infrastructure underneath is another matter — see the CI job's
recovery steps.

Three modes, selected by ASYNC_CSS_PROBE_MODE (see conftest.py):

* ``TestGateRefuses`` — floor raised, switch off: nothing defers, and with the
  stylesheet staying render-blocking the page carries no inline critical block
  either (the block is only worth its bytes when it replaces the blocking
  fetch).
* ``TestForcedDeferral`` — floor raised, switch on: it defers, and every piece
  of the deferred markup must be correct. The pair is the switch's end-to-end
  proof, and the gate's.
* ``TestForcedInlinesTailwindStrings`` — the same switch on a page whose
  stylesheet holds `<` inside a CSS string (see below): the `<` is escaped, the
  block is inlined and the sheet deferred.

* ``TestShippedConfiguration`` — stock flags: what the product does today.

Two fixture pages. The primary one (modpagespeed.com, Tailwind v4) carries
`@property ... syntax:"<color>"` in its stylesheet. The optimizer refuses CSS
that holds a `<` followed by a letter, `/`, `!` or `?` outside strings and
comments, but inside a string it writes the `<` as `\\3c ` and inlines the block
(#681); with the switch on, that page defers. The second ("plain", stock
Bootstrap 5.3.3) has no such sequence at all, so it is the page the deferral
and its inline block are checked on in the other modes. Tests are routed to the page that matches them; the invariants that hold
for both run on both.
"""

import re

import pytest
from conftest import (
    CRITICAL_BLOCK_MARKER,
    DEFERRED_LINK_MARKER,
    DEFERRED_SHEET_IS_EARLY_HINT_PROMOTED,
    DEFERS_WITHOUT_A_VALIDATED_PROFILE,
    FALLBACK_MARKER,
    FIXTURE_CSS_PATH,
    FIXTURE_FOLD_SUBRESOURCE_PATHS,
    LOADER_MARKER,
    PLAIN_CSS_PATH,
    PLAIN_HTML_PATH,
    PRIMITIVE_AS,
    PRIMITIVE_ATTR,
    PRIMITIVE_VALUE,
    SAVED_MEDIA_ATTR,
    UNSAFE_FORCE_WARNING,
    compose_run,
    forced_only,
    gated_only,
    leading_layer_statement,
    link_headers,
    names_cascade_layer,
    shipped_only,
    top_level_layer_order,
)


@forced_only
class TestForcedDeferral:
    def test_every_deferred_link_carries_the_primitive_and_a_noscript_twin(
        self, plain_markup
    ):
        """A deferred <link> is only safe if all three parts are present: the
        primitive that stops it blocking, the record of the media it must be
        restored to, and a twin for clients that will never run the loader."""
        deferred = [e for e in plain_markup.deferred_links() if not e.in_noscript]
        assert deferred, (
            "forced run produced no deferred <link> — the switch did not take "
            "effect, or the page produced no critical CSS to inline"
        )

        for link in deferred:
            assert link.attrs.get(PRIMITIVE_ATTR) == PRIMITIVE_VALUE, (
                f"deferred link must carry {PRIMITIVE_ATTR}={PRIMITIVE_VALUE}, "
                f"got {link.attrs.get(PRIMITIVE_ATTR)!r}: {link}"
            )
            assert link.attrs.get("as") == PRIMITIVE_AS, (
                f'deferred link must carry as="{PRIMITIVE_AS}" so the browser '
                "fetches it at stylesheet priority and matches it to the "
                f"consumer the loader creates, got {link.attrs.get('as')!r}: "
                f"{link}"
            )
            # Without this the loader has nothing to restore the sheet TO, so
            # the stylesheet would never apply.
            assert link.has(SAVED_MEDIA_ATTR), (
                f"deferred link lost its recorded media: {link}"
            )
            assert link.attrs.get(SAVED_MEDIA_ATTR), (
                f"{SAVED_MEDIA_ATTR} must record a media value: {link}"
            )

        fallbacks = [
            e
            for e in plain_markup.by_tag("link")
            if e.in_noscript and (e.attrs.get("rel") or "").lower() == "stylesheet"
        ]
        deferred_hrefs = {e.attrs.get("href") for e in deferred}
        fallback_hrefs = {e.attrs.get("href") for e in fallbacks}
        assert deferred_hrefs <= fallback_hrefs, (
            "every deferred stylesheet needs a <noscript> twin; missing: "
            f"{deferred_hrefs - fallback_hrefs}"
        )

        # The twin must load the sheet, not merely mention it: it stays
        # rel="stylesheet", and it carries the integrity/crossorigin pair
        # verbatim when the original had them (an SRI-verified sheet whose twin
        # dropped `crossorigin` fails to load for exactly the no-JS clients the
        # twin exists for).
        by_href = {e.attrs.get("href"): e for e in fallbacks}
        for link in deferred:
            twin = by_href[link.attrs.get("href")]
            assert twin.attrs.get(PRIMITIVE_ATTR) != PRIMITIVE_VALUE, (
                f"the noscript twin must NOT carry the deferral primitive: {twin}"
            )
            assert not twin.has("as"), (
                f"the noscript twin must be a plain stylesheet, not a typed "
                f"fetch: {twin}"
            )
            for attr in ("integrity", "crossorigin"):
                if link.has(attr):
                    assert twin.has(attr), f"noscript twin dropped {attr}: {twin}"
                    assert twin.attrs.get(attr) == link.attrs.get(attr)

    def test_fixture_stylesheet_is_the_one_that_got_deferred(self, plain_markup):
        """The deferral applies to the plain page's real Bootstrap sheet, not
        to some incidental link."""
        hrefs = [
            e.attrs.get("href")
            for e in plain_markup.deferred_links()
            if not e.in_noscript
        ]
        assert any(PLAIN_CSS_PATH in (h or "") for h in hrefs), (
            f"expected {PLAIN_CSS_PATH} among deferred sheets, got {hrefs}"
        )

    def test_deferred_sheet_is_larger_than_64_kib(self, plain_css_bytes):
        """The deferral above is also the end-to-end check that a stylesheet
        larger than 64 KiB can be confirmed and used. Before the optimizer uses
        a stylesheet it asks the site, with a HEAD request, whether the sheet is
        served to any client; that request once inherited a 64 KiB download
        cap and was refused for every larger sheet, so such a page got no
        critical CSS and no deferral. Keep the plain page's sheet above that
        size, so a fixture swap cannot quietly drop the case."""
        assert len(plain_css_bytes) > 64 * 1024, (
            f"{PLAIN_CSS_PATH} is {len(plain_css_bytes)} bytes; the plain page "
            "needs a stylesheet larger than 64 KiB"
        )

    def test_loader_script_injected_once_at_the_content_addressed_path(
        self, plain_markup, async_css_loader_path
    ):
        """Exactly one loader per document, at the path recomputed from the
        worker header. Two copies would run the media swap twice; zero would
        leave every deferred sheet permanently print-only."""
        assert re.fullmatch(
            r"/pagespeed_static/async_css\.[0-9a-f]{8}\.js", async_css_loader_path
        ), f"loader path is not content-addressed: {async_css_loader_path}"

        loaders = plain_markup.loader_scripts(async_css_loader_path)
        assert len(loaders) == 1, (
            f"expected exactly 1 loader script at {async_css_loader_path}, "
            f"found {len(loaders)}"
        )
        # `defer` is what guarantees the DOM is parsed before the swap runs.
        assert loaders[0].has("defer"), f"loader must be deferred: {loaders[0]}"
        assert loaders[0].has(LOADER_MARKER)

        # No second loader at any OTHER hashed path: a stale path would be
        # served as a 404 and silently break the swap.
        all_loaders = [
            e
            for e in plain_markup.by_tag("script")
            if "/pagespeed_static/async_css." in (e.attrs.get("src") or "")
        ]
        assert len(all_loaders) == 1, (
            f"loader injected at more than one path: "
            f"{[e.attrs.get('src') for e in all_loaders]}"
        )

    def test_loader_path_serves_200_js_immutable(self, client, async_css_loader_path):
        """The path the markup points at is the path the worker serves — the
        drift alarm for a partial deploy."""
        r = client.get(async_css_loader_path)
        assert r.status_code == 200, (
            f"loader 404s at the injected path {async_css_loader_path}: "
            "worker and markup disagree about the loader hash"
        )
        assert "javascript" in r.headers.get("Content-Type", "")
        assert r.headers.get("X-PageSpeed") == "async-css-loader"
        assert r.headers.get("X-Content-Type-Options") == "nosniff"
        cc = r.headers.get("Cache-Control", "")
        assert "max-age=31536000" in cc, f"expected 1-year max-age, got {cc!r}"
        assert "immutable" in cc, f"content-addressed path must be immutable: {cc!r}"
        body = r.text
        assert DEFERRED_LINK_MARKER in body, "loader body looks truncated"
        assert SAVED_MEDIA_ATTR in body, (
            "loader must read back the media it has to restore"
        )

    def test_inline_critical_block_present_when_deferring(self, plain_markup):
        """Deferring without inlining the fold's CSS is the FOUC. The two ship
        together or not at all."""
        injected = [s for s in plain_markup.styles if s.strip()]
        assert injected, "deferring page has no inline <style> at all"
        # The critical block is the substantial one: require more than a few
        # hand-written lines rather than accepting any non-empty <style>.
        assert max(len(s) for s in injected) > 4096, (
            "no substantial inline critical block; largest inline <style> is "
            f"{max(len(s) for s in injected)} bytes"
        )

    def test_deferred_sheet_is_preload_hinted(self, optimized_plain, plain_markup):
        """The Early-Hints sentinel and the transform must agree. The primitive
        is itself a preload, so the hint announces exactly the fetch the markup
        announces — only earlier. (While the primitive demoted the sheet the
        rule was the opposite; primitive.json carries which one is in force, so
        this assertion follows the transform rather than being rewritten.)"""
        deferred_hrefs = {
            e.attrs.get("href")
            for e in plain_markup.deferred_links()
            if not e.in_noscript
        }
        hinted = link_headers(optimized_plain)
        hinted_deferred = [
            h for h in hinted if any((href or "") in h for href in deferred_hrefs)
        ]
        if DEFERRED_SHEET_IS_EARLY_HINT_PROMOTED:
            assert hinted_deferred, (
                f"deferred sheet must be hinted, got Link headers: {hinted}"
            )
        else:
            assert not hinted_deferred, (
                "a deferred sheet must not be Early-Hint promoted; found: "
                f"{hinted_deferred}"
            )

    def test_worker_printed_the_startup_warning(self, probe_services):
        """The warning IS the mitigation for this switch: it is the only thing
        that tells an operator a worker is deferring past its own safety gate.
        A silent force switch is the failure mode worth catching."""
        logs = compose_run("logs", "--no-color", "worker").stdout
        assert UNSAFE_FORCE_WARNING in logs, (
            "the worker was started with --unsafe-force-async-css but printed "
            "no warning; searched for:\n  " + UNSAFE_FORCE_WARNING
        )

    def test_deferred_markup_is_served_from_cache_consistently(self, client):
        """Two fetches of the same optimized page agree. A decision that
        flickers between requests (D6) is a different failure from never
        deferring, and only a repeated fetch can tell them apart."""
        first = client.poll_for_hit(PLAIN_HTML_PATH)
        second = client.get(PLAIN_HTML_PATH)
        assert second.headers.get("X-PageSpeed") == "HIT"
        assert (DEFERRED_LINK_MARKER in first.text) == (
            DEFERRED_LINK_MARKER in second.text
        ), "deferral decision changed between two fetches of the same page"


@forced_only
class TestForcedInlinesTailwindStrings:
    """The switch on, on the primary page. Its Tailwind v4 stylesheet holds
    `@property ... syntax:"<color>"`: a `<` inside a CSS string. Since the
    string-aware scanner (#681) that no longer refuses the block; the `<` is
    written as the CSS escape `\\3c ` and the page is inlined and deferred like
    any other. Before #681 (2.8.0) this page got no inline block and its
    stylesheet stayed render-blocking in every mode; the tests below fail if
    that comes back."""

    # A `<` that could open markup inside the inlined text.
    MARKUP_OPENER = re.compile(r"<[A-Za-z/!?]")

    def test_critical_block_is_inlined(self, home_markup):
        assert home_markup.critical_block_positions(), (
            f"no <style {CRITICAL_BLOCK_MARKER}> for a stylesheet whose only "
            "`<` sits inside a CSS string"
        )
        assert max((len(s) for s in home_markup.styles), default=0) > 4096, (
            "no substantial inline <style> next to the deferred stylesheet"
        )

    def test_inlined_text_cannot_open_markup(self, home_markup):
        """Inlining is only safe because the `<` inside strings is escaped:
        the served block must hold no `<` followed by a letter, `/`, `!` or `?`
        and `</style` must not appear."""
        assert home_markup.critical_block_positions()
        big = max(home_markup.styles, key=len)
        assert not self.MARKUP_OPENER.search(big), (
            "the inlined critical CSS holds a raw `<` that could open markup: "
            f"{self.MARKUP_OPENER.search(big).group(0)!r}"
        )

    def test_stylesheet_is_deferred(self, home_markup):
        deferred = [e for e in home_markup.deferred_links() if not e.in_noscript]
        assert deferred, (
            "the Tailwind page was not deferred: the stylesheet stays "
            "render-blocking as it did before #681"
        )
        sheets = [
            e
            for e in home_markup.stylesheet_links()
            if FIXTURE_CSS_PATH in (e.attrs.get("href") or "") and not e.in_noscript
        ]
        assert not sheets, (
            f"the fixture stylesheet is still a render-blocking link: {sheets}"
        )

    def test_loader_and_fallback_are_present(self, home_markup, async_css_loader_path):
        assert len(home_markup.loader_scripts(async_css_loader_path)) == 1
        assert [e for e in home_markup.elements if e.has(FALLBACK_MARKER)], (
            "deferred without the <noscript> fallback"
        )


@gated_only
class TestGateRefuses:
    """Floor raised, switch off. The argv is identical to the forced mode bar
    that one switch, so everything below is a gate's doing and not the page's.
    Two gates refuse here now — the raised byte floor and the empirical one, the
    fixture having no validated profile — which is why this mode's value is as
    the forced mode's control rather than as a proof about the floor alone."""

    def test_nothing_is_deferred(self, page):
        deferred = [e for e in page.markup.deferred_links() if not e.in_noscript]
        assert not deferred, (
            "the gate refused this page's critical CSS, yet its stylesheet was "
            f"deferred anyway: {deferred}"
        )

    def test_stylesheet_stays_render_blocking(self, page):
        sheets = [
            e
            for e in page.markup.stylesheet_links()
            if page.css_path in (e.attrs.get("href") or "") and not e.in_noscript
        ]
        assert sheets, "the fixture stylesheet vanished from the served markup"
        for sheet in sheets:
            assert sheet.attrs.get(PRIMITIVE_ATTR) != PRIMITIVE_VALUE, (
                f"stylesheet carries the deferral primitive: {sheet}"
            )

    def test_no_loader_and_no_fallback_when_nothing_is_deferred(
        self, page, async_css_loader_path
    ):
        """The loader is injected with the first deferred link and only then;
        the same goes for the <noscript> fallback."""
        assert not page.markup.loader_scripts(async_css_loader_path)
        assert not [e for e in page.markup.elements if e.has(FALLBACK_MARKER)], (
            "async-CSS fallback markup present without a deferral"
        )

    def test_no_inline_critical_block_while_stylesheet_stays_blocking(self, page):
        """The twin of the forced mode's "block ships with the deferral". When
        the gate refuses to defer, the full stylesheet stays render-blocking
        and an inline critical block next to it would only duplicate its bytes
        with no paint benefit, so the optimizer drops the block and keeps the
        page's original <link>. Inlining without deferral is not offered."""
        assert not page.markup.critical_block_positions(), (
            f"a <style {CRITICAL_BLOCK_MARKER}> was inlined although the "
            "stylesheet stays render-blocking"
        )
        assert max((len(s) for s in page.markup.styles), default=0) <= 4096, (
            "a substantial inline <style> sits next to the render-blocking stylesheet"
        )

    def test_loader_is_still_served_even_though_unused(
        self, client, async_css_loader_path
    ):
        """The loader is served unconditionally at preaccess, not gated on a
        page having injected it — so a hash rotation is caught in every mode."""
        r = client.get(async_css_loader_path)
        assert r.status_code == 200
        assert r.headers.get("X-PageSpeed") == "async-css-loader"

    def test_worker_printed_no_startup_warning(self, probe_services):
        """The twin of the forced-mode assertion. A warning that appears when
        the switch is absent would be noise, and noise is how a real warning
        stops being read."""
        logs = compose_run("logs", "--no-color", "worker").stdout
        assert UNSAFE_FORCE_WARNING not in logs, (
            "a worker started WITHOUT --unsafe-force-async-css printed the "
            "force-switch warning"
        )


@shipped_only
class TestShippedConfiguration:
    def test_deferral_matches_what_the_product_does_today(self, page):
        """Stock flags, warm cache, realistic page: does it defer?

        IT NO LONGER DOES. It used to, and that was the defect this effort is
        about: the extractor produces ~45 KB of critical CSS against a ~115 KB
        sheet — comfortably over the 0.10 coverage floor — so the byte-ratio
        gate permitted the deferral without anything ever having checked that
        the inlined block actually covers the fold. The floor is a proxy, and
        this page is the proxy's counter-example.

        Deferral now also requires a validated profile still bound to the
        stylesheet being served. This stack has no browser, so this page never
        acquires one, and the stock-flag answer is "render-blocking, critical
        CSS dropped too, because a block next to a render-blocking sheet only
        duplicates its bytes". Note what did NOT change: the forced mode still
        defers, because the switch bypasses the gate — which is what keeps this
        lane's deferred-markup assertions exercised.
        """
        deferred = [e for e in page.markup.deferred_links() if not e.in_noscript]
        if DEFERS_WITHOUT_A_VALIDATED_PROFILE:
            assert deferred, (
                "this page no longer defers under stock flags. If that is "
                "intended (PR-C), flip DEFERS_WITHOUT_A_VALIDATED_PROFILE to "
                "False in conftest.py."
            )
        else:
            assert not deferred, (
                f"stock flags deferred this page with no validated profile: {deferred}"
            )

    def test_no_inline_critical_block_while_stylesheet_stays_blocking(self, page):
        """Stock flags, no validated profile: nothing defers, so no critical
        block is inlined beside the render-blocking stylesheet."""
        assert not page.markup.critical_block_positions()
        assert max((len(s) for s in page.markup.styles), default=0) <= 4096


class TestBothModes:
    """Invariants that must hold whichever way the worker was started."""

    def test_critical_block_ships_with_the_deferral_or_not_at_all(self, page):
        """The inline critical block and the deferral are decided together: a
        page has exactly one block when its stylesheet is deferred and none when
        it stays render-blocking."""
        deferred = [e for e in page.markup.deferred_links() if not e.in_noscript]
        blocks = page.markup.critical_block_positions()
        assert len(blocks) == (1 if deferred else 0), (
            f"{len(blocks)} critical block(s) with {len(deferred)} deferred "
            "stylesheet link(s)"
        )

    @forced_only
    def test_critical_block_placement(self, plain_markup, plain_css_bytes):
        """The inlined block duplicates rules of the page's sheets, filtered
        to the fold and to the variant's viewport class. Equal-specificity
        ties resolve by source order, so a block AFTER a sheet wins them for
        good once the sheet applies: a responsive override the block dropped
        loses to the base rule the block kept. So the block goes
        before every stylesheet source. A layer's position is fixed by its
        first mention, so a block that names a cascade layer goes first only
        with an `@layer ...;` statement in front that lists the page's layers
        in document order, when the worker can prove that order; otherwise it
        keeps the old placement, after every head stylesheet. This test runs
        on the plain page, whose Bootstrap sheet names no cascade layer, so it
        checks the placement before every stylesheet source and no longer
        exercises the `@layer` statement branch. Either way the block follows
        <meta charset>, which has to stay in the first 1024 bytes. The Tailwind
        page gets no block."""
        home_markup = plain_markup
        fixture_css_bytes = plain_css_bytes
        blocks = home_markup.critical_block_positions()
        assert len(blocks) == 1, (
            f"expected exactly one <style {CRITICAL_BLOCK_MARKER}>, found {len(blocks)}"
        )
        block = blocks[0]
        sources = home_markup.stylesheet_source_positions()
        assert sources, "the served page has no stylesheet source at all"
        assert any(
            PLAIN_CSS_PATH in (home_markup.elements[i].attrs.get("href") or "")
            for i in sources
        ), "the fixture stylesheet is not among the page's stylesheet sources"
        els = home_markup.elements
        text = els[block].text
        statement = leading_layer_statement(text)
        if names_cascade_layer(text) and statement is None:
            pytest.fail(
                "the critical block names a cascade layer but carries no "
                "layer-order statement, so it took the old placement; this "
                "fixture's layer order is provable (one same-origin layered "
                "sheet, no layered inline style)"
            )
        if statement is not None:
            expected = top_level_layer_order(fixture_css_bytes.decode("utf-8"))
            assert statement == expected, (
                f"layer-order statement {statement} is not the fixture "
                f"sheet's order {expected}"
            )
        misplaced = [els[i] for i in sources if i < block]
        assert not misplaced, (
            "the critical block must precede every stylesheet source; "
            f"these come before it: {misplaced}"
        )
        charset = [i for i, e in enumerate(els) if e.tag == "meta" and e.has("charset")]
        assert charset and all(i < block for i in charset), (
            "the critical block moved ahead of <meta charset>"
        )

    def test_page_is_optimized_at_all(self, page):
        assert page.response.status_code == 200
        assert page.response.headers.get("X-PageSpeed") == "HIT"
        assert "</html>" in page.response.text.lower(), "served page is truncated"

    def test_baseline_serves_the_capture_unmodified(self, client):
        """The control path must be the fixture bytes: if PageSpeed leaked into
        /baseline/ the rendered lane would diff optimized against optimized."""
        r = client.get("/baseline/")
        assert r.status_code == 200
        assert DEFERRED_LINK_MARKER not in r.text
        assert "/pagespeed_static/async_css." not in r.text

    def test_fixture_stylesheet_is_served(self, client, fixture_css_bytes):
        r = client.poll_for_hit(FIXTURE_CSS_PATH)
        assert r.status_code == 200
        assert "css" in r.headers.get("Content-Type", "")
        # Optimized bytes are a minification of the capture, never larger.
        assert 0 < len(r.content) <= len(fixture_css_bytes)


@pytest.mark.parametrize(
    "path", ["/", FIXTURE_CSS_PATH, *FIXTURE_FOLD_SUBRESOURCE_PATHS]
)
def test_probe_paths_exist_at_the_origin(probe_services, path):
    """A 404 here means the fixture layout and the captured HTML disagree —
    which would otherwise surface as 'nothing was deferred', or, for the fold
    subresources, as a rendered lane quietly measuring a fold with no web fonts
    and no logo — the exact gap those files were captured to close."""
    import requests as _requests

    r = _requests.get(probe_services["origin_url"] + path, timeout=10)
    assert r.status_code == 200, f"origin does not serve {path}"


@pytest.mark.parametrize("path", FIXTURE_FOLD_SUBRESOURCE_PATHS)
def test_fold_subresources_are_served_as_their_own_type(probe_services, path):
    """Served bytes are not enough: an SVG delivered as octet-stream does not
    paint inside <img> at all, so the logo would be missing from a lane that
    fetched it successfully. The type is the load-bearing half."""
    import requests as _requests

    expected = "font/woff2" if path.endswith(".woff2") else "image/svg+xml"
    r = _requests.get(probe_services["origin_url"] + path, timeout=10)
    assert r.headers.get("Content-Type", "") == expected, (
        f"{path} served as {r.headers.get('Content-Type')!r}, expected {expected!r}"
    )
