# Polaris boot manager (run inside the "build" container)
#
#   make -C src/polaris dos      stage 1, stage 2 and POLARIS.EXE installer
#   make -C src/polaris images   floppy image and demo disk (host python)
#
# Outputs:
#   build/dos/bin/POLARIS.EXE        DOS installer / configurator
#   build/polaris/stage1.bin         boot sector (MBR and floppy)
#   build/polaris/stage2.bin         packed boot menu (loaded from track 0)
#   build/polaris/resident.bin       resident INT 13h module (inside stage 2)

ROOT    := ../..
WATCOM  ?= /opt/watcom
OUTDIR  := $(ROOT)/build/polaris
BINDIR  := $(ROOT)/build/dos/bin
OBJ     := $(ROOT)/build/dos/obj/polaris
WCC     := $(WATCOM)/binl64/wcc
WLINK   := $(WATCOM)/binl64/wlink
NASM    ?= nasm
PYTHON  ?= python3

# 16-bit, 386 instructions, small/tiny model, packed structures, no stack
# checks, size optimised.  Stage 2 has no C library at all (-zl).
CFLAGS  := -zq -3 -ms -zp1 -s -os -bt=dos -wx -i=$(WATCOM)/h
S2FLAGS := $(CFLAGS) -zl -DSTAGE2
INFLAGS := $(CFLAGS) -DINSTALLER

COMMON  := rt tui disk atapi config devlist screens
S2SRC   := $(COMMON) boot stage2
INSRC   := $(COMMON) install

S2OBJ   := $(OBJ)/start.obj $(OBJ)/bios.obj $(S2SRC:%=$(OBJ)/s2_%.obj)
INOBJ   := $(OBJ)/bios.obj $(INSRC:%=$(OBJ)/in_%.obj)
HEADERS := $(wildcard *.h)

.PHONY: dos images clean
dos: $(OUTDIR)/stage1.bin $(OUTDIR)/stage2.bin $(BINDIR)/POLARIS.EXE

$(OBJ) $(OUTDIR) $(BINDIR):
	mkdir -p $@

$(OUTDIR)/stage1.bin: boot/stage1.asm | $(OUTDIR)
	$(NASM) -f bin -o $@ $<

$(OUTDIR)/resident.bin: boot/resident.asm | $(OUTDIR)
	$(NASM) -f bin -o $@ $<

$(OBJ)/start.obj: boot/start.asm $(OUTDIR)/resident.bin | $(OBJ)
	$(NASM) -f obj -i$(OUTDIR)/ -o $@ $<

$(OBJ)/bios.obj: boot/bios.asm | $(OBJ)
	$(NASM) -f obj -o $@ $<

$(OBJ)/s2_%.obj: %.c $(HEADERS) | $(OBJ)
	$(WCC) $(S2FLAGS) -fo=$@ $<

$(OBJ)/in_%.obj: %.c $(HEADERS) | $(OBJ)
	$(WCC) $(INFLAGS) -fo=$@ $<

# stage 2 is linked as a .COM image: CS = DS = SS, origin 100h
$(OBJ)/stage2.com: $(S2OBJ)
	$(WLINK) option quiet format dos com name $@ \
	    option map=$(OBJ)/stage2.map $(addprefix file ,$(S2OBJ))

$(OBJ)/unpack.bin: boot/unpack.asm | $(OBJ)
	$(NASM) -f bin -o $@ $<

# packed: unpacker stub + PLZ stream, must fit LBA 5..62
$(OUTDIR)/stage2.bin: $(OBJ)/stage2.com $(OBJ)/unpack.bin tools/plz.py | $(OUTDIR)
	$(PYTHON) tools/plz.py pack $(OBJ)/stage2.com $(OBJ)/stage2.plz
	cat $(OBJ)/unpack.bin $(OBJ)/stage2.plz > $@
	@size=$$(stat -c %s $@); max=$$((58 * 512)); \
	  echo "stage2: $$size of $$max bytes"; \
	  if [ $$size -gt $$max ]; then echo "stage2 too large" >&2; rm -f $@; exit 1; fi

$(OBJ)/payload.c: $(OUTDIR)/stage1.bin $(OUTDIR)/stage2.bin tools/bin2c.py | $(OBJ)
	$(PYTHON) tools/bin2c.py $@ stage1_bin=$(OUTDIR)/stage1.bin stage2_bin=$(OUTDIR)/stage2.bin

$(OBJ)/in_payload.obj: $(OBJ)/payload.c
	$(WCC) $(INFLAGS) -fo=$@ $<

$(BINDIR)/POLARIS.EXE: $(INOBJ) $(OBJ)/in_payload.obj | $(BINDIR)
	$(WLINK) option quiet system dos name $@ option map=$(OBJ)/polaris.map \
	    option stack=6k $(addprefix file ,$(INOBJ) $(OBJ)/in_payload.obj)

images: dos
	$(PYTHON) tools/plimage.py floppy $(OUTDIR)/polaris-floppy.img

clean:
	rm -rf $(OUTDIR) $(OBJ) $(BINDIR)/POLARIS.EXE
