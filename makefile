#===========================================================================
# H - Creo TOOLKIT plugin makefile (nmake)
#
# Build target: Creo 6.0 TOOLKIT SDK.
# PTC compatibility rule: a DLL built with an OLDER Toolkit runs on that
# same or any NEWER Creo. So one DLL covers Creo 6.0 / 7 / 8 / 9 / 10 / 11 / 12.
#
# Usage: double-click build.bat  (or: nmake /f makefile)
#===========================================================================

CREO6_SDK   = D:\Creo6Toolkit
TOOLKIT_INC = $(CREO6_SDK)\includes\protoolkit\includes
TOOLKIT_LIB = $(CREO6_SDK)\obj\protoolkit\x86e_win64\obj

CC   = cl.exe
LINK = link.exe

TARGET = H.dll
OBJS   = src\h_main.obj

SYSLIBS = winspool.lib ws2_32.lib netapi32.lib advapi32.lib mpr.lib version.lib user32.lib gdi32.lib shell32.lib ole32.lib
TKLIB   = "$(TOOLKIT_LIB)\protk_dll_NU.lib" "$(TOOLKIT_LIB)\protk_dllmd_NU.lib" "$(TOOLKIT_LIB)\protoolkit_NU.lib" "$(TOOLKIT_LIB)\ucore.lib" "$(TOOLKIT_LIB)\udata.lib"

all: $(TARGET)

$(TARGET): $(OBJS)
	$(LINK) /nologo /DLL /INCREMENTAL:NO /OUT:$(TARGET) $(OBJS) $(TKLIB) $(SYSLIBS)

src\h_main.obj: src\h_main.c
	$(CC) /nologo /c /W3 /O2 /EHsc /MD /utf-8 /D_PRO_MESSAGE /DWNT /D_CRT_SECURE_NO_WARNINGS /I"$(TOOLKIT_INC)" /Fo$@ src\h_main.c

clean:
	-del /Q $(TARGET) src\*.obj 2>nul

.PHONY: all clean
