#!/bin/bash
# drive.sh VERSION OLDSTAGE OUT : wait for menu, enter the world, keep copying the trace until 150 s after Continue or exit.
cd "$(git rev-parse --show-toplevel)"
S=${SCRATCH}
V=$1; OLD=$2; OUT=$3; MARK=${4:-asset}; TMP=$OUT.tmp
grab() { st=$(PYTHONPATH=. python3 $S/rawtrace.py $S $V $TMP 2>/dev/null | cut -d' ' -f2); if [ "$st" != "$OLD" ] && [ -s $TMP ] && grep -q -- "$MARK" $TMP && ! grep -q "launch-start-utc-before" $TMP; then cp $TMP $OUT; return 0; fi; return 1; }
alive() { PYTHONPATH=. python3 -c "
import sys,json; sys.path.insert(0,'$S'); import portal as p
_,d=p.portal.request('GET','/api/resourcemanager/processes',read_limit=8<<20)
print(any(r.get('ImageName')=='Spider-Man.exe' for r in json.loads(d)['Processes']))"; }
for i in $(seq 1 60); do grab && grep -q "lobby-save-state=0x00000001" $OUT && ! grep -q "pso-slow kind=graphics n=9[0-9][0-9] wait-ms=[0-9][0-9][0-9][0-9]" <(tail -5 $OUT) && break; sleep 10; done
echo menu $(date -u +%T)
sleep 5
for k in 1 2 3; do python3 -m tools.xbox_remote_input --env-file $S/xbox.env hold:A:0.3 >/dev/null 2>&1; sleep 4; done
date -u +%Y-%m-%dT%H:%M:%SZ > $OUT.continue
for i in $(seq 1 15); do grab; [ "$(alive)" = "True" ] || { echo exited $(date -u +%T); break; }; sleep 10; done
grab; echo alive=$(alive) $(date -u +%T)
python3 -m tools.xbox_remote_input --env-file $S/xbox.env shot:$OUT.png >/dev/null 2>&1 && sips -Z 640 $OUT.png --out $OUT-small.png >/dev/null
grep -c "zone-type-load-error.*detail=0x00000002" $OUT
grep -E "lazy-(range|commit-failed)|d3d12-heap .*result=0x8|fault access" $OUT | cut -c1-200
grep -E "lazy-summary" $OUT | tail -12 | cut -c1-200
grep census-memory $OUT | tail -1 | cut -c1-200
