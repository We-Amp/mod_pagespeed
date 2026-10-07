// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

/**
 * "Copy diagnostics" on the Support page: a plain-text bundle to paste into
 * a support request -- the builds, the optimizer's version, a fingerprint of
 * the configuration (its SHA-256; the configuration itself is not included)
 * and the last warnings and errors. It is assembled from three reads of this
 * console's own admin pages and goes only to the clipboard (or, when the
 * browser refuses, into a text box on the page): nothing is sent anywhere.
 */

import type { ConfigResponse, DaemonHealthResponse, MessagesResponse } from "$lib/api/types";
import { PRODUCT_NAME } from "$lib/data/product-facts-console";
import { daemonAvailability } from "$lib/utils/overview";
import { sha256Hex } from "$lib/utils/sha256";
import { optimizerVersions } from "$lib/utils/versions";

/** How many warnings and errors the bundle lists. */
export const DIAGNOSTICS_MESSAGES = 50;

export interface DiagnosticsInput {
  generatedAt: number;
  /** "whole server" or "this host (<host>)". */
  scope: string;
  moduleBuild: string;
  consoleBuild: string;
  optimizer: string;
  configSha256: string | null;
  /** The message_history list (oldest first); anything else lists nothing. */
  messages: unknown;
}

export interface DiagnosticsApi {
  getConfig(): Promise<ConfigResponse>;
  daemonHealth(): Promise<DaemonHealthResponse>;
  getMessages(): Promise<MessagesResponse>;
}

export interface Clipboardish {
  writeText(text: string): Promise<void>;
}

const SERIOUS: ReadonlySet<string> = new Set(["warning", "error", "fatal"]);

/** The last `n` warnings and worse of a message list (oldest first), newest first. */
export function lastWarnings(messages: unknown, n: number = DIAGNOSTICS_MESSAGES): Array<{ severity: string; message: string }> {
  if (!Array.isArray(messages)) return [];
  const out: Array<{ severity: string; message: string }> = [];
  for (const entry of messages) {
    if (entry === null || typeof entry !== "object") continue;
    const { severity, message } = entry as { severity?: unknown; message?: unknown };
    if (typeof severity === "string" && SERIOUS.has(severity) && typeof message === "string") {
      out.push({ severity, message });
    }
  }
  return out.slice(-n).reverse();
}

/** One line: every control character (line breaks included, tab excepted) becomes a space. */
function oneLine(s: string): string {
  return s.replace(/[\u0000-\u0008\u000a-\u001f\u007f]/g, " ");
}

export function diagnosticsText(d: DiagnosticsInput): string {
  const warnings = lastWarnings(d.messages);
  const heading =
    warnings.length === 0
      ? "Warnings and errors: none in the log"
      : warnings.length === 1
        ? "The last warning or error:"
        : `The last ${warnings.length} warnings and errors, newest first:`;
  const lines = [
    `${PRODUCT_NAME} diagnostics`,
    `Generated: ${new Date(d.generatedAt).toISOString()}`,
    `Console: ${d.scope}`,
    `Module build: ${d.moduleBuild}`,
    `Console build: ${d.consoleBuild}`,
    `Optimizer: ${d.optimizer}`,
    `Configuration SHA-256: ${d.configSha256 ?? "unavailable (the configuration could not be read)"}`,
    "",
    heading,
    ...warnings.map((w) => `[${w.severity}] ${oneLine(w.message)}`),
  ];
  return `${lines.join("\n")}\n`;
}

/** What the bundle says about the optimizer: its version, or why there is none. */
export function optimizerLine(health: PromiseSettledResult<DaemonHealthResponse>): string {
  if (health.status === "fulfilled") {
    const v = optimizerVersions(health.value);
    if (v.version === null) return "answering, version not reported";
    return v.commit === null ? v.version : `${v.version} (${v.commit})`;
  }
  const reason: unknown = health.reason;
  if (!(reason instanceof Error)) return "did not answer";
  switch (daemonAvailability(reason)) {
    case "not-configured":
      return "not configured";
    case "unreachable":
      return "configured, not answering";
    case "unsupported":
      return "too old to report its version";
    default:
      return "did not answer";
  }
}

/** SHA-256 of the configuration this console's scope runs with, else of the server's; null without either. */
export function configHash(config: unknown): string | null {
  if (config === null || typeof config !== "object") return null;
  const c = config as { effective_config?: unknown; config?: unknown };
  const text =
    typeof c.effective_config === "string" && c.effective_config !== ""
      ? c.effective_config
      : typeof c.config === "string"
        ? c.config
        : null;
  return text === null ? null : sha256Hex(text);
}

/** Read the three sources once and assemble the bundle. Never rejects. */
export async function collectDiagnostics(
  api: DiagnosticsApi,
  scope: string,
  build: string,
  now: () => number = Date.now,
): Promise<string> {
  const [config, health, messages] = await Promise.allSettled([api.getConfig(), api.daemonHealth(), api.getMessages()]);
  const list: unknown = messages.status === "fulfilled" ? messages.value : null;
  return diagnosticsText({
    generatedAt: now(),
    scope,
    moduleBuild: build,
    consoleBuild: build,
    optimizer: optimizerLine(health),
    configSha256: config.status === "fulfilled" ? configHash(config.value) : null,
    messages: list !== null && typeof list === "object" ? (list as { messages?: unknown }).messages : [],
  });
}

/**
 * Put `text` on the clipboard: the asynchronous clipboard API when the page
 * may use it, else a selected, invisible textarea and the copy command.
 * False when neither worked; the caller then shows the text to copy by hand.
 */
export async function copyText(
  text: string,
  clipboard: Clipboardish | undefined = globalThis.navigator?.clipboard,
  doc: Document | undefined = globalThis.document,
): Promise<boolean> {
  if (clipboard !== undefined) {
    try {
      await clipboard.writeText(text);
      return true;
    } catch {
      // Refused (permissions, an insecure page): try the textarea.
    }
  }
  if (doc === undefined) return false;
  const area = doc.createElement("textarea");
  area.value = text;
  area.setAttribute("readonly", "");
  area.style.position = "fixed";
  area.style.top = "0";
  area.style.opacity = "0";
  doc.body.appendChild(area);
  area.select();
  let copied = false;
  try {
    copied = doc.execCommand("copy");
  } catch {
    copied = false;
  }
  area.remove();
  return copied;
}
