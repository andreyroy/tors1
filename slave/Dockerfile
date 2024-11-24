FROM ubuntu:latest

RUN apt-get update && apt-get install -y \
    build-essential \
    iputils-ping \
    iproute2 \
    net-tools \
    iptables

WORKDIR /app


COPY . .


RUN g++ -o slave slave.cpp -pthread -lm


CMD ["./slave"]
