# Clipboard

The clipboard follows input: text and PNG images copied on one machine reach the machine the
driver's input is on, and come home with it. It is F15 of #11's inventory, which ships: text and
PNG, both ways, with their size limits.

The rules come from the Python app at `archive/python` (the driver's half in `core/owner.py`,
`mac_app/bridge.py` and `win_app/sender.py`, the driven machine's half in `core/receiver.py`, the
limits in `core/protocol.py`, and the OS clipboards in `mac_app/clipboard_mac.py` and
`win_app/clipboard_win.py`), mined through its tests in `spec/ledger/clipboard.txt`; from the
meaning #14 keeps from `WIRE.md` (the clipboard following input, and the PNG limits); and from
#14's bulk traffic class, in which a newer clipboard cancels an older one still under way.

What stays elsewhere: the take, the let go, the pass on and when driving ends are
`spec/driving.md`'s; how a control or bulk message travels, is cancelled and is limited on the
link is `spec/link.md`'s; how input waiting to be sent merges is `spec/link.md`'s; the
capabilities, and their states, are `spec/platform.md`'s, and a peer's are read from its machine
record (`spec/store.md`).

Not here: the X11 and portal clipboards of the Linux app, which is out of scope (#1).

Every rule is `ci` unless it says otherwise. Translation code (what a PC does with the answers of
the Windows calls that read and write its clipboard) is `ci` on a hosted runner of its own OS
(#15); a rule that only the real OS clipboard can show is `machine`. A clipboard is a machine's
own; what carries its text and image from machine to machine is a clipboard message.

## What a clipboard message carries

> r[clipboard.payload.contents]
> A clipboard message carries text, a PNG image, or both. A machine never sends one with neither,
> and an image copied with no text arrives with no text.
>
> - fuzz: clipboard-payload

> r[clipboard.payload.text-limit]
> A clipboard message's text is at most 256 KiB as UTF-8. A machine never sends longer text, and
> discards longer text it receives.
>
> - fuzz: clipboard-payload

> r[clipboard.payload.image-limit]
> A clipboard message's image is at most 8 MiB. A machine never sends a larger one, and discards a
> larger one it receives.
>
> - parity: Python counted the 8 MiB before base64, and its frames carried the base64 (#14: the bytes of WIRE.md are not kept, so the image travels as its own bytes)
> - fuzz: clipboard-payload

> r[clipboard.payload.part]
> When one part of a clipboard message is over its limit or is not a PNG that fits
> (`clipboard.png.*`), only that part is discarded, and the rest is sent or set as if the message
> had carried it alone. When neither part remains, nothing is sent, or nothing is set.
>
> - fuzz: clipboard-payload

> r[clipboard.payload.not-decoded]
> A machine checks an image against `clipboard.png.*` from its bytes before anything decodes it,
> and never decodes or sets an image that does not fit.
>
> - fuzz: clipboard-payload

## A PNG that fits

These limits keep a PNG of a few megabytes from claiming gigabytes once decoded. An image that
breaks any of them does not fit.

> r[clipboard.png.header]
> An image fits only when it starts with the PNG signature followed at once by an `IHDR` chunk of
> exactly 13 bytes of data.
>
> - fuzz: clipboard-payload

> r[clipboard.png.size]
> An image fits only when its `IHDR` gives a width and a height each from 1 to 16384 pixels, and
> a width times height of at most 2^25 pixels. An 8K display's 7680 by 4320 fits, and so does
> 16384 by 2048; a width of 16385 does not, and nor does 6000 by 6000.
>
> - fuzz: clipboard-payload

> r[clipboard.png.chunks]
> An image fits only when every chunk, each a 4-byte big-endian length, a type, its data and a
> 4-byte CRC, lies wholly inside the image, up to an `IEND` chunk. An image with no `IEND` does
> not fit. Bytes after `IEND` are ignored, and CRCs are not checked.
>
> - fuzz: clipboard-payload

> r[clipboard.png.still]
> An image fits only when it is a still with one header: no second `IHDR`, and no `acTL`, `fcTL`
> or `fdAT` chunk.
>
> - fuzz: clipboard-payload

> r[clipboard.png.metadata]
> An image fits only when its `iCCP` and `zTXt` chunks, and its `iTXt` chunks whose compression
> flag is 1, inflate to at most 4 MiB in all. Each of their zlib streams must be one whole stream
> that ends where its chunk ends, even an empty one: a stream that is not zlib, is cut short or has
> bytes after its end does not fit. Nor does an `iTXt` whose compression flag is neither 0 nor 1,
> or any of the three that lacks the null byte ending its keyword, or, in a compressed `iTXt`,
> the null bytes ending its language and translated keyword. An `iTXt` whose flag is 0 inflates
> nothing.
>
> - fuzz: clipboard-payload

## How it travels

> r[clipboard.travel.control]
> A clipboard message with no image and at most 32 KiB of text as UTF-8, half the largest control
> message (`link.frame.control-limit`), travels as a control message, in order with the input and
> control messages around it (`link.class.order`).
>
> - model: clipboard

> r[clipboard.travel.bulk]
> A clipboard message with an image, or with more than 32 KiB of text, travels as a bulk message
> (`link.class.bulk-stream`).
>
> - parity: Python sent every clipboard message on its one link as a frame of its own, ahead of what followed it (#14: clipboard images and large text go as bulk, on a stream of their own, below every other class)
> - model: clipboard

> r[clipboard.travel.bulk-not-waited]
> Input a driver sends after a clipboard message that travels as a bulk message does not wait for
> it: the driven machine may inject that input before it sets its clipboard from the message.
>
> - parity: Python sent the clipboard ahead of the input on the same link, so input sent after an image waited for the whole image to arrive (#14: bulk is the lowest traffic class, and never delays input)
> - model: clipboard

> r[clipboard.travel.whole]
> A machine sets its clipboard from a clipboard message only once the whole message has arrived.
> Nothing of a bulk clipboard message that was cancelled, by its sender or by its receiver, is ever
> set.
>
> - model: clipboard

> r[clipboard.travel.newer-cancels]
> When a machine starts sending a peer a clipboard message while an earlier one of its to that peer
> is still under way as a bulk message, it cancels the earlier one (`link.class.bulk-cancel`).
>
> - parity: Python could not cancel a clipboard message once it began, and a newer one waited behind it (#14: a bulk stream is reset when a newer clipboard message replaces it)
> - model: clipboard

> r[clipboard.travel.order]
> A machine sets its clipboard from the clipboard messages a peer sent it in the order the peer
> sent them, and never from one after it has set its clipboard from a newer one of that peer's.
>
> - model: clipboard

## Whose clipboard messages are taken

> r[clipboard.expect.from-driver]
> A driven machine takes a clipboard message only from its driver. One from any other peer is
> discarded, and when it travels as a bulk message it is cancelled unread
> (`link.frame.bulk-unexpected`).
>
> - model: clipboard

> r[clipboard.expect.let-go]
> A driver takes a clipboard message from a peer only when it sent that peer a let go no more than
> 10 s before the message's first byte arrived, and only the first such message for each let go.
> Any other clipboard message from a peer is discarded, and when it travels as a bulk message it
> is cancelled unread (`link.frame.bulk-unexpected`).
>
> - model: clipboard

> r[clipboard.expect.began]
> Whether a clipboard message is taken is decided by when its first byte arrived: one that began
> within the 10 s is taken however long it then takes to arrive whole (`link.frame.bulk-deadline`),
> and one that began before the let go is not taken and does not use up the let go's clipboard
> message.
>
> - model: clipboard

> r[clipboard.expect.later-move]
> A move of the input after a let go, a refusal by the next machine included, does not cost the
> machine let go its one clipboard message: it is still taken within the 10 s.
>
> - model: clipboard

> r[clipboard.expect.capability]
> A machine sends its clipboard to a peer only while the peer's machine record reports the peer's
> clipboard capability ready, and sets its clipboard from a peer's clipboard message only while
> its own clipboard capability is ready.
>
> - parity: Python had two capabilities, one for clipboard text and one for clipboard images, and sent each part only to a peer that announced its own (#16: the clipboard is one capability; #14: capabilities are facts in each machine's record)

## The clipboard follows the take

> r[clipboard.take.sent]
> When a driver takes a machine, it sends that machine its clipboard after the take and before
> any input it sends there after the take, unless that machine has its clipboard already
> (`clipboard.take.has`).
>
> - model: clipboard

> r[clipboard.take.has]
> A driver counts a peer as having its clipboard once it has sent the peer its clipboard, or taken
> a clipboard message from the peer or sent one on to it (`clipboard.arrive.send-on`), until a copy
> is made on the driver or the driver takes a clipboard message from another peer. It does not
> send a peer that has its clipboard the clipboard again, so what was copied on one machine and
> brought home reaches the next machine taken, and never returns to the machine it was copied on.
>
> - model: clipboard

> r[clipboard.take.not-had]
> A peer stops counting as having the driver's clipboard when the input comes home before the peer
> accepted its take, refused, unanswered or for any other reason; when a pass on to it is given up
> (`driving.pass-on.given-up`); when its link closes; and when the clipboard could not be read or
> sent to it (`clipboard.read.retry`). The next take of that peer sends the clipboard again.
>
> - model: clipboard

> r[clipboard.take.pass-on]
> When a driver takes a machine because of a pass on and that machine accepts, the driver sends it
> its clipboard after the let go of the machine that passed on and before the queued input
> (`driving.pass-on.accepted`).
>
> - model: clipboard

> r[clipboard.take.pass-on-not-taken]
> A copy made on the driver while a pass on's answer is awaited is sent to no machine until the
> wait ends. When the pass on is not taken (`driving.pass-on.not-taken`), the copy goes to the
> machine that passed on, after the reach update and before the queued input.
>
> - model: clipboard

> r[clipboard.take.never-waits]
> A take never waits for the clipboard: the take is sent at once, and when the clipboard is empty
> or nothing of it can be sent (`clipboard.payload.part`, `clipboard.expect.capability`), the take
> goes alone.
>
> - model: clipboard

> r[clipboard.take.input-waits]
> Input the driver sends to a machine after its clipboard waits until that clipboard has been read
> here and sent, or found to have nothing to send, then goes in the order it was made, merged as
> any waiting input is (`link.input.merge`): pointer moves that waited become one move that keeps
> each of them as a step (`link.input.merge-steps`). Once the clipboard is sent, input goes
> straight through again.
>
> - parity: Python's Mac sent the pointer moves that waited one by one, as made, where its PC merged them (one reading for both platforms: the PC's, with each move kept as a step, which the driven machine's edges and zones take as they take any merged move, `crossing.zone.merged-steps` and `crossing.zone.merged-long`, so a return edge there still feels the moves where its pointer is)
> - model: clipboard

> r[clipboard.take.read-not-in-the-way]
> A clipboard read, however slow, never delays a take, a let go or input to any other peer, nor
> input acting here: while one takes 2 s, the pointer here moves as the user moves it, and a take of
> another peer is sent at once.

> r[clipboard.take.unsendable-once]
> A clipboard read for a peer that has nothing to send, because it is empty, its parts are over
> their limits or the peer's clipboard capability is not ready, counts as sent: the driver does not
> read it again for a later take until a copy is made on it.
>
> - model: clipboard

> r[clipboard.take.link-lost]
> When a peer's link closes while the driver's clipboard is being read for it, the input that waited
> behind the read is not sent, and nor is the clipboard. A take of the peer on its next link sends
> the take and then the clipboard.
>
> - model: clipboard

> r[clipboard.take.after-a-write]
> A clipboard read for a peer starts only after every clipboard write the driver decided before it
> has been made, so what is read is what that write left on the clipboard.

> r[clipboard.take.quit]
> When Beamer quits while the driver's clipboard is being read for the machine its input is on,
> the let go is still sent within 1 s, and the clipboard is not sent.

## Copies on the driver

> r[clipboard.copy.what]
> A copy is any change to a machine's clipboard that Beamer did not make by setting it from a
> peer's clipboard message: the user copying, or any program writing to the clipboard, even the
> same contents again.
>
> - tier: machine (what counts as a change is the real Mac and Windows clipboards')

> r[clipboard.copy.after-write]
> A copy made on a machine just after Beamer set its clipboard from a peer's clipboard message is a
> copy, never part of that write. A write that failed changes nothing about which copies were
> made.
>
> - model: clipboard

> r[clipboard.copy.while-driving]
> While the driver's input is on a machine that has accepted its take, and the driver is not itself
> driven, a copy made on the driver is noticed within 100 ms and sent to that machine. Whether
> Beamer's window is shown makes no difference.
>
> - model: clipboard

> r[clipboard.copy.before-accept]
> A copy made on the driver after it sent a take and before the accept arrives is sent to that
> machine as the accept arrives, before the input that follows.
>
> - model: clipboard

> r[clipboard.copy.at-home]
> A copy made on the driver while its input is home is sent to no machine until a take.
>
> - model: clipboard

> r[clipboard.copy.while-driven]
> A copy made on a machine while it is driven is not sent by it to any machine as a driver: it
> reaches its driver only on the let go (`clipboard.let-go.changed`).
>
> - model: clipboard

## The let go

> r[clipboard.let-go.changed]
> When a driver lets go of a driven machine, that machine sends the driver its clipboard when a
> copy was made on it while that driver drove it, and sends nothing otherwise, so a peer cannot
> collect a machine's clipboard by taking it while idle and letting it go.
>
> - model: clipboard

> r[clipboard.let-go.not-returned]
> A driven machine setting its clipboard from its driver's clipboard message is not a copy: what
> the driver brought is never returned to it on the let go, even when its write finishes after the
> let go.
>
> - model: clipboard

> r[clipboard.let-go.copy-after-write]
> A copy made on a driven machine just after it set its clipboard from its driver's clipboard
> message is a copy, and is sent to the driver on the let go.
>
> - model: clipboard

> r[clipboard.let-go.failed-write]
> A clipboard message from the driver that could not be set does not hide an earlier copy made on
> the driven machine: that copy is still sent on the let go.
>
> - model: clipboard

> r[clipboard.let-go.only-let-go]
> When driving ends any other way than by a let go (`driving.end.when`), the driven machine sends
> its driver no clipboard.
>
> - model: clipboard

> r[clipboard.let-go.unknown]
> When the driven machine cannot tell whether its clipboard changed while the driver drove it,
> because the OS could not say when the clipboard last changed, as driving began or as it ended, it
> sends no clipboard.
>
> - model: clipboard

> r[clipboard.let-go.copy-after-let-go]
> A copy made on the driven machine after the let go is never sent to the driver: when the
> clipboard changes after the let go, before or while it is read for the driver, nothing is sent.
>
> - model: clipboard

> r[clipboard.let-go.new-driver]
> Once the driven machine accepts another take, or the old driver's link closes, nothing more is
> read or sent for the old driver's let go: it never gets the next driver's clipboard.
>
> - model: clipboard

> r[clipboard.let-go.in-order]
> A clipboard message from the driver that arrived before its let go is set before the clipboard is
> read for the let go, and one from a new take after it is set after, so neither is mistaken for a
> copy.
>
> - model: clipboard

> r[clipboard.let-go.empty]
> When the clipboard read for a let go is empty, the driven machine sends nothing, reads it no
> more, and logs nothing.
>
> - model: clipboard

## The driver takes a clipboard message

> r[clipboard.arrive.set]
> A driver sets its clipboard from a clipboard message it takes from a machine let go
> (`clipboard.expect.let-go`), so what is brought home stays on the driver.
>
> - model: clipboard

> r[clipboard.arrive.send-on]
> When the driver's input is on a machine as it takes a clipboard message, it also sends the message
> on to that machine, so what was copied on a machine left in a chain reaches the next one.
>
> - model: clipboard

> r[clipboard.arrive.as-taken]
> A clipboard message the driver sends on carries what the driver took of it, after
> `clipboard.payload.part`, and nothing else.

> r[clipboard.arrive.failed-write]
> When the driver cannot set its clipboard from a clipboard message it took
> (`clipboard.write.retry`), its clipboard counts as changed by a copy: the next take of any
> machine, the one let go included, sends the driver's clipboard.
>
> - model: clipboard

## Reading the clipboard here

> r[clipboard.read.retry]
> When the driver cannot read its clipboard for a take or for a copy it sends while driving, or
> its link to that peer refuses the clipboard message (`link.send.full`), it tries again twice,
> 50 ms apart, reading the clipboard again each time. A try again stops as soon as the input moves
> or a copy changes the clipboard, so it never sends a newer copy for an older take. After the
> third failure, the input waiting behind the read is sent, and the clipboard is tried again at the
> next take of that peer, or the next copy.
>
> - model: clipboard

> r[clipboard.read.retry-after-accept]
> A clipboard that could not be read or sent for a take is tried again only once that take has been
> accepted.
>
> - model: clipboard

> r[clipboard.read.let-go-retry]
> When the driven machine cannot read its clipboard for a let go, it tries again every 50 ms for up
> to 1 s, and stops at once, sending nothing, when `clipboard.let-go.copy-after-let-go` or
> `clipboard.let-go.new-driver` applies. A read still failing after 1 s is given up
> (`clipboard.log.given-up`).
>
> - model: clipboard

> r[clipboard.read.failed-not-empty]
> A read of a clipboard that offers text or an image, and gives neither, is a failed read, tried
> again under `clipboard.read.retry` or `clipboard.read.let-go-retry`. An empty clipboard is not a
> failure: it has nothing to send and is not read again.
>
> - tier: machine (an app that offers a format and then fails to give it is the real OS clipboard's)

> r[clipboard.read.both]
> A read takes the clipboard's text and its image together, and both are sent: an image copied
> with its file name or address as text arrives with both.
>
> - tier: machine (what a copy puts on the clipboard is the real OS's)

> r[clipboard.read.part-failed]
> A read that gives the clipboard's text but not the image it offers, or its image but not the text
> it offers, is not a failed read: what it gave is sent at once, without the part that failed, and
> the clipboard is not read again for it.
>
> - tier: machine (a text or image provider that fails is the real OS clipboard's)

> r[clipboard.read.steady]
> On a Mac, a read during which the clipboard changes reads it again, up to three times in all, and
> sends the newest contents it read whole. A clipboard that never stays still for a read is a
> failed read.
>
> - tier: machine (when the Mac clipboard changes is the real one's)

> r[clipboard.read.mac-image]
> On a Mac, an image the clipboard offers as PNG is sent as it is, and one it offers only as TIFF
> is converted to PNG. A TIFF that cannot be converted gives no image, which is not a failed read.
>
> - tier: machine (TIFF and PNG on the clipboard are the real Mac clipboard's)

> r[clipboard.read.pc-image]
> On a PC, the image is read from the clipboard's device-independent bitmap and sent as a PNG of
> the same pixels (`clipboard.dib.*`).
>
> - tier: machine (the bitmap Windows gives for a copied image is the real clipboard's)

## Writing the clipboard here

> r[clipboard.write.replaces]
> Setting the clipboard here from a peer's clipboard message replaces everything on it with the
> message's text and image: nothing copied before stays offered beside them.
>
> - tier: machine (what the clipboard offers is the real OS clipboard's)

> r[clipboard.write.retry]
> When the clipboard here cannot be written, because another program has it open or the write
> fails, the write is tried again every 50 ms for up to 1 s, and then given up with a warning in
> the log. A write blocked for less than 1 s still lands.
>
> - parity: Python's driver tried to set a clipboard message it took three times, 50 ms apart, where its driven machine tried for up to 1 s (one rule for every clipboard message set from a peer)
> - model: clipboard

> r[clipboard.write.copy-wins]
> A copy made here while a write is being tried again ends the write: the copy stays on the
> clipboard.
>
> - model: clipboard

> r[clipboard.write.stale]
> On a driven machine, a clipboard message from the driver is not written once that driving has
> ended or the driver's link has closed, unless its write had already begun.
>
> - model: clipboard

> r[clipboard.write.newest]
> When several clipboard messages from the same driver wait to be written, only the newest is
> written.
>
> - model: clipboard

> r[clipboard.write.not-in-the-way]
> Writing the clipboard never delays input: while a write takes 3 s, input the driver sends during
> it is injected and acknowledged within 1 s of arriving.

> r[clipboard.write.no-window]
> Writing the clipboard works while none of Beamer's windows is shown, and never brings one up or
> takes the keyboard.
>
> - tier: machine (a clipboard write needs the real OS clipboard)

> r[clipboard.write.mac]
> On a Mac, text is written as a string and an image as PNG only.
>
> - tier: machine (the types on the clipboard are the real Mac clipboard's)

> r[clipboard.write.pc-text]
> On a PC, text is written with CRLF line endings: each LF, and each CRLF, becomes one CRLF.
>
> - tier: machine (the text on the clipboard is the real Windows clipboard's)

> r[clipboard.write.pc-image]
> On a PC, an image is written as a 32-bit device-independent bitmap with a 40-byte header, its
> rows bottom-up, and its alpha in each pixel's fourth byte.
>
> - tier: machine (the bitmap on the clipboard is the real Windows clipboard's)

> r[clipboard.write.pc-undecodable]
> On a PC, an image that does not decode is not written: its text alone is, and when there is no
> text the clipboard is left as it was.
>
> - tier: machine (decoding for the real Windows clipboard)

> r[clipboard.write.pc-not-emptied]
> On a PC, when Windows refuses to empty the clipboard Beamer has opened for a write, nothing is
> written to it, so nothing is mixed into another program's clipboard, and the write has failed
> (`clipboard.write.retry`).

## Reading a device-independent bitmap

A PC's clipboard keeps images as a device-independent bitmap (DIB): a header, then pixel rows.
Reading one gives the pixels the PNG is made of.

> r[clipboard.dib.kinds]
> A DIB is read when its header is 40, 52, 56, 108 or 124 bytes long, its width is above 0 and its
> height is not 0, and it is 24-bit or 32-bit uncompressed, or 32-bit bitfields whose red, green
> and blue masks are `0x00FF0000`, `0x0000FF00` and `0x000000FF`. Any other DIB, junk included,
> gives no image, and reading it never fails the read of the text.
>
> - fuzz: clipboard-dib

> r[clipboard.dib.rows]
> A DIB's rows are bottom-up when its height is positive and top-down when it is negative, each row
> padded to a multiple of 4 bytes. A DIB shorter than its rows need gives no image.
>
> - fuzz: clipboard-dib

> r[clipboard.dib.masks]
> In a bitfields DIB with a 40-byte header, the three masks follow the header and the pixels follow
> the masks. A bitfields DIB's 52-byte header carries three masks and no alpha mask; its 56-, 108-
> and 124-byte headers carry an alpha mask after them. An uncompressed DIB's masks are never read,
> whatever its header holds. A palette's entries, when the header counts any, come before the
> pixels.
>
> - fuzz: clipboard-dib

> r[clipboard.dib.alpha]
> A 24-bit DIB reads as opaque. In a 32-bit DIB, each pixel's fourth byte is its alpha, unless every
> pixel's fourth byte is 0: the image then reads as opaque. In a bitfields DIB with an alpha mask of
> `0xFF000000`, the fourth byte is the alpha even when every one is 0. An uncompressed DIB never
> has an alpha mask: one whose header holds `0xFF000000` where a bitfields DIB's alpha mask would
> be, and whose fourth bytes are all 0, reads as opaque.
>
> - fuzz: clipboard-dib

> r[clipboard.dib.round-trip]
> A 32-bit image written to a PC's clipboard (`clipboard.write.pc-image`) reads as the same
> pixels again.
>
> - fuzz: clipboard-dib

## Logs

r[clipboard.log.part]
When a driver does not send a part of its clipboard because it is over its limit or is not a PNG
that fits, it logs a warning saying which part.

r[clipboard.log.given-up]
A read or write of the clipboard given up after its tries again logs one warning naming the peer
it was for.
