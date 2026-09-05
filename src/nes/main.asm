; ===================================================================
;  NES WEB SERVER  --  controller port 1 link layer
;
;  Everything runs over port 1 alone.  The NES has exactly one
;  software controlled output line to the port (OUT0, the latch on
;  pin 3) and the clock line (pin 2) pulses as a side effect of
;  reading $4016, so:
;
;    OUT0  (pin 3)  NES -> gateway data, and the poll strobe
;    CLK   (pin 2)  clock for both directions, one pulse per
;                   read of $4016 (~1 CPU cycle wide, not widenable)
;    D0    (pin 4)  gateway -> NES data, read as $4016 bit 0
;
;  $4017 is never read, so port 2 stays free.  See docs/protocol.md
;  for the full wire format.
; ===================================================================

; --- iNES Header (NROM-256, 32K PRG, 8K CHR, vertical mirroring) ---
.db "NES",$1a, $02, $01, $01, $00, 0,0,0,0,0,0,0,0

; --- Link timing ---------------------------------------------------
; Delay routines burn 7*n + 16 cycles.  One NES cycle is 0.559us.
; BIT_DELAY sets the gap between clock pulses: the gateway samples
; from an interrupt handler, so this has to comfortably exceed its
; worst case interrupt latency.  4 gives ~37us per bit (~27 kbit/s);
; drop it once the link is proven on your hardware.
BIT_DELAY    = 4
STROBE_DELAY = 24               ; ~103us, long enough to poll for

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

v1:
    BIT $2002
    BPL v1
v2:
    BIT $2002
    BPL v2

    ; Cyan screen (CHR is blank, so every tile draws as colour 0)
    LDA #$3F
    STA $2006
    LDA #$00
    STA $2006
    LDA #$1C
    STA $2007
    LDA #$00
    STA $2006                   ; reset the PPU address latch
    STA $2006
    LDA #$1E
    STA $2001

ServerLoop:
    JSR PollRequest             ; carry set = a request was waiting
    BCC ServerLoop
    JSR ProcessRequest
    JSR SendResponse
    JMP ServerLoop

; --- Poll (gateway -> NES) -----------------------------------------
; Strobe OUT0, then clock in a 9 bit frame: a ready flag followed by
; the 8 bit page id, LSB first.  The gateway arms itself on the
; falling edge of the strobe and drives D0 low whenever it is idle,
; so an unanswered poll simply reads back as zero.
PollRequest:
    LDA #$01
    STA $4016                   ; latch high
    JSR StrobeDelay
    LDA #$00
    STA $4016                   ; latch low: the gateway arms here
    JSR StrobeDelay

    LDA $4016                   ; bit 0: ready flag
    LSR A
    BCS PollReady

    LDX #$08                    ; nothing pending, back off ~0.8ms
PollBackoff:
    JSR StrobeDelay
    DEX
    BNE PollBackoff
    CLC
    RTS

PollReady:
    LDX #$08
PollIdLoop:
    JSR BitDelay                ; let the gateway present the bit
    LDA $4016
    LSR A
    ROR $00                     ; shift in from the top, LSB first
    DEX
    BNE PollIdLoop
    LDA $00
    STA $01
    SEC
    RTS

; --- Lookup Logic --------------------------------------------------
; PageCount comes from the generated data.asm.  The table holds
; PageCount real pages followed by the 404 page, so any out of range
; id lands on the 404 entry instead of running off the end.
ProcessRequest:
    LDA $01
    CMP #PageCount
    BCC IdInRange
    LDA #PageCount
IdInRange:
    ASL A
    TAX
    LDA LookupTable, x
    STA $02
    LDA LookupTable+1, x
    STA $03
    RTS

; --- Response Logic ------------------------------------------------
; Each page is a 16 bit little endian length followed by that many
; packet bytes.  The length goes out first so the gateway knows how
; much to buffer.
SendResponse:
    LDY #$00
    LDA ($02), y
    STA $05                     ; length low
    INY
    LDA ($02), y
    STA $06                     ; length high

    LDA $05
    JSR SendByte
    LDA $06
    JSR SendByte

    LDA $05
    ORA $06
    BEQ RespDone                ; empty page, nothing to send

RespLoop:
    INY
    BNE NoWrap
    INC $03                     ; Y wrapped, step the base up a page
NoWrap:
    LDA ($02), y
    JSR SendByte

    LDA $05                     ; 16 bit decrement of $05:$06
    BNE SkipDecHigh
    DEC $06
SkipDecHigh:
    DEC $05

    LDA $05
    ORA $06
    BNE RespLoop
RespDone:
    RTS

; --- Byte transmit (NES -> gateway) --------------------------------
; Data bit onto OUT0, then read $4016 to pulse the clock.  The
; gateway samples on the falling edge, by which point the data has
; been stable for several microseconds and stays stable for the rest
; of the bit period.  Preserves Y for SendResponse; clobbers A and X.
SendByte:
    STA $04
    LDX #$08
SendBitLoop:
    LDA #$00
    LSR $04
    BCC SendBitLow
    LDA #$01
SendBitLow:
    STA $4016                   ; data bit on OUT0
    LDA $4016                   ; the read pulses CLK
    JSR BitDelay
    DEX
    BNE SendBitLoop
    RTS

; --- Delays --------------------------------------------------------
; 7*n + 16 cycles including the JSR. Clobbers A only.
BitDelay:
    LDA #BIT_DELAY
    JMP DelayA
StrobeDelay:
    LDA #STROBE_DELAY
DelayA:
    SEC
    SBC #$01
    BNE DelayA
    RTS

; Unused vectors: the ROM never enables NMI or IRQ, but point them
; somewhere harmless in case of a soft reset.
IrqNmi:
    RTI

.include "../../build/data.asm"
.pad $FFFA, $00
.dw IrqNmi, Reset, IrqNmi
.pad $12000, $00
