# Why Zenoh, and Where: A Position on Messaging Fabrics for Robot Fleets and IO-MCU Systems

*Position paper — draft. Author: Glenn Edgar. Status: working draft for review.*

## Abstract / Thesis

Most robot-fleet messaging guidance defaults to MQTT because it is mature, broker-based,
and ubiquitous. That default is wrong for the systems described here. Our workload is not
fire-and-forget topic streaming; it is **stateful discovery and query** — "what robots
exist, what state are they in, what is each one's last value" — layered over live telemetry
and commands. For that workload **Zenoh is the better fabric than MQTT**, because it unifies
publish/subscribe with query and storage and gives first-class namespace discovery. It is
also the direction the robotics field itself is moving: ROS 2 is migrating its middleware
off DDS toward Zenoh for exactly these reasons.

This paper argues a second, sharper point: **the messaging fabric belongs on Linux.**
Microcontrollers should not run a discovery/pub-sub stack. They should speak a small,
deterministic field-bus protocol and reach the fabric through a **bridge agent** at the
Linux edge. We support both claims with two production systems: a Linux farm-robot fleet
that uses Zenoh as its internal fabric, and an RS-485 IO-MCU bus that connects to that
fabric through a Linux Zenoh agent — having explicitly tried, and rejected, running Zenoh
on the microcontroller itself.

We are not anti-MQTT. MQTT remains the right protocol at the *external* boundary, where
dashboards, analytics, and third-party consumers subscribe. The position is about the
*internal* fabric and the *embedded* edge — the two places where the MQTT default fails us.

---

## 1. MQTT and its limits for stateful fleets

MQTT is a publish/subscribe protocol built around a central **broker**. Clients connect to
the broker, publish to hierarchical topics (`fleet/robot7/position`), and subscribe with
two wildcards: `+` (exactly one level) and `#` (the remaining levels, tail only). It offers
three QoS levels (at-most-once, at-least-once, exactly-once), a **retained message** (the
broker keeps the last message on a topic for late subscribers), and a last-will message for
disconnect detection. It is simple, battle-tested, has clients in every language and on
nearly every microcontroller, and an enormous operational ecosystem.

For streaming telemetry to a dashboard, MQTT is hard to beat. The trouble starts when the
workload is **stateful and discoverable** rather than streaming:

- **No query.** MQTT can only deliver messages as they are published (or one retained
  message per topic). There is no way to *ask* "what is the current state of every robot of
  class X" in one operation. You approximate it with retained messages plus a subscription
  and then reassemble state client-side — re-implementing a database over a message bus.
- **No storage.** The broker is a router, not a store (retained messages aside, which are a
  single value per topic with no history and no query surface).
- **Positional, tail-only wildcards.** `+`/`#` match by position; `#` only matches the
  tail. Cross-cutting queries like "the `heartbeat` leaf of *every* robot regardless of
  class/instance depth" are awkward or impossible to express cleanly.
- **The broker is mandatory infrastructure.** Every message traverses a central broker:
  a single point of failure, a scaling bottleneck, and a service you must deploy and operate
  *as platform infrastructure*. For an encapsulated robot that should be self-contained, a
  mandatory external broker breaks the model.

None of these are bugs; they are the consequence of MQTT being a *topic-streaming* protocol.
Our fleet workload wants discovery, last-value query, and storage as first-class operations.
That is the gap the next section fills.

---

## 2. Zenoh: discovery, query, and storage in one fabric

Zenoh (Zero Overhead Network Protocol) unifies three primitives that MQTT splits or omits:
**publish/subscribe**, **query** (`get`), and **storage**. A value `put` to a key can be
streamed to subscribers *and* retained in a storage *and* returned to a later `get` — one
fabric, one key space.

Key differences that matter for this workload:

- **Key-expression wildcards.** Keys are slash-separated like MQTT topics, but the wildcards
  are stronger: `*` matches exactly one segment and `**` matches **zero or more** segments
  *anywhere* in the key. `**/heartbeat` finds the heartbeat leaf of every node at any depth;
  `class/*/state` finds one leaf across all instances. Discovery is a wildcard, not a
  bookkeeping exercise.
- **Query + storage = last-value semantics for free.** Stateful leaves (a robot's `state`,
  `desired_state`, `capabilities`) live in a memory-backed storage. A late joiner `get`s the
  current value; there is no retained-message dance and no client-side state reassembly.
- **Brokerless *or* routed.** Zenoh peers can discover and talk to each other directly
  (peer-to-peer), or route through a `zenohd` router. The router, when used, can be
  **application-scoped** — shipped inside the controller, not run as shared platform
  infrastructure. There is no mandatory central broker.
- **An embedded profile.** `zenoh-pico` is a small C implementation of the same protocol for
  microcontrollers, so in principle the *same* key space and operations reach down to a
  32 KB chip. (Section 5 argues we should *not* take that bait — but the capability exists.)

**Honest counter-weight.** Zenoh is younger and far less ubiquitous than MQTT; the
operational ecosystem (managed brokers, tooling, StackOverflow surface) is thinner. We have
also hit real sharp edges in practice: the `zenoh-pico` pub/sub path silently dropped
multi-KB payloads until we switched to publish-per-sample (never blob); and our token-hash
client binding (FNV1a-32 key hashes, not key-expression strings) **cannot** subscribe to
wildcards at the robot client at all — wildcard discovery has to happen at the gateway layer.
A position paper has to own these. They are the cost of a less-trodden path; the rest of the
paper argues the cost is worth paying for the systems we actually build.

> **The reframe.** "Zenoh vs MQTT" is not really a head-to-head for the same slot. The
> position is *layered*: Zenoh as the **internal** fabric (this section's strengths), MQTT
> and HTTP at the **external** boundary (this section's ubiquity), and **no pub/sub fabric at
> all** on the MCU (Section 5). Each protocol where it is strongest.

---

## 3. External validation: why ROS adopted Zenoh

The strongest evidence that this is not an idiosyncratic preference is that ROS — the
dominant robotics framework — reached the same conclusion. ROS 2 abstracts its transport
behind a middleware interface (RMW) and historically defaulted to **DDS** (Data Distribution
Service) implementations such as Fast DDS and Cyclone DDS. DDS is a capable, standardized
pub/sub middleware, but ROS 2 deployments repeatedly hit the same walls — the same ones
Section 1 raises about message-bus middleware for fleets:

- **Discovery does not scale.** DDS uses distributed, all-to-all participant discovery. As
  the number of nodes and robots grows, discovery traffic and per-node bookkeeping explode;
  large multi-robot systems suffer discovery storms and slow, fragile startup.
- **It does not route.** DDS is designed for a single LAN multicast domain. Getting ROS 2
  across WiFi, subnets, NAT, or the internet requires extra machinery (Discovery Servers,
  tunnels) and is brittle on lossy links — exactly the network a real robot fleet lives on.
- **Configuration is heavy.** DDS QoS matrices and XML profiles are notoriously intricate,
  and getting them wrong is a classic source of "nodes can't see each other" failures.

Zenoh addresses these directly, and not by accident: it comes from **ZettaScale**, the company
that DDS veteran Angelo Corsaro (formerly a leader in the DDS world) spun out of DDS research.
Zenoh began as a telecom/5G protocol and was then taken up by robotics, where its scaling and
networking behavior turned out to be exactly what DDS lacked. Robotics adoption came in two
stages:

1. **`zenoh-bridge-ros2dds`** (the `zenoh-plugin-ros2dds`) — a bridge that carries ROS 2 / DDS
   traffic over Zenoh to connect robots across WiFi, subnets, and the internet. It was the
   practical answer for multi-robot and cloud-connected ROS 2 *before* any native integration,
   and proved Zenoh's routing and lossy-link behavior in real deployments.
2. **`rmw_zenoh`** — a native, non-DDS ROS 2 middleware that runs ROS on Zenoh under the same
   RMW interface, so existing ROS 2 code runs unchanged. This was not a side experiment: Open
   Robotics' official **"ROS 2 Alternative Middleware Report"** evaluated the candidates,
   **selected Zenoh** as best meeting the requirements (and the most user-recommended), and
   **Intrinsic's open-source robotics team took on building it**. It is now an official `ros2`
   project, and **Eclipse Zenoh ships in ROS 2 binary releases beginning with Kilted Kaiju
   (2025)**. It pairs each ROS node with a Zenoh session and routes discovery through a Zenoh
   router's *gossip* — with multicast disabled by default, deliberately sidestepping the
   "misconfigured networks, operating systems, or containers" that dog DDS multicast discovery
   (its own design doc's words).

Two honest qualifiers: Zenoh is **not** the ROS 2 default — Fast DDS still is — so this is the
officially-selected *alternative*, not a replacement of DDS. But the direction is clear and the
gains are concrete: ROS users have reported **discovery-traffic reductions of 97–99% versus
DDS**.

The reasons ROS chose Zenoh are the reasons this paper argues for it: **efficient, routable
discovery; working over real networks (WiFi, subnets, internet); far simpler configuration;
and unified pub/sub + query + storage** that maps cleanly onto ROS's topics, services, and
actions. When the official robotics ecosystem evaluates its middleware options and selects
Zenoh as the alternative to DDS, a fleet builder choosing Zenoh for the same reasons is on
well-trodden ground — even where the broader IoT default is still MQTT.

---

## 4. Zenoh in farm robot systems (the internal Linux fabric)

Our farm-robot fleet ("fleet_design", running live as the LaCima irrigation analytics
controller) uses Zenoh as a **strictly internal** fabric. The external world never speaks
Zenoh; it speaks HTTP / MQTT / NATS / KB feeds through gateway processes. Inside the
controller, Zenoh is the connective tissue between a handful of single-purpose processes.

**The encapsulation model.** Each robot owns a namespace `<class>/<instance>/` and publishes
its leaves *flat* under it — no `telemetry/`, `status/`, `commands/` sub-trees:

```
irrigation_analytics/lacima/state          ← lifecycle state
irrigation_analytics/lacima/heartbeat      ← liveness
irrigation_analytics/lacima/<telemetry...> ← live values
irrigation_analytics/lacima/desired_state  ← what it should be doing
```

Cross-cutting access is a **wildcard on the leaf name**: `**/heartbeat` is every live robot;
`<class>/*/state` is every instance's state. Class is firmware-defined; instance is
operator-assigned at commissioning; hardware UID is metadata, not identity. This is the
microservices-with-private-databases / ports-and-adapters pattern, not a ROS-style shared
namespace — internals are private, and the only public surface is the gateway's curated API.

**Why Zenoh specifically here, and not MQTT:**

- **Discovery is the design.** The whole model rests on wildcard discovery and last-value
  query of stateful leaves. Zenoh does this natively; MQTT would force retained-message
  hacks plus client-side state assembly (Section 1).
- **The router is not infrastructure.** `zenohd` ships *inside* the controller container as
  an application-scoped router. A robot is self-contained; it does not depend on a
  platform-level broker to function.
- **Stateful + streaming in one place.** `state`/`desired_state`/`capabilities` are queried;
  telemetry is streamed; events are published — all one key space, with write authority
  split *by leaf* (the robot owns telemetry/state; the fleet manager owns `desired_state`;
  gateways own inbound command leaves).
- **The boundary is enforced by gateways.** "Publish apps" subscribe to internal Zenoh and
  re-expose a stable external contract (HTTP for the dashboard, and MQTT/NATS for other
  consumers). This is exactly where MQTT belongs — at the edge, not the core.

**What it buys in production.** The live irrigation controller runs ~10 analysis processes
("KBs") that poll a legacy controller and publish per-sample results into Zenoh; a
persistence service subscribes and writes SQLite; an application gateway serves the operator
dashboard over HTTP. New analysis processes attach by publishing under the namespace — no
schema rebuild, no broker reconfiguration, no central catalog edit. The encapsulation +
discovery model is what makes that additive.

---

## 5. Zenoh ↔ IO MCUs (the edge-bridge pattern)

The second system is the inverse case: **connecting the Linux Zenoh fabric to
microcontroller IO** — sensors, actuators, interlocks on small chips. The naive answer,
which Zenoh's `zenoh-pico` profile invites, is "run Zenoh on the MCU." **We tried that
direction (an early four-chip-dongle design that put a `zenoh-pico` C agent on the chip) and
rejected it.** The position here is the strongest in the paper because it is a lived
decision, not an assertion.

**The architecture we settled on (the `slow_bus` project):**

```
  clients ──TCP──► zenohd ──TCP──► zenoh_agent ──UDP/USB──► Pico W BC ──RS-485──► slaves
  └────────── Zenoh control plane (all TCP) ──────────┘ └──── libcomm, NOT Zenoh ────┘
```

- The MCU layer is a **deterministic 9-bit RS-485 multidrop bus** at a fixed 460000 baud,
  single-master polled, with granted peer windows. Timing is bounded and predictable. The
  chips speak **libcomm** frames (SLIP framing + CRC-8, `OP_SHELL_EXEC`/`OP_SHELL_REPLY`).
- A Linux **Zenoh agent** terminates the fabric. It is a `zenoh-pico` *client* over **TCP**
  to a `zenohd` router (multicast scouting disabled), exposing the bus as Zenoh keys:
  an RPC server on `slow_bus/bc/cmd`, plus pub/sub — it `publish`es batched bus feedback to
  `slow_bus/bc/feedback` and `subscribe`s commands from `slow_bus/bc/ctl`.
- **`zenoh-pico` is never on the MCU.** The chip streams the *same* libcomm frames whether
  the uplink to the agent is USB-CDC or WiFi/UDP — only the transport under it changes,
  behind a `bus_uplink.h` seam.

**The agent is a protocol bridge, not a packet relay.** This is the subtle, important part:
there is no "TCP-to-UDP translation" in the gateway sense. The agent is a full endpoint on
both sides and maps *operations*:

- **RPC** (TCP request → libcomm `exec` over UDP → reply): a Zenoh request `{op:…}` becomes
  one blocking libcomm command-frame to the bus controller; the reply is encoded back over
  Zenoh. One-in-flight blocking makes request/reply correlation trivial.
- **Feedback** (libcomm frame over UDP → Zenoh publish): the controller emits batched
  per-cycle feedback asynchronously; the agent parses it and publishes to `…/feedback` for
  fan-out. (It listens for these even while an RPC is mid-flight, so async data is never
  dropped behind a pending reply.)
- **Control** (Zenoh publish → libcomm inject over UDP, fire-and-forget): clients publish to
  `…/ctl`; the agent drains and injects into the controller's producer path; results return
  asynchronously via the feedback path.

The two transports (Zenoh-over-TCP north, libcomm-over-UDP/USB south) are terminated
independently and never tunnelled into each other.

**Why this layering, and why not Zenoh-on-chip:**

1. **Right protocol at the right layer.** The real-time IO layer wants *determinism* — a
   polled field bus with bounded timing. A discovery/pub-sub stack on the MCU adds an IP
   stack, dynamic membership, and non-deterministic timing the IO loop cannot afford.
2. **Footprint and focus.** Keeping the MCU on a small libcomm codec leaves its flash/RAM
   and its cores for the application and safety interlocks, not for a network fabric.
3. **One bridge serves the whole bus.** A single Linux agent exposes every slave on the bus
   as Zenoh keys; the fleet sees `slow_bus/bc/*` and the bus/peer complexity stays hidden.
   Heterogeneous slaves (e.g. master-only SAMD21 chips alongside peer-capable Picos)
   interoperate via the *bus contract*, not via Zenoh.
4. **The transport is swappable for free.** USB-CDC on the bench, WiFi/UDP in the field —
   same libcomm stream, same bridge logic. The fabric does not care how the bytes arrive.

The general principle: **terminate the fabric at the Linux edge; give the embedded layer a
deterministic bus and a thin bridge.** Zenoh's reach-to-the-chip capability is real, but the
right boundary is one hop higher. (This edge-bridge pattern — and the broader case for never
doing low-level I/O on the application processor at all — is developed in the companion
paper, *I/O Belongs on Microcontrollers, Reached as Services*, where the Zenoh agent here is
generalized into USB/WiFi proxy containers over the same deterministic bus.)

---

## 6. Recommendation — which fabric, where

| Layer | Use | Why |
|---|---|---|
| Robot-fleet **internal** fabric (Linux) | **Zenoh** | discovery + query + storage as first-class ops; app-scoped router; encapsulated namespaces; the way ROS 2 is going |
| **External** boundary (dashboards, analytics, third parties) | **MQTT / HTTP / NATS** via gateways | ubiquity, mature tooling, stable public contract; protocol-per-consumer |
| **Embedded IO** layer (MCUs) | **Deterministic field bus + Linux bridge agent** — *no pub/sub fabric on the chip* | bounded real-time timing, small footprint, one bridge per bus |

Concretely:

- **Default to Zenoh for the internal fabric** when the workload is stateful discovery and
  query, not just telemetry streaming — which is most fleet-control workloads. Accept the
  younger ecosystem and bind the sharp edges (publish-per-sample, wildcards at the gateway).
- **Keep MQTT at the edge,** not the core. It is the right *external* contract; it is the
  wrong *internal* fabric for this workload.
- **Never run the messaging fabric on a microcontroller.** Use a deterministic bus and a
  Linux edge agent that bridges *operations* (RPC/publish/subscribe) to *frames*
  (exec/inject/feedback). We tried Zenoh-on-chip and the edge-bridge is strictly better for
  real-time IO.

The unifying idea is not "Zenoh everywhere." It is **the right protocol at each layer, with
a clean seam between layers** — Zenoh internal, MQTT/HTTP external, deterministic bus at the
embedded edge — which is exactly what these two production systems, and the wider ROS 2
migration, demonstrate.
