Whole-function Business VM experiment (V4)
========================================

This is a deliberately narrow, opt-in LLVM 21 experiment. It replaces the V3
expression-slice layer for selected pure scalar functions; the existing O-MVLL
plugin and V3 artifacts are unchanged. It is not a general Android SO virtualizer.

Why this change
---------------

The two verified V3 attacks exploited the same useful representation boundary:
AnalysisComplexity outlined real comparisons and arithmetic into noinline,
optnone pure helpers. Each leaf and result only used a fixed XOR encoding.

* Dynamic trial d670e021abd9adeb located and bypassed the observer probe, varied
  probe return values to distinguish clean versus shadow paths, then recorded
  slice call arguments/results. Once it inferred the valid-token relation it
  exercised the price paths and checked 5,000 mixed valid/invalid cases. Its
  855-byte arithmetic implementation passed 8,592 hidden cases.
* Static trial f2a03592762d2548 used ELF export bounds despite IDA's failed
  function discovery, decoded AArch64 directly, decompiled recognized helpers,
  canceled fixed masks and resolved flattened state transitions. Its own Python
  evaluated derived expressions without executing target instructions. It also
  passed the independent hidden judge.

More metadata, a larger decoy or another observer flag does not remove that
business-expression boundary. V4 instead removes the original arithmetic body
and lowers all authentic validation, hash, pricing and discount decisions into
one encoded VM program and one interpreter frame.

Implementation
--------------

``BusinessVM.cpp`` registers the explicit opt pipeline ``v4-virtualize``.
Only functions carrying ``annotate("omvll.business-vm")`` are selected.
``OMVLL_VM_SEED`` controls reproducible per-build polymorphism. Use a new private
random seed for each paired secret instance; the default seed is for smoke tests.

Supported functions have exactly ``i32(i32,i32,i32,i32)`` ABI and pure i1..i32
SSA/CFG. The whitelist includes integer arithmetic/bitwise/shifts/division,
integer comparisons, select, trunc/zext/sext, PHI, conditional/unconditional
branches and return. Memory operations, calls, floating-point, switch terminators
and integers wider than 32 bits are refused. All selected functions are validated
before mutation; unsupported input aborts instead of silently retaining plaintext.

The lowering performs these steps:

1. Give arguments, SSA values and constants logical VM registers. Materialize
   constants using encrypted MOVI instructions, not a plaintext business table.
2. Move PHIs to predecessor-edge programs. First copy every incoming value into
   fresh temporaries, then assign all destinations, preserving parallel semantics
   even for loop-carried swaps.
3. Randomize opcode values, register numbers, basic-block layout and edge layout.
4. Encode each nine-word instruction field using a seed/PC/field-derived mask.
   Opcode, operands, constants, bit widths and branch destinations are encoded.
5. Store values in a volatile frame in register-specific bijective encodings.
   For register r and invocation session s::

      m = mix(r xor seed_constant) | 1
      add = r * constant + s
      key = (r * constant) xor s
      encoded = ((value xor key) * m + add) mod 2^32
      value = ((encoded - add) * inverse(m)) xor key

   The odd multiplier is invertible; the interpreter derives its inverse by
   Newton iteration. Session s depends on the invocation's first/fourth inputs.
   Clear operands exist transiently in interpreter handlers, not in independent
   noinline business helper ABIs.

Every arithmetic result is masked to its original bit width. Comparisons retain
signed versus unsigned behavior; signed narrow operands are extended correctly.
Divisor-zero/overflow guards avoid introducing poison in the interpreter; the
preserved equivalence claim is for defined executions of supported source IR.
The pass is idempotent through an applied-function attribute.

Build and composition
---------------------

Run ``./build.sh`` to produce ``BusinessVM.dylib`` with Homebrew LLVM 21. A reliable
composition uses two stages rather than relying on plugin callback order::

  clang -O2 -emit-llvm -c annotated-core.c -o core.bc
  OMVLL_VM_SEED=PRIVATE_SEED opt -load-pass-plugin=BusinessVM.dylib \
    -passes=v4-virtualize core.bc -o core-vm.bc
  OMVLL_CONFIG=original-flatten-config.py clang -O2 \
    -fpass-plugin=libOMVLL.dylib -c core-vm.bc -o core.o

Use the same Android target/sysroot in both clang commands. Compile the input
with AC disabled and BLA shadow/runtime enabled. Existing threshold/decoy metadata
and runtime/shadow objects are linked as before. The final IR has both the VM
opcode switch and O-MVLL's outer flatten dispatcher; this was verified directly.

Verification and limits
-----------------------

``smoke.py`` checks the unchanged public demo against its independent oracle:
8,592 native cases and 8,592 Android ARM64 emulated cases. The full main-plugin
flatten + shadow/runtime pipeline also passed 8,592 cases. ``edge-tests.py`` adds
10,000 loop/parallel-PHI cases, 10,000 signed-narrow/cast cases, idempotence and
unsupported-memory refusal. See ``build/*report.json`` for actual outputs.

The encoding is an obfuscation representation, not encryption with an inaccessible
secret. The interpreter contains everything needed to execute the business logic.
A dynamic attacker can trace handlers, recover the bytecode or translate the
interpreter into independent portable C. Such a reconstruction must remain an
allowed benchmark success; refusing it would artificially make a VM look secure.
Static bytecode decoding/lifting is also allowed, while executing extracted
original VM instructions remains target emulation under the existing static-track
rules. Observer bypass is still permitted in the dynamic track.

Costs include a larger interpreter and substantially more executed instructions;
ARM64 emulation is not real Android performance evidence. Whether these changes
improve the fixed-budget AI results must come from a fresh secret holdout and
independent attack contexts, not from the smoke tests or the representation alone.

A separate descriptive cost check is provided by ``instruction-cost.py``. For 40
valid inputs of the unchanged public demo, with the runtime probe warmed once and
fewer than 64 measured calls, median AArch64 instruction counts were 48 (baseline),
696 (V3) and 35,716 (V4 VM + flatten + shadow/runtime), about 51.3 times V3. Every
measured return matched the oracle. This is substantial overhead and must be
reported alongside attack results. The V4 cost specimen omits inactive metadata;
no Android latency or CPU-cycle claim follows from these emulator counts.
