#!/usr/bin/env bash
# Checks the SPDX license header of every tracked C/C++ source under src/ (third_party
# excluded) — README, "Licença":
#   Apache-2.0     src/shared/** and the broker client (src/service_broker/Services/
#                  ServiceClient.* and ServiceIdentity.*)
#   AGPL-3.0-only  everything else
# Fails on a missing header or on the wrong license for the file's location (e.g. a service
# file copied into src/shared/ with its AGPL header). Runs first in ci.yml; by hand:
#
#   scripts/check_spdx.sh
set -euo pipefail

cd "$(dirname "${BASH_SOURCE[0]}")/.."

expected_license() {
  case "$1" in
    src/shared/*) echo "Apache-2.0" ;;
    src/service_broker/Services/ServiceClient.* | src/service_broker/Services/ServiceIdentity.*)
      echo "Apache-2.0" ;;
    *) echo "AGPL-3.0-only" ;;
  esac
}

errors=0
checked=0
while IFS= read -r file; do
  checked=$((checked + 1))
  expected="$(expected_license "$file")"
  found="$(head -n 5 "$file" | sed -n 's|^// SPDX-License-Identifier: *\(.*\)$|\1|p')"
  if [[ -z "$found" ]]; then
    echo "missing SPDX header: $file (expected $expected)"
    errors=$((errors + 1))
  elif [[ "$found" != "$expected" ]]; then
    echo "wrong license: $file is $found, expected $expected"
    errors=$((errors + 1))
  fi
done < <(git ls-files 'src/*.cpp' 'src/*.h' 'src/*.hpp' 'src/*.c' 'src/*.cc' | grep -v '^src/third_party/')

if ((errors > 0)); then
  echo "$errors of $checked files with SPDX problems — add the header from README, \"Licença\":"
  echo "  // SPDX-FileCopyrightText: <year> Rafael Dias"
  echo "  // SPDX-License-Identifier: <Apache-2.0 | AGPL-3.0-only>"
  exit 1
fi
echo "SPDX headers ok ($checked files)"
