<!--
  SPDX-License-Identifier: Apache-2.0
  Copyright (c) 2026 We-Amp B.V.
-->

<template>
  <div id="probe" class="box">
    <h1>Hello</h1>
    <p>Text  with   two spaces</p>
    <component :is="'style'">{{ css }}</component>
    <p><button id="inc" @click="n++">inc</button><span id="count">{{ n }}</span></p>
    <img :src="src" alt="pic" width="1" height="1">
    <iframe src="/f.html" title="f" width="1" height="1"></iframe>
  </div>
</template>
<script setup>
import { onMounted, ref } from 'vue';
const n = ref(0);
const src = '/i.png';
const css = '\n.box {\n  color: red;\n  padding: 4px 8px;\n}\n.t   { margin: 0 }\n';
onMounted(() => { window.__hydrated = true; });
</script>
