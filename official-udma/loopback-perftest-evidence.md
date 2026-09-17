# Official UDMA loopback perftest evidence

Validation date: 2026-09-17

This checkpoint uses the unmodified official OLK UDMA driver and stock UMDK
provider in one gem5 full-system guest. Two independent `urma_perftest`
processes exercise separate UMMU translation contexts through the same
modeled `udma0` device.

Server:

```sh
urma_perftest send_lat -d udma0 --eid_idx 0 --ctp -s 128 \
  -P 21115 -J 1 -I 128 -n 5 --enable_imm
```

Client:

```sh
urma_perftest send_lat -d udma0 -S 127.0.0.1 --eid_idx 0 --ctp \
  -s 128 -P 21115 -J 1 -I 128 -n 5 --enable_imm
```

Both processes returned normally. The client reported:

```text
bytes  iterations  t_min[us]  t_max[us]  t_median[us]  t_avg[us]  t_stdev[us]
128    5           2001.05    3999.71    3999.09       3499.74    865.27
```

The server reported:

```text
bytes  iterations  t_min[us]  t_max[us]  t_median[us]  t_avg[us]  t_stdev[us]
128    5           2791.82    4006.58    3600.56       3499.88    521.11
```

The numbers are recorded only to prove completed samples; this checkpoint is
a functional gate and does not calibrate latency. The model trace shows:

- server resources use UMMU TID 8 and client resources use TID 9, even though
  the two processes map their queues at the same IOVAs;
- direct-SQE doorbells are consumed for Jettys 1024 and 1025;
- five 128-byte receives complete in each direction;
- both `DEACTIVE_TP (0x23)` responses are returned; and
- Jetty flush queries complete, allowing normal resource destruction and
  process exit.

Local detailed evidence is retained in `run-official-ummu-v35-model.log` and
`run-official-ummu-v35-20260917/`. Generated run directories and logs are
intentionally excluded from Git.

This is not the two-node G6 result. It validates the official stack up to a
single simulated device's process-to-process loopback boundary; the next gate
is transport between two independent gem5 full-system machines.
