# OpenWatcom makefile to build CoolEdit plugin 'cool_wv4.flt' for Win32

LIBNAME = cool_wv4

DLLFILE = $(LIBNAME).flt
LNKFILE = $(LIBNAME).lnk

CC = wcc386
RC = wrc

WAVPACK_LIB = wavpack.lib

CFLAGS = -bt=nt -d0 -zq -bm -5s -fp5 -fpi87 -sg -oeatxh -ei
#CFLAGS+= -j
# warnings:
CFLAGS+= -wx
# newer OpenWatcom versions enable W303 by default:
CFLAGS+= -wcd=303
# include paths:
CFLAGS+= -I"$(%WATCOM)/h/nt" -I"$(%WATCOM)/h"
CFLAGS+= -I"../include"
# to build a dll:
CFLAGS+= -bd
RCFLAGS = -q -r -bt=nt -I"$(%WATCOM)/h/nt"

SRCS = cool_wv4.c
RCSRCS = wavpack.rc

.extensions:
.extensions: .lib .flt .obj .res .c .rc

OBJS = $(SRCS:.c=.obj)
RCOBJS= $(RCSRCS:.rc=.res)

all: $(DLLFILE)

$(DLLFILE): $(OBJS) $(RCOBJS) $(LNKFILE)
  @echo * Link: $@
  wlink @$(LNKFILE)

$(LNKFILE):
  @%create $@
  @%append $@ SYSTEM nt_dll INITINSTANCE TERMINSTANCE
  @%append $@ NAME $(DLLFILE)
  @for %i in ($(OBJS)) do @%append $@ FILE %i
  @%append $@ OPTION QUIET
  @%append $@ OPTION RESOURCE=$(RCOBJS)
  @%append $@ LIB $(WAVPACK_LIB)
  @%append $@ export 'QueryCoolFilter'='_QueryCoolFilter@4'
  @%append $@ export 'FilterUnderstandsFormat'='_FilterUnderstandsFormat@4'
  @%append $@ export 'GetSuggestedSampleType'='_GetSuggestedSampleType@12'
  @%append $@ export 'OpenFilterInput'='_OpenFilterInput@24'
  @%append $@ export 'FilterGetFileSize'='_FilterGetFileSize@4'
  @%append $@ export 'ReadFilterInput'='_ReadFilterInput@12'
  @%append $@ export 'CloseFilterInput'='_CloseFilterInput@4'
  @%append $@ export 'FilterOptions'='_FilterOptions@4'
  @%append $@ export 'FilterOptionsString'='_FilterOptionsString@8'
  @%append $@ export 'OpenFilterOutput'='_OpenFilterOutput@28'
  @%append $@ export 'CloseFilterOutput'='_CloseFilterOutput@4'
  @%append $@ export 'WriteFilterOutput'='_WriteFilterOutput@12'
  @%append $@ export 'FilterGetOptions'='_FilterGetOptions@24'
  @%append $@ export 'FilterWriteSpecialData'='_FilterWriteSpecialData@20'
  @%append $@ export 'FilterGetFirstSpecialData'='_FilterGetFirstSpecialData@8'
  @%append $@ export 'FilterGetNextSpecialData'='_FilterGetNextSpecialData@8'
  @%append $@ OPTION MAP=$*
  @%append $@ OPTION ELIMINATE
  @%append $@ OPTION SHOWDEAD

.c.obj:
  $(CC) $(CFLAGS) -Fo=$^@ $<
.rc.res:
  $(RC) $(RCFLAGS) -Fo=$^@ $<

clean: .SYMBOLIC
  @if exist *.obj rm *.obj
  @if exist *.res rm *.res
  @if exist *.err rm *.err
  @if exist $(LNKFILE) rm $(LNKFILE)

distclean: .SYMBOLIC clean
  @if exist $(DLLFILE) rm $(DLLFILE)
  @if exist *.map rm *.map
