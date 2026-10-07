// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

/**
 * The cache leaf describes each cache as an expression such as
 * "Compressed(WriteThroughCache(l1=Stats(prefix=shm_cache,cache=SharedMemCache<64>),…))".
 * This turns it into the layers the Caches page draws. The input comes from
 * the server, so parsing must end on any input.
 */

export interface CacheNode {
  type: string;
  props: Record<string, string>;
  children: CacheNode[];
  args: string[];
}

/** Parse a cache summary expression like "Compressed(Fallback(small=Stats(...)))" into a tree. */
export function parseCacheSummary(s: string): CacheNode | null {
  s = s.trim();
  if (!s || s === "none") return null;

  let pos = 0;

  function parseNode(): CacheNode | null {
    // Read type name (letters, digits, angle brackets for SharedMemCache<64>)
    let name = "";
    while (pos < s.length && s[pos] !== "(" && s[pos] !== ")" && s[pos] !== "," && s[pos] !== "=") {
      name += s[pos];
      pos++;
    }
    name = name.trim();
    if (!name) return null;

    const node: CacheNode = { type: name, props: {}, children: [], args: [] };

    // If followed by '(', parse contents
    if (pos < s.length && s[pos] === "(") {
      pos++; // skip '('
      // Parse comma-separated entries inside parens
      while (pos < s.length && s[pos] !== ")") {
        const before = pos;
        skipWhitespace();
        // Check if this is key=value or just a positional child
        const startPos = pos;
        let key = "";
        while (pos < s.length && s[pos] !== "=" && s[pos] !== "(" && s[pos] !== ")" && s[pos] !== ",") {
          key += s[pos];
          pos++;
        }
        key = key.trim();

        if (pos < s.length && s[pos] === "=") {
          pos++; // skip '='
          // value is either a nested node or a plain string
          const child = parseNode();
          if (child) {
            child.props["_key"] = key;
            node.children.push(child);
          } else {
            node.props[key] = "";
          }
        } else {
          // Rewind — this was a positional arg that is itself a node name
          pos = startPos;
          const child = parseNode();
          if (child !== null && child.children.length === 0 && Object.keys(child.props).length === 0 && /^[a-z][\w-]*$/.test(child.type)) {
            // A bare lower-case word is an argument of this cache (a tier
            // name such as "small_tier"), not a nested cache.
            node.args.push(child.type);
          } else if (child !== null) {
            node.children.push(child);
          }
        }

        skipWhitespace();
        if (pos < s.length && s[pos] === ",") {
          pos++; // skip comma
        }
        // Every pass must consume input: a "(" right after "(", "," or "="
        // yields no name and would otherwise spin here forever.
        if (pos === before) pos++;
      }
      if (pos < s.length && s[pos] === ")") {
        pos++; // skip ')'
      }
    }

    return node;
  }

  function skipWhitespace() {
    while (pos < s.length && (s[pos] === " " || s[pos] === "\t")) pos++;
  }

  return parseNode();
}

const LABELS: Record<string, string> = {
  HTTPCache: "HTTP Cache",
  CycloneCache: "Cyclone Disk Cache",
  Compressed: "Compression",
  WriteThroughCache: "Write-Through (L1, then L2)",
  Fallback: "Fallback (small and large values)",
  Stats: "Statistics Wrapper",
};

/** A cache type in words; its arguments (a tier name) in parentheses. */
export function nodeLabel(type: string, args: readonly string[] = []): string {
  let label: string;
  if (type.startsWith("SharedMemCache")) {
    // The template argument is the block size in bytes.
    const m = type.match(/<(\d+)>/);
    label = m ? `Shared Memory (${m[1]}-byte blocks)` : "Shared Memory";
  } else {
    label = LABELS[type] ?? type;
  }
  return args.length > 0 ? `${label} (${args.map(formatRole).join(", ")})` : label;
}

/** A one-word kind for a cache layer. The layer's full label already says
 * what it is; this is the short marker that used to be an emoji. */
export function nodeKindLabel(type: string): string {
  if (type.startsWith("SharedMemCache")) return "Memory";
  const kinds: Record<string, string> = {
    CycloneCache: "Disk",
    HTTPCache: "HTTP",
    Compressed: "Compressed",
    Fallback: "Fallback",
    Stats: "Stats",
  };
  return kinds[type] ?? "Cache";
}

/** Flatten cache tree into a list of layers with depth, for display. */
export interface CacheLayer {
  depth: number;
  kind: string;
  label: string;
  role: string; // e.g. "small", "large", or ""
  prefix: string; // stats prefix if present
}

export function flattenTree(node: CacheNode | null, depth: number = 0, role: string = ""): CacheLayer[] {
  if (!node) return [];
  const layers: CacheLayer[] = [];
  // "cache" is the slot a Stats wrapper keeps its cache in, not a role:
  // the l1/l2 (or small/large) slot around it is.
  const ownKey = node.props["_key"];
  const keyRole = (ownKey && ownKey !== "cache" ? ownKey : "") || role;

  // For Stats nodes, extract the prefix and continue into the cache child
  if (node.type === "Stats") {
    // The prefix is stored as a child with _key="prefix", its type is the prefix value
    const prefixChild = node.children.find((c) => c.props["_key"] === "prefix");
    const prefix = prefixChild ? prefixChild.type : "";
    const cacheChildren = node.children.filter((c) => c.props["_key"] !== "prefix");
    // Stats wraps exactly one cache child usually
    for (const child of cacheChildren) {
      // Don't pass "cache" as a role — it's structural, not meaningful
      const childRole = child.props["_key"] === "cache" ? keyRole : (child.props["_key"] || keyRole);
      const childLayers = flattenTree(child, depth, childRole);
      if (childLayers.length > 0) {
        childLayers[0].prefix = prefix;
      }
      layers.push(...childLayers);
    }
    // If Stats has no cache children, show the Stats node itself
    if (cacheChildren.length === 0) {
      layers.push({
        depth,
        kind: nodeKindLabel(node.type),
        label: nodeLabel(node.type, node.args),
        role: keyRole,
        prefix,
      });
    }
    return layers;
  }

  layers.push({
    depth,
    kind: nodeKindLabel(node.type),
    label: nodeLabel(node.type, node.args),
    role: keyRole,
    prefix: "",
  });

  for (const child of node.children) {
    layers.push(...flattenTree(child, depth + 1, ""));
  }

  return layers;
}

/** Parse a Property Cache summary which has multiple cohorts. */
export interface CacheCohort {
  name: string;
  layers: CacheLayer[];
}

export function parseCacheCohorts(summary: string): CacheCohort[] | null {
  const lines = summary.split("\n").filter((l) => l.trim());
  // Check if this looks like cohorts (name:expression per line)
  if (lines.length < 2 || !lines.every((l) => l.includes(":"))) return null;

  return lines.map((line) => {
    const colonIdx = line.indexOf(":");
    const name = line.substring(0, colonIdx).trim();
    const expr = line.substring(colonIdx + 1).trim();
    const tree = parseCacheSummary(expr);
    return { name, layers: flattenTree(tree) };
  });
}

/** Parse backend_stats into key-value pairs. */
export function parseBackendStats(raw: string): Array<{ key: string; value: string }> {
  if (!raw || !raw.trim()) return [];
  return raw
    .split("\n")
    .filter((l) => l.trim())
    .map((line) => {
      const eqIdx = line.indexOf(":");
      if (eqIdx >= 0) {
        return { key: line.substring(0, eqIdx).trim(), value: line.substring(eqIdx + 1).trim() };
      }
      const spIdx = line.indexOf(" ");
      if (spIdx >= 0) {
        return { key: line.substring(0, spIdx).trim(), value: line.substring(spIdx + 1).trim() };
      }
      return { key: line.trim(), value: "" };
    });
}

/** Format a role label for display. */
export function formatRole(role: string): string {
  return role
    .replace(/_/g, " ")
    .replace(/\b\w/g, (c) => c.toUpperCase());
}
