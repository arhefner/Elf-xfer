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
            mov   ra,0
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
            glo   r8
            stxd
            ghi   r9
            stxd
            glo   r9
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
            bnz   lberror
            ldi   0aah
            call  f_type

lbnext:     call  f_read
            bz    lbover
            call  f_type

            smi   1
            bnz   lberror

            call  f_read
            phi   rc
            call  f_type

            call  f_read
            plo   rc
            dec   rc
            call  f_type

            call  f_read
            phi   r9
            call  f_type

            call  f_read
            plo   r9
            plo   r8
            add16 r9,ra
            glo   r8
            call  f_type

readlp:     call  f_read

            str   r9
            inc   r9

            untl  rc,readlp

ack:        ldi   0aah
            call  f_type

            br    lbnext

lbover:     call  f_read
            xri   'x'
            bnz   lberror

lbdone:     clc
            lskp
lberror:    stc

            ghi   r8
            phi   re

            irx
            ldxa
            plo   rc
            ldxa
            phi   rc
            ldxa
            plo   r9
            ldxa
            phi   r9
            ldxa
            plo   r8
            ldx
            phi   r8

            rtn

            end   start
