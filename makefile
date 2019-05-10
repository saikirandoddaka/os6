CC=gcc
CFLAGS=-Wall -g -Werror

all: oss user

user: user.c memory.h
	$(CC) $(CFLAGS) user.c -o user

oss: oss.c memory.h
	$(CC) $(CFLAGS) oss.c -o oss

clean:
	rm -f ./*.o ./oss ./user output.txt
