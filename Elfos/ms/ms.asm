; ms - send one or more files over the serial port
;
; Usage: ms [-u|-b] [-v] filename [filename...]
;
;   -u   use the UART (f_uread/f_utype) rather than the console
;   -b   use the bit-banged port (f_bread/f_btype) rather than the console
;   -v   report each file as it is sent
;
; Each filename may contain the wildcards * and ? in its last part, and
; then stands for every matching file in that directory, other than hidden
; files and directories. A pattern that matches nothing is tried as an
; ordinary name. Only the last part of each name is sent to the host.
;
; This is the companion to "max-xfr -r" on the host, and it works the way
; MS does in ELF-DOS. Every exchange after the $AA/$55 handshake is a chunk:
; we send a two-byte big-endian length, wait for $AA, send that many bytes,
; and wait for $AA again, which the host sends only once it has finished
; with them. A length of zero has no data and gets a single acknowledgment;
; it ends the current file, or the whole session if it comes where a file's
; header was expected. A header chunk is the file's name, a zero byte, and
; its size as four bytes. The host finishes with 'x'.
;
; A file that cannot be opened is counted and passed over. An error on the
; wire, or reading a file once its header has gone, ends the session, as
; there is no way to get back in step with the host.
;
; Nothing is printed between the handshake and that 'x' unless -v is given.
; By default the messages would go down the same wire as the transfer, and
; the host would take them for part of it. Only use -v when the transfer
; is on a different port from the console.

#include opcodes.def
#include bios.inc
#include kernel.inc

            org   2000h
start:      br    main


            ; Build information

            ever

            db    'See github.com/arhefner/Elfos-ms for more info',0


            ; Main code starts here, check for options

main:       lda   ra                    ; move past any spaces
            smi   ' '
            lbz   main
            dec   ra                    ; move back to non-space character
            ldn   ra                    ; get byte
            lbz   usage                 ; jump if no filename given

            mov   rf,ra                 ; an option is exactly -u, -b or -v
            lda   rf
            smi   '-'
            lbnz  session
            lda   rf
            plo   r8                    ; save option letter
            lda   rf                    ; must be the end of the argument
            lbz   optend
            smi   ' '
            lbnz  session
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
            lbnz  session               ; anything else is a filename

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
            mov   rd,putbyte+1
            ghi   r9
            str   rd
            inc   rd
            glo   r9
            str   rd
            mov   rd,sendlp+2
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


session:    mov   rd,argp               ; ra is at the first filename
            ghi   ra
            str   rd
            inc   rd
            glo   ra
            str   rd

            mov   rd,savere
            ghi   re                    ; save UART timing
            str   rd
            ani   0feh                  ; turn off echo
            phi   re


            ; Take the next filename from the command line

nextarg:    mov   rd,abort
            ldn   rd
            lbnz  failed

            mov   rd,argp
            lda   rd
            phi   ra
            ldn   rd
            plo   ra
argskip:    lda   ra                    ; move past any spaces
            smi   ' '
            lbz   argskip
            dec   ra
            ldn   ra
            lbz   over                  ; jump if no more filenames

            mov   rd,arg
            ghi   ra
            str   rd
            inc   rd
            glo   ra
            str   rd

            ldi   0
            plo   r7                    ; set if there is a wildcard
            mov   r8,ra                 ; start of the last part of the name
argscan:    lda   ra                    ; look for first <= space
            plo   re
            smi   33
            lbnf  argend
            glo   re
            smi   '*'
            lbz   argwild
            glo   re
            smi   '?'
            lbz   argwild
            glo   re
            smi   '/'
            lbnz  argscan
            mov   r8,ra
            lbr   argscan
argwild:    ldi   1
            plo   r7
            lbr   argscan

argend:     dec   ra                    ; backup to char
            ldn   ra
            lbz   argsave
            ldi   0                     ; need proper termination
            str   ra
            inc   ra
argsave:    mov   rd,argp
            ghi   ra
            str   rd
            inc   rd
            glo   ra
            str   rd

            mov   rd,pattern
            ghi   r8
            str   rd
            inc   rd
            glo   r8
            str   rd

            glo   r7
            lbnz  wild

literal:    mov   rd,arg                ; send the file of exactly this name
            lda   rd
            phi   r8
            ldn   rd
            plo   r8
            mov   rf,path
litcopy:    lda   r8
            str   rf
            inc   rf
            lbnz  litcopy
            call  sendfile
            lbr   nextarg


            ; The name has a wildcard. Copy the directory part of it, then
            ; send every file in that directory that the rest matches. The
            ; kernel has only the one descriptor for directories and it is
            ; used in opening any file, so the directory is opened afresh
            ; for each match, and read from where the last one was found.

wild:       mov   rd,arg
            lda   rd
            phi   r8
            ldn   rd
            plo   r8
            mov   rd,pattern
            lda   rd
            phi   r9
            ldn   rd
            plo   r9
            mov   rf,path
wildcopy:   glo   r8                    ; copy up to the pattern
            str   r2
            glo   r9
            sm
            lbz   wilddir
            lda   r8
            str   rf
            inc   rf
            lbr   wildcopy
wilddir:    mov   rd,nameat             ; matching names go here
            ghi   rf
            str   rd
            inc   rd
            glo   rf
            str   rd

            mov   rd,entry
            ldi   0
            str   rd
            inc   rd
            str   rd
            mov   rd,found
            ldi   0
            str   rd

wildnext:   mov   rd,abort
            ldn   rd
            lbnz  failed

            mov   rd,nameat             ; cut the path back to the directory
            lda   rd
            phi   rf
            ldn   rd
            plo   rf
            ldi   0
            str   rf
            mov   rf,path
            call  o_opendir
            lbdf  wilddone
            mov   rf,dirfd
            ghi   rd
            str   rf
            inc   rf
            glo   rd
            str   rf

            mov   rd,entry              ; pass over the entries already seen
            lda   rd
            phi   r9
            ldn   rd
            plo   r9
wildskip:   ghi   r9
            lbnz  wildpass
            glo   r9
            lbz   wildread
wildpass:   dec   r9
            mov   rd,skip
            ghi   r9
            str   rd
            inc   rd
            glo   r9
            str   rd
            call  readent
            lbdf  wilddone
            mov   rd,skip
            lda   rd
            phi   r9
            ldn   rd
            plo   r9
            lbr   wildskip

wildread:   call  readent
            lbdf  wilddone
            mov   rd,entry+1
            ldn   rd
            adi   1
            str   rd
            dec   rd
            ldn   rd
            adci  0
            str   rd

            mov   rd,dirent             ; skip deleted entries
            lda   rd
            lbnz  wildused
            lda   rd
            lbnz  wildused
            lda   rd
            lbnz  wildused
            ldn   rd
            lbz   wildread
wildused:   mov   rd,dirent+6           ; skip directories and hidden files
            ldn   rd
            ani   9
            lbnz  wildread
            call  match
            lbdf  wildread

            mov   rd,found
            ldi   1
            str   rd
            mov   rd,nameat
            lda   rd
            phi   rf
            ldn   rd
            plo   rf
            mov   r8,dirent+12
wildname:   lda   r8
            str   rf
            inc   rf
            lbnz  wildname
            call  sendfile
            lbr   wildnext

wilddone:   mov   rd,found              ; if nothing matched, then try it
            ldn   rd                    ;  as an ordinary name
            lbz   literal
            lbr   nextarg


            ; All of the files have been sent, or tried

over:       mov   rd,shaken             ; if nothing went out then the
            ldn   rd                    ;  host is not expecting anything
            lbz   summary
            call  endmark
            lbdf  failed
            call  getbyte               ; host finishes with an 'x'
            xri   'x'
            lbz   summary

failed:     mov   rd,result
            ldi   1
            str   rd


            ; The wire is idle again, so it is safe to say how it went

summary:    mov   rd,savere             ; restore previous echo setting
            ldn   rd
            phi   re

            mov   rd,nok
            ldn   rd
            call  prbyte
            call  o_inmsg
            db    ' file(s) sent.',13,10,0

            mov   rd,nerr
            ldn   rd
            lbz   sumdone
            call  prbyte
            call  o_inmsg
            db    ' file(s) failed.',13,10,0
            mov   rd,result
            ldi   1
            str   rd

sumdone:    mov   rd,result
            ldn   rd
            rtn                         ; return to Elf/OS

usage:      call  o_inmsg
            db    'Usage: ms [-u|-b] [-v] filename [filename...]',13,10,0
            ldi   1
            rtn                         ; and return to os


            ; Send the file named in path. A file that cannot be opened is
            ; counted in nerr; an error once the host is expecting the file
            ; sets abort.

sendfile:   call  chkdir                ; a directory would open as a file
            lbnf  sferr
            mov   rf,path
            mov   rd,fildes             ; get file descriptor
            ldi   FF_READ               ; file flags
            plo   r7
            call  o_open                ; attempt to open file
            lbnf  sfopen                ; jump if file opened

sferr:      mov   rd,nerr
            ldn   rd
            adi   1
            str   rd
            mov   rd,verbose
            ldn   rd
            lbz   sfret
            call  o_inmsg
            db    'Cannot open ',0
            mov   rf,path
            call  o_msg
            call  o_inmsg
            db    '.',13,10,0
sfret:      rtn

sfopen:     mov   r8,0                  ; seek to the end to get the size
            mov   r7,0
            mov   rc,SEEK_END
            mov   rd,fildes
            call  o_seek
            mov   rd,size
            ghi   r8
            str   rd
            inc   rd
            glo   r8
            str   rd
            inc   rd
            ghi   r7
            str   rd
            inc   rd
            glo   r7
            str   rd
            mov   r8,0                  ; and back to the start
            mov   r7,0
            mov   rc,SEEK_SET
            mov   rd,fildes
            call  o_seek

            ; The handshake is left until there is a file to send, so that
            ; a mistyped name does not start a session with the host.

            mov   rd,shaken
            ldn   rd
            lbnz  sfhdr
            call  getbyte
            xri   0aah
            lbz   sfshake
            mov   rd,verbose
            ldn   rd
            lbz   sfabort
            call  o_inmsg
            db    'No response from host.',13,10,0
            lbr   sfabort

sfshake:    ldi   055h
            call  putbyte
            mov   rd,shaken
            ldi   1
            str   rd

sfhdr:      mov   r8,path               ; find the last part of the name
            mov   r9,path
sfbase:     lda   r8
            lbz   sfname
            smi   '/'
            lbnz  sfbase
            mov   r9,r8
            lbr   sfbase

sfname:     mov   rd,name
            ghi   r9
            str   rd
            inc   rd
            glo   r9
            str   rd

            mov   rf,buf                ; header is the name, then the size
            mov   r7,4
sfncopy:    lda   r9
            str   rf
            inc   rf
            inc   r7
            lbnz  sfncopy
            mov   rd,size
            lda   rd
            str   rf
            inc   rf
            lda   rd
            str   rf
            inc   rf
            lda   rd
            str   rf
            inc   rf
            ldn   rd
            str   rf
            call  sendblk
            lbdf  sfwire

sfdata:     mov   rf,buf
            mov   rc,512
            mov   rd,fildes
            call  o_read                ; read next block from file
            lbdf  sfread
            mov   r7,rc
            ghi   r7
            lbnz  sfblk
            glo   r7
            lbz   sfeof
sfblk:      call  sendblk
            lbdf  sfwire
            lbr   sfdata

sfeof:      call  endmark               ; end of this file
            lbdf  sfwire
            mov   rd,fildes
            call  o_close
            mov   rd,nok
            ldn   rd
            adi   1
            str   rd
            mov   rd,verbose
            ldn   rd
            lbz   sfret
            call  o_inmsg
            db    'Sent ',0
            mov   rd,name
            lda   rd
            phi   rf
            ldn   rd
            plo   rf
            call  o_msg
            call  o_inmsg
            db    '.',13,10,0
            rtn

sfread:     mov   rd,verbose
            ldn   rd
            lbz   sfabort
            call  o_inmsg
            db    'Error reading file.',13,10,0
            lbr   sfabort

sfwire:     mov   rd,verbose
            ldn   rd
            lbz   sfabort
            call  o_inmsg
            db    'Send error.',13,10,0

sfabort:    mov   rd,fildes
            call  o_close
            mov   rd,abort
            ldi   1
            str   rd
            rtn


            ; Test whether path names a directory, returning DF=0 if it
            ; does. The kernel only follows a path as far as its last
            ; slash, so one is added for the test and taken off again.

chkdir:     mov   rf,path
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
            mov   rf,path
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


            ; Read the next directory entry, returning DF=1 at the end

readent:    mov   rd,dirfd
            lda   rd
            phi   r8
            ldn   rd
            plo   r8
            mov   rd,r8
            mov   rf,dirent
            mov   rc,32
            call  o_read
            lbdf  renone
            glo   rc
            smi   32
            lbnz  renone
            clc
            rtn
renone:     stc
            rtn


            ; Compare the name in the directory entry with the pattern,
            ; returning DF=0 if it matches. A ? stands for any character
            ; and a * for any number of them, including none.

match:      mov   rd,pattern
            lda   rd
            phi   r8
            ldn   rd
            plo   r8
            mov   r9,dirent+12
            ldi   0
            plo   r7                    ; set once a * has been seen

mtloop:     ldn   r9
            lbz   mtend                 ; jump if name is used up
            ldn   r8
            smi   '*'
            lbz   mtstar
            ldn   r8
            smi   '?'
            lbz   mtsame
            ldn   r8
            str   r2
            ldn   r9
            sm
            lbnz  mtback
mtsame:     inc   r8
            inc   r9
            lbr   mtloop

mtstar:     inc   r8                    ; remember where, to come back to
            mov   ra,r8
            mov   rb,r9
            ldi   1
            plo   r7
            lbr   mtloop

mtback:     glo   r7                    ; have the last * take one more
            lbz   mtfail
            inc   rb
            mov   r9,rb
            mov   r8,ra
            lbr   mtloop

mtend:      ldn   r8                    ; only *'s may be left over
            smi   '*'
            lbnz  mtlast
            inc   r8
            lbr   mtend
mtlast:     ldn   r8
            lbnz  mtfail
            clc
            rtn
mtfail:     stc
            rtn


            ; Print in decimal the byte in D

prbyte:     plo   r8
            mov   r9,numbuf+3           ; digits are produced last first
            ldi   0
            str   r9

prdigit:    ldi   0                     ; divide by ten
            plo   r7
            glo   r8
prdiv:      smi   10
            lbnf  prrem
            inc   r7
            lbr   prdiv
prrem:      adi   10+'0'
            dec   r9
            str   r9
            glo   r7
            plo   r8
            lbnz  prdigit

            mov   rf,r9
            call  o_msg
            rtn


            ; Every byte of the transfer goes through these, apart from
            ; the data itself. The addresses are changed by -u and -b.

getbyte:    lbr   f_read

putbyte:    lbr   f_type


            .align page

            ; Send the chunk in buf, whose length is in R7, returning DF=1
            ; if the host does not acknowledge it. The second acknowledgment
            ; comes only once the host is ready for what we send next.

sendblk:    ghi   r7                    ; send high byte of size
            call  putbyte
            glo   r7                    ; send low byte of size
            call  putbyte

            call  getbyte               ; check ack
            xri   0aah
            bnz   sberr

            mov   rf,buf
            mov   rc,r7
            dec   rc
sendlp:     lda   rf                    ; send block
            call  f_type
            untl  rc,sendlp

            call  getbyte               ; check ack
            xri   0aah
            bnz   sberr
            clc
            rtn

            ; Send a chunk of no length, which ends a file or the session

endmark:    ldi   0
            call  putbyte
            ldi   0
            call  putbyte
            call  getbyte               ; check ack
            xri   0aah
            bnz   sberr
            clc
            rtn

sberr:      stc
            rtn


verbose:    db    0                     ; -v given
shaken:     db    0                     ; handshake has been done
abort:      db    0                     ; session cannot go on
found:      db    0                     ; pattern has matched something
result:     db    0                     ; value to return to Elf/OS
nok:        db    0                     ; files sent
nerr:       db    0                     ; files that could not be opened
savere:     db    0
argp:       dw    0                     ; rest of the command line
arg:        dw    0                     ; filename being dealt with
pattern:    dw    0                     ;  and the last part of it
nameat:     dw    0                     ; where a matching name goes in path
name:       dw    0                     ; last part of the name in path
slash:      dw    0                     ; set by chkdir
dirfd:      dw    0                     ; kernel's directory descriptor
entry:      dw    0                     ; directory entries read so far
skip:       dw    0
size:       db    0,0,0,0

            ; File descriptor for the file being sent

fildes:     db    0,0,0,0
            dw    dta
            db    0,0
            db    0
            db    0,0,0,0
            dw    0,0
            db    0,0,0,0

            db    0                     ; chkdir looks one before path

path:       ds    280
numbuf:     ds    4
dirent:     ds    32
buf:        ds    512
dta:        ds    512

            end   start
