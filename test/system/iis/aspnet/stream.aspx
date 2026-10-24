<%@ Page Language="C#" %>
<script runat="server">
    // Streamed-HTML fixture for the PageSpeed IIS module (issue #314).
    // The page writes one HTML document in ten Response.Write +
    // Response.Flush steps with 50 ms sleeps, so the module sees a single
    // response delivered across several sends. The output is a valid HTML
    // document the rewrite pipeline can parse. The long runs of spaces are
    // collapsible whitespace: collapse_whitespace (CoreFilters) removes
    // them when the page is rewritten. ASCII only. No caching headers are
    // set here; rewritten-HTML cache headers come from the module.
    private const int ChunkDelayMs = 50;

    private void Chunk(string html) {
        Response.Write(html);
        Response.Flush();
        System.Threading.Thread.Sleep(ChunkDelayMs);
    }
</script>
<%
    Response.ContentType = "text/html";

    Chunk("<!DOCTYPE html>\n<html>\n<head>\n<title>Streamed HTML fixture</title>\n</head>\n");
    Chunk("<body>\n<h1>Streamed HTML fixture</h1>\n");
    Chunk("<p>This page is written in several flushed chunks so the module must handle multiple sends for one response.</p>\n");
    Chunk("                                                        \n                        \n");
    Chunk("<p>The long whitespace runs around this paragraph collapse when the page is rewritten.</p>                                                       \n");
    Chunk("                                                        \n                        \n");
    Chunk("<img src=\"../mod_pagespeed_test/image_rewrite_with_flush/a.jpg\" alt=\"stream fixture image\">\n");
    Chunk("<p>The image above is an existing test image, so extend_cache can rewrite its URL.</p>                                                       \n");
    Chunk("<div>stream fixture body</div>\n                                                        \n");
    Chunk("<!-- stream-end -->\n</body>\n</html>\n");
%>
