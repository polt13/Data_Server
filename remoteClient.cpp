
#include <signal.h>
#include <ctype.h>
#include <stdlib.h>
#include <netdb.h>
#include <unistd.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <stdio.h>
#include <cstring>
#include <string>
#include <pthread.h>
#include <fcntl.h>
#include <iostream>
#include <vector>
#include <stdint.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <errno.h>

char ip[40] = "127.0.0.1", dir[4096] = "test";

int port = 12900, _socket;

struct file_item {
  int socket_target;
  std::string name;
  size_t fsize;
  file_item(std::string name, int socket_target, size_t fsize) {
    this->socket_target = socket_target;
    this->name = name;
    this->fsize = fsize;
  }
};

int main(int argc, char* argv[]) {
  for (int i = 1; i < argc - 1; ++i) {
    if (strcmp(argv[i], "-p") == 0) {
      port = atoi(argv[i + 1]);
    } else if (strcmp(argv[i], "-i") == 0) {
      strcpy(ip, argv[i + 1]);
    } else if (strcmp(argv[i], "-d") == 0) {
      strcpy(dir, argv[i + 1]);
    }
  }

  sockaddr_in server;

  in_addr ipstruct;

  if (inet_aton(ip, &ipstruct) == 0) {
    perror("converting ip");
    exit(EXIT_FAILURE);
  }
  sockaddr* serverptr = (sockaddr*)&server;
  hostent* rem =
      gethostbyaddr((const char*)&ipstruct, sizeof(ipstruct), AF_INET);

  if (rem == NULL) {
    perror("get host");
    exit(EXIT_FAILURE);
  }

  // create socket
  if ((_socket = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
    perror("socket creation");
    exit(EXIT_FAILURE);
  }

  server.sin_family = AF_INET;
  memcpy(&server.sin_addr, rem->h_addr, rem->h_length);
  server.sin_port = htons(port);

  if (connect(_socket, serverptr, sizeof(server)) < 0) {
    perror("connecting from client");
    exit(EXIT_FAILURE);
  }
  if (write(_socket, dir, strlen(dir) + 1) < 0) {
    perror("write to socket from client");
    exit(EXIT_FAILURE);
  }

  uint32_t fcount;

  if (read(_socket, &fcount, sizeof(fcount)) < 0) {
    perror("reading filename\n");
    exit(EXIT_FAILURE);
  }

  fcount = ntohl(fcount);

  std::cout << "Files " << fcount << '\n';

  while (fcount > 0) {
    char fname[4096];
    uint32_t fname_length;

    if (read(_socket, &fname_length, sizeof(fname_length)) < 0) {
      perror("reading filename size\n");
      exit(EXIT_FAILURE);
    }

    fname_length = ntohl(fname_length);

    if (read(_socket, fname, fname_length) < 0) {
      perror("reading filename\n");
      exit(EXIT_FAILURE);
    }

    char fname_copy[4096];
    strcpy(fname_copy, fname);

    std::vector<std::string> directories;
    char* t = strtok(fname_copy, "/");
    // make a vector of all directories leading to file
    // and then remove the last element (its the file name)
    while (t) {
      directories.push_back(std::string(t));
      t = strtok(NULL, "/");
    }

    // remove filename itself from the vector, only keep directories

    directories.pop_back();

    while (directories.empty() == false) {
      std::string dir_to_make = directories.front();
      DIR* dirc = opendir(dir_to_make.c_str());
      // if directory doesnt exist
      if (dirc == NULL) {
        if (mkdir(dir_to_make.c_str(), S_IRWXU | S_IRWXG | S_IRWXO) == -1) {
          if (errno != EEXIST) {
            perror("creating directory in client side");

            exit(EXIT_FAILURE);
          }
        }
      }
      // remove directory from vector
      closedir(dirc);
      directories.erase(directories.begin());
      // prefix the next directory with the current directory
      // so if we have a/b, vector has {a,b},
      // first we get a, then make the next element {a/b}
      if (directories.empty() == false)
        directories[0] = dir_to_make + "/" + directories[0];
    }

    // read file size to know when to stop the while loop that follows
    uint32_t filesize;

    if (read(_socket, &filesize, sizeof(filesize)) < 0) {
      perror("reading filename\n");
      exit(EXIT_FAILURE);
    }

    filesize = ntohl(filesize);

    // // delete file if it exists
    unlink(fname);

    std::string outputdir;
    if (fname[0] == '/') {  // absolute path
      outputdir = "." + std::string(fname);
    } else {  // relative path
      outputdir = "./" + std::string(fname);
    }

    // if file exists, unlink first
    unlink(outputdir.c_str());

    // create file
    int clientfile_fd = open(outputdir.c_str(), O_WRONLY | O_CREAT);

    char buff[4096];
    int n, bytes_read = 0;
    while ((n = read(_socket, buff, 4096)) > 0) {
      write(clientfile_fd, buff, n);
      bytes_read += n;

      if (bytes_read >= filesize) break;
    }

    if (write(_socket, "FIN", strlen("FIN") + 1) < 0) {
      perror("ok_3 from client\n");
      exit(EXIT_FAILURE);
    }

    std::cout << "Received " << outputdir << ", size: " << filesize
              << " bytes\n";

    close(clientfile_fd);

    fcount--;
  }

  close(_socket);
}