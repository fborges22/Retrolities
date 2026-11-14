# Windows 3.x, 16-bit protected mode (286 and up).
# TYPE=console programs use Open Watcom's default windowing system (-bw):
# stdio is shown in a text window.
PLATFORM_ID   := WIN16
BITS          := 16
CC            := wcc
CXX           := wpp
MODELS        := s m c l
DEFAULT_MODEL := l
DEFAULT_CPU   := 2
CPU_SUFFIX    :=
PLAT_CFLAGS   := -bt=windows -zw
PLAT_CFLAGS_console := -bw
PLAT_AFLAGS   := -bt=windows
PLAT_INCS     := -i=$(WATCOM)/h -i=$(WATCOM)/h/win
PLAT_RCFLAGS  := -bt=windows -i=$(WATCOM)/h/win
HAS_RES       := 1
HAS_UNICODE   :=
exe_ext        = EXE
link_sys       = system windows
