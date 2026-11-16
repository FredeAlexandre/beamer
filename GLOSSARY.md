# Beamer

Beamer shares one keyboard, pointer, clipboard, sound, camera and set of displays across several
computers. This glossary fixes the words the Rust rewrite's spec, code and agents use for it.

## Machines and links

**Machine**:
One install of Beamer, identified by its own key pair.
_Avoid_: host, computer, node, side

**Machine id**:
A short name for a machine, derived from its public key, so only the key's holder can claim it.
_Avoid_: uuid, device id

**Peer**:
A machine this one has paired with and whose public key it has pinned.
_Avoid_: partner, remote, client, server

**Pairing**:
The one-time exchange, proved by a six-digit code, after which two machines each hold the other's
public key.
_Avoid_: bonding, trusting, linking

**Link**:
The one encrypted connection between two peers. It carries everything between them, in both
directions.
_Avoid_: connection, session, channel, initiator, responder

**Traffic class**:
One of input, control, media and bulk, in that order of priority. A class never waits behind a
class below it.
_Avoid_: channel, lane, priority

## Shared state

**Cluster**:
Every machine reachable from this one over links, directly or through other machines.
_Avoid_: mesh, network, group

**Store**:
The replicated state of which every machine in the cluster holds a copy.
_Avoid_: document, flood list, shared settings

**Machine record**:
A store entry that only its own machine writes, such as its devices, displays, capabilities or
paired list.
_Avoid_: device list, announcement

**Shared entry**:
A store entry that any machine may write, where the newest stamp wins, such as a route, a display's
owner or a shared setting.
_Avoid_: route document, setting
