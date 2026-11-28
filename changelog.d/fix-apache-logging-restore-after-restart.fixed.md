- **Apache: module log lines reach the error log and the console again.** After
  Apache started or did a graceful restart, its children wrote mod_pagespeed
  warnings and other log lines raw to standard error: without a timestamp,
  ignoring `LogLevel`, and missing from the console's message history. They
  go through the Apache error log again. nginx and IIS were not affected.
