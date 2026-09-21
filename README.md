# CS 2520: Project 1 

This repository contains the code for Project 1 and instructions for
testing under emulated network conditions using Docker.

## Docker setup

We have provided a `Dockerfile` that can be used to set up a Docker image with
all of the necessary dependencies.

To build the image, run:
```
docker build . -t "cs2520_proj1"
```

We will test your projects using a Docker [bridge
network](https://docs.docker.com/network/network-tutorial-standalone/). To
create a bridge network named `cs2520`, you can run (on your host, not inside a
container):
```
docker network create --driver bridge cs2520
```

Then, to create and interactively run two containers `rcv` and `ncp` that both
connect to the bridge, you can run (in two separate terminal windows):
```
docker run -it --cap-add=NET_ADMIN --name rcv --network cs2520 cs2520_proj1
docker run -it --cap-add=NET_ADMIN --name ncp --network cs2520 cs2520_proj1
```

With this setup, you can use the names of the containers (i.e. `rcv` and
`ncp`) as the hostnames for communication. Alternatively, you can find the
IP address for each container by running `ip addr` in the container and then
use the IP addresses.

Note that the `NET_ADMIN` capability is needed to emulate LAN and WAN
characteristics using the netem tool (see next section).

## Network emulation

Here we provide instructions for emulating the network conditions under which
you will test your programs. You can find more detailed information in the man
pages for [tc](https://man7.org/linux/man-pages/man8/tc.8.html) and
[netem](https://man7.org/linux/man-pages/man8/tc-netem.8.html)

### Emulating Bandwidth Constraints

For this exercise, you will tune your protocols to a network with 100Mbps
bandwidth (you are encouraged to experiment with other bandwidths to better
understand how your protocol tuning interacts with the network bandwidth, but
your submitted code must be tuned to 100Mbps).

To emulate 100Mbps bandwidth between your docker containers, you must run the
following commands on **each** container:
```
tc qdisc add dev eth0 root handle 1: htb default 3
tc class add dev eth0 parent 1: classid 1:3 htb rate 100Mbit
```

Note: you may see a message `Warning: sch_htb: quantum of class 10003 is big.
Consider r2q change.` This is ok.

To test that the bandwidth constraint was added successfully, you can use the
`iperf` tool. For example, from your `rcv` container, run:
```
iperf -s
```

And then, from your `ncp` container, run:
```
iperf -c rcv
```

This will send TCP traffic from the `ncp` container to the `rcv` container. You
should see that the throughput reported is slightly less than 100Mbps. When you
are done, you can use ctrl-c to kill the iperf server on the `rcv` container.

### Emulating Delay

For your LAN experiments, you do not need to emulate any network propagation
delay. For your WAN experiments, you should emulate a delay of 20ms on the link
between your containers. To do this you should use the following command, for
WAN experiments only (to be run after the commands for bandwidth emulation
above):
```
tc qdisc add dev eth0 parent 1:3 handle 3: netem limit 100000 delay 20ms
```

To test that the delay was added successfully, you can use the `ping` tool. For
example, from your `rcv` container, you can run:
```
ping ncp
```

You should see a delay of about 40ms.

### Removing Emulated Bandwidth Constraints and Delay

To remove all emulated network characteristics, you can use the command:
```
tc qdisc del dev eth0 root
```

## Test file generation

For your experiments, you should copy a 100-Megabyte file.

You can use the provided python program to generate an appropriate text file as follows:
```
python3 gen_file.py -f test100MB.txt -n 100000000
```

Your program should work with both text and binary files. You can generate a
random 100-Megabyte binary file as follows:
```
python3 gen_file.py -f test100MB.txt -n 100000000 -b
```

Note that you may find it useful to test your program with smaller files to
start with.

## Docker cleanup

When you are done, remove both containers:
```
docker rm rcv
docker rm ncp
```

And remove the network:
```
docker network rm cs2520
```
