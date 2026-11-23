# Windows 3.x, 32-bit flat model through the Open Watcom Windows 386
# extender (386 enhanced mode; no Win32s needed).  Programs use the Win16
# API with 32-bit code and data.  The linker writes a .REX image which
# wbind joins to the WIN386 supervisor and the resources, producing an
# ordinary Windows 3.x executable.
PLATFORM_ID   := WIN386
BITS          := 32
CC            := wcc386
CXX           := wpp386
MODELS        := f
DEFAULT_MODEL := f
DEFAULT_CPU   := 3
CPU_SUFFIX    := r
PLAT_CFLAGS   := -bt=windows -zW
PLAT_AFLAGS   := -bt=windows
PLAT_INCS     := -i=$(WATCOM)/h -i=$(WATCOM)/h/win
PLAT_RCFLAGS  := -bt=windows -i=$(WATCOM)/h/win
HAS_RES       := 1
HAS_UNICODE   :=
BIND          := 1
exe_ext        = EXE
link_sys       = system win386
