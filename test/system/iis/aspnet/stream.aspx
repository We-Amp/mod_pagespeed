<%@ Page Language="C#" %>
<script runat="server">
    // SPDX-License-Identifier: Apache-2.0
    // Streamed-HTML fixture for the PageSpeed IIS module (issue #314).
    // The page writes one HTML document in ten Response.Write +
    // Response.Flush steps with 50 ms sleeps, so the module sees a single
    // response delivered across several sends. The output is a valid HTML
    // document the rewrite pipeline can parse. The long runs of spaces are
    // collapsible whitespace: collapse_whitespace removes them
    // when the test requests that filter with
    // ?PageSpeedFilters=+collapse_whitespace. Each step ends with an ordered
    // marker paragraph (c0 .. c9), so a test can tell that every step arrived
    // once and in order. ?padKb=N adds an N KB HTML comment to every step:
    // with 100 KB or more queued the rewriter flushes before the page is done,
    // which is the path issue #314 hardens. ASCII only. No caching headers are
    // set here; rewritten-HTML cache headers come from the module.
    private const int ChunkDelayMs = 50;
    private const int MaxPadKb = 512;
    private int chunkIndex = 0;
    private string pad = "";

    private void Chunk(string html) {
        Chunk(html, "");
    }

    // tail follows the marker and the pad, so the closing tags stay last.
    private void Chunk(string html, string tail) {
        Response.Write(html);
        Response.Write("<p id=\"c" + chunkIndex + "\">c" + chunkIndex + "</p>\n");
        Response.Write(pad);
        Response.Write(tail);
        chunkIndex++;
        Response.Flush();
        System.Threading.Thread.Sleep(ChunkDelayMs);
    }
</script>
<%
    Response.ContentType = "text/html";

    int padKb = 0;
    int.TryParse(Request.QueryString["padKb"], out padKb);
    if (padKb < 0) padKb = 0;
    if (padKb > MaxPadKb) padKb = MaxPadKb;
    if (padKb > 0) {
        pad = "<!-- pad " + new string('p', padKb * 1024) + " -->\n";
    }

    Chunk("<!DOCTYPE html>\n<html>\n<head>\n<title>Streamed HTML fixture</title>\n</head>\n<body>\n");
    Chunk("<h1>Streamed HTML fixture</h1>\n");
    Chunk("<p>This page is written in several flushed chunks so the module must handle multiple sends for one response.</p>\n");
    Chunk("                                                        \n                        \n");
    Chunk("<p>The long whitespace runs around this paragraph collapse when the page is rewritten.</p>                                                       \n");
    Chunk("                                                        \n                        \n");
    Chunk("<img src=\"../mod_pagespeed_test/image_rewrite_with_flush/a.jpg\" alt=\"stream fixture image\">\n");
    Chunk("<p>The image above is an existing test image, so extend_cache can rewrite its URL.</p>                                                       \n");
    Chunk("<div>stream fixture body</div>\n                                                        \n");
    Chunk("", "<!-- stream-end -->\n</body>\n</html>\n");
%>
