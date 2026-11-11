#!/bin/bash
# Probe the rtvc encoder's private periodic-refresh keys: lose frame 60, take no recovery action,
# and watch whether the decoded picture heals by itself.
cd "$(dirname "$0")/.."
R=out/loss.jsonl
for cfg in "$@"; do
  P=()
  for kv in $cfg; do P+=(--prop "$kv"); done
  python3 experiments/runner.py $R "ir:$cfg" -- run --seconds 8 --stats 0 --verify-drop 60 "${P[@]}" --trace out/t_ir.jsonl > /dev/null
  python3 - "$cfg" <<'PY'
import json, sys
s = json.loads(open('out/loss.jsonl').readlines()[-1])
st = s['start']
print(sys.argv[1], 'set:', {k: v['status'] for k, v in st.get('props', {}).items() if k[0].isupper() and k in ('PeriodicRefreshMode','MaxRefreshFrameIntervalDuration')},
      'readback:', {k: st['readback'].get(k) for k in ('PeriodicRefreshMode', 'MaxRefreshFrameIntervalDuration')})
fr = {r['f']: r for r in map(json.loads, open('out/t_ir.jsonl'))}
print('  psnr f61..226 step 5:', [round(fr[k].get('psnr_y', -1), 1) for k in range(61, 230, 5)])
print('  sizes f61..97 step 3:', [fr[k]['size'] for k in range(61, 100, 3)], 'keys', s['keys'], 'p50/p95/max', s['p_bytes']['p50'], s['p_bytes']['p95'], s['p_bytes']['max'], 'kbps', s['kbps_mean'])
PY
done
