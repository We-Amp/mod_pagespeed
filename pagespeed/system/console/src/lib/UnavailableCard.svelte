<!--
  SPDX-License-Identifier: Apache-2.0
  Copyright (c) 2024-2026 We-Amp B.V.
-->

<script lang="ts" module>
  export const DOCS_URL = "https://modpagespeed.com/docs/admin-console/";
</script>

<script lang="ts">
  // A view the server has turned off (see UnavailableError): said plainly,
  // with no error styling, no alert role and no retry.
  import EmptyState from "$lib/EmptyState.svelte";

  let { testid = "unavailable" }: { testid?: string } = $props();
</script>

<EmptyState
  title="Not available on this console."
  detail="The server administrator has turned this view off."
  action={{ label: "About the admin console", href: DOCS_URL }}
  {testid}
/>
