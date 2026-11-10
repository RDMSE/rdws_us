#!/usr/bin/env bash
# Prints the version of the solution (README, "Versões"): `git describe` limited to SemVer tags
# (vMAJOR.MINOR.PATCH), so older tags like development/phase-5/end don't count. Single source
# for cmake/RdwsVersion.cmake, ci.yml, deploy-qa.yml and qa_simulators.sh.
#
#   v0.1.0                    on the tag
#   v0.1.0-3-g1a2b3c4         3 commits after it
#   v0.1.0-3-g1a2b3c4-dirty   with uncommitted changes
#   v0.0.0-dev+g1a2b3c4       no SemVer tag yet
#   v0.0.0-unknown            no git (or not a checkout)
#
# A non-empty RDWS_VERSION in the environment is printed as is.
set -euo pipefail

if [[ -n "${RDWS_VERSION:-}" ]]; then
  echo "$RDWS_VERSION"
  exit 0
fi

cd "$(dirname "${BASH_SOURCE[0]}")/.."
if ! described="$(git describe --tags --match 'v[0-9]*' --dirty --always 2>/dev/null)"; then
  echo "v0.0.0-unknown"
elif [[ "$described" == v[0-9]* ]]; then
  echo "$described"
else
  # --always with no SemVer tag: only the commit
  echo "v0.0.0-dev+g$described"
fi
