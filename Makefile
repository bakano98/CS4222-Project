CONTIKI_PROJECT = nbr nbr-part2-sender nbr-part2-requester
all: $(CONTIKI_PROJECT)

CONTIKI = ../..


MAKE_NET = MAKE_NET_NULLNET
include $(CONTIKI)/Makefile.include
