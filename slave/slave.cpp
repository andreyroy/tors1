#include <arpa/inet.h>
#include <chrono>
#include <cstring>
#include <iostream>
#include <netinet/in.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

struct Task {
  double start;
  double end;
  double step;
};

bool createUDPSocket(int &udpSocket) {
  udpSocket = socket(AF_INET, SOCK_DGRAM, 0);
  if (udpSocket < 0) {
    std::cerr << "Ошибка: не удалось создать UDP сокет" << std::endl;
    return false;
  }

  int reuseOption = 1;
  if (setsockopt(udpSocket, SOL_SOCKET, SO_REUSEADDR, &reuseOption,
                 sizeof(reuseOption)) < 0) {
    std::cerr << "Ошибка: не удалось установить опцию SO_REUSEADDR"
              << std::endl;
    close(udpSocket);
    return false;
  }

  return true;
}

bool bindUDPSocket(int udpSocket, int udpPort) {
  struct sockaddr_in socketAddress = {};
  socketAddress.sin_family = AF_INET;
  socketAddress.sin_addr.s_addr = INADDR_ANY;
  socketAddress.sin_port = htons(udpPort);

  if (bind(udpSocket, reinterpret_cast<struct sockaddr *>(&socketAddress),
           sizeof(socketAddress)) < 0) {
    std::cerr << "Ошибка: не удалось привязать UDP сокет к порту" << std::endl;
    close(udpSocket);
    return false;
  }

  return true;
}

void handleIncomingMessage(int udpSocket, struct sockaddr_in &senderAddress,
                           socklen_t senderAddressLength) {
  char receiveBuffer[256];

  std::cout << "Ожидание broadcast сообщения..." << std::endl;
  ssize_t receivedBytes =
      recvfrom(udpSocket, receiveBuffer, sizeof(receiveBuffer) - 1, 0,
               reinterpret_cast<struct sockaddr *>(&senderAddress),
               &senderAddressLength);

  if (receivedBytes > 0) {
    receiveBuffer[receivedBytes] = '\0';
    std::cout << "Получено сообщение: " << receiveBuffer << std::endl;

    if (strcmp(receiveBuffer, "PING") == 0) {
      const char *responseMessage = "OK";
      sendto(udpSocket, responseMessage, strlen(responseMessage), 0,
             reinterpret_cast<struct sockaddr *>(&senderAddress),
             senderAddressLength);
      std::cout << "Отправлен ответ: OK" << std::endl;
    }
  }
}

void PingHandler(int udpPort) {
  int udpSocket;

  if (!createUDPSocket(udpSocket)) {
    return;
  }

  if (!bindUDPSocket(udpSocket, udpPort)) {
    return;
  }

  struct sockaddr_in senderAddress;
  socklen_t senderAddressLength = sizeof(senderAddress);

  while (true) {
    handleIncomingMessage(udpSocket, senderAddress, senderAddressLength);
  }

  close(udpSocket);
}

double calculateIntegral(const Task &task) {
  double integralResult = 0.0;
  for (double x = task.start; x < task.end; x += task.step) {
    integralResult += (x * x) * task.step; // integral x^2
  }
  sleep(20);
  return integralResult;
}

bool createTCPSocket(int &serverSocket) {
  serverSocket = socket(AF_INET, SOCK_STREAM, 0);
  if (serverSocket < 0) {
    std::cerr << "Ошибка: не удалось создать TCP сокет" << std::endl;
    return false;
  }
  int reuseOption = 1;
  if (setsockopt(serverSocket, SOL_SOCKET, SO_REUSEADDR, &reuseOption,
                 sizeof(reuseOption)) < 0) {
    std::cerr << "Ошибка: установка опции SO_REUSEADDR не удалась" << std::endl;
    close(serverSocket);
    return false;
  }
  return true;
}

bool bindAndListenSocket(int serverSocket, int port) {
  struct sockaddr_in serverAddress = {};
  serverAddress.sin_family = AF_INET;
  serverAddress.sin_addr.s_addr = INADDR_ANY;
  serverAddress.sin_port = htons(port);

  if (bind(serverSocket, reinterpret_cast<struct sockaddr *>(&serverAddress),
           sizeof(serverAddress)) < 0) {
    std::cerr << "Ошибка: не удалось привязать TCP сокет к порту" << std::endl;
    close(serverSocket);
    return false;
  }

  if (listen(serverSocket, 5) < 0) {
    std::cerr << "Ошибка: функция listen() не удалась" << std::endl;
    close(serverSocket);
    return false;
  }
  return true;
}

void processClientTask(int clientSocket) {
  Task receivedTask;
  ssize_t bytesRead =
      recv(clientSocket, &receivedTask, sizeof(receivedTask), 0);

  if (bytesRead == sizeof(Task)) {
    std::cout << "Получена задача: start=" << receivedTask.start
              << ", end=" << receivedTask.end << ", step=" << receivedTask.step
              << std::endl;

    double computationResult = calculateIntegral(receivedTask);
    std::cout << "Вычислен результат: " << computationResult << std::endl;

    send(clientSocket, &computationResult, sizeof(computationResult), 0);
    std::cout << "Результат отправлен клиенту" << std::endl;
  }
}

void TaskHandler(int port) {
  int serverSocket;

  if (!createTCPSocket(serverSocket)) {
    return;
  }

  if (!bindAndListenSocket(serverSocket, port)) {
    return;
  }

  while (true) {
    struct sockaddr_in clientAddress = {};
    socklen_t clientAddressLength = sizeof(clientAddress);

    int clientSocket = accept(
        serverSocket, reinterpret_cast<struct sockaddr *>(&clientAddress),
        &clientAddressLength);
    if (clientSocket < 0) {
      std::cerr << "Ошибка: не удалось принять соединение" << std::endl;
      continue;
    }

    processClientTask(clientSocket);
    close(clientSocket);
  }

  close(serverSocket);
}

int main() {
  int udp_port = 1488;
  int tcp_port = 1489;

  std::thread discovery_thread(PingHandler, udp_port);
  std::thread task_thread(TaskHandler, tcp_port);

  discovery_thread.join();
  task_thread.join();

  return 0;
}