# Helm: install and verify mod_pagespeed on Kubernetes

Source: https://modpagespeed.com/docs/helm-deployment/. Header: `X-PageSpeed`.
Scope: the `pagespeed` chart, a two-container pod (nginx with the module and
the optimizer worker) in front of an origin Service in the cluster.

## 1. Prerequisites

Kubernetes 1.28 or newer, Helm 3.12 or newer, and an origin Service the pod can
reach. The chart pulls public images; no registry credentials are needed.

```bash
kubectl config current-context   # show it; the operator confirms this is the target cluster
kubectl version
helm version --short
kubectl get svc <origin-service> -n <namespace>   # the origin to put the module in front of
helm status pagespeed -n <namespace>              # must fail with "release: not found"
```

Stop and ask if a `pagespeed` release already exists in the namespace.

## 2. Install

```bash
helm repo add weamp https://modpagespeed.com/charts
helm repo update
helm install pagespeed weamp/pagespeed -n <namespace> \
  --set backend.host=<origin-service>.<namespace>.svc.cluster.local \
  --set backend.port=<origin-port> \
  --wait --timeout 3m
```

## 3. Minimal configuration

The two `backend.*` values above are the only required ones. Replicas,
ingress, cache size and resources go in a values file
(`helm install pagespeed weamp/pagespeed -n <namespace> -f my-values.yaml`);
the value reference and the sizing tables are in the source document.

## 4. Verify

Run the block as one shell command:

```bash
kubectl get pods -n <namespace> -l app.kubernetes.io/name=pagespeed   # READY 2/2
kubectl port-forward -n <namespace> svc/pagespeed 8080:80 >/dev/null 2>&1 &
PF_PID=$!
sleep 3
curl -s -o /dev/null -D - 'http://localhost:8080/?mps-verify=agent' | grep -i '^x-pagespeed:'
kill "$PF_PID"
```

Pass: an `X-PageSpeed` line. `MISS` proves the module is active; `HIT` on a
later request is informational only: optimization is asynchronous, and
responses the origin marks uncacheable stay `MISS`
(https://modpagespeed.com/docs/troubleshooting/#cache-miss-on-every-request).
No header: retry at most 3 times, then read both containers' logs
(`kubectl logs -n <namespace> -l app.kubernetes.io/name=pagespeed -c nginx` and
`-c worker`) and https://modpagespeed.com/docs/troubleshooting/

## 5. Rollback

```bash
helm history pagespeed -n <namespace> && helm rollback pagespeed <revision> -n <namespace>   # a previous chart revision
helm uninstall pagespeed -n <namespace>                                                     # remove everything the chart created
```

The `emptyDir` cache goes with the pods. https://modpagespeed.com/docs/uninstall/#helm
