// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// Builds one server-rendered page per framework version from the versions this
// directory's package-lock.json pins. Output: out/<fixture>/index.html and its
// bundle, plus the shared stylesheets. Each page sets window.__hydrated once
// the framework has taken over, and #count increments on a click of #inc only
// if it has.

import fs from 'node:fs';
import path from 'node:path';
import { createRequire } from 'node:module';
import { fileURLToPath } from 'node:url';
import zlib from 'node:zlib';
import esbuild from 'esbuild';
import sveltePlugin from 'esbuild-svelte';

const HERE = path.dirname(fileURLToPath(import.meta.url));
const OUT = path.join(HERE, 'out');
const WORK = path.join(OUT, '.work');
const require = createRequire(import.meta.url);
fs.mkdirSync(WORK, { recursive: true });

// Whitespace a CSS minifier would remove: an edited copy differs from the
// server-rendered text.
const STYLE = '\n.box {\n  color: red;\n  padding: 4px 8px;\n}\n.t   { margin: 0 }\n';
fs.writeFileSync(path.join(OUT, 'a.css'), 'body {  margin:  0;  }\n.box {  display:  block;  }\n');
fs.writeFileSync(path.join(OUT, 'b.css'), '.p {  color:  green;  }\n');
fs.writeFileSync(path.join(OUT, 'f.html'), '<!doctype html><title>f</title>\n');
fs.writeFileSync(path.join(OUT, 'async.js'), 'window.__asyncRuns = (window.__asyncRuns || 0) + 1;\n');
// A 1x1 PNG with padding the image rewriter can drop.
fs.writeFileSync(path.join(OUT, 'i.png'), Buffer.from(
    'iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mNkYPhfDwAChwGA60e6kgAAAABJRU5ErkJggg==',
    'base64'));

// A 160x160 PNG stored without compression: too large to inline, so the image
// rewriter serves it under a new URL. Coloured blocks with a little fixed noise
// keep the optimized copy above the inlining limit as well.
function storedPng(width, height) {
  const crcTable = Array.from({ length: 256 }, (_, n) => {
    let c = n;
    for (let k = 0; k < 8; k++) c = (c & 1) ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
    return c >>> 0;
  });
  const crc = (buf) => {
    let c = 0xffffffff;
    for (const b of buf) c = crcTable[(c ^ b) & 0xff] ^ (c >>> 8);
    return (c ^ 0xffffffff) >>> 0;
  };
  const chunk = (type, data) => {
    const head = Buffer.alloc(8);
    head.writeUInt32BE(data.length, 0);
    head.write(type, 4, 'ascii');
    const tail = Buffer.alloc(4);
    tail.writeUInt32BE(crc(Buffer.concat([head.subarray(4), data])), 0);
    return Buffer.concat([head, data, tail]);
  };
  const ihdr = Buffer.alloc(13);
  ihdr.writeUInt32BE(width, 0);
  ihdr.writeUInt32BE(height, 4);
  ihdr.set([8, 2, 0, 0, 0], 8);  // 8-bit RGB
  const rows = Buffer.alloc((width * 3 + 1) * height);
  let seed = 12345;
  for (let y = 0; y < height; y++) {
    const row = y * (width * 3 + 1);
    for (let x = 0; x < width; x++) {
      const block = ((x >> 5) + (y >> 5) * 5) * 37;
      for (let ch = 0; ch < 3; ch++) {
        seed = (seed * 1103515245 + 12345) >>> 0;
        rows[row + 1 + x * 3 + ch] = ((block * (ch + 1)) & 0xf8) | ((seed >>> 16) & 7);
      }
    }
  }
  return Buffer.concat([
    Buffer.from([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a]),
    chunk('IHDR', ihdr),
    chunk('IDAT', zlib.deflateSync(rows, { level: 0 })),
    chunk('IEND', Buffer.alloc(0)),
  ]);
}
fs.writeFileSync(path.join(OUT, 'big.png'), storedPng(160, 160));

const SHELL_HEAD = '<meta charset="utf-8"><title>t</title>' +
    '<link rel="stylesheet" href="/a.css"><link rel="stylesheet" href="/b.css">' +
    '<style>\n.hd {  color:  blue;  }\n</style>' +
    '<script src="/async.js" async></script>';
const shell = (name, body) => '<!doctype html><html><head>' + SHELL_HEAD +
    `</head><body><div id="app">${body}</div>` +
    `<script src="/${name}/bundle.js" defer></script></body></html>`;

const PROD = { 'process.env.NODE_ENV': '"production"' };

async function bundle(name, source, options = {}) {
  const dir = path.join(OUT, name);
  fs.mkdirSync(dir, { recursive: true });
  const entry = path.join(WORK, `${name}${options.ext || '.jsx'}`);
  fs.writeFileSync(entry, source);
  await esbuild.build({
    entryPoints: [entry],
    bundle: true,
    outfile: path.join(dir, 'bundle.js'),
    define: PROD,
    minify: true,
    logLevel: 'error',
    nodePaths: [path.join(HERE, 'node_modules')],
    ...options.esbuild,
  });
}

function write(name, html) {
  const dir = path.join(OUT, name);
  fs.mkdirSync(dir, { recursive: true });
  fs.writeFileSync(path.join(dir, 'index.html'), html);
}

// One component text for every React version, with the imports passed in. The
// style element and the iframe sit inside the hydrated tree. `wrap` adds a
// Suspense boundary. React 19 renders a preload hint for each image.
const reactApp = (wrap) => `
const e = React.createElement;
const STYLE = ${JSON.stringify(STYLE)};
function Counter() {
  const [n, setN] = React.useState(0);
  React.useEffect(() => { window.__hydrated = true; }, []);
  return e('p', null,
      e('button', {id: 'inc', onClick: () => setN(n + 1)}, 'inc'),
      e('span', {id: 'count'}, String(n)));
}
const Box = () => e('div', {className: 'box', id: 'probe'},
    e('h1', null, 'Hello'), e('p', null, 'Text  with   two spaces'), e('style', null, STYLE), e(Counter),
    e('img', {src: '/i.png', alt: 'pic', width: 1, height: 1}),
    e('img', {src: '/big.png', alt: 'big', width: 160, height: 160}),
    e('iframe', {src: '/f.html', title: 'f', width: 1, height: 1}));
const App = ${wrap ? "() => e(React.Suspense, {fallback: null}, e(Box))" : 'Box'};
`;

// The older Reacts live under aliases; the bundler points each copy of react-dom
// at the matching react, for the server render as well as the client.
async function reactRootFixture(name, wrap, reactPkg, domPkg, clientMount) {
  const alias = { react: reactPkg, 'react-dom': domPkg };
  const ssrEntry = path.join(WORK, `${name}-ssr.jsx`);
  const ssrOut = path.join(WORK, `${name}-ssr.cjs`);
  fs.writeFileSync(ssrEntry, `import React from 'react';
import {renderToString} from 'react-dom/server';
${reactApp(wrap)}
export default renderToString(e(App));`);
  await esbuild.build({
    entryPoints: [ssrEntry],
    bundle: true,
    platform: 'node',
    format: 'cjs',
    outfile: ssrOut,
    define: PROD,
    alias,
    logLevel: 'error',
    nodePaths: [path.join(HERE, 'node_modules')],
  });
  write(name, shell(name, require(ssrOut).default));
  await bundle(name, `import React from 'react';
import * as ReactDOM from '${clientMount.module}';
${reactApp(wrap)}
const root = document.getElementById('app');
window.__recoverable = [];
${clientMount.code}`, { esbuild: { alias } });
}

const MOUNT_19 = {
  module: 'react-dom/client',
  code: `ReactDOM.hydrateRoot(root, e(App), {onRecoverableError: (err) => window.__recoverable.push(String(err))});`,
};
await reactRootFixture('react19-root', true, 'react', 'react-dom', MOUNT_19);
await reactRootFixture('react19-root-plain', false, 'react', 'react-dom', MOUNT_19);
await reactRootFixture('react18-root', true, 'react18', 'react18-dom', MOUNT_19);
await reactRootFixture('react18-root-plain', false, 'react18', 'react18-dom', MOUNT_19);
await reactRootFixture('react17-root', false, 'react17', 'react17-dom', {
  module: 'react-dom',
  code: 'ReactDOM.hydrate(e(App), root);',
});

// React owns the whole document. `precedence` makes React emit data-precedence
// and manage that stylesheet by its href.
const reactDoc = (precedence) => `${reactApp(false)}
const Doc = () => e('html', {lang: 'en'},
    e('head', null,
      e('meta', {charSet: 'utf-8'}), e('title', null, 't'),
      e('link', {rel: 'stylesheet', href: '/a.css'${precedence ? ", precedence: 'default'" : ''}}),
      e('link', {rel: 'stylesheet', href: '/b.css'${precedence ? ", precedence: 'default'" : ''}}),
      e('style', null, '\\n.hd {  color:  blue;  }\\n'),
      e('script', {src: '/async.js', async: true})),
    e('body', null, e('div', {id: 'app'}, e(App))NAME_SLOT));
`;
for (const [name, precedence] of [['react19-doc', true], ['react19-doc-plain', false]]) {
  const React = require('react');
  const { renderToString } = require('react-dom/server');
  const script = `, e('script', {src: '/${name}/bundle.js', defer: true})`;
  const source = reactDoc(precedence).replace('NAME_SLOT', script);
  const Doc = new Function('React', `${source}; return Doc;`)(React);
  write(name, '<!doctype html>' + renderToString(React.createElement(Doc)));
  await bundle(name, `import React from 'react';
import {hydrateRoot} from 'react-dom/client';
${source}
window.__recoverable = [];
hydrateRoot(document, e(Doc), {onRecoverableError: (err) => window.__recoverable.push(String(err))});`);
}

// Vue. `fragment` renders two root nodes, which Vue brackets with comments.
const vueApp = (fragment) => `
const STYLE = ${JSON.stringify(STYLE)};
const App = {
  data: () => ({n: 0}),
  mounted() { window.__hydrated = true; },
  render() {
    const box = h('div', {class: 'box', id: 'probe'}, [
      h('h1', 'Hello'), h('p', 'Text  with   two spaces'), h('style', STYLE),
      h('p', [h('button', {id: 'inc', onClick: () => this.n++}, 'inc'),
              h('span', {id: 'count'}, String(this.n))]),
      h('img', {src: '/i.png', alt: 'pic', width: 1, height: 1}),
      h('iframe', {src: '/f.html', title: 'f', width: 1, height: 1})]);
    return ${fragment ? "[box, h('footer', 'f')]" : 'box'};
  },
};
`;
const vueDefine = {
  __VUE_OPTIONS_API__: 'true',
  __VUE_PROD_DEVTOOLS__: 'false',
  __VUE_PROD_HYDRATION_MISMATCH_DETAILS__: 'false',
  ...PROD,
};
for (const [name, fragment] of [['vue', true], ['vue-plain', false]]) {
  const { createSSRApp, h } = await import('vue');
  const { renderToString } = await import('@vue/server-renderer');
  const App = new Function('h', `${vueApp(fragment)}; return App;`)(h);
  write(name, shell(name, await renderToString(createSSRApp(App))));
  await bundle(name, `import {createSSRApp, h} from 'vue';
${vueApp(fragment)}
createSSRApp(App).mount('#app');`, { ext: '.js', esbuild: { define: vueDefine } });
}

// Svelte.
const SVELTE = `<script>
  import {onMount} from 'svelte';
  let n = $state(0);
  onMount(() => { window.__hydrated = true; });
</script>
<div class="box" id="probe"><h1>Hello</h1><p>Text  with   two spaces</p>{@html ${JSON.stringify(`<style>${STYLE}</style>`)}}<p><button id="inc" onclick={() => n++}>inc</button><span id="count">{n}</span></p><img src="/i.png" alt="pic" width="1" height="1"><iframe src="/f.html" title="f" width="1" height="1"></iframe></div>
`;
{
  const name = 'svelte';
  const appPath = path.join(WORK, 'App.svelte');
  fs.writeFileSync(appPath, SVELTE);
  await bundle(name, `import {hydrate} from 'svelte';
import App from './App.svelte';
hydrate(App, {target: document.getElementById('app')});`, {
    ext: '.js',
    esbuild: {
      plugins: [sveltePlugin({ compilerOptions: { generate: 'client' } })],
      conditions: ['browser', 'production'],
    },
  });
  const ssrEntry = path.join(WORK, 'svelte-ssr.js');
  const ssrOut = path.join(WORK, 'svelte-ssr.mjs');
  fs.writeFileSync(ssrEntry, `import {render} from 'svelte/server';
import App from './App.svelte';
export default render(App).body;`);
  await esbuild.build({
    entryPoints: [ssrEntry],
    bundle: true,
    platform: 'node',
    format: 'esm',
    outfile: ssrOut,
    plugins: [sveltePlugin({ compilerOptions: { generate: 'server' } })],
    logLevel: 'error',
    nodePaths: [path.join(HERE, 'node_modules')],
  });
  const body = (await import(ssrOut)).default;
  write(name, shell(name, body));
}

fs.rmSync(WORK, { recursive: true, force: true });
console.log('built');
