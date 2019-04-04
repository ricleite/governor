
CCX=g++
DFLAGS=-ggdb -g -fno-omit-frame-pointer
CXXFLAGS=-std=gnu++14 -Wall $(DFLAGS)
LDFLAGS=-ldl -pthread -latomic

OBJS=governor.o governor_hooks.o
HEADERS=governor.h
DEFS=-DUSE_GOVERNOR=1

default: $(OBJS)

%.o : %.cpp $(HEADERS)
	$(CCX) $(CXXFLAGS) -c -o $@ $< $(LDFLAGS) $(DEFS)

clean:
	rm -f *.o

