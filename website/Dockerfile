# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

FROM node:22-alpine@sha256:1314fb30ec37592d5845e5a160d522c0e680972aa534549940b8d73bde88481a AS build
WORKDIR /app
COPY package.json package-lock.json ./
RUN npm ci
COPY . .
# src/data/product-facts.mjs imports ../../../shared/product-facts.mjs, which
# lives one level above this build context. The builder supplies that directory
# as the named context "shared" (compose: build.additional_contexts; plain
# docker: --build-context shared=../shared), and it lands where the relative
# import resolves from /app/src/data.
COPY --from=shared . /shared/
# A build inside an image has no route to FastSpring credentials. Default 0
# keeps the strict behaviour (a stale committed price list fails the build);
# a harness that only needs the site to render sets it to 1.
ARG PRICING_ALLOW_STALE=0
ENV PRICING_ALLOW_STALE=${PRICING_ALLOW_STALE}
RUN npm run build

FROM node:22-alpine@sha256:1314fb30ec37592d5845e5a160d522c0e680972aa534549940b8d73bde88481a AS runtime
WORKDIR /app
COPY --from=build /app/dist ./dist
COPY --from=build /app/node_modules ./node_modules
COPY --from=build /app/package.json ./
USER node
ENV HOST=0.0.0.0
ENV PORT=4321
EXPOSE 4321
CMD ["node", "dist/server/entry.mjs"]
