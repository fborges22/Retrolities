# ShellIV - a Program Manager and File Manager replacement for Windows 3.x.
# The Windows 3.x build is a 32-bit program (Windows 386 extender, see
# mk/platform/win386.mk); the same source builds for Windows 9x and NT.
NAME      := shelliv
TYPE      := gui
PLATFORMS := win386 win9x winnt
RES       := shelliv.rc
LIBS      := retro
LDFLAGS_win386 := option stack=64k
LDFLAGS_win9x  := option stack=256k library comdlg32 library shell32 library mpr
LDFLAGS_winnt  := $(LDFLAGS_win9x)
