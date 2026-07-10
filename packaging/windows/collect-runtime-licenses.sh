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

if [[ ! -d $staged_tree || ! -s $gdal_license ]]; then
	echo "staged tree or GDAL license is missing" >&2
	exit 1
fi

owners_file=$(mktemp)
trap 'rm -f "$owners_file"' EXIT

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
		license_files=()
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
			echo "no packaged license files for $owner" >&2
			exit 1
		fi

		echo
		echo "===== $(pacman -Q "$owner") ====="
		for license_file in "${license_files[@]}"; do
			echo
			echo "----- $license_file -----"
			cat "$license_file"
		done
	done < "$owners_file"
} > "$output"

test -s "$output"
