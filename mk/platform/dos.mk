# MS-DOS, 16-bit real mode (8086 and up).  Model 't' produces a .COM file.
PLATFORM_ID   := DOS
BITS          := 16
CC            := wcc
CXX           := wpp
MODELS        := t s m c l h
DEFAULT_MODEL := s
DEFAULT_CPU   := 0
CPU_SUFFIX    :=
PLAT_CFLAGS   := -bt=dos
PLAT_AFLAGS   := -bt=dos
PLAT_INCS     := -i=$(WATCOM)/h
HAS_RES       :=
HAS_UNICODE   :=
exe_ext        = $(if $(filter t,$(1)),COM,EXE)
link_sys       = system $(if $(filter t,$(2)),com,dos)
