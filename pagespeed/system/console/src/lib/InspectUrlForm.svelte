<!--
  SPDX-License-Identifier: Apache-2.0
  Copyright (c) 2024-2026 We-Amp B.V.
-->

<script lang="ts">
  // "Inspect a URL": paste a full URL, press Enter, land on that URL's page
  // (the same on every console). Nothing is sent from here: the text is
  // parsed in the browser and becomes a route. Text that is not a full URL
  // stays in the box with one line saying what is expected.
  import { router } from "$lib/router.svelte";
  import { inspectTarget } from "$lib/utils/url-inspect";

  let text = $state("");
  let problem = $state("");

  function submit(event: SubmitEvent) {
    event.preventDefault();
    if (text.trim() === "") return;
    const target = inspectTarget(text);
    if (!target.ok) {
      problem = target.message;
      return;
    }
    problem = "";
    router.navigate(target.href);
  }
</script>

<div class="inspect" data-testid="inspect-url-form">
  <form class="control-group" onsubmit={submit} novalidate>
    <label class="field-label" for="inspect-url">Inspect a URL</label>
    <input
      id="inspect-url"
      class="field inspect-field"
      type="search"
      inputmode="url"
      autocomplete="off"
      autocapitalize="off"
      spellcheck="false"
      placeholder="https://www.example.com/image.jpg"
      data-testid="inspect-url"
      aria-describedby="inspect-url-problem"
      aria-invalid={problem !== "" ? "true" : undefined}
      bind:value={text}
      oninput={() => (problem = "")}
    />
    <button class="btn btn-secondary" type="submit">Inspect</button>
  </form>
  <!-- Always in the page, so a screen reader hears the line when it appears. -->
  <p class="note inspect-problem" id="inspect-url-problem" role="status" data-testid="inspect-url-problem">{problem}</p>
</div>

<style>
  .inspect {
    margin: 0 0 var(--ps-space-md);
  }

  /* Wide enough for a URL, never wider than the page. */
  .inspect-field {
    flex: 1 1 var(--ps-search-width);
    max-width: var(--ps-card-max);
  }

  .inspect-problem {
    margin: var(--ps-space-xs) 0 0;
  }

  .inspect-problem:empty {
    margin: 0;
  }
</style>
