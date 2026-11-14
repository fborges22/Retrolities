# Picasso - a Windows 95 style paint program for Windows 3.x.
NAME      := picasso
TYPE      := gui
PLATFORMS := win16
RES       := picasso.rc
LIBS      := retro imgio
LDFLAGS   := option stack=24k library commdlg library shell
