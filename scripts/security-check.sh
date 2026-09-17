#!/usr/bin/env bash
set -euo pipefail

cd "$(git rev-parse --show-toplevel)"

for forbidden in \
  firmware/radio_config.h \
  server/data/admin.json \
  server/data/radio-config.json \
  server/bin/ffmpeg
do
  if git ls-files --error-unmatch "$forbidden" >/dev/null 2>&1; then
    echo "ERROR: private/runtime file is tracked: $forbidden" >&2
    exit 1
  fi
done

if git ls-files -z | grep -zE '\.(bin|elf|map|o|log)$' >/dev/null; then
  echo "ERROR: build output or log file is tracked." >&2
  exit 1
fi

if git grep -nEI \
  '(BEGIN[[:space:]]+(RSA |OPENSSH |EC |DSA )?PRIVATE KEY|gh[pousr]_[A-Za-z0-9_]{20,}|github_pat_[A-Za-z0-9_]{20,}|AKIA[0-9A-Z]{16}|sk-[A-Za-z0-9]{20,})' \
  -- . ':!scripts/security-check.sh'
then
  echo "ERROR: possible private key or access token found." >&2
  exit 1
fi

if git grep -nE '/Users/|/home/[A-Za-z0-9._-]+/' -- . ':!scripts/security-check.sh'; then
  echo "ERROR: personal absolute path found." >&2
  exit 1
fi

echo "Security check passed: no forbidden tracked files or common secret patterns found."
