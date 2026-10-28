# QEMU Sspesa review: change list (2026-10-28)

Branch riscv_sspesa_rfc_v2 at 48d5e46e57. Review notes only; you make the edits.

## 1. Data TLB miss: exact PC
- Where: riscv_cpu_tlb_fill() (cpu_helper.c:1769) calls pmu_tlb_fill_incr_ctr() (:1792)
  -> riscv_pmu_incr_ctr() -> riscv_pmu_sspesa_capture() (pmu.c:259).
- Problem: env->pc is not synced at tlb_fill time, so it holds the block-start or
  last-updated PC.
- Change: pass retaddr down to the capture point. When retaddr != 0, call
  cpu_unwind_state_data(cs, retaddr, data) (include/accel/tcg/cpu-loop.h:34).
  It is read-only and leaves env untouched.
- Rebuild the PC the same way restore_state_to_opc does (tcg-cpu.c ~239):
  CF_PCREL ? (env->pc & TARGET_PAGE_MASK) | data[0] : data[0].
- Don't use cpu_restore_state(): the block keeps running after a successful fill.
- MMU_INST_FETCH (retaddr == 0, translate time): keep env->pc; it can't be exact.
- The timer caller (pmu.c:452) keeps env->pc.

## 3. M-mode overflow recorded as MODE=U (needs your decision)
- Where: pmu.c:204. Only PRV_S is checked, so PRV_M falls through to MODE=0 (U).
- Option A: MODE = env->priv (priv-spec encoding, M=3). Linux's
  riscv_pmu_sbi.c:1152 default case already falls back safely.
- Option B: skip the capture when env->priv == PRV_M.
- Either way, check SHPMSDATA_MODE_* in cpu_bits.h ~612 for a matching M define.

## 4. Simultaneous overflow: drop the PC compare
- Where: pmu.c:220 (`env->shpmspc == env->pc && ctr_idx < cur_ctr_idx`).
- Only riscv_pmu_timer_cb() (pmu.c:459) can produce simultaneous overflows (cycles
  then instret, in the same call). A single incr_ctr call handles one event, which
  maps to one counter.
- Change: track "captured in this timer callback" (e.g. a flag set at the start of
  riscv_pmu_timer_cb). Apply the lowest-CNTRID rule only while that flag is set;
  otherwise, with LCOFIP already pending, do nothing.

## 5. Sspesa implied extensions (needs your decision)
- Where: SSPESA_IMPLIED (cpu.c:2949) implies only sscofpmf.
- Linux now needs Ssccfg as well, and CDE needs Smcdeleg (OpenSBI sbi_hart.c:163).
- Decide: add smcdeleg + ssccfg to the implied list, or leave them to the command line.

## 7. Commit message, patch 1 (805098d8a4)
- "Added the 'sspesa' and ssplcofi CPU properties": use imperative "Add", and quote
  both names the same way.
- After the fixes above, recheck patch 2 (48d5e46e57). It currently says timer
  overflows "do not guarantee instruction-accurate attribution". Also update the
  "lowest counter ID without replacing the sample PC" line if item 4 changes.

## Also still open
- Linux: stale-sample invalidate after read (write CNTRID=1, i.e. time) — your call.
- Linux: force-update origin/riscv_sspesa_rfc_v2 from laptop (new branch
  riscv_sspesa_rfc_v2_20261028 already pushed at 2c74cb9342a1).
- QEMU origin URL still embeds a ghp_ token; switch to SSH.
- Old QEMU pending-flag edits: stash@{0}; old tip: tag backup/sspesa-qemu-v2-prepull-20261028.
