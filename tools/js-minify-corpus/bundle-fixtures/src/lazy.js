const ticks = [..."lazy"].map((ch, i) => ch.charCodeAt(0) + i);

export function lazySummary() {
  return `lazy:${ticks.reduce((a, b) => a ^ b, 0)}`;
}
