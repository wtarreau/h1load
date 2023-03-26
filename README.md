# h1load

h1load is a simple yet efficient HTTP/1 load generator. Just like its ancestor
"[inject](https://github.com/wtarreau/inject/)" it focuses on real-time
monitoring of the traffic conditions (concurrent connections, request rate,
bit rate, response time). As explained in the [dpbench project](https://github.com/dpbench/dpbench/tree/main/howtos)
these are critical to make sure that what is being measured matches what is
intended to be measured. Reporting just an average at the end of a test means
nothing at all, and test results must not be kept if variations or anomalies
are observed during the test session.

##  Basic features

h1load supports the following features:

- HTTP/1.1 traffic with keep-alive and chunked encoding support
- SSL support ( tested with [OpenSSL](https://github.com/openssl/openssl)
  and [wolfSSL](https://github.com/wolfssl/wolfssl) )
- multi-threading support to distribute the load to multiple CPU cores
- response time measurement
- progressive ramp-up
- think time support to simulate more realistic users
- ability to limit the request rate
- optional realtime report of status codes distribution
- time-to-first-byte and time-to-last-byte percentiles
- long and long long report format with absolute dates for merging from
  multiple load generators
- support both HEAD and GET
- zero-copy data drain when supported by the OS

Contrary to its ancestor "inject", cookies, POST and multiple-URLs are not
supported right now.

## Building

By default, running "make" (or "gmake" on BSD systems) will make the program
with OpenSSL support for Linux. h1load requires epoll as the poller, but there
exists an efficient wrapper for operating systems supporting kqueue:
[epoll-shim](https://github.com/jiixyj/epoll-shim). This wrapper was tested
on FreeBSD and shown to deliver the expected performance levels.

Building on Linux with OpenSSL support:
```
make
```

Building on Linux without OpenSSL support:
```
make USE_SSL=0
```

Building on Linux with an alternate OpenSSL library installed in $HOME/openssl111:
```
make SSL_IFLAGS="-I$HOME/openssl111/include"  SSL_LFLAGS="-L$HOME/openssl111/lib -lssl -lcrypto"
```

Building on Linux with the wolfSSL library installed in $HOME/local:
```
make SSL_CFLAGS="-I$HOME/local/include/wolfssl -I$HOME/local/include -include $HOME/local/include/wolfssl/options.h" SSL_LFLAGS="-L$HOME/local/lib -lwolfssl -Wl,-rpath=$HOME/local/lib"
```

Building on FreeBSD with epoll-shim:
```
gmake USR_CFLAGS="-I/usr/local/include/libepoll-shim" USR_LFLAGS="-L/usr/local/lib -lepoll-shim"
```

## Issues caused by OpenSSL 3.0

There are some design issues affecting OpenSSL 3.0 that cause extreme
performance degradation particularly in client mode, compared to version
1.1.1. Some performance losses of 200x were measured in field! A number of
them are documented on the OpenSSL issue tracker, with a particular impact
of locking as documented [here](https://github.com/openssl/openssl/issues/20286)
and [here](https://github.com/openssl/openssl/issues/17627). OpenSSL 3.1
partially addresses these issues but remains far behind 1.1.1 (in the order
of 10x slower).

In short OpenSSL 3.x is simply not usable at all with load generators, which
means that on modern distros such as Ubuntu 22 or RHEL 8, an alternate SSL
library absolutely needs to be installed for performance testing. If openssl
1.1.1 is available in a compatible package, it can be used as shown above.
Otherwise [wolfSSL](https://github.com/wolfssl/wolfssl) is light, builds fast
and doesn't suffer from the locking issues that affect OpenSSL so it will
generally show higher performance.

## Usage

The most simple way to use h1load is to run it with a number of concurrent
connections and a URL:
```
h1load -c 100 https://1.2.3.4:4443/page_to_test.html
```
It will then be stopped by pressing Ctrl-C.

**Warning**: never run a load generator on a remote machine without setting a
limit to the number of requests, because if it causes some network saturation,
you may very well lose access and not even be able to stop it!

Limiting the number of requests is done using "-n", it should preferably be
an integral multiple of the number of connections:
```
h1load -c 100 -n 100000 https://1.2.3.4:4443/page_to_test.html
```

For large numbers of connections it's better to use a slow rampup so as
not to exagerate the load beyond the target level on the tested server.
For example, if a server routinely runs at 10k concurrent connections, it's
likely that these arrive over several seconds, not all at once in a few
milliseconds. This is particularly important for SSL where key computation
takes time on the server. If desired to test how a server reacts to a live
restart, setting a ramp-up of 3s which corresponds to a TCP retransmit will
usually show realistic conditions of an extreme situation:
```
h1load -c 10000 -s 3 https://1.2.3.4:4443/page_to_test.html
```

When reaching loads of tens of thousands of requests per second and/or
multiple gigabit/s traffic, it's recommended to use multiple threads.
Generally starting as many threads as there are CPUs available gives food
results. It is important however that the number of connections is an
integral multiple of the number of threads so as to keep them evenly
loaded. Example, for a 14-core CPU with 2 threads per core, hence 28
threads, we can have 200 connections per thread:
```
h1load -t 28 -c 5600 -s 5 https://1.2.3.4:4443/
```

Another commonly used options is `-e`, to stop on the first error; it is
very convenient to stop h1load when the server is stopped or crashes:
```
h1load -e -t 28 -c 5600 -s 3 -n 5600000 https://1.2.3.4:4443/
```

If connections are not evenly distributed on the server's threads (which
depends on its accept frontend), it can be useful to force to regularly
close and reopen connections using `-r`. The option is followed by an
argument indicating how many requests will be passed over each connection.
The default "0" is infinite. By passing lower values such as 100, this
means that connections will be closed and reopened every 100 requests,
and can help the servers better redistribute incoming connections. This
will often result in a higher load on the server due to more reconnections,
particularly when using SSL:
```
h1load -e -t 28 -c 5600 -s 3 -n 5600000 -r 100 https://1.2.3.4:4443/
```

## Testing proxies, firewalls or passive network components

Testing proxies or network equipment requires a server. h1load is commonly
combined with [httpterm](https://github.com/wtarreau/httpterm) which is able
to deliver objects of the size requested in the URL. It is particularly
convenient since there's no need to create dummy files on disk, no I/O
limitations caused by file-system access, and it supports configurable
response times (in the request) that allow to test the behavior of the
component when dealing with large numbers of concurrent connections.

## Performance and troubleshooting

h1load was [shown](https://www.haproxy.com/blog/haproxy-forwards-over-2-million-http-requests-per-second-on-a-single-aws-arm-instance/)
to reach 2 million requests per second and 100 Gbps on a single process with
64 threads. Performance will essentially depend on the CPU cache architecture.
As a general rule it's fine to use as many threads per process as are attached
to each L3 cache instance. On multi-socket systems, never run the same process
across multiple sockets, as the kernel-side locking on the file descriptor
table will kill performance. Similarly on certain CPUs which involve multiple
L3 caches, the performance will scale better if separate processes are run over
each L3 cache instance due to the same kernel-side locking issues. See commands
such as "taskset" on Linux to properly bind processes to the desired CPU cores.

It's important to always monitor the CPU usage with "top" and "vmstat" in
parallel. For example if at some point a "ksoftirqd" kernel thread appears
with an important load, it probably means that the network interface queues
are unevenly distributed, and one may need to verify the incoming queue
distribution algorithm using "ethtool" and check how interrupts are assigned.
This is a common concern at loads above 40 Gbps. Always kill the "irqbalance"
daemon if it's running, as it will always cause important performance
degradations and prevent results from being reproduced.

The number of connections will be limited by the process' limit on file
descriptors. A test is usually performed upon startup to make sure that all
requested connections will be allocatable. Please use "ulimit -n" to raise the
value if needed. If not running as root, using "sudo prlimit -nXXX -p$$" can
do the job on Linux.


