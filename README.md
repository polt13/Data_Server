**Compilation**

Use `make` in the project's root directory. This generates both `dataServer` and `remoteClient`.

**Running** 

Data Server:

`./dataServer -p {portNumber} -s {threadCount} -q {queueLength} -b {bufferLength}`

Remote Client:

`./remoteClient -p {serverPortNumber} -i {serverIP} -d {serverDirectory}`

The `-d` argument in `remoteClient` needs to correspond to a directory that exists and is relative to the server. 

Assuming 'dir1' exists and contains the server's executable and also the subdirectories `dira', 'dirb'..., if  the client 
requests 'dira', then 'dira' will be created in the remoteCLient's directory along with all its contents (including subfolders etc)

For example, if the client requests the directory 'dira/dirc` (dirc is dira's subfolder), then the entirety of the directory tree will be created,
meaning dira,dirc and all of dirc's content. Finall, if a path in the form of `../dirk` is requested, the client will receive a directory that looks like
`./../dirk` (where `.` is the directory that has the remoteClient executable).

Generally speaking, all of the directories that are created are relative to the directory of the remoteClient executable. 

***

On server start, a handler is setup that ignores SIGPIPE. This is done so that the server doesn't unexpectedly terminate when a client closes the connection while
the server is writing to its socket. Then, the server creates the worker threads, which remain idle until something is appended to the queue (this is implemented using
condition variables).

When the server receives a new socket from `accept`, it creates a new mapping (client_files) which sets the number of files that correspond to the socket to 0. After that,
it creates a new `comm_thread` that executes `manage_dir`. This function calls `read_dir` and `count_files`. 2 passes are needed: the first pass detects how many files are in the
directory (so that we know when the client is done receiving, and also when the worker thread is done). As soon as the file count is calculated, the comm_thread sends it to the client and the client
is expecting to read the length of the first filename. When the 2nd pass is done to comm thread starts appending the files to the queue and the worker threads start receiving tasks.

For traversing the directories, two different structs are used: `file_item`, `dir_handler`. dir_handler contains information regarding the directory: its name and the socket that needs to be 
mapped to its files. This is needed because when we finally discover a file inside a directory, we need to build a new file_item that contains the socket it's directed towards, so that each worker thread
has access to this information. Also, if a file is discovered, `stat` finds its size which is also a field in the `file_item` struct. After all the metadata is gathered, the `file_item` is appended to the queue,
if enough space is available. Otherwise, the comm_thread sleeps on a condition variable, until some worker thread notifies it that it has freed up space in the queue.

Everything that follows in write_file is wrapped inside mutexes, in order to avoid race conditions.

When a thread receives a task it searches the `socket_to_mutex` map, to check if the socket to which it's supposed to write is occupied by another thread.

Every socket is mapped to the address of a mutex, which is created using the `new` operator. This is so that the mutex is legally accessible by all threads. This creation only occurs if the socket the thread
is supposed to write to doesn't already exist as a key in the `socket_to_mutex` map. If it already exists, the thread is simply assigned the mutex that was already created and immediatelly waits on the mutex, until it gets a chance
to enter the critical section.

The worker thread first sends the length of the file name to the client (always a 4-byte integer), so that the client knows exactly how many bytes it's supposed to read (and not read bytes that don't correspond to the filename).
AFter that, the server writes the file size to the socket, opens the file on its side and starts reading it, while simultaneously writing `blocksize` bytes to the buffer every time and then sending the buffer's contents to the client.
Finally, it closes the file and waits for the client to send it a `FIN` message, before it reduces the file count that correspond to the socket. If the new file count is 0, then that signifies that all the files that were meant for a specific
socket have been successfully sent. Therefore, the socket can be safely removed from the map, free the memory for the mutex it's mapped to and close the socket on the server side.

***

When the client receives a filename in the form of `a/b/file.txt`, it first splits on every `/` so that it can create each directory ({a,b}) separately. If the directory arleady exists, it's ignored. This is implemented using a vector
and traversing the filename from beginning toend, adding the previous piece of the directory to the new one. For example, if the filename is `a/b/c/file.txt`, then the client will create `a`, then `a/b/`, finally `a/b/c/`.
Finally, since `.` is the current client directory, the final file will be in `./a/b/c/file.txt`. If the pathname looked like `../a/b/file.txt` the final directory would be `./../a/b/file.txt`.

If the file already exists, it's deleted (using unlink) and creates it again, writin the contents it just read from the server. It's decided  (arbitrarily ) that the client reads from the server in 4096-byte chunks. If the blocksize is smaller,
there's no issue. The client will only read as much as needed with each read call (up to 4096), until it's read `filesize` bytes in total and then break, so that it no longer blocks on `read`.

When the client has finally copied the file, it reduces the filecount it's expecting to read. When that reaches 0, it shuts down the socket on its end.

