CONTIKI_PROJECT = nbr nbr-part2-slave nbr-part2-master
all: $(CONTIKI_PROJECT)

CONTIKI = ../..


MAKE_NET = MAKE_NET_NULLNET
include $(CONTIKI)/Makefile.include
