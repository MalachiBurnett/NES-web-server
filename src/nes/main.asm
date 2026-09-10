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
;  $4017 is never read, so port 2 stays free.  Note that an empty
;  port floats and reads back as ones.  To move to port 2, change
;  the reads of $4016 below to $4017 - the strobe writes stay on
;  $4016, which is one latch line shared by both ports - and
;  physically move the gateway.
;
;  The cable can be plugged in or pulled at any time.  Neither end
;  trusts the other to have been there a moment ago: every poll is a
;  fresh start, and a frame that does not check out is thrown away.
;  See docs/protocol.md for the wire format and
;  docs/status-colours.md for the on-screen codes.
; ===================================================================

; --- iNES Header (NROM-256, 32K PRG, 8K CHR, vertical mirroring) ---
.db "NES",$1a, $02, $01, $01, $00, 0,0,0,0,0,0,0,0

; --- Link timing ---------------------------------------------------
; Delay routines burn 7*n + 16 cycles.  One cycle is 0.559us on NTSC
; and 0.601us on PAL; the protocol has no minimum speed, so either
; works.  BIT_DELAY sets the gap between clock pulses: the gateway
; samples from an interrupt handler, so this has to comfortably
; exceed its worst case interrupt latency.  4 gives ~37us per bit.
BIT_DELAY    = 4
STROBE_DELAY = 24               ; ~103us from the strobe to the first read

; --- Status colours ------------------------------------------------
; Written to $3F00.  CHR is blank so every tile draws as colour 0,
; which makes the whole screen the backdrop colour.  Resting colours
; pulse between $0x and $1x to show the poll loop is still turning -
; a frozen screen means the ROM has hung.
COL_BOOT     = $01              ; dark blue  - init done, loop not started
COL_IDLE     = $0C              ; cyan pulse - idle, nothing served yet
COL_OK       = $0A              ; green pulse- last request served a page
COL_404      = $07              ; amber pulse- last request was unknown id
COL_JUNK     = $06              ; red pulse  - port reads junk: no gateway
COL_SEND     = $28              ; yellow     - streaming a response now

; --- Zero page -----------------------------------------------------
;   $00      page id received from the gateway
;   $01      its complement, as received
;   $02-$03  pointer to the page being sent
;   $04      SendByte shift register
;   $05-$06  bytes of packet left to send
;   $07      heartbeat counter
;   $08      status colour being shown, dark shade
;   $09-$0A  running checksum: sum1, sum2
;   $0B      result colour of the last request, shown again once junk clears
;   $0C      heartbeat phase, $00 or $10
;   $0D      scratch

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

    LDA #COL_BOOT               ; visible only if we hang below
    JSR SetBG
    LDA #$1E
    STA $2001

    LDA #$00
    STA $07                     ; heartbeat counter
    STA $0C                     ; heartbeat phase
    LDA #COL_IDLE
    STA $08
    STA $0B
    JSR SetBG

ServerLoop:
    JSR PollRequest             ; carry set = a request that checked out
    BCS GotRequest

    INC $07                     ; no request: pulse the backdrop every
    BNE ServerLoop              ; 256 polls (~0.6s) to prove we are alive
    LDA $0C
    EOR #$10
    STA $0C
    ORA $08
    JSR SetBG
    JMP ServerLoop

GotRequest:
    JSR ProcessRequest          ; also sets the result colour
    LDA #COL_SEND
    JSR SetBG
    JSR SendResponse
    LDA $08                     ; settle on the result colour
    ORA $0C
    JSR SetBG
    JMP ServerLoop

; --- Poll (gateway -> NES) -----------------------------------------
; Hold OUT0 high on a quiet line for ~2ms, drop it, then clock in a
; 25 bit frame, LSB first:
;
;     8 zeros | ready | id (8 bits) | ~id (8 bits)
;
; The quiet hold is how the gateway tells a poll from a data bit: a
; data edge always has a clock pulse within ~100us of it.  The zeros
; and the complement are how this end tells a gateway from junk: a
; floating port reads as ones, and a gateway plugged in partway
; through a frame hands over half of one.  Neither passes both.
;
; An idle gateway holds D0 at 0 (the wire high - the console inverts
; it), so an unanswered poll reads all zeros.  The zeros up front also
; keep other software safe: a flash cart menu reads 8 bits per strobe
; and so never sees a button, even if a request is waiting.
;
; Carry set: a request, id in $00.  Carry clear: nothing, or junk.
PollRequest:
    LDA #$01
    STA $4016                   ; OUT0 high, and keep the line quiet
    JSR HoldDelay
    LDA #$00
    STA $4016                   ; OUT0 low: the gateway arms here
    JSR StrobeDelay

    LDX #$08
PollPreamble:
    LDA $4016
    LSR A
    BCS PollJunk                ; a 1 here is not a gateway in step
    JSR BitDelay
    DEX
    BNE PollPreamble

    LDA $4016                   ; ready flag
    LSR A
    BCS PollReady
    LDA $0B                     ; a clean idle poll: the line is healthy,
    STA $08                     ; so drop any junk colour
    CLC
    RTS

PollReady:
    LDX #$10                    ; id then ~id, 16 bits LSB first
PollIdLoop:
    JSR BitDelay                ; let the gateway present the bit
    LDA $4016
    LSR A
    ROR $01                     ; shift in from the top of $01:$00
    ROR $00
    DEX
    BNE PollIdLoop

    LDA $00
    EOR $01
    CMP #$FF                    ; id and complement must differ in every bit
    BNE PollJunk
    SEC
    RTS

PollJunk:
    LDA #COL_JUNK
    STA $08
    CLC
    RTS

; --- Lookup Logic --------------------------------------------------
; PageCount comes from the generated data.asm.  The table holds
; PageCount real pages followed by the 404 page, so any out of range
; id lands on the 404 entry instead of running off the end.
ProcessRequest:
    LDA $00
    CMP #PageCount
    BCS IdUnknown
    LDX #COL_OK
    JMP IdResolved
IdUnknown:
    LDX #COL_404
    LDA #PageCount
IdResolved:
    STX $08
    STX $0B
    ASL A
    TAX
    LDA LookupTable, x
    STA $02
    LDA LookupTable+1, x
    STA $03
    RTS

; --- Response Logic ------------------------------------------------
;     [id] [length lo] [length hi] [ ... length bytes ... ] [sum1] [sum2]
;
; The id is echoed so the gateway knows the response answers its
; request, and the trailer is a checksum over everything before it
; (sum1 += byte, sum2 += sum1).  Between them, a response the gateway
; joined partway through, or lost bits of, is refused rather than
; taken for a page.  Each page in the ROM is stored as its 16 bit
; length followed by the packet.
SendResponse:
    LDA #$00
    STA $09
    STA $0A
    LDA $00
    JSR SendByte                ; echo the id we were asked for

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
    BEQ RespTrailer             ; empty page, nothing to send

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

RespTrailer:
    LDA $0A                     ; sending sum1 moves sum2, so keep a copy
    STA $0D
    LDA $09
    JSR SendByte
    LDA $0D
    JMP SendByte

; --- Byte transmit (NES -> gateway) --------------------------------
; Folds the byte into the checksum, then for each bit: data onto
; OUT0, then read $4016 to pulse the clock.  The gateway samples on
; the falling edge, by which point the data has been stable for
; several microseconds and stays stable for the rest of the bit
; period.  Preserves Y for SendResponse; clobbers A and X.
SendByte:
    STA $04
    CLC
    ADC $09                     ; sum1 += byte
    STA $09
    CLC
    ADC $0A                     ; sum2 += sum1
    STA $0A
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

; --- Status colour -------------------------------------------------
; A = palette index, written to the backdrop at $3F00.  Rendering is
; left on: the screen is a single flat colour, so the scroll glitch a
; mid-frame $2006 write causes is not visible.
; Clobbers X.  Preserves A and Y.
SetBG:
    LDX #$3F
    STX $2006
    LDX #$00
    STX $2006
    STA $2007
    STX $2006                   ; leave the address latch somewhere safe
    STX $2006
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

; ~2ms, two full runs of DelayA.  Long enough that no data bit can be
; mistaken for the quiet line in front of a poll.
HoldDelay:
    LDA #$FF
    JSR DelayA
    LDA #$FF
    JMP DelayA

; Unused vectors: the ROM never enables NMI or IRQ, but point them
; somewhere harmless in case of a soft reset.
IrqNmi:
    RTI

.include "../../build/data.asm"
.pad $FFFA, $00
.dw IrqNmi, Reset, IrqNmi
.pad $12000, $00
