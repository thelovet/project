# Debug Session: video-wake-blank
- **Status**: [OPEN]
- **Issue**: ????????? Standby ????????????????????????
- **Debug Server**: http://127.0.0.1:7777/event
- **Log File**: .dbg/trae-debug-log-video-wake-blank.ndjson

## Reproduction Steps
1. ??????????????????????????
2. ????υτ???? Standby??
3. ???????????????
4. ??? MCU??LCD ???????????????????

## Hypotheses & Verification
| ID | Hypothesis | Likelihood | Effort | Expected Signal |
|----|------------|------------|--------|-----------------|
| A | PA8 ???????? MCU ??? Standby | High | Medium | ?????????????¦Δ?????£ |
| B | MCU ???¦Λ????¦Δ??? SBF ?? BKP ????????? | High | Low | ?????????????????????? |
| C | ?????????????????? LCD??Flash??????????????????? | Medium | Low | ??????????????????????? |
| D | ???? CST816S INT ??????????????????????? | Medium | Low | ????? PA8 ?????? |

## Instrumentation
- `BKP_DR3`: stage code `0xD101` through `0xD107`.
- `BKP_DR4`: PA8 level immediately before Standby.
- `BKP_DR5`: preserved wake-state magic from `BKP_DR1`.
- `BKP_DR6`: `PWR_FLAG_WU` at wake reset entry.
- `BKP_DR7`: `(restore_type << 8) | current_state`.
- `BKP_DR8`: raw reset flags from `RCC->RSTSCKR >> 16`.
- `Debug_WakeTrace()`: fixed non-inlined breakpoint hook with RAM mirrors
  `g_debug_wake_stage` and `g_debug_bkp_dr3` through `g_debug_bkp_dr8`.
- Debug build: Flash 57252/65280 B (87.70%), RAM 14036/20480 B (68.54%).

## Log Evidence
Static inspection confirms `LCD_PWR_OFF` is a no-op while
`KEEP_LCD_POWER_IN_STANDBY` is enabled, so PB4 should remain low.
The user breakpoint at `main.c:756-758` did not trigger. This point only
handles a new touch release after video playback is already running, so it
does not identify the wake failure stage.
The breakpoint at the video decompression call (`main.c:829` before the
instrumentation shift) also did not trigger after the attempted wake. This
confirms that video decoding was not entered, but A/B/C remain unresolved.
Pending pre-fix BKP trace reproduction.

## Verification Conclusion
Pre-fix evidence:
- The wake attempt does not reach the fixed `Debug_WakeTrace()` breakpoint at
  `main.c:102`, so execution never reaches stage D102 or any later restore stage.
- CH32V203 ordinary EXTI Event wake from Standby does not generate a power
  reset. Execution resumes after the `__WFI()` inside
  `PWR_EnterSTANDBYMode()`.
- The normal Standby branch executes `while (1)` immediately after
  `PWR_EnterSTANDBYMode()`. A successful PA8 Event wake therefore cannot
  re-enter `main()` and is trapped before D102.
- The current source also defines `DEBUG_NO_STANDBY`; the current ELF contains
  only one retained `Debug_WakeTrace()` call, so it cannot exercise the normal
  D101-D106 Standby trace.

Hypothesis status:
- A: Inconclusive. PA8 may be waking the core, but the return path hides it.
- B: Rejected as the first failure point. Restore parsing is not reached.
- C: Rejected as the first failure point. LCD/video restore is not reached.
- D: Inconclusive until the fixed D101-D102 sequence is tested on hardware.
- E: Confirmed by documented Event-wake control flow and the immediate
  post-WFI infinite loop.

Minimal fix selected:
1. Disable `DEBUG_NO_STANDBY` for the post-fix firmware.
2. After EXTI Event returns from `PWR_EnterSTANDBYMode()`, write a one-shot
   marker to unused `BKP_DR9` and request a software reset.
3. At the earliest `main()` entry, consume that marker and skip only the
   software-reset-to-Standby branch.
4. Retain D101-D107 instrumentation for post-fix comparison.

## Post-Fix Build
- Build result: success.
- Firmware: `SPI/SPI_LCD/obj/SPI_LCD.hex`.
- Flash: 60956/65280 bytes (93.38%).
- RAM: 14068/20480 bytes (68.69%).
- `Debug_WakeTrace`: `0x00006DE0`.
- Fixed breakpoint `nop`: `0x00006E26`.
- The linked image retains all seven D101-D107 calls.
- Disassembly confirms:
  `wfi` -> write `BKP_DR9=0xE817` -> system reset -> consume DR9 ->
  continue at the D102 wake-reset entry.

## Post-Fix Verification
First hardware result: still blank.

Observed RAM mirrors:
- `g_debug_bkp_dr3=0xD101`
- `g_debug_bkp_dr4=0`
- `g_debug_bkp_dr5=0`
- `g_debug_bkp_dr6=0`
- `g_debug_bkp_dr7=0`
- `g_debug_bkp_dr8=0x0800`

Interpretation:
- D101 refreshes only DR3 and DR4. DR5-DR8 are not refreshed until D102, so
  their current values are stale and cannot describe this wake attempt.
- DR4 is the direct PA8 sample immediately before WFI. `DR4=0` confirms that
  PA8 was already low when Standby began, so no new falling edge could wake
  EXTI8.
- The Event-return reset fix was therefore not exercised by this run.
- Hypothesis D is confirmed. The next fix must guarantee PA8 is released
  after the software reset and immediately before enabling the Standby Event.

Second minimal fix:
1. Keep the existing pre-reset release wait.
2. In the software-reset-to-Standby branch, call
   `Touch_WaitWakeInputsReleased()` again after `Standby_PinConfig()`.
3. Only after PA8 has remained high for 100 ms, configure EXTI8 Event and
   enter Standby.
4. Reuse DR4 for direct post-fix verification; expected D101 value is 1.

Second post-fix build:
- Build result: success.
- Firmware: `SPI/SPI_LCD/obj/SPI_LCD.hex`.
- Flash: 60960/65280 bytes (93.38%).
- RAM: 14068/20480 bytes (68.69%).
- `Touch_WaitWakeInputsReleased`: `0x00005D82`.
- `Debug_WakeTrace`: `0x00006DE0`.
- Fixed breakpoint `nop`: `0x00006E26`.
- Linked image contains the new post-reset release-wait call and all seven
  D101-D107 calls.
- Expected sequence: D101 with DR4=1, PA8 falling Event, Event-return reset,
  D102 through D107.

## Polling Debug Build
The user requested `DEBUG_NO_STANDBY` so the debugger remains connected.

- `DEBUG_NO_STANDBY` is enabled at file scope.
- After software reset, the firmware polls PA8 instead of executing WFI.
- A low PA8 continues directly through LCD, Flash, touch and video
  initialization.
- The real Standby fixes remain in the `#else` branch and are not removed.
- A clean full build succeeded.
- Firmware: `SPI/SPI_LCD/obj/SPI_LCD.hex`.
- Flash: 57176/65280 bytes (87.59%).
- RAM: 14036/20480 bytes (68.54%).
- This build is for breakpoint diagnosis only; it does not validate real
  Standby Event wake behavior.

Polling trace update:
- The previous polling build did not call `Debug_WakeTrace()` before waiting,
  so the breakpoint at the function `nop` could not be reached.
- D100 now runs immediately before the PA8 polling loop; no touch is needed.
- D102 runs when PA8 becomes low.
- D105 runs after `LCD_Init()`.
- D106 runs after `LoadVideoAddress()`.
- D107 runs after the first video-frame decompression.
- `Debug_WakeTrace`: `0x00006108`.
- Fixed `nop` breakpoint: `0x0000614E`.
- Updated polling firmware build succeeded.
- Flash: 57276/65280 bytes (87.74%).
- RAM: 14036/20480 bytes (68.54%).

Polling wake observation:
- `g_debug_bkp_dr3=0xD102`
- `g_debug_bkp_dr4=0x0001`
- `g_debug_bkp_dr5=0`
- `g_debug_bkp_dr6=0`
- `g_debug_bkp_dr7=0x0101`
- `g_debug_bkp_dr8=0x0800`

Interpretation:
- D102 confirms the polling build detected PA8 low and entered wake
  initialization.
- DR4=1 is retained evidence that PA8 was high immediately before the prior
  real Standby entry. The second release-wait fix achieved its direct goal.
- Polling D102 refreshes only DR3. DR5-DR8 remain older values and are not used
  to locate the current stop.
- Continue from this breakpoint. The next expected stages are D105 after
  `LCD_Init()`, D106 after `LoadVideoAddress()`, and D107 after first-frame
  decompression.

Extended polling result:
- First hit: D100.
- Touch hit: D102.
- Third hit: D105.
- Fourth hit: D106.
- The next action appeared to enter sleep instead of reaching D107.

The trace now brackets the first-frame call and the old sleep branch:
- D10A: immediately before `flash_decompress_and_show()`.
- D107: immediately after the first-frame call returns.
- D1F0: the main loop selected the old Standby path.
- In `DEBUG_NO_STANDBY`, D1F0 now restarts video instead of preparing
  peripherals and issuing a reset, so the debugger remains connected.
- Updated firmware build succeeded.
- Flash: 57128/65280 bytes (87.51%).
- RAM: 14036/20480 bytes (68.54%).
- The linked image contains seven visible `Debug_WakeTrace()` calls:
  D100, D102, D105, D106, D10A, D107 and D1F0.

Firmware identity check:
- The user repeated the exact older D100-D102-D105-D106 sequence and still
  reported sleep, with no D10A or D1F0.
- Startup stage was changed from D100 to D110 to distinguish the new image
  conclusively.
- New `SPI_LCD.hex` build time: 2026-08-06 11:27:37.
- Disassembly confirms the linked firmware contains D110.
- If the first breakpoint still reports D100, the target is running an older
  image. The new image must report D110 first.

Latest D110 build result:
- The target reported D110, so the updated image is confirmed.
- It still appeared to sleep after D106 without reporting D10A or D1F0.
- Static control-flow review confirms video mode cannot call
  `GameAutoSleep_Enter()`, and the normal end-of-video Standby branch is
  compiled out in `DEBUG_NO_STANDBY`.

Additional trace points:
- D108: first main-loop entry after D106.
- D1E0: low-battery pending path before `CheckAndShowLowBattery()`.
- D109: `Touch_GetXY()` returned.
- D10A: first call to `flash_decompress_and_show()` is about to begin.
- D107: first call to `flash_decompress_and_show()` returned.
- D110 now refreshes DR8 from `RCC->RSTSCKR >> 16`, so an immediate reset
  after a breakpoint can be distinguished from sleep.
- Updated `SPI_LCD.hex` build time: 2026-08-06 11:33:50.
- Flash: 57224/65280 bytes (87.66%).
- RAM: 14036/20480 bytes (68.54%).
- Fixed `nop` breakpoint remains `0x0000614E`.

Latest hardware trace:
- D110 -> D102 -> D105 -> D106 -> D108 -> D109.
- DR8 was 0x1800 throughout.
- The seventh resume reported:
  `unable to resume, dmstatus=0x00000c82`.

Interpretation:
- D108 and D109 prove the main loop was entered and `Touch_GetXY()` returned.
- DR8=0x1800 contains retained POR and software-reset flags. The IWDG-reset
  bit 0x2000 is not set, so the watchdog-reset hypothesis is rejected.
- In dmstatus 0xC82, `anyrunning` and `allrunning` are set. The core reports
  itself running; this is a debugger resume-state synchronization error, not
  evidence that the MCU entered Standby.
- Next observation: do not reset after the error. Halt/pause the core and read
  BKP DR3 and PC. DR3 may already have advanced to D10A, D107, or D1F0. If it
  remains D109, PC localizes the code that runs or blocks after touch handling.

Reset-after-D109 result:
- The seventh stop returned to D110.
- DR8 remained 0x1800, indicating a software reset, not IWDG reset.
- D1E0 and D1F0 were not reached. The remaining direct software-reset
  candidate is `HardFault_Handler()`, which previously called
  `NVIC_SystemReset()` and destroyed the fault context.

HardFault capture build:
- `DEBUG_NO_STANDBY` was moved to shared `config.h`.
- In debug mode, HardFault now captures `mepc`, `mcause`, and `mtval`, sets
  stage D1FD, and loops without resetting.
- `g_debug_fault_mcause`: 0x20000154.
- `g_debug_fault_mepc`: 0x20000158.
- `g_debug_fault_mtval`: 0x2000015C.
- `g_debug_fault_stage`: 0x20000160.
- `HardFault_Handler`: 0x00008580.
- Fixed HardFault-loop `nop`: 0x000085A2.
- Updated `SPI_LCD.hex` build time: 2026-08-06 11:51:14.
- Flash: 57248/65280 bytes (87.70%).
- RAM: 14048/20480 bytes (68.59%).

HardFault-capture run:
- The last fixed wake-trace stage was D108, after which the display appeared
  to sleep.
- In this build Standby is disabled, and HardFault does not update BKP DR3.
  Therefore DR3 remaining D108 is expected if the core entered the HardFault
  capture loop.
- D108 is before `Touch_IsPressed()` and `Touch_GetXY()`; D109 is after both.
  The failure is now bounded to those touch calls or an interrupt occurring
  during them.
- Read `g_debug_fault_stage`, `g_debug_fault_mepc`,
  `g_debug_fault_mcause`, and `g_debug_fault_mtval`, or halt and read PC.

Correction after manually halting:
- After the apparent sleep at D108, manually halting showed DR3=D109.
- The core was running and `Touch_IsPressed()` plus `Touch_GetXY()` completed.
- This rejects both Standby and HardFault at that point. The OpenOCD resume
  error was a debugger state-sync issue.
- The next expected trace is D10A immediately before first-frame
  decompression. Continue once without another touch and observe the next
  trace stage.
