# LMU5541 Direct Messging
The new Asgard X host app performs direct JSON reporting from the LMU to the server over HTTP or WebSocket. HTTPS/WSS are supported via TLS (OpenSSL 3.5.4 LTS, statically linked). The LMU can use a local CA certificate or the system bundle at `/etc/ssl/certs/ca-certificates.crt`. DNS resolution is supported for both transports.

The **HTTP(S)** transport currently supports GET and POST. It implements the following error handling and retry logic:

- HTTP errors: Retries on 5xx and 429.
- Network errors: Retries on DNS failures, connection failures, and TLS/I/O errors.
- Timeouts: Retries when a request does not complete within conn_timeout_ms.
- Retry strategy:

  1. Exponential backoff: `delay = base * 2^attempt`

  2. Jitter: randomized within `±retry_jitter_pct%` to avoid retry storms

  3. Attempts: total `attempts = 1 (initial try) + retry_max_attempts`

- Scheduling: a worker thread maintains a due-time–ordered retry queue; attempts are reissued when due

- Concurrency guard: a global max_inflight limit (default 16) prevents overload

The **WebSocket** transport supports text frames and PING/PONG control frames, and handles the following conditions:

- Automatic reconnection: When the connection is lost, the client attempts to reconnect using exponential backoff with jitter to avoid retry storms.

- Keepalive:

  1. The host app periodically sends PING frames

  2. The host app responds to server PING with PONG

  3. Any inbound frame (TEXT/PING/PONG) is treated as activity and resets the reconnection timer

## Input state change report
Once a GPIO input state has changed, a report will be send by **POST** method to the Asgard X server. The message will be in JSON as follows:
```json
{
  "esn": "5572042715",
  "uuid": "ca3745bc-9f79-414b-bbc7-104ed03c41da",
  "timestamp": 1739165847,
  "event": "input",
  "payload": {
    "p1": {
      "value": 1,
      "count": 123
    },
    "p2": {
      "value": 0,
      "count": 123
    }
    ... // any other ports if their values changed
  },
  "location": {
    "lat": -37.9467853,
    "lon": 145.0799433,
    "alt": 60.77,
    "speed": 0.5,
    "course": 8
  }
}
```
Where:
- `esn`: The ESN of the LMU sending the report.
- `uuid`: The UUID of the LMU sending the report.
- `timestamp`: The timestamp of the event in Unix epoch format.
- `event`: The type of event, in this case, it is always "input".
- `payload`: An object containing the input states, where the keys are the input port numbers and the values (HIGH=1, LOW=0)

> For greater flexibility, input ports on the LMU are no longer hard-coded to function-specific names (e.g., “BinLift”).

## Regular GPS report
The LMU will send a regular GPS report every 1 seconds to the Asgard X server. The message will be in JSON as below:
```json
{
  "esn": "5572042715",
  "uuid": "ca3745bc-9f79-414b-bbc7-104ed03c41da",
  "timestamp": 1739165847,
  "event": "gps",
  "payload": {
    "lat": -37.9467853,
    "lon": 145.0799433,
    "alt": 60.770,
    "speed": 0.5,
    "course": 8
  }
}
```
## Configuration updates (future)

Remote configuration is not currently supported, but may be added in a future release. When enabled:

- HTTP: the LMU will periodically poll the Asgard X server for configuration updates.

- WebSocket: the server can push configuration messages to the LMU in real time.

Configuration messages may include:

- GPIO input debounce time

- GPIO output state

- RFID read/write parameters

- RFID read/write commands

- GPS reporting frequency

- Other settings as required

Example request URL:
```
https://xxxx/xxx?req=gpio_clr
```
Expected response
```json
{
  "esn": "5572042715",
  "uuid": "ca3745bc-9f79-414b-bbc7-104ed03c41da",
  "timestamp": 1739165847,
  "event": "gpio_clr",
  "payload":{
    "p0":false,
    "p1":true,   // Default bin-lift
    "P2":true,   // Default reverse
...
    "p7":false
  }
}
```