CPPFLAGS = -D_GNU_SOURCE
CFLAGS   = -ggdb -O2 -Wall
LDFLAGS  = $(CFLAGS)

.PHONY: test clean

i2c: i2c.o

clean:
	$(RM) i2c.o i2c

test: i2c testfile1 testfile2 testfile3
	./i2c testfile1
	./i2c testfile2
	./i2c -s section testfile3
