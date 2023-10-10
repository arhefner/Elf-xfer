#include opcodes.def
#include bios.inc
#include kernel.inc

            org   2000h
start:      br    main


            ; Build information

            ever

            db    'See github.com/arhefner/Elfos-mr for more info',0


            ; Main code starts here, check provided argument

main:       lda   ra                    ; move past any spaces
            smi   ' '
            bz    main
            dec   ra                    ; move back to non-space character
            ldn   ra                    ; get byte
            bnz   arg                   ; jump if argument given
            mov   rf,usage
            call  o_msg                 ; otherwise display usage message
            ldi   01h
            rtn                         ; and return to os

arg:        mov   rf, ra                ; copy argument address to rf

loop1:      lda   ra                    ; look for first less <= space
            smi   33
            bdf   loop1
            dec   ra                    ; backup to char
            ldi   0                     ; need proper termination
            str   ra
            mov   rd,fildes             ; get file descriptor
            ldi   FF_CREATE | FF_TRUNC  ; file flags
            plo   r7
            call  o_open                ; attempt to open file
            bnf   opened                ; jump if file opened
            mov   rf,filerr             ; point to error message
            call  o_msg                 ; display error message
            ldi   02h
            rtn                         ; return to Elf/OS

opened:     mov   rc,512                ; requested buffer size
            ldi   00h
            plo   r7                    ; allocation flags
            call  o_alloc
            lbnf  xfer
            mov   rf,allocerr           ; point to error message
            call  o_msg                 ; display error message
            ldi   03h
            plo   r8
            lbr   done1                 ; return to Elf/OS

            .align page

xfer:       mov   ra,rf                 ; save buffer address in ra

loadbin:    ghi   re                    ; save UART timing
            phi   r8
            ani   0feh                  ; turn off echo
            phi   re

            call  f_read
            xri   55h
            bz    shake
            mov   rf,hskerr
            call  o_msg
            ldi   04h
            plo   r8
            br    done
 
shake:      ldi   0aah
            call  f_type

next:       call  f_read
            bz    over
            call  f_type
            
            smi   1
            bz    cmd01
            mov   rf,cmderr
            call  o_msg
            ldi   05h
            plo   r8
            br    done

cmd01:      call  f_read
            phi   r7
            call  f_type

            call  f_read
            plo   r7
            call  f_type

            call  f_read
            call  f_type

            call  f_read
            plo   re
            mov   rf,ra                 ; ignore address, always read
                                        ; into pre-allocated buffer
            mov   rc,r7
            dec   rc                    ; adjust loop count
            glo   re
            call  f_type

readlp:     call  f_read

            str   rf
            inc   rf

            untl  rc,readlp

            mov   rc,r7                 ; get count of bytes
            mov   rf,ra                 ; point to buffer
            call  o_write
            bnf   ack
            mov   rf,wrterr
            call  o_msg
            ldi   06h
            plo   r8
            br    done

ack:        ldi   0aah
            call  f_type

            br    next

over:       call  f_read
            xri   'x'
            bz    done
            mov   rf,cmderr
            call  o_msg
            ldi   07h
            plo   r8
            br    done

            ldi   00h                   ; return success
            plo   r8

done:       mov   rf,ra
            call  o_dealloc

done1:      call  o_close

            ghi   r8                    ; restore previous echo setting
            phi   re

            glo   r8
            rtn                         ; return to Elf/OS

usage:      db    'Usage: mr filename',13,10,0
filerr:     db    'File not found.',13,10,0
allocerr:   db    'Memory allocation error.',13,10,0
hskerr:     db    'Invalid handshake.',13,10,0
cmderr:     db    'Unrecognized command.',13,10,0
wrterr:     db    'Error writing file.',13,10,0

            ; File descriptor for loading image data

fildes:     db    0,0,0,0
            dw    dta
            db    0,0
            db    0
            db    0,0,0,0
            dw    0,0
            db    0,0,0,0

dta:        ds    512

            end   start
