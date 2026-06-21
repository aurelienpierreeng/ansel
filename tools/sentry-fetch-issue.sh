#!/usr/bin/env bash
#
# Fetch a Sentry issue's latest event (stack trace, tags, contexts) and its
# attachments (e.g. the gdb-backtrace.txt Ansel attaches on Linux crashes), so a
# developer - or an AI coding assistant - can read the backtrace and fix the bug
# without clicking through the Sentry web UI.
#
# Usage:
#   tools/sentry-fetch-issue.sh <issue-id|issue-url> [output-dir]
#
# Examples:
#   tools/sentry-fetch-issue.sh 129371422
#   tools/sentry-fetch-issue.sh https://aurelienpierreeng.sentry.io/issues/129371422/
#
# Auth: reads SENTRY_AUTH_TOKEN from the environment, or from a local .sentry-auth
# file at the repository root (git-ignored). Reading issues/events needs a token
# with the scopes 'event:read', 'project:read' and 'org:read' - the organization
# auth token used for symbol upload (project:releases/project:write) is NOT enough.
# Create a User Auth Token with those scopes at:
#   https://aurelienpierreeng.sentry.io/settings/account/api/auth-tokens/
# A single User Auth Token can carry both the read scopes (for this script) and the
# release/write scopes (for tools/sentry-upload-symbols.sh).
#
# Org/project/region default to Ansel's and can be overridden via env vars.
set -euo pipefail

SENTRY_ORG="${SENTRY_ORG:-aurelienpierreeng}"
SENTRY_PROJECT="${SENTRY_PROJECT:-ansel}"
# Ansel's project lives in Sentry's EU (de) region - see the DSN host
# "ingest.de.sentry.io". The data API must be called on the matching region host.
SENTRY_HOST="${SENTRY_HOST:-https://de.sentry.io}"
API="${SENTRY_HOST}/api/0"

err() { printf '%s\n' "$*" >&2; }
die() { err "error: $*"; exit 1; }

command -v curl >/dev/null || die "curl is required"
command -v jq   >/dev/null || die "jq is required"

# --- arguments -------------------------------------------------------------
[ $# -ge 1 ] || die "usage: $0 <issue-id|issue-url> [output-dir]"
RAW_ARG="$1"
# Accept a full issue URL or a bare numeric id; keep only the issue number.
ISSUE_ID="$(printf '%s' "$RAW_ARG" | grep -oE '[0-9]{5,}' | head -n1 || true)"
[ -n "$ISSUE_ID" ] || die "could not parse an issue id from '$RAW_ARG'"

OUT_DIR="${2:-sentry-issue-${ISSUE_ID}}"

# --- auth token ------------------------------------------------------------
SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd -- "${SCRIPT_DIR}/.." && pwd)"
# Reading issues/events needs a user-level token (event:read etc.). Prefer the
# personal token file, then a generic one. The org-level token (.sentry-auth-orga,
# used for symbol upload) is intentionally not used here - it returns HTTP 403.
if [ -z "${SENTRY_AUTH_TOKEN:-}" ]; then
  for f in "${REPO_ROOT}/.sentry-auth-perso" "${REPO_ROOT}/.sentry-auth"; do
    if [ -f "$f" ]; then SENTRY_AUTH_TOKEN="$(tr -d ' \t\r\n' < "$f")"; break; fi
  done
fi
[ -n "${SENTRY_AUTH_TOKEN:-}" ] \
  || die "SENTRY_AUTH_TOKEN not set and no .sentry-auth-perso/.sentry-auth file found"

AUTH=(-H "Authorization: Bearer ${SENTRY_AUTH_TOKEN}")

api_get() {
  # api_get <path> -> stdout (fails on HTTP >= 400). No -f, so we can surface the
  # JSON error body (e.g. a scope problem) rather than a bare curl exit code.
  local path="$1"
  local body http
  body="$(curl -sS "${AUTH[@]}" "${API}${path}" -w $'\n%{http_code}')" || {
    err "request failed (network): GET ${API}${path}"
    return 1
  }
  http="${body##*$'\n'}"
  body="${body%$'\n'*}"
  if [ "$http" -ge 400 ]; then
    err "HTTP ${http} for GET ${API}${path}"
    printf '%s\n' "$body" >&2
    if [ "$http" = "403" ]; then
      err "hint: this token lacks read scopes. Create a User Auth Token with"
      err "      'event:read', 'project:read' and 'org:read' at"
      err "      https://${SENTRY_ORG}.sentry.io/settings/account/api/auth-tokens/"
    fi
    return 1
  fi
  printf '%s' "$body"
}

mkdir -p "$OUT_DIR"
err "==> issue ${ISSUE_ID} (org=${SENTRY_ORG} project=${SENTRY_PROJECT} region=${SENTRY_HOST})"

# --- issue metadata + latest event ----------------------------------------
api_get "/organizations/${SENTRY_ORG}/issues/${ISSUE_ID}/" > "${OUT_DIR}/issue.json"
api_get "/organizations/${SENTRY_ORG}/issues/${ISSUE_ID}/events/latest/" > "${OUT_DIR}/event.json"

EVENT_ID="$(jq -r '.id // .eventID // empty' "${OUT_DIR}/event.json")"
[ -n "$EVENT_ID" ] || die "no event id in latest event (is the issue id correct?)"

# --- human-readable summary ------------------------------------------------
SUMMARY="${OUT_DIR}/summary.txt"
{
  echo "Issue : $(jq -r '.title // "?"' "${OUT_DIR}/issue.json")"
  echo "Culprit: $(jq -r '.culprit // "?"' "${OUT_DIR}/issue.json")"
  echo "Events : $(jq -r '.count // "?"' "${OUT_DIR}/issue.json")  Users: $(jq -r '.userCount // "?"' "${OUT_DIR}/issue.json")"
  echo "Last  : $(jq -r '.lastSeen // "?"' "${OUT_DIR}/issue.json")"
  echo "Event : ${EVENT_ID}"
  echo

  echo "== Tags =="
  jq -r '(.tags // [])[] | "  \(.key) = \(.value)"' "${OUT_DIR}/event.json"
  echo

  echo "== Contexts (device / display / os / module_usage / processed_image) =="
  jq -r '(.contexts // {}) | to_entries[] | "  [\(.key)] \(.value | del(.type) | tojson)"' "${OUT_DIR}/event.json" 2>/dev/null || true
  echo

  echo "== Exception / stack trace =="
  # Walk the exception entry; print innermost frame last (crash point).
  jq -r '
    (.entries // [])[] | select(.type=="exception") | .data.values[]? |
    "\n--- \(.type // "?"): \(.value // "") ---",
    ( (.stacktrace.frames // []) | reverse[] |
      "  \(.function // "?")  (\(.filename // .module // "?"):\(.lineNo // "?"))"
      + (if .instructionAddr then "  [\(.instructionAddr)]" else "" end) )
  ' "${OUT_DIR}/event.json" 2>/dev/null || echo "  (no symbolicated stack trace in event JSON)"
} > "$SUMMARY"

# --- attachments (gdb-backtrace.txt, etc.) --------------------------------
ATTACH_JSON="${OUT_DIR}/attachments.json"
if api_get "/projects/${SENTRY_ORG}/${SENTRY_PROJECT}/events/${EVENT_ID}/attachments/" > "$ATTACH_JSON" 2>/dev/null; then
  count="$(jq 'length' "$ATTACH_JSON" 2>/dev/null || echo 0)"
  if [ "${count:-0}" -gt 0 ]; then
    err "==> downloading ${count} attachment(s)"
    jq -r '.[] | "\(.id)\t\(.name)"' "$ATTACH_JSON" | while IFS=$'\t' read -r aid aname; do
      [ -n "$aid" ] || continue
      safe="$(printf '%s' "$aname" | tr -c 'A-Za-z0-9._-' '_')"
      curl -fsS "${AUTH[@]}" \
        "${API}/projects/${SENTRY_ORG}/${SENTRY_PROJECT}/events/${EVENT_ID}/attachments/${aid}/?download" \
        -o "${OUT_DIR}/${safe}" && err "    ${safe}"
    done
  else
    err "==> no attachments on this event"
  fi
else
  err "==> attachments endpoint unavailable (token may lack event:read, or none stored)"
fi

err
err "Wrote:"
err "  ${SUMMARY}            <- read this first"
err "  ${OUT_DIR}/event.json        <- full latest event"
err "  ${OUT_DIR}/issue.json        <- issue metadata"
ls "${OUT_DIR}"/*backtrace* "${OUT_DIR}"/*.txt 2>/dev/null | grep -v summary.txt | sed 's/^/  /' >&2 || true

# Echo the summary to stdout too, so it can be piped into an assistant.
cat "$SUMMARY"
