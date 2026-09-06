// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

import { mount } from "svelte";
import App from "./App.svelte";
import "$lib/theme.css";

const app = mount(App, {
  target: document.getElementById("app")!,
});

export default app;
