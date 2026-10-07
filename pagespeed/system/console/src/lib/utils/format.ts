// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// The console's one formatter set. Pure; the clock is always a parameter.
// Missing or nonsensical input renders as an em dash, never as NaN.

const countFormat = new Intl.NumberFormat("en-US");

/** n with `digits` significant digits, grouped once it outgrows the budget. */
export function formatSig(n: number, digits = 3): string {
  if (!Number.isFinite(n)) return "—";
  if (n === 0) return "0";
  const order = Math.floor(Math.log10(Math.abs(n)));
  if (order >= digits) {
    return countFormat.format(Number(n.toPrecision(digits)));
  }
  return n.toFixed(digits - 1 - order);
}

/** Bytes at 3 significant digits: 512 B, 1.50 KB, 108 KB, 2.20 MB. */
export function formatBytes(bytes: number | null | undefined): string {
  if (bytes === null || bytes === undefined || !Number.isFinite(bytes) || bytes < 0) return "—";
  if (bytes < 1024) return `${formatSig(bytes)} B`;
  const units = ["KB", "MB", "GB", "TB"] as const;
  let value = bytes;
  let unit = -1;
  do {
    value /= 1024;
    unit++;
  } while (value >= 1024 && unit < units.length - 1);
  return `${formatSig(value)} ${units[unit]}`;
}

/** A byte rate: the byte formatter with "/s" ("9.82 KB/s"); "—" when there is no value. */
export function formatBytesRate(bytesPerSecond: number | null | undefined): string {
  const bytes = formatBytes(bytesPerSecond);
  return bytes === "—" ? bytes : `${bytes}/s`;
}

/** A whole percent; a real but sub-half saving is "<1%", never "0%".
 *  `amount` is the quantity behind an already-rounded percentage: when it
 *  is nonzero, a percentage that reads 0 still renders "<1%". */
export function formatPercent(percent: number | null | undefined, amount?: number): string {
  if (percent === null || percent === undefined || !Number.isFinite(percent)) return "—";
  const clamped = Math.min(100, Math.max(0, percent));
  const real = clamped > 0 || (amount !== undefined && Number.isFinite(amount) && amount > 0);
  if (real && clamped < 0.5) return "<1%";
  return `${Math.round(clamped)}%`;
}

/** Grouped thousands: 1,234,567. */
export function formatCount(n: number | null | undefined): string {
  if (n === null || n === undefined || !Number.isFinite(n)) return "—";
  return countFormat.format(n);
}

/** "12 s ago" / "5 min ago" / "3 h ago" / "2 d ago"; pair with formatIsoTitle. */
export function formatRelative(ms: number, now: number): string {
  const s = Math.floor(Math.max(0, now - ms) / 1000);
  if (s < 60) return `${s} s ago`;
  const m = Math.floor(s / 60);
  if (m < 60) return m === 1 ? "1 min ago" : `${m} min ago`;
  const h = Math.floor(m / 60);
  if (h < 24) return h === 1 ? "1 h ago" : `${h} h ago`;
  const d = Math.floor(h / 24);
  return d === 1 ? "1 d ago" : `${d} d ago`;
}

/** The ISO string shown as the tooltip behind a relative time. */
export function formatIsoTitle(ms: number): string {
  return new Date(ms).toISOString();
}

/** "12s" / "41m 9s" / "5h 3m" / "2d 5h 3m". */
export function formatDuration(seconds: number | null | undefined): string {
  if (seconds === null || seconds === undefined || !Number.isFinite(seconds) || seconds < 0) {
    return "—";
  }
  const total = Math.floor(seconds);
  const d = Math.floor(total / 86400);
  const h = Math.floor((total % 86400) / 3600);
  const m = Math.floor((total % 3600) / 60);
  const s = total % 60;
  if (d > 0) return `${d}d ${h}h ${m}m`;
  if (h > 0) return `${h}h ${m}m`;
  if (m > 0) return `${m}m ${s}s`;
  return `${s}s`;
}

/** The display unit implied by a counter suffix, or null when none is known. */
export function formatUnit(name: string): string | null {
  if (name.endsWith("_us")) return "µs";
  if (name.endsWith("_ms")) return "ms";
  if (name.endsWith("_bytes")) return "B";
  if (name.endsWith("_kb")) return "KB";
  return null;
}

/** Local wall-clock time "10:25:51"; an em dash when there is no time. */
export function formatClock(ms: number): string {
  if (!Number.isFinite(ms) || ms <= 0) return "—";
  const d = new Date(ms);
  if (Number.isNaN(d.getTime())) return "—";
  const pad = (n: number) => n.toString().padStart(2, "0");
  return `${pad(d.getHours())}:${pad(d.getMinutes())}:${pad(d.getSeconds())}`;
}
