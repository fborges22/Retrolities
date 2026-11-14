# Picasso - a Windows 95 style paint program for Windows 3.x, 9x and NT/2000.
NAME      := picasso
TYPE      := gui
PLATFORMS := win16 win9x winnt
RES       := picasso.rc
LIBS      := retro imgio
LDFLAGS_win16 := option stack=24k library commdlg library shell
LDFLAGS_win9x := option stack=256k library comdlg32 library shell32
LDFLAGS_winnt := $(LDFLAGS_win9x)
