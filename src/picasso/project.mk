# Picasso - a Windows 95 style paint program for Windows 3.x and Windows 9x.
NAME      := picasso
TYPE      := gui
PLATFORMS := win16 win9x
RES       := picasso.rc
LIBS      := retro imgio
LDFLAGS_win16 := option stack=24k library commdlg library shell
LDFLAGS_win9x := option stack=256k library comdlg32 library shell32
