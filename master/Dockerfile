FROM ubuntu:latest

RUN apt-get update && apt-get install -y \
    build-essential \
    iputils-ping \
    iproute2 \
    net-tools \
    iptables

WORKDIR /app


COPY . .


RUN g++ -o master master.cpp -lm -pthread


CMD ["./master", "0", "10", "0.0001"]
