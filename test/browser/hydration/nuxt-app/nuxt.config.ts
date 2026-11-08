// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 We-Amp B.V.

export default defineNuxtConfig({
  app: {
    baseURL: '/nuxt/',
    head: { link: [{ rel: 'stylesheet', href: '/a.css' }, { rel: 'stylesheet', href: '/b.css' }] },
  },
  ssr: true,
  telemetry: false,
  devtools: { enabled: false },
});
