#!/usr/bin/env bash

# Build an aggregate notice from the DLL closure staged for Windows. MSYS2
# owns every packaged runtime DLL except the locally built GDAL library.
# Non-DLL payloads such as translations and data files remain a separate
# handoff-review responsibility.

set -euo pipefail

if [[ $# -ne 4 ]]; then
	echo "usage: $0 STAGED_TREE OUTPUT GDAL_LICENSE GDAL_VERSION" >&2
	exit 2
fi

staged_tree=$1
output=$2
gdal_license=$3
gdal_version=$4
mingw_prefix=${MINGW_PREFIX:-/mingw64}
source_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)

if [[ ! -d $staged_tree || ! -s $gdal_license ]]; then
	echo "staged tree or GDAL license is missing" >&2
	exit 1
fi

owners_file=$(mktemp)
missing_owners_file=$(mktemp)
trap 'rm -f "$owners_file" "$missing_owners_file"' EXIT

while IFS= read -r -d '' dll; do
	basename=$(basename "$dll")
	if [[ $basename == libgdal-*.dll || $basename == gdal*.dll ]]; then
		continue
	fi
	source_path="$mingw_prefix/bin/$basename"

	if [[ ! -e $source_path ]]; then
		plugin_matches=()
		if [[ -d $mingw_prefix/share/qt5/plugins ]]; then
			while IFS= read -r match; do
				plugin_matches+=("$match")
			done < <(find "$mingw_prefix/share/qt5/plugins" -type f -name "$basename" -print)
		fi
		if [[ ${#plugin_matches[@]} -eq 1 ]]; then
			source_path=${plugin_matches[0]}
		elif [[ ${#plugin_matches[@]} -gt 1 ]]; then
			echo "multiple MSYS2 plugin matches for $dll" >&2
			printf '  %s\n' "${plugin_matches[@]}" >&2
			exit 1
		fi
	fi

	if owner=$(pacman -Qqo "$source_path" 2>/dev/null); then
		printf '%s\n' "$owner" >> "$owners_file"
	else
		echo "no package owner for staged DLL: $dll (source candidate: $source_path)" >&2
		exit 1
	fi
done < <(find "$staged_tree" -type f -iname '*.dll' -print0)

sort -u -o "$owners_file" "$owners_file"
mkdir -p "$(dirname "$output")"

{
	echo "Third-party runtime notices for OOM - COC Edition - Base"
	echo "Generated from the exact staged Windows DLL closure."
	echo
	echo "===== GDAL $gdal_version ====="
	cat "$gdal_license"

	while IFS= read -r owner; do
		[[ -n $owner ]] || continue
		owner_version=$(pacman -Q "$owner")
		package_version=${owner_version#"$owner "}
		package_licenses=$(
			pacman -Qi "$owner" |
				awk -F ': ' '/^Licenses[[:space:]]*:/ { print $2 }'
		)
		license_files=()
		fallback_label=
		while IFS= read -r license_file; do
			license_files+=("$license_file")
		done < <(
			pacman -Ql "$owner" |
				awk \
					-v licenses_prefix="$mingw_prefix/share/licenses/" \
					-v share_prefix="$mingw_prefix/share/" \
					'index($2, licenses_prefix) == 1 && $2 !~ /\/$/ ||
					 (index($2, share_prefix) == 1 && $2 ~ /\/(LICENSE|COPYING)(\.[^\/]*)?$/) {
						 print $2
					 }'
		)

		if [[ ${#license_files[@]} -eq 0 ]]; then
			case $owner in
			mingw-w64-x86_64-libidn2)
				if [[ $package_licenses == *spdx:GPL-2.0-or-later* && -s $source_root/COPYING ]]; then
					license_files+=("$source_root/COPYING")
					fallback_label="curated fallback: GPL-3.0 under libidn2's GPL-2.0-or-later option"
				fi
				;;
			mingw-w64-x86_64-lz4)
				fallback_file="$source_root/packaging/windows/runtime-license-fallbacks/lz4-1.10.0-BSD-2-Clause.txt"
				if [[ $package_version == 1.10.0-* && $package_licenses == *BSD* && -s $fallback_file ]]; then
					license_files+=("$fallback_file")
					fallback_label="curated fallback: LZ4 1.10.0 library BSD-2-Clause license"
				fi
				;;
			esac
		fi

		if [[ ${#license_files[@]} -eq 0 ]]; then
			printf '%s\n' "$owner" >> "$missing_owners_file"
			continue
		fi

		echo
		echo "===== $owner_version ====="
		for license_file in "${license_files[@]}"; do
			echo
			if [[ -n $fallback_label ]]; then
				echo "----- $fallback_label -----"
			else
				echo "----- $license_file -----"
			fi
			cat "$license_file"
		done
	done < "$owners_file"

	if [[ -s $missing_owners_file ]]; then
		echo "packages without owned license files:" >&2
		while IFS= read -r owner; do
			printf '  %s (licenses: %s)\n' \
				"$owner" \
				"$(pacman -Qi "$owner" | awk -F ': ' '/^Licenses[[:space:]]*:/ { print $2 }')" >&2
		done < "$missing_owners_file"
		exit 1
	fi
} > "$output"

test -s "$output"
