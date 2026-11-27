<!-- SPDX-License-Identifier: Apache-2.0
     Copyright (c) 2024-2026 We-Amp B.V. -->

<script lang="ts">
  import { onMount, onDestroy } from 'svelte';

  // -- Types ------------------------------------------------------------------

  export type DiffMode = 'slider' | 'blink' | 'pixeldiff';

  interface DiffStats {
    totalPixels: number;
    diffPixels: number;
    diffPercent: number;
  }

  // -- Props ------------------------------------------------------------------

  let {
    beforeSrc,
    afterSrc,
    mode = 'slider',
    zoom = 1,
  }: {
    beforeSrc: string;
    afterSrc: string;
    mode?: DiffMode;
    zoom?: number;
  } = $props();

  // -- Internal state ---------------------------------------------------------

  let containerEl: HTMLDivElement | undefined = $state();
  let sliderPos = $state(50); // percent 0..100
  let dragging = $state(false);
  let blinkShowAfter = $state(false);
  let blinkAuto = $state(true);
  let blinkTimer: ReturnType<typeof setInterval> | null = null;
  let destroyed = false;

  // Image dimensions (derived from loaded images).
  let imgWidth = $state(0);
  let imgHeight = $state(0);
  let beforeLoaded = $state(false);
  let afterLoaded = $state(false);

  // Pixel diff state.
  let diffCanvas: HTMLCanvasElement | undefined = $state();
  let diffStats: DiffStats | null = $state(null);
  let diffComputing = $state(false);

  // -- Image load handling ----------------------------------------------------

  function onBeforeLoad(event: Event) {
    const img = event.target as HTMLImageElement;
    imgWidth = img.naturalWidth;
    imgHeight = img.naturalHeight;
    beforeLoaded = true;
  }

  function onAfterLoad() {
    afterLoaded = true;
  }

  // -- Slider drag handling ---------------------------------------------------

  function startDrag(event: MouseEvent | TouchEvent) {
    if (mode !== 'slider') return;
    event.preventDefault();
    dragging = true;
    updateSlider(event);
  }

  function onMove(event: MouseEvent | TouchEvent) {
    if (!dragging) return;
    updateSlider(event);
  }

  function stopDrag() {
    dragging = false;
  }

  function updateSlider(event: MouseEvent | TouchEvent) {
    if (!containerEl) return;
    const rect = containerEl.getBoundingClientRect();
    let clientX: number;
    if ('touches' in event) {
      clientX = event.touches[0].clientX;
    } else {
      clientX = event.clientX;
    }
    const x = clientX - rect.left;
    const pct = Math.max(0, Math.min(100, (x / rect.width) * 100));
    sliderPos = pct;
  }

  // -- Blink mode -------------------------------------------------------------

  function startBlink() {
    stopBlink();
    if (mode === 'blink' && blinkAuto) {
      blinkTimer = setInterval(() => {
        blinkShowAfter = !blinkShowAfter;
      }, 500);
    }
  }

  function stopBlink() {
    if (blinkTimer) {
      clearInterval(blinkTimer);
      blinkTimer = null;
    }
  }

  function toggleBlink() {
    blinkShowAfter = !blinkShowAfter;
  }

  // -- Pixel diff computation -------------------------------------------------

  async function computePixelDiff() {
    if (!beforeLoaded || !afterLoaded || !diffCanvas) return;
    diffComputing = true;
    diffStats = null;

    try {
      // Dynamic import for SSR safety.
      const pixelmatch = (await import('pixelmatch')).default;

      const beforeImg = new Image();
      beforeImg.crossOrigin = 'anonymous';
      beforeImg.src = beforeSrc;
      await new Promise<void>((resolve, reject) => {
        beforeImg.onload = () => resolve();
        beforeImg.onerror = () => reject(new Error('Failed to load before image'));
      });

      const afterImg = new Image();
      afterImg.crossOrigin = 'anonymous';
      afterImg.src = afterSrc;
      await new Promise<void>((resolve, reject) => {
        afterImg.onload = () => resolve();
        afterImg.onerror = () => reject(new Error('Failed to load after image'));
      });

      // Use the minimum dimensions to handle any mismatch.
      const w = Math.min(beforeImg.naturalWidth, afterImg.naturalWidth);
      const h = Math.min(beforeImg.naturalHeight, afterImg.naturalHeight);

      if (w <= 0 || h <= 0) {
        diffComputing = false;
        return;
      }

      // Draw images to offscreen canvases to get pixel data.
      const offBefore = new OffscreenCanvas(w, h);
      const ctxBefore = offBefore.getContext('2d')!;
      ctxBefore.drawImage(beforeImg, 0, 0, w, h);
      const dataBefore = ctxBefore.getImageData(0, 0, w, h);

      const offAfter = new OffscreenCanvas(w, h);
      const ctxAfter = offAfter.getContext('2d')!;
      ctxAfter.drawImage(afterImg, 0, 0, w, h);
      const dataAfter = ctxAfter.getImageData(0, 0, w, h);

      const diffData = new Uint8ClampedArray(w * h * 4);

      const numDiff = pixelmatch(
        dataBefore.data,
        dataAfter.data,
        diffData,
        w,
        h,
        { threshold: 0.1, alpha: 0.1, diffColor: [255, 0, 0] },
      );

      // Render diff to the visible canvas.
      diffCanvas.width = w;
      diffCanvas.height = h;
      const ctx = diffCanvas.getContext('2d')!;
      const imageData = new ImageData(diffData, w, h);
      ctx.putImageData(imageData, 0, 0);

      const totalPixels = w * h;
      diffStats = {
        totalPixels,
        diffPixels: numDiff,
        diffPercent: totalPixels > 0 ? (numDiff / totalPixels) * 100 : 0,
      };
    } catch (e) {
      console.error('Pixel diff failed:', e);
      diffStats = null;
    } finally {
      diffComputing = false;
    }
  }

  // -- Lifecycle & effects ----------------------------------------------------

  $effect(() => {
    // Restart blink when mode or blinkAuto changes.
    if (mode === 'blink') {
      startBlink();
    } else {
      stopBlink();
    }
  });

  $effect(() => {
    // Compute pixel diff when entering pixeldiff mode and both images loaded.
    const _before = beforeSrc;
    const _after = afterSrc;
    if (mode === 'pixeldiff' && beforeLoaded && afterLoaded) {
      computePixelDiff();
    }
  });

  // Reset loaded state when sources change.
  $effect(() => {
    const _b = beforeSrc;
    beforeLoaded = false;
    afterLoaded = false;
    diffStats = null;
  });

  $effect(() => {
    const _a = afterSrc;
    afterLoaded = false;
    diffStats = null;
  });

  onMount(() => {
    window.addEventListener('mouseup', stopDrag);
    window.addEventListener('touchend', stopDrag);
  });

  onDestroy(() => {
    destroyed = true;
    stopBlink();
    if (typeof window !== 'undefined') {
      window.removeEventListener('mouseup', stopDrag);
      window.removeEventListener('touchend', stopDrag);
    }
  });

  // -- Derived ----------------------------------------------------------------

  let displayWidth = $derived(imgWidth * zoom);
  let displayHeight = $derived(imgHeight * zoom);
</script>

<div
  class="image-diff"
  bind:this={containerEl}
  style="width:{displayWidth}px;height:{displayHeight}px"
>
  {#if mode === 'slider'}
    <!-- Slider mode: before on left, after on right with draggable divider -->
    <!-- svelte-ignore a11y_no_noninteractive_element_interactions -->
    <div
      class="slider-container"
      role="img"
      aria-label="Original/Optimized slider comparison"
      onmousedown={startDrag}
      onmousemove={onMove}
      ontouchstart={startDrag}
      ontouchmove={onMove}
    >
      <!-- After image (background, fully visible) -->
      <img
        src={afterSrc}
        alt="Optimized"
        class="slider-img after-img"
        style="width:{displayWidth}px;height:{displayHeight}px"
        onload={onAfterLoad}
        draggable="false"
      />

      <!-- Before image (clipped by divider position) -->
      <div
        class="slider-before-clip"
        style="width:{sliderPos}%;height:100%"
      >
        <img
          src={beforeSrc}
          alt="Original"
          class="slider-img before-img"
          style="width:{displayWidth}px;height:{displayHeight}px"
          onload={onBeforeLoad}
          draggable="false"
        />
      </div>

      <!-- Divider line and handle -->
      <div class="slider-divider" style="left:{sliderPos}%">
        <div class="slider-handle">
          <div class="slider-handle-line"></div>
          <div class="slider-handle-grip">
            <span class="grip-arrow left">&lsaquo;</span>
            <span class="grip-arrow right">&rsaquo;</span>
          </div>
          <div class="slider-handle-line"></div>
        </div>
      </div>

      <!-- Labels -->
      <span class="slider-label slider-label-before">Original</span>
      <span class="slider-label slider-label-after">Optimized</span>
    </div>

  {:else if mode === 'blink'}
    <!-- Blink mode: alternates images -->
    <!-- svelte-ignore a11y_no_static_element_interactions -->
    <div
      class="blink-container"
      onclick={toggleBlink}
      onkeydown={(e) => { if (e.key === ' ' || e.key === 'Enter') toggleBlink(); }}
      role="button"
      tabindex="0"
      aria-label="Click to toggle original/optimized"
    >
      {#if blinkShowAfter}
        <img
          src={afterSrc}
          alt="Optimized"
          class="blink-img"
          style="width:{displayWidth}px;height:{displayHeight}px"
          onload={onAfterLoad}
          draggable="false"
        />
        <span class="blink-label">Optimized</span>
      {:else}
        <img
          src={beforeSrc}
          alt="Original"
          class="blink-img"
          style="width:{displayWidth}px;height:{displayHeight}px"
          onload={onBeforeLoad}
          draggable="false"
        />
        <span class="blink-label">Original</span>
      {/if}
    </div>

  {:else if mode === 'pixeldiff'}
    <!-- Pixel diff mode: computed diff output -->
    <div class="pixeldiff-container">
      {#if diffComputing}
        <div class="pixeldiff-loading">
          <div class="ps-spinner" aria-label="Computing diff"></div>
          <span>Computing pixel diff...</span>
        </div>
      {/if}

      <canvas
        bind:this={diffCanvas}
        class="pixeldiff-canvas"
        style="width:{displayWidth}px;height:{displayHeight}px"
      ></canvas>

      {#if diffStats}
        <div class="pixeldiff-stats">
          <span class="stat">
            {diffStats.diffPixels.toLocaleString()} different pixels
          </span>
          <span class="stat">
            {diffStats.diffPercent.toFixed(2)}% changed
          </span>
          <span class="stat">
            {diffStats.totalPixels.toLocaleString()} total
          </span>
        </div>
      {/if}

      <!-- Hidden images for loading -->
      <img
        src={beforeSrc}
        alt=""
        class="hidden-img"
        onload={onBeforeLoad}
        draggable="false"
      />
      <img
        src={afterSrc}
        alt=""
        class="hidden-img"
        onload={onAfterLoad}
        draggable="false"
      />
    </div>
  {/if}
</div>

<style>
  .image-diff {
    position: relative;
    overflow: hidden;
    border: 1px solid var(--ps-border);
    border-radius: var(--ps-radius-md);
    background:
      repeating-conic-gradient(#e0e0e0 0% 25%, #f5f5f5 0% 50%)
      50% / 16px 16px;
    user-select: none;
  }

  @media (prefers-color-scheme: dark) {
    :global(:root:not([data-vscode])) .image-diff {
      background:
        repeating-conic-gradient(#333 0% 25%, #2a2a2a 0% 50%)
        50% / 16px 16px;
    }
  }

  /* -- Slider mode -------------------------------------------------------- */

  .slider-container {
    position: relative;
    width: 100%;
    height: 100%;
    cursor: ew-resize;
  }

  .slider-img {
    display: block;
    object-fit: contain;
    pointer-events: none;
  }

  .after-img {
    position: absolute;
    top: 0;
    left: 0;
  }

  .slider-before-clip {
    position: absolute;
    top: 0;
    left: 0;
    overflow: hidden;
    z-index: 1;
  }

  .before-img {
    display: block;
  }

  .slider-divider {
    position: absolute;
    top: 0;
    bottom: 0;
    z-index: 2;
    transform: translateX(-50%);
    display: flex;
    flex-direction: column;
    align-items: center;
  }

  .slider-handle {
    display: flex;
    flex-direction: column;
    align-items: center;
    height: 100%;
  }

  .slider-handle-line {
    flex: 1;
    width: 2px;
    background: var(--ps-accent);
  }

  .slider-handle-grip {
    display: flex;
    align-items: center;
    justify-content: center;
    width: 32px;
    height: 32px;
    background: var(--ps-accent);
    border-radius: 50%;
    color: var(--ps-accent-fg);
    font-size: 14px;
    font-weight: 700;
    flex-shrink: 0;
    box-shadow: 0 2px 6px rgba(0, 0, 0, 0.3);
    gap: 2px;
  }

  .grip-arrow {
    line-height: 1;
  }

  .slider-label {
    position: absolute;
    top: var(--ps-space-sm);
    z-index: 3;
    font-family: var(--ps-font-family);
    font-size: var(--ps-font-size-sm);
    font-weight: 600;
    color: #fff;
    background: rgba(0, 0, 0, 0.5);
    padding: 2px var(--ps-space-sm);
    border-radius: var(--ps-radius-sm);
    pointer-events: none;
  }

  .slider-label-before {
    left: var(--ps-space-sm);
  }

  .slider-label-after {
    right: var(--ps-space-sm);
  }

  /* -- Blink mode --------------------------------------------------------- */

  .blink-container {
    position: relative;
    width: 100%;
    height: 100%;
    cursor: pointer;
    outline: none;
  }

  .blink-container:focus-visible {
    outline: 2px solid var(--ps-border-focus);
    outline-offset: -2px;
  }

  .blink-img {
    display: block;
    object-fit: contain;
    pointer-events: none;
  }

  .blink-label {
    position: absolute;
    top: var(--ps-space-sm);
    left: var(--ps-space-sm);
    font-family: var(--ps-font-family);
    font-size: var(--ps-font-size-sm);
    font-weight: 600;
    color: #fff;
    background: rgba(0, 0, 0, 0.5);
    padding: 2px var(--ps-space-sm);
    border-radius: var(--ps-radius-sm);
    pointer-events: none;
  }

  /* -- Pixel diff mode ---------------------------------------------------- */

  .pixeldiff-container {
    position: relative;
    width: 100%;
    height: 100%;
  }

  .pixeldiff-canvas {
    display: block;
    image-rendering: pixelated;
  }

  .pixeldiff-loading {
    position: absolute;
    top: 50%;
    left: 50%;
    transform: translate(-50%, -50%);
    display: flex;
    align-items: center;
    gap: var(--ps-space-sm);
    font-family: var(--ps-font-family);
    font-size: var(--ps-font-size);
    color: var(--ps-fg-secondary);
    background: var(--ps-bg-tertiary);
    padding: var(--ps-space-sm) var(--ps-space-lg);
    border-radius: var(--ps-radius-md);
    border: 1px solid var(--ps-border);
    z-index: 1;
  }

  .pixeldiff-stats {
    position: absolute;
    bottom: var(--ps-space-sm);
    left: var(--ps-space-sm);
    display: flex;
    gap: var(--ps-space-md);
    font-family: var(--ps-font-mono);
    font-size: var(--ps-font-size-sm);
    color: #fff;
    background: rgba(0, 0, 0, 0.6);
    padding: var(--ps-space-xs) var(--ps-space-sm);
    border-radius: var(--ps-radius-sm);
    pointer-events: none;
  }

  .stat {
    white-space: nowrap;
  }

  .hidden-img {
    position: absolute;
    width: 1px;
    height: 1px;
    opacity: 0;
    pointer-events: none;
  }
</style>
