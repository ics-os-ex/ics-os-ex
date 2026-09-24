#!/bin/bash
# Regenerate base/icsos.hlp from console command //- comments.
# Only keep lines that successfully extract "cmd- description".
HELP_FILE=base/icsos.hlp

{
  echo "ICS-OS Commands"
  echo "---------------"
  grep -n 'strcmp(u' kernel/console/console.c | grep '//--' | while IFS= read -r line; do
    cmd=$(printf '%s\n' "$line" | sed -n 's/^[^"]*"\([^"]*\)".*/\1/p')
    desc=$(printf '%s\n' "$line" | sed -n 's/.*\/\/-- *//p')
    # Skip nested strtok strcmp helpers (echo/noecho under stty).
    case "$cmd" in
      echo|noecho) continue ;;
    esac
    if [ -n "$cmd" ] && [ -n "$desc" ]; then
      printf '%s- %s\n' "$cmd" "$desc"
    fi
  done | sort -u
} > "$HELP_FILE"
