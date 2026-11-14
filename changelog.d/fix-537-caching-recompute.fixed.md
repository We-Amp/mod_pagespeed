- Response headers whose caching is worked out again after a change that makes
  them uncacheable no longer keep the earlier cache lifetime or the earlier
  "implicitly cacheable" mark; both are cleared, as for headers that were
  never cacheable.
