import { defineConfig } from "vite";
import { svelte } from "@sveltejs/vite-plugin-svelte";
import { viteSingleFile } from "vite-plugin-singlefile";

export default defineConfig({
  plugins: [svelte(), viteSingleFile()],
  resolve: {
    alias: {
      $lib: "/src/lib",
    },
  },
  server: {
    proxy: {
      "/stats_json": {
        target: "http://localhost:8080",
        rewrite: (path: string) => `/pagespeed_admin${path}`,
      },
      "/config": {
        target: "http://localhost:8080",
        rewrite: (path: string) => `/pagespeed_admin${path}`,
      },
      "/histograms": {
        target: "http://localhost:8080",
        rewrite: (path: string) => `/pagespeed_admin${path}`,
      },
      "/cache": {
        target: "http://localhost:8080",
        rewrite: (path: string) => `/pagespeed_admin${path}`,
      },
      "/console": {
        target: "http://localhost:8080",
        rewrite: (path: string) => `/pagespeed_admin${path}`,
      },
      "/message_history": {
        target: "http://localhost:8080",
        rewrite: (path: string) => `/pagespeed_admin${path}`,
      },
      "/graphs": {
        target: "http://localhost:8080",
        rewrite: (path: string) => `/pagespeed_admin${path}`,
      },
      "/v1/": {
        target: "http://localhost:8080",
        rewrite: (path: string) => `/pagespeed_admin${path}`,
      },
    },
  },
  build: {
    target: "esnext",
    assetsInlineLimit: Infinity,
  },
});
