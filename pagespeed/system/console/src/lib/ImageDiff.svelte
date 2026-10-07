<!--
  SPDX-License-Identifier: Apache-2.0
  Copyright (c) 2024-2026 We-Amp B.V.
-->

<script lang="ts">
  // Before/after image comparison, ported from the 2.0 dashboard: slider
  // and blink modes. The comparison is a labelled group; each image keeps
  // its own alt text; the slider handle is a sibling role="slider" control
  // (arrows ±5, Home/End) — never inside an image role. Blink is a toggle
  // button over two mounted images (no timer, no refetch per toggle). Both
  // images load through the page's shared limiter.
  import QueuedImage from "$lib/QueuedImage.svelte";
  import type { Limiter } from "$lib/utils/image-queue";
  import { sliderKeyTarget, sliderValueText, type DiffMode } from "$lib/utils/image-diff";

  let {
    beforeSrc,
    afterSrc,
    beforeAlt,
    afterAlt,
    limiter,
    beforeProbe,
    afterProbe,
    mode = "slider",
    zoom = 1,
  }: {
    beforeSrc: string;
    afterSrc: string;
    beforeAlt: string;
    afterAlt: string;
    limiter: Limiter;
    beforeProbe: () => Promise<number>;
    afterProbe: () => Promise<number>;
    mode?: DiffMode;
    zoom?: number;
  } = $props();

  let frameEl: HTMLDivElement | undefined = $state();
  let handleEl: HTMLDivElement | undefined = $state();
  let sliderPos = $state(50); // percent 0..100
  let dragging = $state(false);
  let showAfter = $state(false);
  let imgWidth = $state(0);
  let imgHeight = $state(0);

  let displayWidth = $derived(imgWidth * zoom);
  let displayHeight = $derived(imgHeight * zoom);
  let sizeStyle = $derived(
    imgWidth > 0 ? `width:${displayWidth}px;height:${displayHeight}px` : "",
  );

  function onBeforeReady(img: HTMLImageElement) {
    imgWidth = img.naturalWidth;
    imgHeight = img.naturalHeight;
  }

  // -- Slider: a pointer drag on the frame, keys on the handle -------------

  function updateSlider(clientX: number) {
    if (!frameEl) return;
    const rect = frameEl.getBoundingClientRect();
    if (rect.width === 0) return;
    sliderPos = Math.max(0, Math.min(100, ((clientX - rect.left) / rect.width) * 100));
  }

  // Pointer events with capture on the FRAME: one handler covers mouse,
  // touch and pen, and a press on the handle starts the drag too — capture
  // keeps every move coming to the frame even with the divider (or the
  // window edge) under the cursor.
  function startDrag(event: PointerEvent) {
    if (event.button !== 0) return; // primary button / touch / pen only
    event.preventDefault(); // no text selection, no native image drag
    handleEl?.focus(); // preventDefault above would suppress the native focus
    frameEl?.setPointerCapture(event.pointerId);
    dragging = true;
    updateSlider(event.clientX);
  }

  function onMove(event: PointerEvent) {
    if (dragging) updateSlider(event.clientX);
  }

  function stopDrag(event: PointerEvent) {
    if (!dragging) return;
    dragging = false;
    if (frameEl?.hasPointerCapture(event.pointerId)) {
      frameEl.releasePointerCapture(event.pointerId);
    }
  }

  function onSliderKeydown(event: KeyboardEvent) {
    const next = sliderKeyTarget(event.key, sliderPos);
    if (next === null) return;
    event.preventDefault();
    sliderPos = next;
  }
</script>

<div class="image-diff" role="group" aria-label="Original and optimized comparison">
  {#if mode === "slider"}
    <!-- Pointer dragging is a mouse/touch convenience; the keyboard control
         is the role="slider" handle below. -->
    <!-- svelte-ignore a11y_no_static_element_interactions -->
    <div
      class="slider-frame"
      style={sizeStyle}
      bind:this={frameEl}
      onpointerdown={startDrag}
      onpointermove={onMove}
      onpointerup={stopDrag}
      onpointercancel={stopDrag}
    >
      <div class="slider-stage">
        <div class="after-layer">
          <QueuedImage
            src={afterSrc}
            alt={afterAlt}
            {limiter}
            probe={afterProbe}
            imgClass="diff-img"
            imgStyle={sizeStyle}
          />
        </div>
        <div class="slider-before-clip" style="width:{sliderPos}%">
          <QueuedImage
            src={beforeSrc}
            alt={beforeAlt}
            {limiter}
            probe={beforeProbe}
            imgClass="diff-img"
            imgStyle={sizeStyle}
            onready={onBeforeReady}
          />
        </div>
        <span class="diff-label diff-label-before" aria-hidden="true">Original</span>
        <span class="diff-label diff-label-after" aria-hidden="true">Optimized</span>
      </div>
      <div class="slider-divider" style="left:{sliderPos}%">
        <div
          class="slider-handle"
          role="slider"
          tabindex="0"
          aria-label="Comparison position"
          aria-valuemin={0}
          aria-valuemax={100}
          aria-valuenow={Math.round(sliderPos)}
          aria-valuetext={sliderValueText(sliderPos)}
          bind:this={handleEl}
          onkeydown={onSliderKeydown}
        >
          <span class="slider-handle-line" aria-hidden="true"></span>
          <span class="slider-handle-grip" aria-hidden="true">
            <span class="grip-arrow">&lsaquo;</span>
            <span class="grip-arrow">&rsaquo;</span>
          </span>
          <span class="slider-handle-line" aria-hidden="true"></span>
        </div>
      </div>
    </div>
  {:else}
    <div class="blink-frame" style={sizeStyle}>
      <div class="blink-layer" hidden={showAfter}>
        <QueuedImage
          src={beforeSrc}
          alt={beforeAlt}
          {limiter}
          probe={beforeProbe}
          imgClass="diff-img"
          imgStyle={sizeStyle}
          onready={onBeforeReady}
        />
      </div>
      <div class="blink-layer" hidden={!showAfter}>
        <QueuedImage
          src={afterSrc}
          alt={afterAlt}
          {limiter}
          probe={afterProbe}
          imgClass="diff-img"
          imgStyle={sizeStyle}
        />
      </div>
      <span class="diff-label diff-label-before" data-testid="blink-label">
        {showAfter ? "Optimized" : "Original"}
      </span>
    </div>
    <button
      type="button"
      class="btn btn-secondary blink-toggle"
      aria-pressed={showAfter}
      aria-label="Toggle between original and optimized"
      onclick={() => (showAfter = !showAfter)}
    >
      Show {showAfter ? "original" : "optimized"}
    </button>
  {/if}
</div>

<style>
  .image-diff {
    display: inline-flex;
    flex-direction: column;
    gap: var(--ps-space-sm);
  }
  .slider-frame,
  .blink-frame {
    position: relative;
    overflow: hidden;
    min-width: 64px;
    min-height: 64px;
    border: 1px solid var(--ps-border);
    border-radius: var(--ps-border-radius-lg);
    background: repeating-conic-gradient(var(--ps-border) 0% 25%, var(--ps-bg-secondary) 0% 50%) 50% / 16px 16px;
    user-select: none;
  }
  .slider-frame {
    /* The frame owns the pointer drag: without this a touch drag scrolls. */
    touch-action: none;
  }
  .slider-stage {
    position: absolute;
    inset: 0;
    cursor: ew-resize;
  }
  .after-layer,
  .slider-before-clip {
    position: absolute;
    top: 0;
    left: 0;
    height: 100%;
  }
  .slider-before-clip {
    overflow: hidden;
    z-index: 1;
  }
  .image-diff :global(.diff-img) {
    display: block;
    max-width: none;
    object-fit: contain;
    pointer-events: none;
  }
  .slider-divider {
    position: absolute;
    top: 0;
    bottom: 0;
    z-index: 2;
    transform: translateX(-50%);
  }
  .slider-handle {
    display: flex;
    flex-direction: column;
    align-items: center;
    height: 100%;
    border-radius: var(--ps-border-radius);
    cursor: ew-resize;
  }
  .slider-handle:focus-visible {
    outline: 2px solid var(--ps-primary);
    outline-offset: 2px;
  }
  .slider-handle-line {
    flex: 1;
    width: 2px;
    background: var(--ps-primary);
  }
  .slider-handle-grip {
    display: flex;
    align-items: center;
    justify-content: center;
    width: 32px;
    height: 32px;
    background: var(--ps-primary);
    border-radius: 50%;
    color: var(--ps-text-inverse);
    font-size: var(--ps-font-size-sm);
    font-weight: 700;
    flex-shrink: 0;
    box-shadow: var(--ps-shadow-md);
    gap: 2px;
  }
  .grip-arrow {
    line-height: 1;
  }
  .blink-layer {
    position: absolute;
    inset: 0;
  }
  .blink-layer[hidden] {
    display: block;
    visibility: hidden;
  }
  .diff-label {
    position: absolute;
    top: var(--ps-space-sm);
    z-index: 3;
    font-size: var(--ps-font-size-sm);
    font-weight: 600;
    color: var(--ps-on-scrim);
    background: var(--ps-scrim);
    padding: 2px var(--ps-space-sm);
    border-radius: var(--ps-border-radius);
    pointer-events: none;
  }
  .diff-label-before {
    left: var(--ps-space-sm);
  }
  .diff-label-after {
    right: var(--ps-space-sm);
  }
  .blink-toggle {
    align-self: flex-start;
  }
</style>
