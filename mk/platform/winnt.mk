# Windows NT 4.0 / 2000 and later, Win32 (Pentium-tuned, UNICODE opt-in).
PLATFORM_ID   := WINNT
BITS          := 32
CC            := wcc386
CXX           := wpp386
MODELS        := f
DEFAULT_MODEL := f
DEFAULT_CPU   := 5
CPU_SUFFIX    := s
PLAT_CFLAGS   := -bt=nt -DWINVER=0x0400 -D_WIN32_WINNT=0x0400
PLAT_AFLAGS   := -bt=nt
PLAT_INCS     := -i=$(WATCOM)/h -i=$(WATCOM)/h/nt
PLAT_RCFLAGS  := -bt=nt -i=$(WATCOM)/h/nt
HAS_RES       := 1
HAS_UNICODE   := 1
exe_ext        = EXE
link_sys       = system $(if $(filter gui,$(1)),nt_win runtime windows=4.0,nt runtime console=4.0)
