; ===================================================================
;  NES WEB SERVER  --  link layer bring-up test ROM
;
;  This is NOT the web server.  It exercises the three controller
;  port lines one at a time so each can be proved or ruled out on
;  its own.  Pair it with src/firmware/NES_debug.ino on the ESP32.
;
;  Controller port 1 ($4016).  Runs forever, cycling five phases:
;
;    1  D0 SENSE    green/red mirror of what the NES reads on D0
;    2  OUT0 DRIVE  NES toggles OUT0; blue mirror of what it drives
;    3  CLK FAST    exactly 1000 clock pulses ~40us apart, yellow
;    4  CLK SLOW    exactly 1000 clock pulses ~110us apart, orange
;    5  IDLE        port untouched, black.  the quiet baseline
;
;  There is deliberately no handshake with the gateway: each side
;  runs free, so nothing can go out of sync.  Read the TV for which
;  phase you are in and the serial log for what actually arrived.
; ===================================================================

; --- iNES Header (NROM-256, 32K PRG, 8K CHR, vertical mirroring) ---
.db "NES",$1a, $02, $01, $01, $00, 0,0,0,0,0,0,0,0

BIT_DELAY    = 4                ; ~37us NTSC / ~40us PAL between pulses
SLOW_DELAY   = 24               ; ~103us NTSC / ~111us PAL
PHASE_FRAMES = 150              ; ~3s PAL, ~2.5s NTSC
BURST_OUTER  = $04              ; 4 * 250 = exactly 1000 clock pulses
BURST_INNER  = $FA

COL_D0_HIGH  = $2A              ; green     - D0 reads 1
COL_D0_LOW   = $16              ; red       - D0 reads 0
COL_OUT_HIGH = $21              ; pale blue - driving OUT0 high
COL_OUT_LOW  = $01              ; dark blue - driving OUT0 low
COL_CLK_FAST = $28              ; yellow    - fast clock burst
COL_CLK_SLOW = $27              ; orange    - slow clock burst
COL_IDLE     = $0F              ; black     - baseline, port untouched

; Zero page: $10 loop counter, $11 OUT0 state

.org $8000

Reset:
    SEI
    CLD
    LDX #$40
    STX $4017                   ; APU frame counter, no IRQ
    LDX #$FF
    TXS
    INX                         ; X = 0
    STX $2000                   ; NMI off
    STX $2001                   ; rendering off until the palette is set
    STX $4010                   ; DMC off: its DMA corrupts $4016 reads
    STX $4015                   ; all APU channels off
    STX $4016                   ; OUT0 known low before we start

v1:
    BIT $2002
    BPL v1
v2:
    BIT $2002
    BPL v2

    LDA #COL_IDLE
    JSR SetBG
    LDA #$1E
    STA $2001

MainLoop:
    JSR PhaseD0
    JSR PhaseOut0
    JSR PhaseClkFast
    JSR PhaseClkSlow
    JSR PhaseIdle
    JMP MainLoop

; --- Phase 1: can the NES see D0 at all? ---------------------------
; Read D0 once per frame and paint what came back.  Gateway holding
; D0 low gives solid red, holding it high gives solid green, and the
; 1Hz square wave gives a clean alternation.  A restless flicker
; means the line is floating: nothing is driving it.
; Costs one $4016 read per frame, so the gateway should also see a
; steady ~50 clock edges a second through this phase.
PhaseD0:
    LDA #PHASE_FRAMES
    STA $10
PhaseD0Loop:
    LDA $4016
    LSR A
    BCC PhaseD0Low
    LDA #COL_D0_HIGH
    JMP PhaseD0Paint
PhaseD0Low:
    LDA #COL_D0_LOW
PhaseD0Paint:
    JSR SetBG
    JSR WaitFrame
    DEC $10
    BNE PhaseD0Loop
    RTS

; --- Phase 2: does OUT0 reach the gateway? -------------------------
; Toggle OUT0 every 4 frames and paint what we are driving.  The
; gateway counts falling edges, so expect its out0 counter to tick
; up here and stay at zero everywhere else.  Nothing reads $4016 in
; this phase, so its clock counter should be zero throughout.
PhaseOut0:
    LDA #PHASE_FRAMES
    STA $10
    LDA #$00
    STA $11
PhaseOut0Loop:
    LDA $11
    STA $4016                   ; drive OUT0
    BEQ PhaseOut0Low
    LDA #COL_OUT_HIGH
    JMP PhaseOut0Paint
PhaseOut0Low:
    LDA #COL_OUT_LOW
PhaseOut0Paint:
    JSR SetBG
    JSR WaitFrame
    LDA $10
    AND #$03                    ; flip every 4th frame
    BNE PhaseOut0Next
    LDA $11
    EOR #$01
    STA $11
PhaseOut0Next:
    DEC $10
    BNE PhaseOut0Loop
    LDA #$00
    STA $4016                   ; leave OUT0 low
    RTS

; --- Phase 3 and 4: does the clock pulse reach the gateway? --------
; Exactly 1000 reads of $4016, so exactly 1000 falling edges on the
; clock line.  This is the whole question, because the pulse is only
; about one CPU cycle wide and cannot be widened in software.
;   1000  the line is sound
;      0  nothing is getting through at all
;  between  marginal - edges are being rounded off or missed
; Fast runs at protocol speed; slow sends the same 1000 pulses with
; a much longer gap, which separates a signal problem (both fail)
; from a rate problem (slow passes, fast does not).
PhaseClkFast:
    LDA #COL_CLK_FAST
    JSR SetBG
    LDX #BURST_OUTER
PhaseClkFastOuter:
    LDY #BURST_INNER
PhaseClkFastInner:
    LDA $4016
    JSR BitDelay
    DEY
    BNE PhaseClkFastInner
    DEX
    BNE PhaseClkFastOuter
    JSR SettleGap
    RTS

PhaseClkSlow:
    LDA #COL_CLK_SLOW
    JSR SetBG
    LDX #BURST_OUTER
PhaseClkSlowOuter:
    LDY #BURST_INNER
PhaseClkSlowInner:
    LDA $4016
    JSR SlowDelay
    DEY
    BNE PhaseClkSlowInner
    DEX
    BNE PhaseClkSlowOuter
    JSR SettleGap
    RTS

; --- Phase 5: the quiet baseline -----------------------------------
; Touch nothing.  Both counters must read zero for this whole phase.
; If they do not, something is generating edges that the ROM is not,
; and every other number in the log is suspect.
PhaseIdle:
    LDA #COL_IDLE
    JSR SetBG
    LDA #PHASE_FRAMES
    STA $10
PhaseIdleLoop:
    JSR WaitFrame
    DEC $10
    BNE PhaseIdleLoop
    RTS

; --- Helpers --------------------------------------------------------
; ~1s of quiet after a burst, so the gateway's twice a second report
; lands the burst total in a window of its own.
SettleGap:
    LDA #$32
    STA $10
SettleGapLoop:
    JSR WaitFrame
    DEC $10
    BNE SettleGapLoop
    RTS

WaitFrame:
    BIT $2002
    BPL WaitFrame
    RTS

; A = palette index -> the backdrop at $3F00.  CHR is blank, so the
; whole screen becomes this colour.  Clobbers X, preserves A and Y.
SetBG:
    LDX #$3F
    STX $2006
    LDX #$00
    STX $2006
    STA $2007
    STX $2006                   ; leave the address latch somewhere safe
    STX $2006
    RTS

; 7*n + 16 cycles including the JSR.  Clobbers A, preserves X and Y.
BitDelay:
    LDA #BIT_DELAY
    JMP DelayA
SlowDelay:
    LDA #SLOW_DELAY
DelayA:
    SEC
    SBC #$01
    BNE DelayA
    RTS

IrqNmi:
    RTI

.pad $FFFA, $00
.dw IrqNmi, Reset, IrqNmi
.pad $12000, $00
