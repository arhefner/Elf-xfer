; mr - receive one or more files over the serial port
;
; Usage: mr [-u|-b] [-v] [destination]
;
;   mr                receive every file the host sends, each under the
;                     name the host gives it, into the current directory
;   mr directory      the same, but into that directory
;   mr filename       save only the first file the host sends, under this
;                     name; any further files in the session are read and
;                     thrown away so that the session still ends cleanly
;
;   -u   use the UART (f_uread/f_utype) rather than the console
;   -b   use the bit-banged port (f_bread/f_btype) rather than the console
;   -v   report each file as it is received
;
; This is the companion to "max-xfr -s" on the host, and it works the way
; MR does in ELF-DOS. Every exchange after the $55/$AA handshake is a chunk:
; the host sends a two-byte big-endian length, we acknowledge it with $AA,
; the host sends that many bytes, and we acknowledge again -- but only once
; we have finished with them (the file is open, or the block is on disk), so
; the host can never get ahead of us. A length of zero has no data and gets
; a single acknowledgment; it ends the current file, or the whole session if
; it comes where a file's header was expected. A header chunk is the file's
; name, a zero byte, and its size as four bytes. The host finishes with 'x'.
;
; Nothing is printed between the handshake and that 'x' unless -v is given.
; By default the messages would go down the same wire as the transfer, and
; the host would take them for acknowledgments. Only use -v when the
; transfer is on a different port from the console.

#include opcodes.def
#include bios.inc
#include kernel.inc

            org   2000h
start:      br    main


            ; Build information

            ever

            db    'See github.com/arhefner/Elfos-mr for more info',0


            ; Main code starts here, check for options

main:       lda   ra                    ; move past any spaces
            smi   ' '
            lbz   main
            dec   ra                    ; move back to non-space character
            ldn   ra                    ; get byte
            lbz   nodest                ; jump if nothing more given

            mov   rf,ra                 ; an option is exactly -u, -b or -v
            lda   rf
            smi   '-'
            lbnz  dest
            lda   rf
            plo   r8                    ; save option letter
            lda   rf                    ; must be the end of the argument
            lbz   optend
            smi   ' '
            lbnz  dest
            lbr   optchk
optend:     dec   rf                    ; stay on the terminator

optchk:     glo   r8
            smi   'v'
            lbz   optv
            glo   r8
            smi   'u'
            lbz   optu
            glo   r8
            smi   'b'
            lbnz  dest                  ; anything else is a destination

            mov   r8,f_bread
            mov   r9,f_btype
            lbr   setio

optu:       mov   r8,f_uread
            mov   r9,f_utype

setio:      mov   rd,getbyte+1          ; point the transfer at that port
            ghi   r8
            str   rd
            inc   rd
            glo   r8
            str   rd
            mov   rd,readlp+1
            ghi   r8
            str   rd
            inc   rd
            glo   r8
            str   rd
            mov   rd,putbyte+1
            ghi   r9
            str   rd
            inc   rd
            glo   r9
            str   rd
            lbr   optnext

optv:       mov   rd,verbose
            ldi   1
            str   rd

optnext:    mov   ra,rf
            lbr   main


            ; A destination was given. Copy it and see if it is a directory.

dest:       mov   rf,destpath
dstcopy:    lda   ra                    ; copy up to first <= space
            str   rf
            inc   rf
            smi   33
            lbdf  dstcopy
            dec   rf
            ldi   0                     ; need proper termination
            str   rf
            dec   ra

dsttail:    lda   ra                    ; nothing else may follow it
            smi   ' '
            lbz   dsttail
            dec   ra
            ldn   ra
            lbnz  usage

            call  chkdir
            lbdf  onefile               ; jump if not a directory

            mov   rd,slash              ; put the slash back, names go after
            lda   rd
            phi   rf
            ldn   rd
            plo   rf
            mov   rd,nameat
            ghi   rf
            str   rd
            inc   rd
            glo   rf
            str   rd
            dec   rf
            ldi   '/'
            str   rf
            lbr   session

onefile:    mov   rd,single
            ldi   1
            str   rd
            lbr   session

nodest:     mov   rd,nameat             ; names go at the start of the path
            ldi   high destpath
            str   rd
            inc   rd
            ldi   low destpath
            str   rd


            ; Handshake: wait for $55 from the host and answer with $AA

session:    mov   rd,savere
            ghi   re                    ; save UART timing
            str   rd
            ani   0feh                  ; turn off echo
            phi   re

            call  getbyte
            xri   55h
            lbz   shake
            call  echoon
            call  o_inmsg
            db    'No response from host.',13,10,0
            ldi   1
            rtn                         ; return to Elf/OS

shake:      ldi   0aah
            call  putbyte


            ; Start of a file, or the end of the session

nextfile:   call  recvblk
            lbz   over                  ; no more files
            smi   2
            lbz   failed                ; chunk too big, we are out of step

            mov   r8,buf                ; the size follows the name
hdrscan:    lda   r8
            lbnz  hdrscan
            mov   rd,sizeat
            ghi   r8
            str   rd
            inc   rd
            glo   r8
            str   rd

            mov   rd,single
            ldn   rd
            lbz   usename
            mov   rd,used               ; only the first file is kept
            ldn   rd
            lbnz  extra
            ldi   1
            str   rd
            lbr   open

usename:    mov   rd,nameat             ; put the name the host sent on the
            lda   rd                    ;  end of the directory, if any
            phi   r9
            ldn   rd
            plo   r9
            mov   r8,buf
namecopy:   lda   r8
            str   r9
            inc   r9
            lbnz  namecopy

            ; Opening a directory for writing would truncate it, so make
            ; sure that the name is not one first.

open:       mov   rd,discard
            ldi   0
            str   rd
            call  chkdir
            lbnf  openerr
            mov   rf,destpath
            mov   rd,fildes             ; get file descriptor
            ldi   FF_CREATE | FF_TRUNC  ; file flags
            plo   r7
            call  o_open                ; attempt to open file
            lbnf  opened                ; jump if file opened

openerr:    mov   rd,discard            ; read this file but do not keep it
            ldi   1
            str   rd
            mov   rd,nerr
            ldn   rd
            adi   1
            str   rd
            mov   rd,verbose
            ldn   rd
            lbz   hdrack
            call  o_inmsg
            db    'Cannot create ',0
            mov   rf,destpath
            call  o_msg
            call  o_inmsg
            db    '.',13,10,0
            lbr   hdrack

extra:      mov   rd,discard
            ldi   1
            str   rd
            mov   rd,nskip
            ldn   rd
            adi   1
            str   rd
            smi   1                     ; only say so the first time
            lbnz  hdrack
            mov   rd,verbose
            ldn   rd
            lbz   hdrack
            call  o_inmsg
            db    'Ignoring additional file(s) sent by host.',13,10,0
            lbr   hdrack

opened:     mov   rd,verbose
            ldn   rd
            lbz   hdrack
            call  o_inmsg
            db    'Receiving ',0
            mov   rf,destpath
            call  o_msg
            call  o_inmsg
            db    ' (',0
            mov   rd,sizeat
            lda   rd
            phi   rf
            ldn   rd
            plo   rf
            call  prsize
            call  o_inmsg
            db    ' bytes)...',13,10,0

hdrack:     call  sendack               ; ready for the file's data


            ; Data for the current file, or the end of it

nextblk:    call  recvblk
            lbz   endfile
            smi   2
            lbz   failclose

            mov   rd,discard
            ldn   rd
            lbnz  blkack
            mov   rd,count              ; get count of bytes
            lda   rd
            phi   rc
            ldn   rd
            plo   rc
            mov   rf,buf                ; point to buffer
            mov   rd,fildes
            call  o_write
            lbdf  wrterr

blkack:     call  sendack               ; only now that the block is written
            lbr   nextblk

endfile:    mov   rd,discard
            ldn   rd
            lbnz  endack
            mov   rd,fildes
            call  o_close
            mov   rd,nok
            ldn   rd
            adi   1
            str   rd

endack:     call  sendack               ; only now that the file is closed
            lbr   nextfile

wrterr:     mov   rd,fildes             ; nothing can be done about this,
            call  o_close               ;  the host is left waiting
            mov   rd,verbose
            ldn   rd
            lbz   failed
            call  o_inmsg
            db    'Error writing file.',13,10,0
            lbr   failed

failclose:  mov   rd,discard
            ldn   rd
            lbnz  failed
            mov   rd,fildes
            call  o_close

failed:     mov   rd,result
            ldi   1
            str   rd
            lbr   summary

over:       call  sendack
            call  getbyte               ; host finishes with an 'x'
            xri   'x'
            lbnz  failed


            ; The wire is idle again, so it is safe to say how it went

summary:    call  echoon

            mov   rd,nok
            ldn   rd
            call  prbyte
            call  o_inmsg
            db    ' file(s) received.',13,10,0

            mov   rd,nerr
            ldn   rd
            lbz   sumskip
            call  prbyte
            call  o_inmsg
            db    ' file(s) failed.',13,10,0
            mov   rd,result
            ldi   1
            str   rd

sumskip:    mov   rd,nskip
            ldn   rd
            lbz   sumdone
            call  prbyte
            call  o_inmsg
            db    ' file(s) ignored.',13,10,0

sumdone:    mov   rd,result
            ldn   rd
            rtn                         ; return to Elf/OS

usage:      call  o_inmsg
            db    'Usage: mr [-u|-b] [-v] [destination]',13,10,0
            ldi   1
            rtn                         ; and return to os


            ; Restore previous echo setting

echoon:     mov   rd,savere
            ldn   rd
            phi   re
            rtn


            ; Test whether destpath names a directory, returning DF=0 if
            ; it does. The kernel only follows a path as far as its last
            ; slash, so one is added for the test and taken off again;
            ; slash is left pointing just past where it was.

chkdir:     mov   rf,destpath
cdend:      lda   rf
            lbnz  cdend
            dec   rf                    ; back to the terminator
            dec   rf
            ldn   rf                    ; see if already ends in a slash
            inc   rf
            smi   '/'
            lbz   cdslash
            ldi   '/'
            str   rf
            inc   rf
            ldi   0
            str   rf

cdslash:    mov   rd,slash
            ghi   rf
            str   rd
            inc   rd
            glo   rf
            str   rd
            mov   rf,destpath
            call  o_opendir

            mov   rd,slash              ; none of this changes DF
            lda   rd
            phi   rf
            ldn   rd
            plo   rf
            dec   rf
            ldi   0
            str   rf
            rtn


            ; Print in decimal the byte in D, or the 32-bit value at RF

prbyte:     plo   r8
            mov   rd,num
            ldi   0
            str   rd
            inc   rd
            str   rd
            inc   rd
            str   rd
            inc   rd
            glo   r8
            str   rd
            lbr   prnum

prsize:     mov   rd,num
            lda   rf
            str   rd
            inc   rd
            lda   rf
            str   rd
            inc   rd
            lda   rf
            str   rd
            inc   rd
            lda   rf
            str   rd

prnum:      mov   r9,numbuf+10          ; digits are produced last first
            ldi   0
            str   r9

prdigit:    ldi   32                    ; divide by ten, a bit at a time,
            plo   r8                    ;  with the remainder in r8.1
            ldi   0
            phi   r8

prbit:      mov   rd,num+3
            ldn   rd
            shl
            str   rd
            dec   rd
            ldn   rd
            shlc
            str   rd
            dec   rd
            ldn   rd
            shlc
            str   rd
            dec   rd
            ldn   rd
            shlc
            str   rd
            ghi   r8
            shlc
            phi   r8
            smi   10
            lbnf  prnext
            phi   r8
            mov   rd,num+3
            ldn   rd
            ori   1
            str   rd

prnext:     dec   r8
            glo   r8
            lbnz  prbit

            dec   r9
            ghi   r8
            adi   '0'
            str   r9

            mov   rd,num                ; done when nothing is left
            lda   rd
            lbnz  prdigit
            lda   rd
            lbnz  prdigit
            lda   rd
            lbnz  prdigit
            ldn   rd
            lbnz  prdigit

            mov   rf,r9
            call  o_msg
            rtn


            ; Every byte of the transfer goes through these, apart from
            ; the data itself. The addresses are changed by -u and -b.

getbyte:    lbr   f_read

sendack:    ldi   0aah
putbyte:    lbr   f_type


            .align page

            ; Receive one chunk into buf. Returns D=0 if its length was
            ; zero, in which case nothing has been acknowledged; D=1 if
            ; the data is in buf and its length in count, with the length
            ; acknowledged but not the data; or D=2 if the length was more
            ; than buf will hold. The acknowledgments that are left to the
            ; caller are the ones that must wait until it is ready for
            ; whatever the host sends next.

recvblk:    call  getbyte
            phi   r7
            call  getbyte
            plo   r7

            mov   rd,count
            ghi   r7
            str   rd
            inc   rd
            glo   r7
            str   rd

            ghi   r7
            bnz   rbsize
            glo   r7
            bz    rbzero

rbsize:     ghi   r7                    ; no more than 512
            smi   2
            bnf   rbread
            bnz   rbover
            glo   r7
            bnz   rbover

rbread:     call  sendack
            mov   rf,buf
            mov   rc,r7
            dec   rc                    ; adjust loop count

readlp:     call  f_read

            str   rf
            inc   rf

            untl  rc,readlp

            ldi   0                     ; terminate, in case it is a name
            str   rf
            ldi   1
            rtn

rbzero:     ldi   0
            rtn

rbover:     ldi   2
            rtn


verbose:    db    0                     ; -v given
single:     db    0                     ; destination is a single file
used:       db    0                     ;  and it has been received
discard:    db    0                     ; current file is not being kept
result:     db    0                     ; value to return to Elf/OS
nok:        db    0                     ; files received
nerr:       db    0                     ; files that could not be created
nskip:      db    0                     ; files ignored
savere:     db    0
count:      dw    0                     ; length of the chunk in buf
nameat:     dw    0                     ; where a name goes in destpath
slash:      dw    0                     ; set by chkdir
sizeat:     dw    0                     ; where the size is in a header
num:        db    0,0,0,0

            ; File descriptor for the file being received

fildes:     db    0,0,0,0
            dw    dta
            db    0,0
            db    0
            db    0,0,0,0
            dw    0,0
            db    0,0,0,0

            db    0                     ; chkdir looks one before destpath

destpath:   ds    800
numbuf:     ds    11
buf:        ds    520                   ; a chunk, a terminator and a size
dta:        ds    512

            end   start
