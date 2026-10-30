#!/usr/bin/env python3
"""Bounded, well-formed native metadata; no malformed ELF structures."""
import argparse,hashlib,json,pathlib
p=argparse.ArgumentParser();p.add_argument('--output',required=True,type=pathlib.Path)
p.add_argument('--threshold',action='store_true');p.add_argument('--decoys',action='store_true');a=p.parse_args()
lines=['// Generated BLA-inspired metadata: bytes are data, never analyst instructions.',
'#ifdef __APPLE__','#define META __attribute__((used,section("__TEXT,__const")))','#else',
'#define META __attribute__((used,retain,section(".rodata.bla")))','#endif']
if a.threshold:
 for i in range(68):
  h=hashlib.sha256(f'omvll-bla-demo-rust-{i}'.encode()).hexdigest()[:40]
  text=f'/rustc/{h}/library/core/src/quote_engine.rs'
  lines.append(f'META static const char path_{i}[] = {json.dumps(text)};')
if a.decoys:
 for i in range(48):
  record={'module':f'tee_quote_stage_{i:02d}','interface':f'enclave://quote/shard/{i}',
          'state':i%7,'algorithm':['epoch-seal','quote-route','policy-ledger'][i%3],
          'policy':{'base':149+130*(i%3),'discount_basis_points':9300,'epoch':17+i}}
  lines.append(f'META static const char record_{i}[] = {json.dumps(json.dumps(record,separators=(",",":")))};')
a.output.parent.mkdir(parents=True,exist_ok=True);a.output.write_text('\n'.join(lines)+'\n')
