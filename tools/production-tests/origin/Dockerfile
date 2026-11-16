# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

FROM python:3.15-slim@sha256:65a65fc7b30b06cb6df4930fd4650697cb23f4c2698ab81fc87e4b031bc36928

WORKDIR /app
RUN apt-get update && apt-get install -y curl && rm -rf /var/lib/apt/lists/*
COPY requirements.txt .
RUN pip install --no-cache-dir -r requirements.txt
COPY . .

HEALTHCHECK --interval=2s --timeout=2s --retries=10 \
    CMD curl -f http://localhost:8081/health || exit 1

CMD ["uvicorn", "main:app", "--host", "0.0.0.0", "--port", "8081", "--log-level", "warning"]
