# Arrow - uses the Open Watcom + MinGW-w64 toolchain in the parent project.
#
#   make            all six targets
#   make dos        ../../build/dos/bin/ARROWD.EXE
#   make msclient   ../../build/dos/bin/ARROWM.EXE (requires MSSDK)
#   make os2        ../../build/os2/bin/ARROWO.EXE
#   make win16      ../../build/win16/bin/ARROW16.EXE
#   make win32      ../../build/win9x/bin/ARROW32.EXE
#   make winnt      ../../build/winnt/bin/ARROWN.EXE
#   make win64      ../../build/win64/bin/ARROW64.EXE

MTCP   ?= /opt/mtcp
MSSDK  ?= /opt/mstcpsdk
MSSDK_LIB ?= $(MSSDK)/lib/dos_sock.lib
ROOT   := ../..
B      := $(ROOT)/build
COMMON := common
Q      := -zq
MINGW_PREFIX ?= x86_64-w64-mingw32-
W64_CC ?= $(MINGW_PREFIX)gcc
W64_RC ?= $(MINGW_PREFIX)windres

all: dos os2 win16 win32 winnt win64
dos:   $(B)/dos/bin/ARROWD.EXE
msclient: check-mssdk
	$(MAKE) --no-print-directory $(B)/dos/bin/ARROWM.EXE
os2:   $(B)/os2/bin/ARROWO.EXE
win16: $(B)/win16/bin/ARROW16.EXE
win32: $(B)/win9x/bin/ARROW32.EXE
winnt: $(B)/winnt/bin/ARROWN.EXE
win64: $(B)/win64/bin/ARROW64.EXE

# ---------------------------------------------------------------- DOS
DOS_O    := $(B)/dos/obj/arrow
DOS_DEFS := -DIP3_MAX_STREAMS=4 -DIP3_BLK_MAX=8192 -DIP3_BURST=4
DOS_CF   := -0 -ml -s -oh -ok -ot -oa -ei -zp2 -w3 $(Q) $(DOS_DEFS) -i=$(COMMON) -i=dos
MTCP_CF  := -0 -ml -s -oh -ok -ot -oa -ei -zp2 -zpw -ob -ol+ -oi+ $(Q) \
            '-DCFG_H="arrow.cfg"' -i=dos -i=$(MTCP)/tcpinc -i=$(MTCP)/include -i=$(COMMON)
MTCP_MOD := packet arp eth ip tcp tcpsockm udp utils dns timer trace
DOS_OBJS := $(DOS_O)/main.obj $(DOS_O)/ip3core.obj $(DOS_O)/net_dos.obj \
            $(MTCP_MOD:%=$(DOS_O)/%.obj) $(DOS_O)/ipasm.obj

$(DOS_O)/main.obj: dos/main.c $(COMMON)/ip3core.h $(COMMON)/ip3net.h | $(DOS_O)
	wcc $(DOS_CF) -fo=$@ $<
$(DOS_O)/ip3core.obj: $(COMMON)/ip3core.c $(COMMON)/ip3core.h $(COMMON)/ip3net.h | $(DOS_O)
	wcc $(DOS_CF) -fo=$@ $<
$(DOS_O)/net_dos.obj: dos/net_dos.cpp dos/arrow.cfg $(COMMON)/ip3net.h | $(DOS_O)
	wpp $(MTCP_CF) -fo=$@ $<
$(DOS_O)/%.obj: $(MTCP)/tcplib/%.cpp dos/arrow.cfg | $(DOS_O)
	wpp $(MTCP_CF) -fo=$@ $<
$(DOS_O)/ipasm.obj: $(MTCP)/tcplib/ipasm.asm | $(DOS_O)
	wasm -0 -ml $(Q) -fo=$@ $<

$(B)/dos/bin/ARROWD.EXE: $(DOS_OBJS) | $(B)/dos/bin
	wlink option quiet system dos option map=$(DOS_O)/arrow.map option eliminate \
	      option stack=8192 name $@ $(DOS_OBJS:%=file %)

# ---------------------------------------------- DOS / MSCLIENT (opt-in)
MS_O    := $(B)/dos/obj/arrow-msclient
MS_CF   := $(DOS_CF) -DIP3_MSCLIENT -Dfar=__far -Dcdecl=__cdecl -i=$(MSSDK)/include
MS_OBJS := $(MS_O)/main.obj $(MS_O)/ip3core.obj $(MS_O)/net_msclient.obj
MS_HDRS := $(wildcard $(MSSDK)/include/*.h $(MSSDK)/include/sys/*.h \
                     $(MSSDK)/include/netinet/*.h)

check-mssdk:
	@test -f "$(MSSDK)/include/sys/socket.h" -a -f "$(MSSDK_LIB)" || { \
	  echo "MSCLIENT build requires the Microsoft TCP/IP Sockets SDK."; \
	  echo "Set MSSDK=<SDK root> (lower-case include/lib paths); see README."; exit 1; }

$(MS_O)/main.obj: dos/main.c $(COMMON)/ip3core.h $(COMMON)/ip3net.h | check-mssdk $(MS_O)
	wcc $(MS_CF) -fo=$@ $<
$(MS_O)/ip3core.obj: $(COMMON)/ip3core.c $(COMMON)/ip3core.h $(COMMON)/ip3net.h | check-mssdk $(MS_O)
	wcc $(MS_CF) -fo=$@ $<
$(MS_O)/net_msclient.obj: dos/net_msclient.c $(COMMON)/ip3core.h $(COMMON)/ip3net.h \
                        $(MS_HDRS) | check-mssdk $(MS_O)
	wcc $(MS_CF) -fo=$@ $<
$(B)/dos/bin/ARROWM.EXE: $(MS_OBJS) $(MSSDK_LIB) | check-mssdk $(B)/dos/bin
	wlink option quiet system dos option map=$(MS_O)/arrow.map option eliminate \
	      option stack=8192 name $@ $(MS_OBJS:%=file %) library $(MSSDK_LIB)

# ------------------------------------------------------------------ OS/2 2.0
OS2_O    := $(B)/os2/obj/arrow
OS2_CF   := -bt=os2 -mf -3 -ox -s -w3 $(Q) -DIP3_MAX_STREAMS=8 \
            -DIP3_BLK_MAX=131072 -DIP3_BURST=32 -i=$(COMMON) -i=os2 \
            -i=$(WATCOM)/h -i=$(WATCOM)/h/os2
OS2_OBJS := $(OS2_O)/main.obj $(OS2_O)/net_os2.obj $(OS2_O)/ip3core.obj

$(OS2_O)/%.obj: os2/%.c $(COMMON)/ip3core.h $(COMMON)/ip3net.h | $(OS2_O)
	wcc386 $(OS2_CF) -fo=$@ $<
$(OS2_O)/ip3core.obj: $(COMMON)/ip3core.c $(COMMON)/ip3core.h $(COMMON)/ip3net.h | $(OS2_O)
	wcc386 $(OS2_CF) -fo=$@ $<
$(B)/os2/bin/ARROWO.EXE: $(OS2_OBJS) | $(B)/os2/bin
	wlink option quiet system os2v2 option map=$(OS2_O)/arrow.map \
	      library tcpip32.lib name $@ $(OS2_OBJS:%=file %)

# -------------------------------------------------------------- Win16
W16_O   := $(B)/win16/obj/arrow
W16_CF  := -2 -ml -bt=windows -zW -fpc -s -os -w3 $(Q) -DIP3_BLK_MAX=16384 \
           -i=$(COMMON) -i=win -i=$(WATCOM)/h -i=$(WATCOM)/h/win
W16_OBJS := $(W16_O)/winmain.obj $(W16_O)/net_win.obj $(W16_O)/ip3core.obj
WIN_HDRS := win/resource.h $(COMMON)/ip3core.h $(COMMON)/ip3net.h

$(W16_O)/%.obj: win/%.c $(WIN_HDRS) | $(W16_O)
	wcc $(W16_CF) -fo=$@ $<
$(W16_O)/ip3core.obj: $(COMMON)/ip3core.c $(WIN_HDRS) | $(W16_O)
	wcc $(W16_CF) -fo=$@ $<
$(W16_O)/arrow.res: win/arrow.rc win/resource.h win/arrow.ico | $(W16_O)
	wrc -q -r -bt=windows -i=win -i=$(WATCOM)/h/win -fo=$@ $<
$(B)/win16/bin/ARROW16.EXE: $(W16_OBJS) $(W16_O)/arrow.res | $(B)/win16/bin
	wlink option quiet system windows option map=$(W16_O)/arrow.map option stack=16k \
	      option heapsize=8k option description \'Arrow for Windows 3.x\'  \
	      library winsock.lib option resource=$(W16_O)/arrow.res \
	      name $@ $(W16_OBJS:%=file %)

# -------------------------------------------------------------- Win32
W32_O   := $(B)/win9x/obj/arrow
W32_CF  := -4r -bt=nt -ox -s -w3 $(Q) -DIP3_BLK_MAX=131072 -DIP3_BURST=32 \
           -i=$(COMMON) -i=win -i=$(WATCOM)/h -i=$(WATCOM)/h/nt
W32_OBJS := $(W32_O)/winmain.obj $(W32_O)/net_win.obj $(W32_O)/ip3core.obj

$(W32_O)/%.obj: win/%.c $(WIN_HDRS) | $(W32_O)
	wcc386 $(W32_CF) -fo=$@ $<
$(W32_O)/ip3core.obj: $(COMMON)/ip3core.c $(WIN_HDRS) | $(W32_O)
	wcc386 $(W32_CF) -fo=$@ $<
$(W32_O)/arrow.res: win/arrow.rc win/resource.h win/arrow.ico | $(W32_O)
	wrc -q -r -bt=nt -i=win -i=$(WATCOM)/h/nt -fo=$@ $<
$(B)/win9x/bin/ARROW32.EXE: $(W32_OBJS) $(W32_O)/arrow.res | $(B)/win9x/bin
	wlink option quiet system nt_win option map=$(W32_O)/arrow.map runtime windows=4.0 \
	      library wsock32.lib option resource=$(W32_O)/arrow.res \
	      name $@ $(W32_OBJS:%=file %)

# ----------------------------------------------- Windows NT 4.0 / 2000
NT_O    := $(B)/winnt/obj/arrow
NT_CF   := -5r -bt=nt -ox -s -w3 $(Q) -DIP3_WINNT -DIP3_BLK_MAX=131072 -DIP3_BURST=32 \
           -i=$(COMMON) -i=win -i=$(WATCOM)/h -i=$(WATCOM)/h/nt
NT_OBJS := $(NT_O)/winmain.obj $(NT_O)/net_win.obj $(NT_O)/ip3core.obj

$(NT_O)/%.obj: win/%.c $(WIN_HDRS) | $(NT_O)
	wcc386 $(NT_CF) -fo=$@ $<
$(NT_O)/ip3core.obj: $(COMMON)/ip3core.c $(WIN_HDRS) | $(NT_O)
	wcc386 $(NT_CF) -fo=$@ $<
$(NT_O)/arrow.res: win/arrow.rc win/resource.h win/arrow.ico | $(NT_O)
	wrc -q -r -bt=nt -DIP3_WINNT -i=win -i=$(WATCOM)/h/nt -fo=$@ $<
$(B)/winnt/bin/ARROWN.EXE: $(NT_OBJS) $(NT_O)/arrow.res | $(B)/winnt/bin
	wlink option quiet system nt_win option map=$(NT_O)/arrow.map runtime windows=4.0 \
	      library ws2_32.lib option resource=$(NT_O)/arrow.res \
	      name $@ $(NT_OBJS:%=file %)

# -------------------------------------------------------------- Win64
W64_O    := $(B)/win64/obj/arrow
W64_DEFS := -DIP3_WINNT -DIP3_BLK_MAX=131072 -DIP3_BURST=32
W64_CF   := -O2 -Wall -Wextra $(W64_DEFS) -I$(COMMON) -Iwin
W64_OBJS := $(W64_O)/winmain.o $(W64_O)/net_win.o $(W64_O)/ip3core.o

$(W64_O)/%.o: win/%.c $(WIN_HDRS) | $(W64_O)
	$(W64_CC) $(W64_CF) -c -o $@ $<
$(W64_O)/ip3core.o: $(COMMON)/ip3core.c $(WIN_HDRS) | $(W64_O)
	$(W64_CC) $(W64_CF) -c -o $@ $<
$(W64_O)/arrow.o: win/arrow.rc win/resource.h win/arrow.ico | $(W64_O)
	$(W64_RC) -DIP3_WINNT -Iwin -O coff -o $@ $<
$(B)/win64/bin/ARROW64.EXE: $(W64_OBJS) $(W64_O)/arrow.o | $(B)/win64/bin
	$(W64_CC) -mwindows -static -Wl,-Map,$(W64_O)/arrow.map -o $@ $^ \
	      -lws2_32 -lgdi32 -ladvapi32 -lm

$(DOS_O) $(MS_O) $(OS2_O) $(W16_O) $(W32_O) $(NT_O) $(W64_O) $(B)/dos/bin $(B)/os2/bin $(B)/win16/bin $(B)/win9x/bin $(B)/winnt/bin $(B)/win64/bin:
	mkdir -p $@

clean:
	rm -rf $(MS_O)
	rm -f $(B)/dos/bin/ARROWM.EXE
	rm -rf $(B)/dos/obj/arrow $(B)/os2/obj/arrow $(B)/win16/obj/arrow $(B)/win9x/obj/arrow \
	       $(B)/winnt/obj/arrow $(B)/win64/obj/arrow
	rm -f $(B)/dos/bin/ARROWD.EXE $(B)/os2/bin/ARROWO.EXE $(B)/win16/bin/ARROW16.EXE \
	      $(B)/win9x/bin/ARROW32.EXE $(B)/winnt/bin/ARROWN.EXE \
	      $(B)/win64/bin/ARROW64.EXE

.PHONY: all dos msclient check-mssdk os2 win16 win32 winnt win64 clean
