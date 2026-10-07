// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

export interface Msg {
  severity: string;
  message: string;
}

export interface MessagesPage {
  scope?: string;
  next?: number;
  messages: Msg[];
}

export interface MessagesState {
  items: Msg[];
  next: number;
}

/**
 * Fold one polled page of /message_history into the running message list.
 *
 * Newest first; `next` is the backend's monotonic write-count cursor. A
 * cursor that goes backwards means the process restarted (its history is
 * gone), so start over from just this page instead of prepending onto
 * stale content. The retained list is capped at `cap` entries.
 *
 * Idempotent when `page.next === state.next`: the same page merged twice
 * (a manual "Refresh Now" firing while a scheduled poll is already in
 * flight, or -- historically -- a reactive effect re-running itself on a
 * page it already folded in) returns `state` unchanged rather than
 * duplicating its messages.
 */
export function mergeMessages(
  state: MessagesState,
  page: MessagesPage,
  cap: number,
): MessagesState {
  const next = page.next ?? 0;
  if (next === state.next) {
    return state;
  }
  const fresh = [...page.messages].reverse();
  const items = next < state.next ? fresh : [...fresh, ...state.items];
  return { items: items.slice(0, cap), next };
}

/**
 * A message's text without the "[<date>] " prefix the module writes: the
 * Logs page shows the time once, in its own column.
 */
export function messageBody(message: string): string {
  const m = /^\[([^\]]+)\]\s*/.exec(message);
  if (m !== null && !Number.isNaN(Date.parse(m[1]))) return message.slice(m[0].length);
  return message;
}
