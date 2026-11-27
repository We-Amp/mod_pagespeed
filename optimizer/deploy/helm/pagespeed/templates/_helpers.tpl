{{/*
Chart name (truncated).
*/}}
{{- define "pagespeed.name" -}}
{{- default .Chart.Name .Values.nameOverride | trunc 63 | trimSuffix "-" }}
{{- end }}

{{/*
Fully qualified app name.
*/}}
{{- define "pagespeed.fullname" -}}
{{- if .Values.fullnameOverride }}
{{- .Values.fullnameOverride | trunc 63 | trimSuffix "-" }}
{{- else }}
{{- $name := default .Chart.Name .Values.nameOverride }}
{{- if contains $name .Release.Name }}
{{- .Release.Name | trunc 63 | trimSuffix "-" }}
{{- else }}
{{- printf "%s-%s" .Release.Name $name | trunc 63 | trimSuffix "-" }}
{{- end }}
{{- end }}
{{- end }}

{{/*
Chart label.
*/}}
{{- define "pagespeed.chart" -}}
{{- printf "%s-%s" .Chart.Name .Chart.Version | replace "+" "_" | trunc 63 | trimSuffix "-" }}
{{- end }}

{{/*
Common labels.
*/}}
{{- define "pagespeed.labels" -}}
helm.sh/chart: {{ include "pagespeed.chart" . }}
{{ include "pagespeed.selectorLabels" . }}
{{- if .Chart.AppVersion }}
app.kubernetes.io/version: {{ .Chart.AppVersion | quote }}
{{- end }}
app.kubernetes.io/managed-by: {{ .Release.Service }}
{{- end }}

{{/*
Selector labels.
*/}}
{{- define "pagespeed.selectorLabels" -}}
app.kubernetes.io/name: {{ include "pagespeed.name" . }}
app.kubernetes.io/instance: {{ .Release.Name }}
{{- end }}

{{/*
Single image tag for BOTH the worker and nginx containers (lockstep).
The worker emits a content-hashed async-CSS loader path
(/pagespeed_static/async_css.<hash>.js) that nginx must serve byte-identically;
if the two run different builds the path 404s and pages render unstyled (FOUC).
So both images MUST share one tag. Source: .Values.image.tag, else the chart's
appVersion (single source of truth — bump it in Chart.yaml per release). The
legacy per-image worker.image.tag / nginx.image.tag keys are rejected loud — the
Helm analog of the compose ${PAGESPEED_VERSION:?} guard — so a split that would
cause a loader 404 / FOUC can't be set by accident.
*/}}
{{- define "pagespeed.imageTag" -}}
{{- if or (hasKey .Values.worker.image "tag") (hasKey .Values.nginx.image "tag") -}}
{{- fail "worker.image.tag / nginx.image.tag are removed: they allowed a worker/nginx version split (async-CSS loader 404 -> FOUC). Set a single top-level `image.tag` for both images, or leave it unset to track the chart appVersion." -}}
{{- end -}}
{{- .Values.image.tag | default .Chart.AppVersion -}}
{{- end }}
