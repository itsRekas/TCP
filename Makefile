#Reference
#http://makepp.sourceforge.net/1.19/makepp_tutorial.html

CC = gcc -c
SHELL = /bin/bash

# compiling flags here
CFLAGS = -Wall -I.

LINKER = gcc -o
# linking flags here
LFLAGS   = -Wall -lm

OBJDIR = ../obj

CLIENT_OBJECTS := $(OBJDIR)/rdt_sender.o $(OBJDIR)/common.o $(OBJDIR)/packet.o
SERVER_OBJECTS := $(OBJDIR)/rdt_receiver.o $(OBJDIR)/common.o $(OBJDIR)/packet.o

#Program name
CLIENT := $(OBJDIR)/rdt_sender
SERVER := $(OBJDIR)/rdt_receiver

rm       = rm -f
rmdir    = rmdir 

TARGET:	$(OBJDIR) $(CLIENT)	$(SERVER)


$(CLIENT): $(CLIENT_OBJECTS)
	$(LINKER) $@ $(CLIENT_OBJECTS) $(LFLAGS)
	@echo "Link complete!"

$(SERVER): $(SERVER_OBJECTS)
	$(LINKER) $@ $(SERVER_OBJECTS) $(LFLAGS)
	@echo "Link complete!"

$(OBJDIR)/%.o:	%.c common.h packet.h
	$(CC) $(CFLAGS)  $< -o $@
	@echo "Compilation complete!"

clean:
	@if [ -a $(OBJDIR) ]; then rm -r $(OBJDIR); fi;
	@echo "Cleanup complete!"

$(OBJDIR):
	@[ -a $(OBJDIR) ]  || mkdir $(OBJDIR)
