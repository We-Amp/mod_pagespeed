// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// The HTML script close tag inside string, template and regex literals,
// plain and escaped: the bytes a script embedded in a page must keep.
var div = '<div id="x"></div>';
var close = '</script>';
var escaped = '<\\/script>';
var quoted = '<\/script>';
var tpl = `a${'</script>'}b`;
var re = /<\/script>/;
var writer = "document.write('<script src=\"a.js\"><\\/script>');";
