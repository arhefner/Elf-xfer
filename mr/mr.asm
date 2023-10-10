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

            call  f_inmsg
            db    "Start send...",0
            call  loadbin
            bnf   success
            call  f_inmsg
            db    "error!",13,10,0
            br    exit

success:    call  f_inmsg
            db    "done",13,10,0

exit:       mark
            sep   r1

loadbin:    ghi   r8
            stxd
            ghi   ra
            stxd
            glo   ra
            stxd
            ghi   rc
            stxd
            glo   rc
            stxd

            ghi   re
            phi   r8
            ani   0feh
            phi   re

            call  f_read
            xri   55h
            bnz   error
            ldi   0aah
            call  f_type

next:       call  f_read
            bz    over
            call  f_type
            
            smi   1
            bnz   error

            call  f_read
            phi   rc
            call  f_type

            call  f_read
            plo   rc
            dec   rc
            call  f_type

            call  f_read
            phi   ra
            call  f_type

            call  f_read
            plo   ra
            call  f_type

readlp:     call  f_read

            str   ra
            inc   ra

            untl  rc,readlp

ack:        ldi   0aah
            call  f_type

            br    next

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
            plo   rc
            ldxa
            phi   rc
            ldxa
            plo   ra
            ldxa
            phi   ra
            ldx
            phi   r8

            rtn

            end   start
