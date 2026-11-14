# NetBolt

NetBolt is an independent TCP throughput tester that speaks the iperf 3.x
control protocol. It supports client and server operation, parallel TCP
streams, and reverse mode; it does not implement UDP or bidirectional tests.
The project is not affiliated with or endorsed by the iperf3 project.

## Targets

| Executable | Platform | Network stack |
|---|---|---|
| `NETBOLTD.EXE` | MS-DOS / FreeDOS, 8088+ | mTCP and a packet driver |
| `NETBOLT16.EXE` | Windows 3.1 / WfW 3.11 | Winsock 1.1 |
| `NETBOLT32.EXE` | Windows 95 / 98 / ME | Winsock 1.1 |
| `NETBOLTN.EXE` | Windows NT 4.0 / 2000 | Winsock 2.2 |
| `NETBOLT64.EXE` | x64 Windows | Winsock 2.2 |

Build all targets with `docker compose run --rm build`, or choose `dos`,
`win16`, `win9x`, `winnt` or `win64` as the make target. Binaries are written
under `build/<platform>/bin/`.

## Usage

On DOS, load a packet driver and configure mTCP through `MTCPCFG`, then run
`NETBOLTD` for the interactive UI. It also accepts:

```text
NETBOLTD -c 192.168.1.10
NETBOLTD -c 192.168.1.10 -t 30 -P 2 -R
NETBOLTD -s
NETBOLTD -c 192.168.1.10 -b > LOG.TXT
```

The Windows builds provide a GUI for client and server modes. Use the same
TCP port (5201 by default) on both peers. Windows 9x uses the ANSI Winsock 1.1
backend; NT/2000 and x64 use Winsock 2.2.

## Development and licensing

`common/` contains the protocol engine, `dos/` the DOS UI and mTCP backend,
`win/` the Windows UI and Winsock backend, and `tests/` protocol regression
helpers. `sh src/netbolt/tests/run-host.sh` tests the protocol engine against
an installed iperf3 peer. The shared test container includes the required
compiler and peer binary.

The NetBolt source is covered by the repository MIT license. The DOS executable
links mTCP, which retains its GPLv3 license; the build image fetches its license
alongside the sources.
