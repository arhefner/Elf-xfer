#include opcodes.def
#include bios.inc

            org   0100h

            ; Initialize SCRT with initial minimal stack
start:      mov   r2,00ffh
            mov   r6,main
            lbr   f_initcall

            ; Move stack to the end of ram
main:       call  f_freemem
            mov   r2,rf

xfer:       mov   ra,300h               ; initialize address
            mov   rc,11043
            call  f_inmsg
            db    "Start receive...",0
            call  savebin
            bnf   success
            call  f_inmsg
            db    "error",13,10,0
            br    exit

success:    call  f_inmsg
            db    "done",13,10,0

exit:       mark
            sep   r1

savebin:    ghi   r8
            stxd
            ghi   rb
            stxd
            glo   rb
            stxd

            ghi   re
            phi   r8
            ani   0feh
            phi   re

            call  f_read
            xri   0aah
            bnz   error

            ldi   55h
            call  f_type

next:       mov   rb,rc
            sub16 rb,512
            bpz   send512

            mov   rb,rc
            br    sendblk

send512:    mov   rb,512

sendblk:    sub16 rc,rb

            ldi   01h                   ; send 'receive block' command
            call  f_type

            call  f_read                ; check echo
            xri   01h
            bnz   error

            ghi   rb                    ; send high byte of size
            call  f_type

            call  f_read                ; check echo
            str   r2
            ghi   rb
            xor
            bnz   error

            glo   rb                    ; send low byte of size
            call  f_type

            call  f_read                ; check echo
            str   r2
            glo   rb
            xor
            bnz   error

            ghi   ra                    ; send high byte of address
            call  f_type

            call  f_read                ; check echo
            str   r2
            ghi   ra
            xor
            bnz   error

            glo   ra                    ; send low byte of address
            call  f_type

            call  f_read                ; check echo
            str   r2
            glo   ra
            xor
            bnz   error

            dec   rb
sendloop:   lda   ra                    ; send block
            call  f_type
            untl  rb,sendloop

            call  f_read                ; check for ack
            xri   0aah
            bnz   error

            brnz  rc,next

            ldi   00h                   ; send end command
            call  f_type

over:       call  f_read
            xri   'x'
            bnz   error

done:       clc
            lskp
error:      stc

            ghi   r8
            phi   re

            irx
            ldxa
            plo   rb
            ldxa
            phi   rb
            ldx
            phi   r8

            rtn

            end   start
