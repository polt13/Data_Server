
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
#include <cstdint>
#include <queue>
#include <dirent.h>
#include <map>
#include <fcntl.h>
#include <iostream>
#include <sys/stat.h>
#include <stdint.h>
#include <arpa/inet.h>
#include <signal.h>
#include <errno.h>
int port = 12900, threadcount = 4, blocksize = 512;
size_t qsize = 100;

int _socket;

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

// identical as the previous, only kept to semantically differentiate the
// directory handler from the file object in the queue
struct dir_handler {
  int socket_target;
  std::string dir;
  dir_handler(std::string dir, int socket_target) {
    this->socket_target = socket_target;
    this->dir = dir;
  }
};

std::queue<file_item> dir_entries;

pthread_t* worker_threads = new pthread_t[threadcount];

// queue access
pthread_cond_t emptyq = PTHREAD_COND_INITIALIZER;

pthread_cond_t fullq = PTHREAD_COND_INITIALIZER;

pthread_mutex_t q_access_mut = PTHREAD_MUTEX_INITIALIZER;

// for picking mutex
pthread_mutex_t avail_mutex_mut = PTHREAD_MUTEX_INITIALIZER;

// socket and how many files it needs
pthread_mutex_t client_files_mut = PTHREAD_MUTEX_INITIALIZER;
std::map<int, int> client_files;

// socket fd mapped to a mutex
std::map<int, pthread_mutex_t*> socket_to_mutex;

void* count_files(void* arg) {
  // big enough to hold the value so that compiler doesnt complain

  dir_handler* hndlr = (dir_handler*)arg;

  std::string curr_dir(hndlr->dir);

  int target_sock = hndlr->socket_target;

  // free allocated directory handler
  delete hndlr;

  DIR* dir = opendir(curr_dir.c_str());
  if (!dir) {
    perror("opening directory");
    exit(EXIT_FAILURE);
  }

  dirent* _item;

  while (_item = readdir(dir)) {
    std::string item_name(_item->d_name);
    if ((item_name != "..") && (item_name != ".")) {
      if (_item->d_type == DT_DIR) {
        dir_handler* _hndlr =
            new dir_handler(curr_dir + "/" + item_name, target_sock);
        count_files((void*)_hndlr);
      } else {
        if (pthread_mutex_lock(&client_files_mut)) {
          perror("mutex lock");
          exit(EXIT_FAILURE);
        }
        client_files[target_sock]++;
        if (pthread_mutex_unlock(&client_files_mut)) {
          perror("mutex unlock");
          exit(EXIT_FAILURE);
        }
      }
    }
  }

  closedir(dir);
  return NULL;
}

void* read_directory(void* arg) {
  // big enough to hold the value so that compiler doesnt complain

  dir_handler* hndlr = (dir_handler*)arg;

  std::string curr_dir(hndlr->dir);

  int target_sock = hndlr->socket_target;

  // free allocated directory handler
  delete hndlr;

  std::cout << "[" << pthread_self() << "]"
            << ": about to scan directory " << curr_dir << '\n';

  DIR* dir = opendir(curr_dir.c_str());
  if (!dir) {
    perror("opening directory");
    exit(EXIT_FAILURE);
  }

  dirent* _item;

  while (_item = readdir(dir)) {
    std::string item_name(_item->d_name);
    if ((item_name != "..") && (item_name != ".")) {
      if (_item->d_type == DT_DIR) {
        dir_handler* _hndlr =
            new dir_handler(curr_dir + "/" + item_name, target_sock);
        read_directory((void*)_hndlr);
      } else {
        // get exclusive queue access
        if (pthread_mutex_lock(&q_access_mut)) {
          perror("mutex lock");
          exit(EXIT_FAILURE);
        }

        // avoid spurious wakeup
        while (dir_entries.size() == qsize) {
          std::cout << "queue full\n";
          pthread_cond_wait(&fullq, &q_access_mut);
        }
        struct stat finfo;
        std::string file_name = curr_dir + "/" + item_name;
        stat(file_name.c_str(), &finfo);
        std::cout << "[" << pthread_self() << "]"
                  << " : Adding file " << file_name << " to queue \n";
        dir_entries.push(file_item(file_name, target_sock, finfo.st_size));
        // signal that something is in queue
        pthread_cond_signal(&emptyq);
        if (pthread_mutex_unlock(&q_access_mut)) {
          perror("mutex unlock");
          exit(EXIT_FAILURE);
        }
      }
    }
  }

  closedir(dir);
  return NULL;
}

void* manage_dir(void* arg) {
  char dir[4096];
  intptr_t client_socket = (intptr_t)arg;

  if (read(client_socket, dir, 4096) < 0) {
    perror("reading dir name");
    // exit(EXIT_FAILURE);
  }

  // remove trailing forw slash if it exists
  if (dir[strlen(dir) - 1] == '/') {
    dir[strlen(dir) - 1] = '\0';
  }

  // do two passes - one to get file count in the directory, then add to queue
  // info about which directory to read and which socket the files go to
  dir_handler* dir_h = new dir_handler(std::string(dir), client_socket);
  dir_handler* dir_h2 = new dir_handler(std::string(dir), client_socket);

  count_files(dir_h);

  // safe to write the file count without expecting response. the client knows
  // exactly how many bytes its expecting to read
  if (pthread_mutex_lock(&client_files_mut)) {
    perror("mutex lock");
    exit(EXIT_FAILURE);
  }
  uint32_t fcount = htonl(client_files[client_socket]);

  if (pthread_mutex_unlock(&client_files_mut)) {
    perror("mutex unlock");
    exit(EXIT_FAILURE);
  }

  if (write(client_socket, &fcount, sizeof(fcount)) < 0) {
    perror("writing file count");
    // exit(EXIT_FAILURE);
  }

  // add files to queue so worker threads wakeup
  read_directory(dir_h2);

  pthread_detach(pthread_self());

  return NULL;
}

void* write_file(void* arg) {
  char* buffer = new char[blocksize];
  while (1) {
    if (pthread_mutex_lock(&q_access_mut)) {
      perror("mutex lock");
      exit(EXIT_FAILURE);
    }
    // avoid 'spurious wakeups'
    while (dir_entries.empty()) {
      pthread_cond_wait(&emptyq, &q_access_mut);
    }

    file_item file_info = dir_entries.front();
    std::string filename = file_info.name;
    int socket_targ = file_info.socket_target;
    dir_entries.pop();
    pthread_cond_signal(&fullq);

    std::cout << "[" << pthread_self() << "]"
              << " : Received task from queue <name: " << filename
              << ", socket: " << socket_targ << ">\n";

    if (pthread_mutex_unlock(&q_access_mut)) {
      perror("mutex unlock");
      exit(EXIT_FAILURE);
    }

    if (pthread_mutex_lock(&avail_mutex_mut)) {
      perror("mutex lock");
      exit(EXIT_FAILURE);
    }

    pthread_mutex_t* socket_locker;

    // if socket isn't bound to a mutex
    if (socket_to_mutex.find(socket_targ) == std::end(socket_to_mutex)) {
      socket_locker = new pthread_mutex_t;
      pthread_mutex_init(socket_locker, NULL);
      socket_to_mutex[socket_targ] = socket_locker;
    } else {
      // get mutex to which the socket is bound to
      socket_locker = socket_to_mutex[socket_targ];
    }

    if (pthread_mutex_unlock(&avail_mutex_mut)) {
      perror("mutex unlock");
      exit(EXIT_FAILURE);
    }

    if (pthread_mutex_lock(socket_locker)) {
      perror("mutex lock");
      exit(EXIT_FAILURE);
    }

    const char* x = file_info.name.c_str();

    uint32_t fname_len = htonl(strlen(x) + 1);

    if (write(socket_targ, &fname_len, sizeof(fname_len)) < 0) {
      perror("writing file name");
      // exit(EXIT_FAILURE);
    }

    if (write(socket_targ, x, strlen(x) + 1) < 0) {
      perror("writing file name");
      // exit(EXIT_FAILURE);
    }

    uint32_t sz = htonl(file_info.fsize);

    if (write(socket_targ, &sz, sizeof(sz)) < 0) {
      perror("writing file size");
      // exit(EXIT_FAILURE);
    }

    int target_file = open(file_info.name.c_str(), O_RDONLY);

    if (target_file < 0) {
      perror("opening target file in worker");
      exit(EXIT_FAILURE);
    }

    std::cout << "[" << pthread_self() << "]"
              << " : about to read file " << filename
              << " for socket: " << socket_targ << "\n";

    int bytes_read;
    while ((bytes_read = read(target_file, buffer, blocksize)) > 0) {
      if (write(socket_targ, buffer, bytes_read) < 0) {
        // dont kill server if client exits
        perror("error writing file contents");
        // exit(EXIT_FAILURE);
      }
    }

    close(target_file);

    char done[10];
    if (read(socket_targ, done, 10) < 0) {
      perror("read FIN");
      // exit(EXIT_FAILURE);
    }

    if (pthread_mutex_unlock(socket_locker)) {
      perror("mutex unlock");
      exit(EXIT_FAILURE);
    }

    if (pthread_mutex_lock(&client_files_mut)) {
      perror("mutex lock");
      exit(EXIT_FAILURE);
    }
    if (pthread_mutex_lock(&avail_mutex_mut)) {
      perror("mutex lock");
      exit(EXIT_FAILURE);
    }

    if (--client_files[socket_targ] == 0) {
      client_files.erase(socket_targ);
      pthread_mutex_destroy(socket_to_mutex[socket_targ]);
      delete socket_to_mutex[socket_targ];
      socket_to_mutex.erase(socket_targ);
      close(socket_targ);
    }
    if (pthread_mutex_unlock(&avail_mutex_mut)) {
      perror("mutex unlock");
      exit(EXIT_FAILURE);
    }
    if (pthread_mutex_unlock(&client_files_mut)) {
      perror("mutex unlock");
      exit(EXIT_FAILURE);
    }
  }

  return NULL;
}

int main(int argc, char* argv[]) {
  struct sigaction sact;
  // dont terminate the server process if client stops midway through writing
  sact.sa_handler = SIG_IGN;
  sigfillset(&sact.sa_mask);
  sigaction(SIGPIPE, &sact, NULL);

  for (int i = 1; i < argc - 1; ++i) {
    if (strcmp(argv[i], "-p") == 0) {
      port = atoi(argv[i + 1]);
    } else if (strcmp(argv[i], "-s") == 0) {
      threadcount = atoi(argv[i + 1]);
    } else if (strcmp(argv[i], "-q") == 0) {
      qsize = atoi(argv[i + 1]);
    } else if (strcmp(argv[i], "-b") == 0) {
      blocksize = atoi(argv[i + 1]);
    }
  }

  for (int i = 0; i < threadcount; ++i) {
    pthread_create(worker_threads + i, NULL, write_file, NULL);
  }

  sockaddr_in server, client;

  sockaddr *serverptr = (sockaddr*)&server, *clientptr = (sockaddr*)&client;
  socklen_t clientlen = sizeof(client);

  // create socket
  if ((_socket = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
    perror("socket creation");
    exit(EXIT_FAILURE);
  }

  server.sin_family = AF_INET;
  server.sin_addr.s_addr = htonl(INADDR_ANY);
  server.sin_port = htons(port);

  if (bind(_socket, serverptr, sizeof(server)) < 0) {
    perror("binding to port");
    exit(EXIT_FAILURE);
  }

  if (listen(_socket, 750) < 0) {
    perror("listening to connections");
    exit(EXIT_FAILURE);
  }

  std::cout << "Listening for connections @ port : " << port << '\n';

  while (1) {
    intptr_t client_socket = accept(_socket, clientptr, &clientlen);
    std::cout << "Accepted connection from "
              << inet_ntoa(((sockaddr_in*)clientptr)->sin_addr) << "\n";
    if (client_socket < 0) {
      perror("accepting connection");
      exit(EXIT_FAILURE);
    }
    if (pthread_mutex_lock(&client_files_mut)) {
      perror("mutex lock");
      exit(EXIT_FAILURE);
    }
    client_files[client_socket] = 0;

    if (pthread_mutex_unlock(&client_files_mut)) {
      perror("mutex unlock");
      exit(EXIT_FAILURE);
    }

    pthread_t commthread;
    // read filename, count files and add to queue
    pthread_create(&commthread, NULL, manage_dir, (void*)client_socket);
  }
}