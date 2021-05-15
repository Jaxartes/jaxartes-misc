# Assembles (compiles) vic20-ffractal.asm
# See vic20-ffractal.asm for instructions.
# Interesting output files:
#       vic20-ffractal.prg -- compiled program in PRG format
#       vic20-ffractal.d64 -- 1540/1541 floppy disk image containing it
#       vic20-ffractal.lst -- list output from the assembler (for debugging)

TCL="tclsh"
SREC2PRG="srec2prg.tcl"
CRASM="crasm"
C1541="$(HOME)/pkg/vice-gtk3-3.5/bin/c1541"
PETCAT="$(HOME)/pkg/vice-gtk3-3.5/bin/petcat"

NAME=vic20-ffractal

$(NAME).d64: $(NAME).prg $(NAME).srcprg
	(   \
	    echo "end" ; \
	    echo "$(NAME)" ; \
	    echo "build information" ; \
	    echo "date: `date +%Y-%m-%d`" ; \
	    echo "time: `date +%H:%M:%S`" ; \
	    cat $(NAME).asm | grep '^ *config_.*=' | tr 'A-Z' 'a-z' \
	) | $(PETCAT) -w2 -o $(PWD)/$(NAME).stamp
        
	$(C1541) -format "$(NAME),vf" d64 $(PWD)/$(NAME).d64
	$(C1541) -attach $(PWD)/$(NAME).d64 -write $(PWD)/$(NAME).prg $(NAME)
	$(C1541) -attach $(PWD)/$(NAME).d64 -write $(PWD)/$(NAME).srcprg source
	$(C1541) -attach $(PWD)/$(NAME).d64 -write $(PWD)/$(NAME).asm source.ascii,s
	$(C1541) -attach $(PWD)/$(NAME).d64 -write $(PWD)/$(NAME).stamp "`date +%Y%m%d%H%M%S`"

$(NAME).prg: $(NAME).srec
	$(TCL) $(SREC2PRG) < $(NAME).srec > $(NAME).prg

$(NAME).srec: $(NAME).asm
	-$(CRASM) -o $(NAME).srec $(NAME).asm | \
	    tee $(NAME).lst | \
	    egrep '^(ERRORS|WARNINGS): *[0-9]|^>+ +[0-9]+ ERROR:|Abs END_OF_CODE'

$(NAME).srcprg: $(NAME).asm
	tr 'A-Z' 'a-z' < $(NAME).asm | \
	$(PETCAT) -w2 -o $(PWD)/$(NAME).srcprg

clean:
	-rm $(NAME).d64 $(NAME).prg $(NAME).srec $(NAME).lst $(NAME).srcprg \
	    $(NAME).stamp
