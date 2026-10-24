#!/usr/bin/env bash

set -e

document_map=(
1:line_follower
2:line_follower_summary
)

# Document mapping. Add, remove, or edit an INTEGER:BASENAME entry here.
# Entries are separated by whitespace; the script adds the .tex suffix.

list_mapping() {
    local entry doc_id doc_file

    printf '%s\n\n' 'Usage: ./buildTex.sh INTEGER'
    for entry in "${document_map[@]}"; do
        doc_id=${entry%%:*}
        doc_file=${entry#*:}.tex
        printf ' %3s   %s\n' "$doc_id" "$doc_file"
    done
}

if [ "$#" -eq 0 ]; then
    list_mapping
    exit 0
fi

if [ "$#" -ne 1 ]; then
    echo "ERROR: expected exactly one integer argument." >&2
    list_mapping >&2
    exit 2
fi

tex_file=
for entry in "${document_map[@]}"; do
    doc_id=${entry%%:*}
    if [ "$1" = "$doc_id" ]; then
        tex_file=${entry#*:}.tex
        break
    fi
done

if [ -z "$tex_file" ]; then
    echo "ERROR: unknown document number: $1" >&2
    list_mapping >&2
    exit 2
fi

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$script_dir"

if [ ! -f "$tex_file" ]; then
    echo "ERROR: input file not found: ${script_dir}/${tex_file}" >&2
    exit 1
fi

out_dir=tmp
mkdir -p "$out_dir"

if ! command -v pdflatex >/dev/null 2>&1; then
    if type module >/dev/null 2>&1; then
        module load texlive
    else
        echo "ERROR: pdflatex is unavailable and the module command is not defined." >&2
        exit 1
    fi
fi

doc_name="${tex_file%.tex}"
pdflatex -interaction=nonstopmode -halt-on-error \
    -output-directory="$out_dir" "$tex_file"
pdflatex -interaction=nonstopmode -halt-on-error \
    -output-directory="$out_dir" "$tex_file"

pdf_file="${doc_name}.pdf"
mv -f "${out_dir}/${pdf_file}" .

echo "Built $pdf_file (auxiliary files in ${out_dir}/)"

if [ "$(uname -s)" = "Darwin" ]; then
    open "$pdf_file" || true
elif [ -f /etc/os-release ] && grep -qi 'ubuntu' /etc/os-release; then
    xdg-open "$pdf_file" || true
elif hostname | grep -qiE 'login|nid0'; then
    gv "$pdf_file" || true
else
    echo "Environment not matched. PDF is ready at $pdf_file"
fi
