FROM ubuntu:latest

# "Unminimize" to get closer to standard Ubuntu environment
RUN apt-get update -y && DEBIAN_FRONTEND=noninteractive apt-get install -y unminimize && rm -rf /var/lib/apt/lists/* && yes | unminimize

# Install basic packages
RUN apt install -y man-db make gcc iproute2 python3 vim 
RUN apt install -y telnet tcpdump iputils-ping iperf

# Copy code to container
COPY . /app/proj1
WORKDIR /app/proj1
