; ===================================================================
;  NES WEB SERVER  --  D0 line test, the smallest useful program
;
;  Does one thing: reads the gateway's data line and paints it.
;
;      RED    = D0 reads 0
;      GREEN  = D0 reads 1
;
;  Nothing else.  No phases, no bursts, no protocol.  Pair it with
;  src/firmware/NES_d0test/NES_d0test.ino and type 1 or 0 at the
;  serial monitor - the screen should follow immediately.
;
;  Controller port 1 ($4016), gateway -> NES direction only.
;
;  Deliberately does NOT strobe OUT0.  We want the raw level of the
;  data line, and leaving OUT0 alone means the NES drives nothing
;  into the port, so a short between pins 3 and 4 cannot pit the
;  console's output against the gateway's.
; ===================================================================

; --- iNES Header (NROM-256, 32K PRG, 8K CHR, vertical mirroring) ---
.db "NES",$1a, $02, $01, $01, $00, 0,0,0,0,0,0,0,0

COL_HIGH = $2A                  ; green - D0 reads 1
COL_LOW  = $16                  ; red   - D0 reads 0

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
    STX $4016                   ; OUT0 low, and left alone from here

v1:
    BIT $2002
    BPL v1
v2:
    BIT $2002
    BPL v2

    LDA #COL_LOW
    JSR SetBG
    LDA #$1E
    STA $2001

; Read once per frame and paint it.  Waiting for vblank first means
; the palette write lands while the PPU is idle.
Loop:
    BIT $2002
    BPL Loop                    ; wait for vblank

    LDA $4016
    LSR A                       ; bit 0 -> carry
    BCC LoopLow
    LDA #COL_HIGH
    JMP LoopPaint
LoopLow:
    LDA #COL_LOW
LoopPaint:
    JSR SetBG
    JMP Loop

; A = palette index -> the backdrop at $3F00.  CHR is blank, so the
; whole screen becomes this colour.  Clobbers X.
SetBG:
    LDX #$3F
    STX $2006
    LDX #$00
    STX $2006
    STA $2007
    STX $2006                   ; leave the address latch somewhere safe
    STX $2006
    RTS

IrqNmi:
    RTI

.pad $FFFA, $00
.dw IrqNmi, Reset, IrqNmi
.pad $12000, $00
