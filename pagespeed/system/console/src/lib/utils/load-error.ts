// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

/**
 * What a failed first load says: a title in words, one line on the likely
 * cause, and the transport message kept apart for whoever needs it. The
 * transport message ("HTTP 404: ...") is never the page's text.
 */

/** Why the read failed, as far as the answer tells. */
export type LoadErrorKind =
  | "no-answer"
  | "not-found"
  | "unauthorized"
  | "refused"
  | "rejected"
  | "server"
  | "unreadable"
  | "other";

export interface LoadErrorText {
  kind: LoadErrorKind;
  /** "Could not load <noun>." */
  title: string;
  /** One or two plain sentences on the likely cause. */
  cause: string;
  /** The message as the client reported it; null when there is none. */
  raw: string | null;
}

const STATUS_PREFIX = /^HTTP (\d{3})\b/;

/** The message of an error or a string; null for anything without one. */
function rawMessage(error: unknown): string | null {
  if (typeof error === "string") return error === "" ? null : error;
  if (error instanceof Error) return error.message === "" ? null : error.message;
  return null;
}

/**
 * The HTTP status behind `error`: the `status` of an API error, or the one a
 * message of the form "HTTP 404: ..." names. Null when no answer carried one.
 */
export function loadErrorStatus(error: unknown): number | null {
  if (error !== null && typeof error === "object") {
    const status = (error as { status?: unknown }).status;
    if (typeof status === "number" && Number.isInteger(status)) return status;
  }
  const match = STATUS_PREFIX.exec(rawMessage(error) ?? "");
  return match ? Number(match[1]) : null;
}

function isNoAnswer(error: unknown): boolean {
  if (error instanceof Error && error.name === "NetworkError") return true;
  return rawMessage(error) === "cannot reach the server";
}

export function loadErrorKind(error: unknown): LoadErrorKind {
  if (isNoAnswer(error)) return "no-answer";
  const status = loadErrorStatus(error);
  if (status === null) return "other";
  if (status === 404) return "not-found";
  if (status === 401) return "unauthorized";
  if (status === 403) return "refused";
  if (status >= 500 && status <= 599) return "server";
  if (status >= 400 && status <= 499) return "rejected";
  // An answer that arrived but could not be read as the expected data.
  if (status >= 200 && status <= 299) return "unreadable";
  return "other";
}

const CAUSES: Record<LoadErrorKind, string> = {
  "no-answer": "The server did not answer. It may be stopped or restarting, or the connection to it is down.",
  "not-found":
    "The server does not know this view. The module on this server may be older than this console, or the view is not served at this address.",
  unauthorized: "The server refused this request. This view may need a sign-in, or be restricted for this address.",
  refused: "The server refused this request. Access to the admin pages may be restricted for this address or this view.",
  rejected: "The server did not accept this request.",
  server: "The server, or a proxy in front of it, reported an error while answering. The web server's error log may say why.",
  unreadable:
    "The server answered, but not with the data this view expects. Something between this console and the server may have changed the answer.",
  other: "Something went wrong while loading. The technical details have the message.",
};

/**
 * The text for a failed load of `noun` ("the statistics", "the rule packs"):
 * the noun completes "Could not load ...".
 */
export function describeLoadError(error: unknown, noun: string): LoadErrorText {
  const kind = loadErrorKind(error);
  return { kind, title: `Could not load ${noun}.`, cause: CAUSES[kind], raw: rawMessage(error) };
}
