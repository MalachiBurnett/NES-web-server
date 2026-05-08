; --- iNES Header ---
.db "NES",$1a, $02, $01, $01, $00, 0,0,0,0,0,0,0,0

.org $8000

Reset:
    SEI
    CLD
    LDX #$40
    STX $4017
    LDX #$FF
    TXS
    
v1:
    BIT $2002
    BPL v1
v2:
    BIT $2002
    BPL v2

    ; Cyan Screen
    LDA #$3F
    STA $2006
    LDA #$00
    STA $2006
    LDA #$1C
    STA $2007
    LDA #$00
    STA $2001
    LDA #$1E
    STA $2001

ServerLoop:
    JSR ReceiveRequest  
    STA $01             
    JSR ProcessRequest  
    JSR SendResponse    
    JMP ServerLoop

; --- Optimized Receive (Arduino -> NES) ---
ReceiveRequest:
    LDA #$01
    STA $4016
    STA $00FE
    LDA #$00
    STA $4016
    STA $00FE ; Strobe
    
    LDA $4016
    LSR A
    ROR $00
    LDA $4016
    LSR A
    ROR $00
    LDA $4016
    LSR A
    ROR $00
    LDA $4016
    LSR A
    ROR $00
    LDA $4016
    LSR A
    ROR $00
    LDA $4016
    LSR A
    ROR $00
    LDA $4016
    LSR A
    ROR $00
    LDA $4016
    LSR A
    ROR $00
    LDA $00
    RTS

; --- Lookup Logic ---
ProcessRequest:
    LDA $01
    ASL A
    TAX
    LDA LookupTable, x
    STA $02
    LDA LookupTable+1, x
    STA $03
    LDA $02
    ORA $03
    BNE LookupDone
    LDA #<String404
    STA $02
    LDA #>String404
    STA $03
LookupDone:
    RTS

; --- Response Logic ---
SendResponse:
    LDY #$00
    LDA ($02), y
    BEQ RespDone
    TAX
    STX $05
RespLoop:
    INY
    LDA ($02), y
    JSR SendByte
    DEC $05
    BNE RespLoop
RespDone:
    RTS

; --- TURBO Serial Protocol (Unrolled) ---
SendByte:
    STA $04
    ; Bit 0
    LDA #$00
    LSR $04
    BCC s0
    LDA #$01
s0: STA $4016
    STA $00FE
    ORA #$02
    STA $4016
    STA $00FE
    AND #$01
    STA $4016
    STA $00FE
    ; Bit 1
    LDA #$00
    LSR $04
    BCC s1
    LDA #$01
s1: STA $4016
    STA $00FE
    ORA #$02
    STA $4016
    STA $00FE
    AND #$01
    STA $4016
    STA $00FE
    ; Bit 2
    LDA #$00
    LSR $04
    BCC s2
    LDA #$01
s2: STA $4016
    STA $00FE
    ORA #$02
    STA $4016
    STA $00FE
    AND #$01
    STA $4016
    STA $00FE
    ; Bit 3
    LDA #$00
    LSR $04
    BCC s3
    LDA #$01
s3: STA $4016
    STA $00FE
    ORA #$02
    STA $4016
    STA $00FE
    AND #$01
    STA $4016
    STA $00FE
    ; Bit 4
    LDA #$00
    LSR $04
    BCC s4
    LDA #$01
s4: STA $4016
    STA $00FE
    ORA #$02
    STA $4016
    STA $00FE
    AND #$01
    STA $4016
    STA $00FE
    ; Bit 5
    LDA #$00
    LSR $04
    BCC s5
    LDA #$01
s5: STA $4016
    STA $00FE
    ORA #$02
    STA $4016
    STA $00FE
    AND #$01
    STA $4016
    STA $00FE
    ; Bit 6
    LDA #$00
    LSR $04
    BCC s6
    LDA #$01
s6: STA $4016
    STA $00FE
    ORA #$02
    STA $4016
    STA $00FE
    AND #$01
    STA $4016
    STA $00FE
    ; Bit 7
    LDA #$00
    LSR $04
    BCC s7
    LDA #$01
s7: STA $4016
    STA $00FE
    ORA #$02
    STA $4016
    STA $00FE
    AND #$01
    STA $4016
    STA $00FE
    RTS

; --- Data ---
LookupTable:
    .dw StringWelcome, StringAbout
    .dw 0,0,0,0,0,0,0,0

StringWelcome: .db 12, "HELLO WORLD!"
StringAbout:   .db 15, "NES WEB SERVER"
String404:     .db 13, "404 NOT FOUND"

.pad $FFFA, $00
.dw 0, Reset, 0
.pad $12000, $00