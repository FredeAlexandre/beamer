#!/bin/sh
# PROTOTYPE: copy one toolkit's sources to the PC, build it there, drop the exe on the desktop.
# Usage: sync-pc.sh <rune|gpui|slint> [--smoke]
#   --smoke  run it in Alex's desktop session (scheduled task, not session 0) with
#            --test-glow --quit-after 8, then print its log.
set -e
tk="$1"; smoke="$2"
here="$(cd "$(dirname "$0")" && pwd)"
remote='C:/Users/Alex/Developer/beamer-ui-bakeoff'
ssh lebig "New-Item -ItemType Directory -Force '$remote/$tk' | Out-Null"
cd "$here/$tk"
# Sources only: never the target dir.
scp -q -r $(ls -A | grep -v -x -e target -e .DS_Store) "lebig:$remote/$tk/"
ssh lebig "Set-Location '$remote/$tk'; \$s=Get-Date; cargo build --release 2>&1 | Select-Object -Last 25; if (\$LASTEXITCODE) { exit 1 }; 'build seconds: ' + [int]((Get-Date)-\$s).TotalSeconds; \$d=\"\$HOME\\Desktop\\Beamer bake-off\"; New-Item -ItemType Directory -Force \$d | Out-Null; Copy-Item 'target/release/bakeoff-$tk.exe' \$d -Force; Get-Item \"\$d\\bakeoff-$tk.exe\" | Select-Object Name,Length"
if [ "$smoke" = "--smoke" ]; then
  ssh lebig "\$exe=\"\$HOME\\Desktop\\Beamer bake-off\\bakeoff-$tk.exe\"; \$log=\"\$env:LOCALAPPDATA\\BeamerBakeoff\\$tk.log\"; Remove-Item \$log -ErrorAction SilentlyContinue; \$a=New-ScheduledTaskAction -Execute \$exe -Argument '--test-glow --open-settings --quit-after 8'; \$p=New-ScheduledTaskPrincipal -UserId \$env:USERNAME -LogonType Interactive; Register-ScheduledTask -TaskName 'BeamerBakeoffSmoke' -Action \$a -Principal \$p -Force | Out-Null; Start-ScheduledTask 'BeamerBakeoffSmoke'; Start-Sleep 11; Unregister-ScheduledTask 'BeamerBakeoffSmoke' -Confirm:\$false; if (Test-Path \$log) { Get-Content \$log } else { 'NO LOG (did it start? is Alex logged in on the PC?)' }"
fi
