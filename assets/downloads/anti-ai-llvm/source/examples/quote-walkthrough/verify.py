"""Check only the public teaching example; this is not the blind benchmark."""
import ctypes, json, random, sys
from pathlib import Path
lib = ctypes.CDLL(sys.argv[1])
for name in ('quote_demo', 'quote_protected'):
    f = getattr(lib, name)
    f.argtypes = [ctypes.c_uint32] * 4
    f.restype = ctypes.c_uint32
mask = 0xffffffff
rng = random.Random(20261030)
cases = []
for user in (0, 7, 0xffffffff):
    for quantity in (0, 9, 10, 12, 0xffffffff):
        for tier in (0, 2, 0xffffffff):
            expected = (((user * 17 + 9) & mask) ^ 0x13579bdf)
            for token in (expected, expected ^ 1):
                cases.append((user, quantity, tier, token))
for i in range(2000):
    user, quantity, tier, token = [rng.getrandbits(32) for _ in range(4)]
    if i % 2 == 0:
        token = (((user * 17 + 9) & mask) ^ 0x13579bdf)
    cases.append((user, quantity, tier, token))
for user, quantity, tier, token in cases:
    expected = (((user * 17 + 9) & mask) ^ 0x13579bdf)
    gross = (quantity * ((100 + tier * 5) & mask)) & mask
    oracle = (gross - (gross // 10 if quantity >= 10 else 0)) & mask if token == expected else 0
    assert lib.quote_demo(user, quantity, tier, token) == oracle
    assert lib.quote_protected(user, quantity, tier, token) == oracle
report = {'scope':'public teaching example only; native macOS arm64, no Android or attack-strength claim',
          'compiler':'LLVM 21', 'pass':'BusinessVM.cpp', 'seed':'0x2468ace1',
          'cases':len(cases), 'matched':len(cases), 'random':2000, 'boundary':90,
          'worked_example':{'input':[7,12,2,0x13579b5f], 'return':lib.quote_protected(7,12,2,0x13579b5f)}}
Path(sys.argv[2]).write_text(json.dumps(report,ensure_ascii=False,indent=2)+'\n')
print(json.dumps(report,ensure_ascii=False))
