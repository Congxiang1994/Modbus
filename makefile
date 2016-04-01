CC = arm-linux-gcc
OBJECT = sqlite3 

modbusclient:modbusclient.c sqliteunit.o
	$(CC) modbusclient.c sqliteunit.o -o modbusclient -L ./sqlite/lib -Wl,-rpath,./sqlite/lib -l sqlite3  -I ./sqlite/include -l pthread
	
sqliteunit.o:sqliteunit.c
	$(CC) -c sqliteunit.c -o sqliteunit.o
	
clean:
	-rm modbusclient *.o
