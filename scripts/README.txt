Scripts here use gnuplot(1) to generate charts from data gathered
by h1load tool. The scripts assume the h1load was running with '-l'
option.

The list of columns h1load shows on its output is as follows:

    time        - number of seconds since starting (current system time,
                  when running with '-l')

    conns       - maximum number of concurrent connections recorded since
                  last second

    tot_conn    - number of total connections since test has started

    tot_req     - number of finished requests since test has started

    err         - number of errors since test has started

    cps         - connections per second, instant average since last sample
                  record (since last second)

    rps         - request per second, instant average since last sample
                  record (since last second), note connection can handle
                  more than one request

    Bps         - bytes per second since last record (second)

    bps         - bits per second since last record (second)

    ttlb        - time to last byte, average since last record

    ttfb        - time to first byte, average since last record

There is also a "long long" output format ('-ll'), which is easier to consume
from scripts because it doesn't use human readable units but full values only.
It also adds or changes the following columns compared to the '-l' format:

    time        - the system's time, in seconds since epoch, at the moment the
                  line is printed. It is convenient when collecting values from
                  multiple systems as it allows to merge measures by date.

    thr         - throttling ratio: indicates the amount of the configured load
                  that is currently being injected, between 0 and 100%. This is
                  expected to slowly ramp up from 0 to 100 when '-s' is used.
                  It will also fall down to zero at the end of the test when a
                  duration was configured. The value is averaged over the
                  measurement period, which means that lines showing a value of
                  100 were measured entirely under nominal conditions and are
                  usable to perform various other calculations.

All other fields in this format are reported in bits/bytes per second, or
microseconds for the timings.

h1load client can spawn `n` threads (-t `n` option, only single, when
no -t provided). When using more than singe thread, then it is important
to specify -c option (-c `x`, number of concurrent connections) such
x` % `n` is zero (number of connections is divided by number threads with
no reminder), so workload is spread evenly among the threads.

Note although option -d `secs` specifies test duration period it
does not necessarily mean the exact runtime is going to last `secs`.
The actual run time may well exceed the period specified by -d option
The option defines period for which the h1load creates new requests,
as soon as all requests are cent out the tool continues to run and
waits fro responses. This phase when tool waits for requests to
finish processing may distort overall average stats, it's users
call to decide to include or discard the numbers from the 'flame out'
phase after client sends out its all requests. This is similar to
the start of the test (a.k.a. warm up phase), when bulk of concurrent
connections is being created. Again user needs to consider to include
those results to overall stats or not.
