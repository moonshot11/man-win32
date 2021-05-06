CC = bcc
CFLAGS = -w -1
OBJS =  man.obj fnmatch.obj



all: man.exe


man.exe : $(OBJS)
        $(CC) -e$@ $(OBJS)


.c.obj:
        $(CC) -c $(CFLAGS) $<


clean:
        del *.obj
        del *.exe

