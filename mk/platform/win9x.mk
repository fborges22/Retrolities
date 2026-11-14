# Windows 95/98/ME, Win32 (386 and up, ANSI API only).
PLATFORM_ID   := WIN9X
BITS          := 32
CC            := wcc386
CXX           := wpp386
MODELS        := f
DEFAULT_MODEL := f
DEFAULT_CPU   := 3
CPU_SUFFIX    := s
PLAT_CFLAGS   := -bt=nt -DWINVER=0x0400 -D_WIN32_WINDOWS=0x0400
PLAT_AFLAGS   := -bt=nt
PLAT_INCS     := -i=$(WATCOM)/h -i=$(WATCOM)/h/nt
PLAT_RCFLAGS  := -bt=nt -i=$(WATCOM)/h/nt
HAS_RES       := 1
HAS_UNICODE   :=
exe_ext        = EXE
link_sys       = system $(if $(filter gui,$(1)),nt_win runtime windows=4.0,nt runtime console=4.0)
