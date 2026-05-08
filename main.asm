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
    LDA #<Page2
    STA $02
    LDA #>Page2
    STA $03
LookupDone:
    RTS

; --- Response Logic ---
SendResponse:
    LDY #$00
    LDA ($02), y
    STA $05             ; Length Low
    INY
    LDA ($02), y
    STA $06             ; Length High
    
    ; Send length bytes to Arduino first
    LDA $05
    JSR SendByte
    LDA $06
    JSR SendByte

    LDA $05
    ORA $06
    BEQ RespDone        ; If length is 0, done

RespLoop:
    ; Increment pointer (02, 03) + index Y
    INY
    BNE NoWrap
    INC $03
NoWrap:
    LDA ($02), y
    JSR SendByte
    
    ; 16-bit Decrement: $05:$06
    LDA $05
    BNE SkipDecHigh
    DEC $06
SkipDecHigh:
    DEC $05
    
    LDA $05
    ORA $06
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
    .dw Page0, Page1
    .dw Page2, 0
    .dw 0, 0
    .dw 0, 0
    .dw 0, 0

Page0: .db $8b, $00, $32, $0b, $22, $db, $61, $9e, $0b, $c8, $a3, $59, $e6, $97, $39, $7a, $ca, $59, $95, $3a, $29, $41, $a8, $5c, $85, $a2, $40, $58, $97, $52, $93, $63, $a7, $5b, $c5, $a5, $70, $98, $dd, $02, $ef, $6e, $13, $09, $75, $34, $90, $cb, $53, $8a, $bd, $b2, $7c, $91, $00, $31, $a2, $ef, $98, $61, $87, $9d, $c6, $f5, $be, $bb, $b7, $52, $f6, $df, $d7, $2f, $b8, $b2, $bb, $8d, $eb, $79, $58, $61, $87, $9a, $58, $23, $cc, $67, $fc, $a8, $9a, $ee, $dd, $4b, $db, $7f, $5c, $be, $e2, $ca, $c6, $7e, $72, $fe, $7b, $76, $b6, $c4, $d3, $b7, $22, $44, $a7, $8d, $69, $8b, $4f, $58, $26, $f8, $c8, $f8, $cf, $3b, $9b, $4c, $6c, $d6, $df, $14, $ef, $ad, $cc, $2b, $97, $95, $a5, $82, $3c, $ac, $68, $bb, $e0 ; index.html
Page1: .db $7a, $00, $32, $0b, $22, $db, $61, $9e, $0b, $c8, $a3, $59, $e6, $97, $39, $7a, $ca, $59, $95, $3a, $29, $41, $a8, $5c, $85, $a2, $40, $58, $97, $52, $93, $63, $a7, $5b, $c5, $a5, $70, $98, $dd, $02, $ef, $6e, $13, $09, $75, $34, $90, $cb, $53, $8a, $bd, $b2, $7c, $79, $00, $31, $a2, $ef, $98, $61, $87, $9d, $c6, $f5, $bd, $e5, $2e, $ad, $2b, $b8, $de, $b7, $95, $86, $18, $79, $a5, $82, $3c, $c6, $7e, $f2, $97, $56, $cf, $2b, $15, $8c, $fc, $e5, $f5, $93, $4a, $b1, $d0, $9e, $0d, $87, $c2, $53, $da, $9c, $72, $78, $63, $62, $6f, $a1, $30, $37, $c3, $80, $df, $6e, $49, $5c, $bc, $ad, $2c, $11, $e5, $63, $45, $df ; about.html
Page2: .db $58, $00, $32, $0b, $22, $db, $61, $9e, $0b, $c8, $a3, $59, $e6, $97, $39, $7a, $ca, $59, $95, $3a, $29, $41, $a8, $5c, $85, $a2, $40, $58, $97, $52, $93, $63, $a7, $5b, $c5, $a5, $70, $98, $dd, $02, $ef, $6e, $13, $09, $75, $34, $90, $cb, $53, $8a, $bd, $b2, $7c, $3b, $00, $31, $a2, $ef, $9a, $58, $23, $cc, $67, $e9, $79, $09, $4a, $c6, $7e, $72, $f7, $a3, $49, $69, $af, $7b, $29, $17, $57, $10, $2b, $97, $95, $a5, $82, $3c, $ac, $68, $bb, $e0 ; 404.html
.pad $FFFA, $00
.dw 0, Reset, 0
.pad $12000, $00