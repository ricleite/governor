#
# Copyright (C) 2019 Ricardo Leite
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program.  If not, see <https://www.gnu.org/licenses/>.
#

CCX=g++
DFLAGS=-ggdb -g -fno-omit-frame-pointer
CXXFLAGS=-std=gnu++14 -Wall $(DFLAGS)
LDFLAGS=-ldl -pthread -latomic

OBJS=governor.o governor_impl.o governor_hooks.o
HEADERS=governor.h governor_impl.h governor_hooks.h

default: libgovernor.a

libgovernor.a: $(OBJS)
	ar rcs libgovernor.a $^

%.o : %.cpp $(HEADERS)
	$(CCX) $(CXXFLAGS) -c -o $@ $< $(LDFLAGS) -DGOVERNOR=1

clean:
	rm -f *.a *.o

