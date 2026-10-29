#!/bin/sh
# Install the repositories that jtupdate just built.
#
# Usage:
#   jtupdate-install [--plan FILE] [--status-dir DIR] [DIR...]
#
# With no --plan and no DIR, the current directory (normally ~/jtupdate) is
# installed.  The script checks for doas, sudo, or pkexec and re-executes
# itself once with the first available tool, so every repository install shares
# one authentication prompt.

set -u

usage()
{
    cat <<'EOF'
usage: jtupdate-install [--plan FILE] [--status-dir DIR] [DIR...]

Runs "make install" in every directory listed on the command line or in the
plan file (one absolute path per line). With no arguments the current
directory is installed.

If not already root, the script uses doas, then sudo, then pkexec.
EOF
}

case "${1-}" in
    -h|--help)
        usage
        exit 0
        ;;
esac

# Resolve our own path before elevation; doas/pkexec may be started from a
# different directory.
self=$0
if [ ! -f "$self" ]; then
    self=$(command -v "$self" 2>/dev/null || true)
fi
case "$self" in
    /*) ;;
    *) self=$(pwd)/$self ;;
esac

if [ "$(id -u)" -ne 0 ]; then
    if command -v doas >/dev/null 2>&1; then
        exec doas "$self" "$@"
    elif command -v sudo >/dev/null 2>&1; then
        exec sudo "$self" "$@"
    elif command -v pkexec >/dev/null 2>&1; then
        exec pkexec "$self" "$@"
    else
        echo "jtupdate-install: need root, doas, sudo, or pkexec" >&2
        exit 1
    fi
fi

# We are root now.  Run from a private copy so a repository can install a new
# version of this helper without changing the script while it is executing.
# The copy is made after elevation, so doas/sudo rules that name the installed
# helper path still match.
if [ "${JTUPDATE_INSTALL_REEXEC-}" != 1 ]; then
    tmp=$(mktemp "${TMPDIR:-/tmp}/jtupdate-install.XXXXXX") || {
        echo "jtupdate-install: could not create a temporary copy" >&2
        exit 1
    }
    if ! cp "$self" "$tmp"; then
        rm -f "$tmp"
        echo "jtupdate-install: could not copy the helper" >&2
        exit 1
    fi
    chmod 700 "$tmp"
    JTUPDATE_INSTALL_REEXEC=1 exec "$tmp" "$@"
fi
trap 'rm -f "$self"' EXIT
trap 'exit 130' HUP INT TERM

plan=
status_dir=
while [ $# -gt 0 ]; do
    case "$1" in
        -h|--help)
            usage
            exit 0
            ;;
        --plan)
            [ $# -ge 2 ] || {
                echo "jtupdate-install: --plan needs a file" >&2
                exit 2
            }
            plan=$2
            shift 2
            ;;
        --status-dir)
            [ $# -ge 2 ] || {
                echo "jtupdate-install: --status-dir needs a directory" >&2
                exit 2
            }
            status_dir=$2
            shift 2
            ;;
        --)
            shift
            break
            ;;
        -*)
            echo "jtupdate-install: unknown option: $1" >&2
            exit 2
            ;;
        *)
            break
            ;;
    esac
done

if [ -n "$status_dir" ]; then
    mkdir -p "$status_dir" 2>/dev/null || true
fi

install_dir()
{
    dir=$1
    status_file=${2-}
    case "$dir" in
        /*) ;;
        *) dir=$(pwd)/$dir ;;
    esac

    if [ ! -d "$dir" ]; then
        echo "jtupdate-install: missing directory: $dir" >&2
        [ -n "$status_file" ] && echo 1 > "$status_file"
        return 1
    fi
    if [ ! -f "$dir/Makefile" ] && [ ! -f "$dir/makefile" ]; then
        echo "jtupdate-install: no Makefile in $dir" >&2
        [ -n "$status_file" ] && echo 1 > "$status_file"
        return 1
    fi

    echo "==> make install in $dir"
    ( cd "$dir" && make install )
    st=$?
    if [ -n "$status_file" ]; then
        echo "$st" > "$status_file"
    fi
    if [ "$st" -ne 0 ]; then
        echo "jtupdate-install: install failed in $dir" >&2
    fi
    return "$st"
}

status_file_for()
{
    if [ -n "$status_dir" ]; then
        printf '%s/%s.status\n' "$status_dir" "$1"
    fi
}

overall=0

if [ -n "$plan" ]; then
    if [ ! -f "$plan" ]; then
        echo "jtupdate-install: plan not found: $plan" >&2
        exit 1
    fi
    n=0
    while IFS= read -r dir || [ -n "$dir" ]; do
        [ -n "$dir" ] || continue
        sf=$(status_file_for "$n")
        install_dir "$dir" "$sf" || overall=1
        n=$((n + 1))
    done < "$plan"
elif [ $# -gt 0 ]; then
    n=0
    for dir in "$@"; do
        sf=$(status_file_for "$n")
        install_dir "$dir" "$sf" || overall=1
        n=$((n + 1))
    done
else
    sf=$(status_file_for 0)
    install_dir "$(pwd)" "$sf" || overall=1
fi

if [ "$overall" -ne 0 ] && [ -t 0 ]; then
    printf '\nInstall failed. Press Enter to close.\n'
    read _
fi

exit "$overall"
