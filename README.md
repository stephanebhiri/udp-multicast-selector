# udp-multicast-selector

High-performance UDP multicast input selector with instant switching, written in C.

Listens on multiple input multicast addresses and forwards packets from the **active** input to a single output multicast address. Switching between inputs is instant - the next packet comes from the new source.

## Features

- **Multi-input support**: Up to 8 simultaneous multicast inputs
- **Instant switching**: Zero-latency source switching via Unix socket
- **Zero-copy forwarding**: Bit-exact packet forwarding, no payload transformation
- **Non-blocking I/O**: Uses epoll for high performance
- **Real-time safe**: Drops packets rather than blocking on congestion
- **Statistics**: Per-input and aggregate stats via control socket
- **Graceful shutdown**: SIGINT/SIGTERM handling

## Requirements

- Linux (uses epoll)
- GCC

## Installation

```bash
make
```

Or compile directly:

```bash
gcc -O2 -o udp-selector udp-selector.c
```

## Usage

### Basic usage

```bash
./udp-selector mystream 239.0.0.1:5000 239.0.1.1:5001
```

This creates a selector with:
- ID: `mystream`
- Output: `239.0.0.1:5000`
- Input 0: `239.0.1.1:5001` (active by default)

### Multiple inputs

```bash
./udp-selector mystream 239.0.0.1:5000 239.0.1.1:5001 239.0.1.2:5002 239.0.1.3:5003
```

### With explicit interface

```bash
./udp-selector mystream 239.0.0.1:5000 239.0.1.1:5001 239.0.1.2:5002 --interface 127.0.0.1
```

## Control Socket

The selector creates a Unix socket at `/tmp/udp-selector-<id>.sock` for runtime control.

### Commands

| Command | Description |
|---------|-------------|
| `switch:<N>` | Switch to input N (0-indexed) |
| `status` | Show statistics |
| `quit` | Graceful shutdown |

### Examples

```bash
# Switch to input 1
echo "switch:1" | nc -U /tmp/udp-selector-mystream.sock

# Switch back to input 0
echo "switch:0" | nc -U /tmp/udp-selector-mystream.sock

# Get status
echo "status" | nc -U /tmp/udp-selector-mystream.sock

# Shutdown
echo "quit" | nc -U /tmp/udp-selector-mystream.sock
```

### Status output

```
id: mystream
active_input: 0
output: 239.0.0.1:5000
forwarded_packets: 1234567
forwarded_bytes: 1876543210
output_dropped_congestion: 0
output_send_errors: 0
input[0]: addr=239.0.1.1:5001 packets=1234567 bytes=1876543210 errors=0 last_pkt=0s [ACTIVE]
input[1]: addr=239.0.1.2:5002 packets=1234560 bytes=1876500000 errors=0 last_pkt=0s
```

## Architecture

```
Input 0 (239.0.1.1:5001) ──┐
                           │
Input 1 (239.0.1.2:5002) ──┼──► Selector ──► Output (239.0.0.1:5000)
                           │    (forwards
Input N (239.0.1.N:500N) ──┘    active only)
```

- All inputs are received simultaneously
- Only packets from the **active** input are forwarded
- Switching is instant: next packet from new source
- Inactive inputs still receive (for stats and instant switch)

## Performance

- **epoll-based**: Efficient event-driven I/O
- **2MB socket buffers**: Handles traffic bursts
- **Non-blocking output**: Drops packets on congestion (never blocks)
- **Throttled error logging**: Max 1 error log per second
- **Strict multicast bind**: Binds to multicast address (not 0.0.0.0) to prevent cross-group interference when multiple groups share the same port

## Use Cases

- **Redundant feeds**: Switch between primary and backup sources
- **A/B testing**: Compare different encoders or sources
- **Live production**: Hot-swap sources without stream interruption
- **Failover**: Manual or scripted failover between inputs

## Signals

| Signal | Action |
|--------|--------|
| SIGINT | Graceful shutdown |
| SIGTERM | Graceful shutdown |
| SIGPIPE | Ignored |

## License

MIT License
