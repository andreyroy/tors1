#include <algorithm>
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <future>
#include <iostream>
#include <mutex>
#include <netinet/in.h>
#include <pthread.h>
#include <queue>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#include <vector>

#define SERVERS_CNT 4
#define UDP_PORT 1488
#define TCP_PORT 1489
#define TIMEOUT 2

struct Task {
  double start;
  double end;
  double step;
};

struct Server {
  struct sockaddr_in addr;
  bool active;

  bool operator==(const Server &other) const {
    return addr.sin_addr.s_addr == other.addr.sin_addr.s_addr;
  }
};

class ServerList {
private:
  std::vector<Server> servers;
  mutable std::mutex mutex;

public:
  ServerList() = default;

  void addServer(const Server &server) {
    std::lock_guard<std::mutex> lock(mutex);

    auto it = std::find(servers.begin(), servers.end(), server);

    if (it != servers.end()) {
      it->active = true;
      return;
    }

    if (servers.size() >= SERVERS_CNT) {
      return;
    }

    servers.push_back(server);
    std::cout << "Добавлен новый сервер: " << inet_ntoa(server.addr.sin_addr)
              << ":" << ntohs(server.addr.sin_port) << std::endl;
  }

  size_t size() const {
    std::lock_guard<std::mutex> lock(mutex);
    return servers.size();
  }

  Server &operator[](size_t index) {
    std::lock_guard<std::mutex> lock(mutex);
    return servers[index];
  }
};

ServerList serverList;

void FindLivingServers() {
  std::cout << "\nПоиск серверов..." << std::endl;

  int udpSocket = socket(AF_INET, SOCK_DGRAM, 0);
  if (udpSocket < 0) {
    throw std::runtime_error("Ошибка: невозможно создать сокет");
  }

  try {
    int enableBroadcast = 1;
    if (setsockopt(udpSocket, SOL_SOCKET, SO_BROADCAST, &enableBroadcast,
                   sizeof(enableBroadcast)) < 0) {
      throw std::runtime_error(
          "Ошибка: невозможно установить режим broadcast на сокет");
    }

    struct sockaddr_in broadcastAddress = {};
    broadcastAddress.sin_family = AF_INET;
    broadcastAddress.sin_port = htons(UDP_PORT);
    broadcastAddress.sin_addr.s_addr = INADDR_BROADCAST;

    const std::string ping_msg = "PING";
    if (sendto(udpSocket, ping_msg.c_str(), ping_msg.size(), 0,
               reinterpret_cast<struct sockaddr *>(&broadcastAddress),
               sizeof(broadcastAddress)) < 0) {
      throw std::runtime_error(
          "Ошибка: не удалось отправить broadcast-сообщение");
    }

    struct timeval receiveTimeout = {TIMEOUT, 0};
    if (setsockopt(udpSocket, SOL_SOCKET, SO_RCVTIMEO, &receiveTimeout,
                   sizeof(receiveTimeout)) < 0) {
      throw std::runtime_error(
          "Ошибка: не удалось установить таймаут на сокет");
    }

    std::vector<char> responseBuffer(256);
    struct sockaddr_in serverResponseAddress = {};
    socklen_t responseAddressLength = sizeof(serverResponseAddress);

    while (true) {
      int receivedBytes = recvfrom(
          udpSocket, responseBuffer.data(), responseBuffer.size() - 1, 0,
          reinterpret_cast<struct sockaddr *>(&serverResponseAddress),
          &responseAddressLength);

      if (receivedBytes < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
          break;
        }

        std::cerr << "Ошибка: не удалось получить данные (" << strerror(errno)
                  << ")" << std::endl;
        continue;
      }

      responseBuffer[receivedBytes] = '\0';

      serverResponseAddress.sin_port = htons(UDP_PORT);
      Server discoveredServer{serverResponseAddress, true};
      serverList.addServer(discoveredServer);
    }
  } catch (const std::exception &e) {
    close(udpSocket);
    throw;
  }

  close(udpSocket);
}

double executeTask(Server &server, const Task &task) {
  std::cout << "Отправка задачи на сервер " << inet_ntoa(server.addr.sin_addr)
            << " " << task.start << " - " << task.end << ", шаг: " << task.step
            << ")" << std::endl;

  int clientSocket = socket(AF_INET, SOCK_STREAM, 0);
  if (clientSocket < 0) {
    server.active = false;
    throw std::runtime_error("Ошибка создания сокета");
  }

  sockaddr_in serverAddress = server.addr;
  serverAddress.sin_port = htons(TCP_PORT);

  int socketFlags = fcntl(clientSocket, F_GETFL, 0);
  fcntl(clientSocket, F_SETFL, socketFlags | O_NONBLOCK);

  int connectionStatus =
      connect(clientSocket, reinterpret_cast<sockaddr *>(&serverAddress),
              sizeof(serverAddress));
  if (connectionStatus < 0 && errno != EINPROGRESS) {
    server.active = false;
    close(clientSocket);
    throw std::runtime_error("Ошибка подключения");
  }

  fd_set socketSet;
  FD_ZERO(&socketSet);
  FD_SET(clientSocket, &socketSet);
  timeval connectionTimeout = {TIMEOUT, 0};

  connectionStatus = select(clientSocket + 1, nullptr, &socketSet, nullptr,
                            &connectionTimeout);
  if (connectionStatus <= 0) {
    server.active = false;
    close(clientSocket);
    throw std::runtime_error("Ошибка подключения: таймаут или сеть");
  }

  int socketError;
  socklen_t errorLength = sizeof(socketError);
  getsockopt(clientSocket, SOL_SOCKET, SO_ERROR, &socketError, &errorLength);
  if (socketError != 0) {
    server.active = false;
    close(clientSocket);
    throw std::runtime_error("Ошибка подключения");
  }

  fcntl(clientSocket, F_SETFL, socketFlags);

  if (send(clientSocket, &task, sizeof(Task), 0) < 0) {
    server.active = false;
    close(clientSocket);
    throw std::runtime_error("Ошибка отправки");
  }

  double result;
  char *resultBuffer = reinterpret_cast<char *>(&result);
  size_t receivedBytes = 0;
  size_t expectedBytes = sizeof(result);

  while (receivedBytes < expectedBytes) {
    fd_set readSet;
    FD_ZERO(&readSet);
    FD_SET(clientSocket, &readSet);

    timeval readTimeout = {TIMEOUT, 0};
    int readyToRead =
        select(clientSocket + 1, &readSet, nullptr, nullptr, &readTimeout);

    if (readyToRead < 0) {
      server.active = false;
      close(clientSocket);
      throw std::runtime_error("Ошибка сети при ожидании ответа");
    } else if (readyToRead == 0) {
      continue;
    }

    ssize_t readResult = recv(clientSocket, resultBuffer + receivedBytes,
                              expectedBytes - receivedBytes, 0);

    if (readResult < 0) {
      server.active = false;
      close(clientSocket);
      throw std::runtime_error("Ошибка чтения ответа сервера");
    } else if (readResult == 0) {
      server.active = false;
      close(clientSocket);
      throw std::runtime_error(
          "Сервер разорвал соединение до получения полного ответа");
    }

    receivedBytes += readResult;
  }

  std::cout << "Получен результат от сервера "
            << inet_ntoa(server.addr.sin_addr) << ": " << result << std::endl;

  close(clientSocket);
  return result;
}

void assignTasksToServers(
    const std::vector<Task> &tasks, std::queue<size_t> &remainingTasks,
    std::vector<std::pair<size_t, std::future<double>>> &activeFutures) {
  size_t availableServers = serverList.size();

  for (size_t serverIndex = 0;
       serverIndex < availableServers && !remainingTasks.empty();
       ++serverIndex) {
    Server &currentServer = serverList[serverIndex];

    if (!currentServer.active) {
      continue;
    }

    size_t taskIndex = remainingTasks.front();
    remainingTasks.pop();

    activeFutures.push_back(
        {taskIndex, std::async(std::launch::async,
                               [&currentServer, task = tasks[taskIndex]]() {
                                 return executeTask(currentServer, task);
                               })});
  }
}

double collectResultsOrRetryTasks(
    std::vector<std::pair<size_t, std::future<double>>> &activeFutures,
    std::queue<size_t> &remainingTasks, double &accumulatedResult) {
  for (auto &[taskIndex, futureResult] : activeFutures) {
    try {
      double result = futureResult.get();
      accumulatedResult += result;
    } catch (const std::exception &exception) {
      std::cerr << "Ошибка выполнения задачи: " << exception.what()
                << std::endl;
      remainingTasks.push(taskIndex);
      FindLivingServers();
    }
  }
  activeFutures.clear();

  return accumulatedResult;
}

void findNewServersIfNeeded(std::queue<size_t> &remainingTasks) {
  if (!remainingTasks.empty()) {
    std::cout << "Поиск новых серверов для оставшихся задач..." << std::endl;
    FindLivingServers();
  }
}

double Calc(const std::vector<Task> &tasks) {
  double totalResult = 0.0;
  std::vector<std::pair<size_t, std::future<double>>> activeFutures;
  std::queue<size_t> remainingTasks;

  std::cout << "\nРаспределение " << tasks.size() << " задач..." << std::endl;

  for (size_t taskIndex = 0; taskIndex < tasks.size(); ++taskIndex) {
    remainingTasks.push(taskIndex);
  }

  while (!remainingTasks.empty()) {
    size_t remainingCount = remainingTasks.size();
    std::cout << "\nОсталось задач: " << remainingCount << std::endl;

    assignTasksToServers(tasks, remainingTasks, activeFutures);
    collectResultsOrRetryTasks(activeFutures, remainingTasks, totalResult);
    findNewServersIfNeeded(remainingTasks);
  }

  return totalResult;
}

int main(int argc, char *argv[]) {
  try {

    double start = std::stod(argv[1]);
    double end = std::stod(argv[2]);
    double step = std::stod(argv[3]);

    double bucket_size = (end - start) / SERVERS_CNT;

    std::vector<Task> tasks;

    for (double new_start = start; new_start < end; new_start += bucket_size) {
      tasks.emplace_back(Task{new_start, new_start + bucket_size, step});
    }

    std::cout << "Запуск мастера" << std::endl;
    FindLivingServers();

    double result = Calc(tasks);

    std::cout << "\n Интеграл = " << result << std::endl;

  } catch (const std::exception &e) {
    std::cerr << "Ошибка: " << e.what() << std::endl;
    return 1;
  }

  return 0;
}
