CC = g++
.PHONY: all
all: dataServer remoteClient

dataServer: dataServer.o 
	$(CC) $^ -o $@ -pthread 
dataServer.o: dataServer.cpp  
	$(CC) -c $^  
 
remoteClient: remoteClient.o 
	$(CC) $^ -o $@ -pthread 
remoteClient.o: remoteClient.cpp 
	$(CC) -c $^ 

clean:
	rm -rf *.o dataServer remoteClient
    
