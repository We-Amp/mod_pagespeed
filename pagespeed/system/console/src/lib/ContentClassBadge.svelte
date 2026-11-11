<!--
  SPDX-License-Identifier: Apache-2.0
  Copyright (c) 2024-2026 We-Amp B.V.
-->

<script lang="ts">
  // Content-class badge (photo / screenshot / illustration / noisy).
  let { contentClass }: { contentClass: string } = $props();
  let label = $derived(
    contentClass.charAt(0).toUpperCase() + contentClass.slice(1),
  );
  let icon = $derived(
    contentClass === "photo"
      ? "\u{1D4AB}"
      : contentClass === "screenshot"
        ? "▣"
        : contentClass === "illustration"
          ? "⭐"
          : contentClass === "noisy"
            ? "≈"
            : "●",
  );
  // Each known class has its category tint; any other stays neutral.
  const TINT: Record<string, string> = {
    photo: "badge-tint-warning",
    screenshot: "badge-tint-primary",
    illustration: "badge-tint-ok",
    noisy: "badge-tint-error",
  };
  let cls = $derived(TINT[contentClass.replace(/[^a-z]/g, "")] ?? "");
</script>

<span class="badge content-class-badge {cls}" aria-label="Content class: {label}">
  <span class="content-class-icon" aria-hidden="true">{icon}</span>
  {label}
</span>

<style>
  .content-class-badge {
    gap: 3px;
    font-weight: 500;
  }
  .content-class-icon {
    font-size: 10px;
    line-height: 1;
  }
</style>
