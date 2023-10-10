#include opcodes.def
#include bios.inc
#include kernel.inc

            org   2000h
start:      br    main


            ; Build information

            ever

            db    'See github.com/arhefner/Elfos-ms for more info',0


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
            ldi   FF_READ               ; file flags
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
            lbr   done1

            .align page

xfer:       mov   ra,rf                 ; save buffer address in ra
            mov   rb,0                  ; initialize address

savebin:    ghi   re                    ; save UART timing
            phi   r8
            ani   0feh                  ; turn off echo
            phi   re

            call  f_read
            xri   0aah
            bz    shake
            mov   rf,hskerr
            call  o_msg
            ldi   04h
            plo   r8
            br    done
 
shake:      ldi   055h
            call  f_type

next:       mov   rf,ra
            mov   rc,512
            call  o_read                ; read next block from file
            bnf   chksiz
            mov   rf,readerr
            call  o_msg
            ldi   04h
            plo   r8
            br    done

chksiz:     brnz  rc,sendblk

            ldi   00h                   ; send eof indicator
            call  f_type

over:       call  f_read
            xri   'x'
            bz    success
            mov   rf,cmderr
            call  o_msg
            ldi   05h
            plo   r8
            br    done

success:    ldi   00h                   ; return success code
            plo   r8
            br    done

sendblk:    ldi   01h                   ; send 'receive block' command
            call  f_type

            call  f_read                ; check echo
            xri   01h
            bnz   error

            ghi   rc                    ; send high byte of size
            call  f_type

            call  f_read                ; check echo
            str   r2
            ghi   rc
            xor
            bnz   error

            glo   rc                    ; send low byte of size
            call  f_type

            call  f_read                ; check echo
            str   r2
            glo   rc
            xor
            bnz   error

            ghi   rb                    ; send high byte of address
            call  f_type

            call  f_read                ; check echo
            str   r2
            ghi   rb
            xor
            bnz   error

            glo   rb                    ; send low byte of address
            call  f_type

            call  f_read                ; check echo
            str   r2
            glo   rb
            xor
            bnz   error

            add16 rb,rc                 ; update address for next block

            mov   rf,ra
            dec   rc
sendloop:   lda   rf                    ; send block
            call  f_type
            untl  rc,sendloop

            call  f_read                ; check ack
            xri   0aah
            bnz   error

            br    next

error:      mov   rf,senderr
            call  o_msg
            ldi   06h
            plo   r8

done:       mov   rf,ra
            call  o_dealloc

done1:      call  o_close

            ghi   r8                    ; restore previous echo setting
            phi   re

            glo   r8
            rtn                         ; return to Elf/OS

usage:      db    'Usage: ms filename',13,10,0
filerr:     db    'File not found.',13,10,0
allocerr:   db    'Memory allocation error.',13,10,0
hskerr:     db    'Invalid handshake.',13,10,0
readerr:    db    'Error reading file.',13,10,0
senderr:    db    'Send error.',13,10,0
cmderr:     db    'Unrecognized command.',13,10,0

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
