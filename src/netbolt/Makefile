# NetBolt - uses the Open Watcom + MinGW-w64 toolchain in the parent project.
#
#   make            all five targets
#   make dos        ../../build/dos/bin/NETBOLTD.EXE
#   make win16      ../../build/win16/bin/NETBOLT16.EXE
#   make win32      ../../build/win9x/bin/NETBOLT32.EXE
#   make winnt      ../../build/winnt/bin/NETBOLTN.EXE
#   make win64      ../../build/win64/bin/NETBOLT64.EXE

MTCP   ?= /opt/mtcp
ROOT   := ../..
B      := $(ROOT)/build
COMMON := common
Q      := -zq
MINGW_PREFIX ?= x86_64-w64-mingw32-
W64_CC ?= $(MINGW_PREFIX)gcc
W64_RC ?= $(MINGW_PREFIX)windres

all: dos win16 win32 winnt win64
dos:   $(B)/dos/bin/NETBOLTD.EXE
win16: $(B)/win16/bin/NETBOLT16.EXE
win32: $(B)/win9x/bin/NETBOLT32.EXE
winnt: $(B)/winnt/bin/NETBOLTN.EXE
win64: $(B)/win64/bin/NETBOLT64.EXE

# ---------------------------------------------------------------- DOS
DOS_O    := $(B)/dos/obj/netbolt
DOS_DEFS := -DIP3_MAX_STREAMS=4 -DIP3_BLK_MAX=8192 -DIP3_BURST=4
DOS_CF   := -0 -ml -s -oh -ok -ot -oa -ei -zp2 -w3 $(Q) $(DOS_DEFS) -i=$(COMMON) -i=dos
MTCP_CF  := -0 -ml -s -oh -ok -ot -oa -ei -zp2 -zpw -ob -ol+ -oi+ $(Q) \
            '-DCFG_H="netbolt.cfg"' -i=dos -i=$(MTCP)/tcpinc -i=$(MTCP)/include -i=$(COMMON)
MTCP_MOD := packet arp eth ip tcp tcpsockm udp utils dns timer trace
DOS_OBJS := $(DOS_O)/main.obj $(DOS_O)/ip3core.obj $(DOS_O)/net_dos.obj \
            $(MTCP_MOD:%=$(DOS_O)/%.obj) $(DOS_O)/ipasm.obj

$(DOS_O)/main.obj: dos/main.c $(COMMON)/ip3core.h $(COMMON)/ip3net.h | $(DOS_O)
	wcc $(DOS_CF) -fo=$@ $<
$(DOS_O)/ip3core.obj: $(COMMON)/ip3core.c $(COMMON)/ip3core.h $(COMMON)/ip3net.h | $(DOS_O)
	wcc $(DOS_CF) -fo=$@ $<
$(DOS_O)/net_dos.obj: dos/net_dos.cpp dos/netbolt.cfg $(COMMON)/ip3net.h | $(DOS_O)
	wpp $(MTCP_CF) -fo=$@ $<
$(DOS_O)/%.obj: $(MTCP)/tcplib/%.cpp dos/netbolt.cfg | $(DOS_O)
	wpp $(MTCP_CF) -fo=$@ $<
$(DOS_O)/ipasm.obj: $(MTCP)/tcplib/ipasm.asm | $(DOS_O)
	wasm -0 -ml $(Q) -fo=$@ $<

$(B)/dos/bin/NETBOLTD.EXE: $(DOS_OBJS) | $(B)/dos/bin
	wlink option quiet system dos option map=$(DOS_O)/netbolt.map option eliminate \
	      option stack=8192 name $@ $(DOS_OBJS:%=file %)

# -------------------------------------------------------------- Win16
W16_O   := $(B)/win16/obj/netbolt
W16_CF  := -2 -ml -bt=windows -zW -fpc -s -os -w3 $(Q) -DIP3_BLK_MAX=16384 \
           -i=$(COMMON) -i=win -i=$(WATCOM)/h -i=$(WATCOM)/h/win
W16_OBJS := $(W16_O)/winmain.obj $(W16_O)/net_win.obj $(W16_O)/ip3core.obj
WIN_HDRS := win/resource.h $(COMMON)/ip3core.h $(COMMON)/ip3net.h

$(W16_O)/%.obj: win/%.c $(WIN_HDRS) | $(W16_O)
	wcc $(W16_CF) -fo=$@ $<
$(W16_O)/ip3core.obj: $(COMMON)/ip3core.c $(WIN_HDRS) | $(W16_O)
	wcc $(W16_CF) -fo=$@ $<
$(W16_O)/netbolt.res: win/netbolt.rc win/resource.h win/netbolt.ico | $(W16_O)
	wrc -q -r -bt=windows -i=win -i=$(WATCOM)/h/win -fo=$@ $<
$(B)/win16/bin/NETBOLT16.EXE: $(W16_OBJS) $(W16_O)/netbolt.res | $(B)/win16/bin
	wlink option quiet system windows option map=$(W16_O)/netbolt.map option stack=16k \
	      option heapsize=8k option description \'NetBolt for Windows 3.x\'  \
	      library winsock.lib option resource=$(W16_O)/netbolt.res \
	      name $@ $(W16_OBJS:%=file %)

# -------------------------------------------------------------- Win32
W32_O   := $(B)/win9x/obj/netbolt
W32_CF  := -4r -bt=nt -ox -s -w3 $(Q) -DIP3_BLK_MAX=131072 -DIP3_BURST=32 \
           -i=$(COMMON) -i=win -i=$(WATCOM)/h -i=$(WATCOM)/h/nt
W32_OBJS := $(W32_O)/winmain.obj $(W32_O)/net_win.obj $(W32_O)/ip3core.obj

$(W32_O)/%.obj: win/%.c $(WIN_HDRS) | $(W32_O)
	wcc386 $(W32_CF) -fo=$@ $<
$(W32_O)/ip3core.obj: $(COMMON)/ip3core.c $(WIN_HDRS) | $(W32_O)
	wcc386 $(W32_CF) -fo=$@ $<
$(W32_O)/netbolt.res: win/netbolt.rc win/resource.h win/netbolt.ico | $(W32_O)
	wrc -q -r -bt=nt -i=win -i=$(WATCOM)/h/nt -fo=$@ $<
$(B)/win9x/bin/NETBOLT32.EXE: $(W32_OBJS) $(W32_O)/netbolt.res | $(B)/win9x/bin
	wlink option quiet system nt_win option map=$(W32_O)/netbolt.map runtime windows=4.0 \
	      library wsock32.lib option resource=$(W32_O)/netbolt.res \
	      name $@ $(W32_OBJS:%=file %)

# ----------------------------------------------- Windows NT 4.0 / 2000
NT_O    := $(B)/winnt/obj/netbolt
NT_CF   := -5r -bt=nt -ox -s -w3 $(Q) -DIP3_WINNT -DIP3_BLK_MAX=131072 -DIP3_BURST=32 \
           -i=$(COMMON) -i=win -i=$(WATCOM)/h -i=$(WATCOM)/h/nt
NT_OBJS := $(NT_O)/winmain.obj $(NT_O)/net_win.obj $(NT_O)/ip3core.obj

$(NT_O)/%.obj: win/%.c $(WIN_HDRS) | $(NT_O)
	wcc386 $(NT_CF) -fo=$@ $<
$(NT_O)/ip3core.obj: $(COMMON)/ip3core.c $(WIN_HDRS) | $(NT_O)
	wcc386 $(NT_CF) -fo=$@ $<
$(NT_O)/netbolt.res: win/netbolt.rc win/resource.h win/netbolt.ico | $(NT_O)
	wrc -q -r -bt=nt -DIP3_WINNT -i=win -i=$(WATCOM)/h/nt -fo=$@ $<
$(B)/winnt/bin/NETBOLTN.EXE: $(NT_OBJS) $(NT_O)/netbolt.res | $(B)/winnt/bin
	wlink option quiet system nt_win option map=$(NT_O)/netbolt.map runtime windows=4.0 \
	      library ws2_32.lib option resource=$(NT_O)/netbolt.res \
	      name $@ $(NT_OBJS:%=file %)

# -------------------------------------------------------------- Win64
W64_O    := $(B)/win64/obj/netbolt
W64_DEFS := -DIP3_WINNT -DIP3_BLK_MAX=131072 -DIP3_BURST=32
W64_CF   := -O2 -Wall -Wextra $(W64_DEFS) -I$(COMMON) -Iwin
W64_OBJS := $(W64_O)/winmain.o $(W64_O)/net_win.o $(W64_O)/ip3core.o

$(W64_O)/%.o: win/%.c $(WIN_HDRS) | $(W64_O)
	$(W64_CC) $(W64_CF) -c -o $@ $<
$(W64_O)/ip3core.o: $(COMMON)/ip3core.c $(WIN_HDRS) | $(W64_O)
	$(W64_CC) $(W64_CF) -c -o $@ $<
$(W64_O)/netbolt.o: win/netbolt.rc win/resource.h win/netbolt.ico | $(W64_O)
	$(W64_RC) -DIP3_WINNT -Iwin -O coff -o $@ $<
$(B)/win64/bin/NETBOLT64.EXE: $(W64_OBJS) $(W64_O)/netbolt.o | $(B)/win64/bin
	$(W64_CC) -mwindows -static -Wl,-Map,$(W64_O)/netbolt.map -o $@ $^ \
	      -lws2_32 -lgdi32 -ladvapi32 -lm

$(DOS_O) $(W16_O) $(W32_O) $(NT_O) $(W64_O) $(B)/dos/bin $(B)/win16/bin $(B)/win9x/bin $(B)/winnt/bin $(B)/win64/bin:
	mkdir -p $@

clean:
	rm -rf $(B)/dos/obj/netbolt $(B)/win16/obj/netbolt $(B)/win9x/obj/netbolt \
	       $(B)/winnt/obj/netbolt $(B)/win64/obj/netbolt
	rm -f $(B)/dos/bin/NETBOLTD.EXE $(B)/win16/bin/NETBOLT16.EXE \
	      $(B)/win9x/bin/NETBOLT32.EXE $(B)/winnt/bin/NETBOLTN.EXE \
	      $(B)/win64/bin/NETBOLT64.EXE

.PHONY: all dos win16 win32 winnt win64 clean
