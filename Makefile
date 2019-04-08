
CCX=g++
DFLAGS=-ggdb -g -fno-omit-frame-pointer
CXXFLAGS=-std=gnu++14 -Wall $(DFLAGS)
LDFLAGS=-ldl -pthread -latomic

OBJS=governor.o governor_impl.o governor_hooks.o
HEADERS=governor.h governor_impl.h governor_hooks.h
DEFS=-DGOVERNOR=1

default: libgovernor.a

libgovernor.a: $(OBJS)
	ar rcs libgovernor.a $^

%.o : %.cpp $(HEADERS)
	$(CCX) $(CXXFLAGS) -c -o $@ $< $(LDFLAGS) $(DEFS)

clean:
	rm -f *.a *.o

