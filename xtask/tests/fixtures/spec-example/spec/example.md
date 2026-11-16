# Example

A made-up domain that shows every part of the spec format (`spec/README.md`). The `xtask` tests run
tracey and the format check on it. Its rules describe nothing Beamer does.

## Zones

r[example.zone.leads-to-peer]
A zone leads to exactly one peer.

r[example.zone.push+2]
A push against a zone crosses to the zone's peer once the push pressure passes the threshold, and
not before.

> r[example.zone.held-at-edge]
> While push pressure builds, the pointer is held at edge.
>
> Moving the pointer away from the zone drops the push pressure to zero.
>
> - tier: machine (push pressure comes only from the real capture path)
> - parity: Python crossed on the first pixel of the edge, with no push pressure (#11 keeps push pressure, to stop accidental crossings)

## Driving

> r[example.driving.move-number+3]
> A take carrying a move number lower than the driver's current one is refused.
>
> - model: driving

> r[example.driving.landing-point]
> After a crossing, the landing point is where the pointer met the zone, scaled to the driven
> machine's display.
>
> - tier: human (whether the pointer appears where the eye expects is judged at the desk)

## Frames

> r[example.frame.decode]
> A take frame that cannot be decoded is dropped, and the link stays up.
>
> - fuzz: example-frame
