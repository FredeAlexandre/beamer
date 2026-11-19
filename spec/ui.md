# UI

How Beamer is drawn and felt: the look of the settings window (F33: its colour roles in light and
dark, fonts, the widget kit, motion and focus), the window's and pages' geometry, the crossing map
and the crossing ledger as drawn, the overlays (F29's edge glow and beam, the Ring, F28's identify
displays and F16's pointer overlay), the tray icon, and the room left for the crossing effects (F30)
and the notch island and beam (F31). `beamer-ui` draws all of it with Rune (ADR 0001), on a Mac and
on a PC alike.

What stays elsewhere: what every screen, overlay, tray menu and notification shows, enables, says
and asks of `Core` is `spec/viewmodel.md`'s, cited here by id where a drawing depends on it. When a
crossing happens and what Core reports about it (push pressure, ticks, the crossing itself) is
`spec/crossing.md`'s, and when the pointer overlay shows is `crossing.pointer.*`'s. The saved values
behind the Design page's choices are `spec/settings.md`'s. Frame-time budgets are `spec/perf.md`'s.

**Tier.** `beamer-ui` is `human` tier (#24): every rule here is checked by Alex by hand, on the
Mac and on the PC, through the milestone checklist (`cargo xtask checklist`). Each rule keeps its
exact values (sizes, colours, times) for the implementer; its `see:` says what a correct build shows
without a ruler.

**Units and colours.** Sizes are in pt, Rune's logical units, which equal a PC's px at 100 %
scaling. Colours named in backticks (`signal`, `text-2`, `hairline`…) are the colour roles of
`ui.look.palette`, in the palette of the window's appearance (`ui.theme.*`). Where Python's Mac and
PC drew the same thing differently by accident, a rule picks one for both machines and its
`parity:` marker says which (ADR 0001: one Rune UI draws both machines).

Dropped, so no rule here (#11): the Ink and Warp effects, `GlowPreview` and the arrangement diagram
(`ArrangementDiagram`) with the other uncalled code, Linux-only code (#1), and US spelling and the
other languages' run-time switches (F34). Later (#11): the crossing effects (F30) and the notch
island and beam (F31), whose room is `ui.later.effects` and `ui.later.notch`.

The rules come from #11's outcomes, ADR 0001 and the Python app at `archive/python` (root
`theme.py` and `tokens.py`, `core/effects.py`, `core/identify_screens.py`, and the Mac and Windows
apps' windows, widgets, overlays and tray), mined through its tests in `spec/ledger/ui.txt`.

## The look

The settings window's colours, surfaces, contrast and type (F33, rebuilt in Rune, #11 and ADR 0001).
What each page shows and says is `spec/viewmodel.md`'s; this is how it is coloured and set. Sizes
are in pt, Rune's logical units, which a PC's px equal at 100 % scaling.

> r[ui.look.palette]
> The settings window draws with fourteen colour roles, each with one value in Dark and one in
> Light, and draws in no other colour apart from previews of the colours a user can choose:
>
> | Role | Dark | Light | Drawn for |
> |---|---|---|---|
> | `ground` | #0e0f0d | #f1f0ea | the window's background |
> | `panel` | #161714 | #fbfaf6 | module cards, the sidebar, a disabled button |
> | `well` | #1f201c | #e5e4dc | the inside of a control: a switch's track, a button, an option, a ruler's track |
> | `hairline` | #2a2b26 | #dcdbd3 | the gaps between modules, a tile's outline, a disabled control's outline |
> | `outline` | #6e6f66 | #7f7e74 | a live control's boundary, a ruler's minor ticks |
> | `text` | #efeee6 | #1b1c18 | headings, a control's words, a chosen option's fill, a ruler's thumb |
> | `text-2` | #b3b2a7 | #46463f | running text and notes, an unchosen option's words |
> | `text-3` | #8f8e84 | #66655c | eyebrows, a tile's detail, a switch's knob while off |
> | `signal` | #5fd4f4 | #006a88 | what is happening now: the live tone, a switch while on, the keyboard focus ring |
> | `signal-dim` | #1f4a55 | #bfe0ea | selected text behind the caret |
> | `amber` | #f2b544 | #8f5c00 | the waiting tone |
> | `flash` | #b77a00 | #b55f00 | the resistance strip's flash as a zone push crosses |
> | `fault` | #ff7a66 | #b53522 | the fault tone |
> | `off` | #4a4b44 | #aeada3 | a light in the plain tone |
>
> - tier: human (colours are judged by eye)
> - do: Under Design, set "This window" to Dark, then to Light
> - see: In Dark the window is near-black with off-white text and a bright cyan for live states; in Light it is a warm off-white with near-black text and the cyan turns a deep teal

> r[ui.look.surfaces]
> The window's background is `ground`, opaque from the top of a page to its end. Each module of a
> page is a card filled with `panel`, and modules next to each other are 1 pt apart, the gap
> showing `hairline`, so they read as cards divided by hairlines.
>
> - tier: human (surfaces are judged by eye)
> - do: Open the Design page in Dark and in Light and scroll to its end
> - see: The cards stand slightly lighter than the window behind them in both appearances, thin hairlines part cards that sit next to each other, and the background is the same solid colour down to the end

> r[ui.look.contrast.text]
> Every piece of text in the settings window has a contrast ratio (WCAG 2.x) of at least 4.5:1
> against what is drawn behind it, or at least 3:1 for text of 18 pt and over and bold text of
> 14 pt and over, on every page, in Light and in Dark: a chosen option's words on its fill, notes,
> eyebrows and every tone included. Text faded with a disabled control is exempt.
>
> - tier: human (legibility is judged by eye)
> - do: Go through every page in Dark, then choose Light on the Design page and go through every page again
> - see: Every word reads clearly against what is behind it, in both appearances; no text is grey on grey or teal on teal

> r[ui.look.contrast.controls]
> A live control's boundary, drawn in `outline`, has a contrast ratio of at least 3:1 against
> `panel` in both appearances (3.5:1 in Dark, 3.9:1 in Light), so every button, field and switch
> track is told apart from its card.
>
> - tier: human (a control's edge is judged by eye)
> - do: Look at the Design page's switches and buttons in Dark and in Light
> - see: Each switch's track and each button's outline stands out clearly from the card it sits on

> r[ui.look.font.faces]
> The settings window sets its words in Hanken Grotesk and its figures meant to line up (text
> fields, a ruler's and a scale's numbers, codes) in B612 Mono, both carried inside Beamer, so they
> draw the same on a machine that has neither installed. The link status's word and a machine's
> words are in Hanken Grotesk at normal letter spacing.
>
> - tier: human (a typeface is judged by eye)
> - parity: Python's Mac tightened the link status's word by 0.03 em, readouts by 0.04 em and big numerals by 0.05 em, where its PC kept normal letter spacing (ADR 0001: one Rune UI draws both machines; the PC's, which its test checked)
> - do: On a machine without either font installed, open the settings window and look at Overview and at a ruler on the Design page
> - see: The words are in the same rounded grotesque on the Mac and the PC, and a ruler's numbers are in a monospaced face with open, squared letterforms

> r[ui.look.font.sizes]
> Text is set at these sizes, the narrow sizes in a page's narrow layout (`ui.page.narrow-layout`):
>
> | Text | Size | Set as |
> |---|---|---|
> | eyebrow (a module's heading) | 10.5 pt | bold, all capitals, letters spaced 0.14 em, `text-3` |
> | readout key | 9.5 pt | bold, all capitals, letters spaced 0.14 em |
> | small (an option, a tile's detail) | 11.5 pt | regular |
> | note | 12.5 pt | regular, `text-2` |
> | body | 13 pt | regular |
> | button | 13 pt | semibold |
> | text field | 13 pt | B612 Mono |
> | keycap | 20 pt | |
> | the link status's word | 38 pt, 30 pt narrow | |
> | readout | 34 pt, 26 pt narrow | |
> | numeral | 52 pt, 42 pt narrow | |
>
> - tier: human (type sizes are judged by eye)
> - parity: Python's PC rounded each size to whole px, so 10.5, 11.5 and 12.5 drew at 10, 12 and 12 px (ADR 0001: one Rune UI draws both machines, at the sizes Python's tokens give)
> - do: Open the Design page with the window wide, then narrow it to its minimum width and open Overview
> - see: Module headings are small spaced capitals, notes slightly smaller than body text, and the link status's word on Overview is large and shrinks a step at the minimum width

### Tones

How each tone (`viewmodel.status.parts`, `viewmodel.audio.tone`) is coloured, wherever a line, a
word or a light carries one.

> r[ui.look.tone.quiet]
> A line in the quiet tone is drawn in `text-3`.
>
> - tier: human (colour is judged by eye)
> - do: Open the Audio & Camera page while no microphone is shared
> - see: "No microphone is shared: apps that record it hear silence." is in the dim grey of the eyebrows, quieter than the notes around it

> r[ui.look.tone.plain]
> A line in the plain tone is drawn in `text-2`, a word in the plain tone, such as the link status's
> word, in `text`, and a light in the plain tone in `off`.
>
> - tier: human (colour is judged by eye)
> - do: With a peer linked and this machine's input home, look at the link status on Overview
> - see: Its word, "On this Mac" or "On this PC", is in the window's main text colour, neither cyan, amber nor red

> r[ui.look.tone.live]
> A line, word or light in the live tone is drawn in `signal`.
>
> - tier: human (colour is judged by eye)
> - do: Drive this machine from a peer and look at the link status
> - see: Its word and its light are in the signal cyan in Dark, deep teal in Light

> r[ui.look.tone.waiting]
> A line, word or light in the waiting tone is drawn in `amber`.
>
> - tier: human (colour is judged by eye)
> - do: Quit Beamer on the peer, then quit and restart Beamer here and open Overview at once, before the link reports anything (`viewmodel.status.looking`)
> - see: The link status's word, "Waiting", and its light are amber

> r[ui.look.tone.fault]
> A line, word or light in the fault tone is drawn in `fault`.
>
> - tier: human (colour is judged by eye)
> - do: On a paired peer, remove this machine, then look at the link status here
> - see: Its word and its light are in the fault red, a soft coral in Dark and a brick red in Light

### Title bar

> r[ui.look.titlebar.double-click]
> On a Mac, a double-click on the settings window's title bar, on the 18 pt strip at the top of its
> content or on the top of its sidebar does what the Mac's "Double-click a window's title bar to"
> setting says: "Fill" makes the window fill its display's visible area and a second double-click
> returns it to the frame it had, "Minimise" minimises it, and "Do nothing" leaves it as it is. A
> double-click on a window button or on a control in those places acts on that button or control
> only.
>
> - tier: human (window behaviour is judged by hand)
> - do: With the Mac's setting at Fill, double-click the top of the sidebar, then double-click it again; then double-click the window's minimise button
> - see: The window fills the display, then returns to its earlier size and place; the double-click on the minimise button only minimises the window, without filling it first

## Appearance

What each of "System", "Light" and "Dark" (`viewmodel.design.appearance`) draws, and how the window
follows the OS.

> r[ui.theme.fixed]
> With "Light" chosen, the settings window draws in the Light values of `ui.look.palette`, and with
> "Dark" in the Dark values, whatever the OS's own light or dark setting.
>
> - tier: human (the palette is judged by eye)
> - do: With the OS in dark mode, choose "Light"; then, with the OS in light mode, choose "Dark"
> - see: The window is light while the OS is dark, then dark while the OS is light

> r[ui.theme.system]
> With "System" chosen, the settings window draws in Dark while the OS is set to dark and in Light
> while it is set to light: on a Mac, the Mac's Appearance; on a PC, Windows' default app mode, not
> the mode of its taskbar.
>
> - tier: human (the palette is judged by eye)
> - do: On the PC, set Windows' app mode to Light and its Windows mode to Dark, then choose "System"
> - see: The window is light, following the app mode, although the taskbar is dark

> r[ui.theme.live]
> With "System" chosen, when the OS changes between light and dark while the settings window is
> open, the window follows within a second, without being closed or reopened.
>
> - tier: human (following the OS is seen in use)
> - do: Choose "System", keep the window open, and change the OS between light and dark mode
> - see: The window turns light or dark with the OS each time, while it stays open

> r[ui.theme.whole-window]
> After a change of appearance, by choice or by the OS, every page of the settings window draws in
> the new palette, the pages not shown at the time included: each card, line of text, control, light
> and preview on a page opened afterwards has the new values, and none keeps a colour of the old.
>
> - tier: human (the palette is judged by eye)
> - do: In Dark, open the Design page and choose "Light", then open Crossing, Overview and every other page
> - see: Every page is light throughout, its cards included; no dark card or dark text is left anywhere

> r[ui.theme.title-bar]
> The settings window's title bar follows its appearance. On a Mac it is drawn light in Light and
> dark in Dark. On a PC it is painted `ground`, its words `text-2` and its border `hairline`, and
> never takes Windows' accent colour, even while the window is active, with its window buttons drawn
> for the appearance.
>
> - tier: human (a title bar is judged by eye)
> - do: On the PC, turn on "Show accent colour on title bars", then click into the settings window and change between Light and Dark
> - see: The title bar has the window's own background colour in both, never the accent colour, and its buttons stay visible

## Motion

How the settings window moves when a setting changes what it shows.

> r[ui.motion.ease]
> A change the settings window animates runs for 200 ms on a cubic ease-out, fast at first and
> settling at the end, unless a rule here gives it another time.
>
> - tier: human (motion is judged by eye)
> - parity: Python's Mac eased on the curve cubic-bezier(0.16, 1, 0.3, 1) where its PC used a cubic ease-out (ADR 0001: one Rune UI draws both machines)
> - do: On the Design page, turn "Animate crossings" off and on
> - see: The "Style" and "Colour" modules close and open in about a fifth of a second, quick at first and gentle at the end

> r[ui.motion.interrupt]
> A change that arrives while an animation of the same thing runs starts from wherever that
> animation had got to and runs to the new end: a module turned round halfway turns from where it
> is, never jumping to either end first.
>
> - tier: human (motion is judged by eye)
> - do: On the Design page, turn "Animate crossings" off and at once on again
> - see: The closing modules turn round and open again from where they had got to, without a jump

> r[ui.motion.reduced]
> While the OS asks for less motion (on a Mac, "Reduce motion"; on a PC, "Animation effects" off),
> every change in the settings window lands at once, with no slide, fade or cross-fade. The OS's
> setting is read at each change, so turning it on or off applies to the next change.
>
> - tier: human (motion is judged by eye)
> - do: Turn on the OS's reduce-motion setting with the window open, then turn "Animate crossings" off and on and change the appearance
> - see: Each change appears at once, with nothing sliding or fading

> r[ui.motion.unseen]
> A change to a part of the settings window that cannot be seen, because the window is hidden or
> minimised or the part is scrolled out of view, lands at once without animating, so the part is
> already in its end state when it is next seen.
>
> - tier: human (motion is judged by eye)
> - do: With "System" chosen, minimise the settings window, change the OS between light and dark mode, and restore the window
> - see: The window is already in the new appearance as it reappears, with no fade playing

> r[ui.motion.rows]
> A module or line of controls that the settings window hides slides shut while it fades out, and
> what is under it rises to close the gap as it goes, so the page never jumps under the pointer. One
> the window shows slides open while it fades in. The page's height follows the slide, so a page
> scrolled to its end stays at rest while a module near its end closes.
>
> - tier: human (motion is judged by eye)
> - parity: Python's Mac faded a hidden module out over 100 ms and then slid it shut over 200 ms, and faded a shown one in over the second half of 300 ms as it opened (ADR 0001: one Rune UI draws both machines; the PC's, which slides and fades together)
> - do: Scroll the Design page to its end and turn "Animate crossings" off
> - see: The style and colour modules fold away as they fade, the content below rises smoothly, and nothing in view jumps

> r[ui.motion.ring]
> When another tile or colour is chosen in a group, the ring round the chosen one
> (`ui.widget.tiles.chosen`) slides from the old choice to the new one, across lines too, rather than
> vanishing and reappearing.
>
> - tier: human (motion is judged by eye)
> - do: On the Design page, choose another colour in the Light group, then one in the Mono group
> - see: The ring glides from the old colour to the new one each time

> r[ui.motion.cross-fade]
> A change of appearance, and a change of the link status's word or the crossing line's words,
> cross-fades from the old look of that part to the new one over 200 ms.
>
> - tier: human (motion is judged by eye)
> - do: Change the appearance under Design from Dark to Light, then let a peer connect while Overview is shown
> - see: The window dissolves from dark to light rather than flicking over, and the link status's word dissolves into its new word

> r[ui.motion.switch-knob]
> A switch's knob slides along its track to the other end over 150 ms when the switch changes while
> it can be seen.
>
> - tier: human (motion is judged by eye)
> - parity: Python's PC drew the knob at its new end at once (ADR 0001: one Rune UI draws both machines; the Mac's)
> - do: Turn a switch on and off on the Design page
> - see: The knob glides from one end of the track to the other each time

## The widget kit

How each kind of control is drawn. Every control here behaves the same on a Mac and a PC.

### Buttons

> r[ui.widget.button.look]
> A button has corners of 5 pt radius and its words in 13 pt semibold, centred. A plain button is
> `well` with an `outline` border and `text` words; a primary button (`ui.widget.button.kinds`) is
> filled `text` with `ground` words. A disabled button of either kind is `panel` with a `hairline`
> border and `text-3` words, never a faded filled slab.
>
> - tier: human (a button is judged by eye)
> - parity: Python's PC drew a disabled primary button `well` rather than `panel`, and Python's Mac also had a live button, filled `signal`, for its "Send input" button, which 2.0's settings window does not show (ADR 0001: one Rune UI draws both machines, the Mac's)
> - do: Open the pairing sheet with "Pair a machine" on Overview, type an address under "Not listed? Type its address.", then type six digits into the code to enter
> - see: "Cancel" is an outlined button with words in the text colour; "Pair" is a quiet outline with grey words until the sixth digit is typed, then, like "Show a code", a solid block in the text colour with background-coloured words

> r[ui.widget.button.kinds]
> These buttons are primary: "Show a code" and "Pair" on the pairing sheet, the Pause crossing
> button while it reads "Resume crossing" (`viewmodel.crossing.pause.button`), "Relaunch Beamer" on
> a Mac (`viewmodel.permissions.capture-relaunch`), and a button that a line or notice of the
> Audio & Camera page carries while that line or notice is in the waiting tone
> (`viewmodel.audio.tone`). Every other button of the settings window is plain, "Pause crossing",
> "Pair a machine" and "Cancel" included.
>
> - tier: human (a button's kind is judged by eye)
> - parity: Python's PC drew the buttons of the Audio section's notices plain in every tone (ADR 0001: one Rune UI draws both machines, the Mac's)
> - do: On Overview, with a zone on, look at "Pause crossing", press it, and look again; then press "Pair a machine"
> - see: "Pause crossing" is an outlined button, and once pressed "Resume crossing" is a solid block in the text colour; on the sheet "Show a code" is a solid block and "Cancel" is outlined

> r[ui.widget.button.size]
> A button is at least 30 pt tall (40 pt for a big one) and as wide as its words plus 24 pt, never
> narrower than 64 pt nor wider than 240 pt; words longer than 240 pt allows wrap onto more lines
> and the button grows taller. At any window width a button is at least as wide as its longest line
> of words plus 16 pt.
>
> - tier: human (a control's size is judged by eye)
> - do: Narrow the settings window to its minimum width and look at the buttons on each machine's line of Overview
> - see: Every button shows its whole words with room round them and is comfortably tall enough to click

> r[ui.widget.button.hover]
> On a PC, a plain button's border turns `text-3` under the pointer, and a primary button's fill and
> border turn `text-2` (`ui.widget.button.kinds`). On a Mac a button does not change under the
> pointer.
>
> - tier: human (hover is judged by eye)
> - do: On the PC, press "Pair a machine" on Overview, then move the pointer over "Cancel" and over "Show a code"
> - see: The outline of "Cancel" brightens and the fill of "Show a code" turns a softer shade under the pointer

> r[ui.widget.disabled]
> A switch, a ruler, a group of options and a group of tiles or colours that cannot be used is drawn
> at 45 % opacity and takes no click, key or wheel. A disabled group of options keeps its chosen
> option's words semibold but loses its `text` fill.
>
> - tier: human (a disabled control is judged by eye)
> - parity: Python's Mac faded a disabled ruler to 40 %, and its PC drew a disabled plain slider with a `text-3` handle on a `panel` groove (ADR 0001: one Rune UI draws both machines)
> - do: With the Design group of Same on all machines off, choose a peer under "Match design with"
> - see: The style tiles, colours and rulers fade to about half strength and do nothing when clicked

### Switches

> r[ui.widget.switch.look]
> A switch is its words on the left and a track on the right, 12 pt apart, the whole switch at least
> 30 pt tall and clickable anywhere. The track is 34 by 18 pt, fully rounded and filled `well`. Off,
> the track's outline is `outline` and its 12 pt round knob is `text-3`, 3 pt in from the left end;
> on, the outline and the knob are `signal`, the knob 3 pt in from the right end.
>
> - tier: human (a switch is judged by eye)
> - do: On the Design page, click the words of "Animate crossings" to turn it off and on
> - see: Clicking the words turns the switch on or off; off shows a grey knob on the left of a grey-outlined track, on a cyan knob on the right of a cyan-outlined track

> r[ui.widget.switch.wraps]
> A switch whose words are too long for one line wraps them onto more lines and grows taller, and
> its track stays 34 by 18 pt, centred on the words' height.
>
> - tier: human (a switch is judged by eye)
> - do: On the PC, narrow the window to its minimum and look at "Hold this PC's edges while an app on it is full screen" on Overview
> - see: The words run onto two lines, and the track keeps its size, centred beside them

### Rulers

> r[ui.widget.ruler.look]
> A ruler is a 30 pt baseline of ticks with labelled values under it. A line 2 pt thick in `text-2`
> runs along the baseline from the start to the thumb. Minor ticks are 8 pt tall in `outline`; each
> labelled value has a 16 pt tick in `text-3` and its number under it in 10.5 pt B612 Mono `text-3`,
> the first number aligned to the ruler's start and the last to its end. The thumb is a bar 3 pt
> wide and 30 pt tall in `text`, standing in a 9 pt gap of `panel` cut through the ticks.
>
> - tier: human (a ruler is judged by eye)
> - parity: Python's PC drew a ruler without labelled values as a round handle on a groove, filled `signal` up to the handle (ADR 0001: one Rune UI draws both machines; the Mac's, as every ruler)
> - do: Look at the Length and Size rulers on the Design page, then drag one
> - see: Each is a scale of fine ticks with numbered marks, a darker line up to a tall slim thumb that cuts through the ticks, and the thumb follows the drag

> r[ui.widget.ruler.sqrt]
> The double-tap window's ruler, from 50 to 1000 ms, has a square-root scale: a value v sits
> (√v − √50) / (√1000 − √50) of its length from its start, so 150 to 600 ms takes up half its length, and a
> drag to a point sets the value that point stands for. A value beyond either end draws the thumb at
> that end.
>
> - tier: human (where a ruler's marks fall is judged by eye)
> - do: On the Crossing page, look at the ruler for the time between taps and drag it from 150 to 600
> - see: The marks 150, 300 and 600 sit spread across the middle of the ruler, and going from 150 to 600 crosses about half its length

### The resistance strip

> r[ui.widget.strip.look]
> The resistance strip on the Crossing page is 72 pt tall, `ground` inside a `hairline` border with
> 3 pt corners. Its 12 pt track, `well` with a `text-3` outline, runs from 18 pt in from its start to
> 18 pt in from its end and stands for 0 to 500 px, with a tick every 25 px (12 pt tall in `text-3`
> and numbered in B612 Mono every 100, 4 pt in `outline` between). The resistance is a 2 pt `text`
> line across the track. Over it, "THIS MAC" ("THIS PC") is at the start and the peer's label at the
> end, both as eyebrows; the peer's label is cut with "…" to fit half the strip less 16 pt.
>
> - tier: human (the strip is judged by eye)
> - do: On the Crossing page at the window's minimum width, with a peer whose label is long, look at the resistance strip
> - see: The scale from 0 to 500 is fully visible, "THIS MAC" is at the start, and the peer's label ends in "…" without running into it

> r[ui.widget.strip.pressure]
> While push pressure builds at one of this machine's zones, the resistance strip fills its track
> in `signal` from the start up to the push pressure's place on the same 0 to 500 scale as the
> resistance, at any width of the strip, and a 2 pt `signal` mark stands at the track's start (in
> `outline` at rest). When the push crosses, that mark turns `flash` and 3 pt wide for 120 ms, then
> the fill empties.
>
> - tier: human (the strip's live fill is judged by eye)
> - do: Narrow the window to its minimum, open Crossing and push slowly against a zone's edge until the pointer crosses
> - see: A cyan fill grows from the start toward the resistance line as you push, the start mark flashes orange as it crosses, and the fill empties

### Options

A group of options is one choice of several shown as words next to each other, such as "Frame
rate" or "This window".

> r[ui.widget.option.look]
> Each option of a group is filled `well` with its words in 11.5 pt `text-2`, the options together
> in one box with 5 pt corners and 1 pt `outline` lines between them. The chosen option is filled
> `text` with its words in `ground`, semibold, in Light and in Dark.
>
> - tier: human (an option is judged by eye)
> - do: On the Design page, choose each of "System", "Light" and "Dark" under "This window"
> - see: The chosen option is a solid block in the text colour with background-coloured words, black in Light and off-white in Dark, and the others are quiet

> r[ui.widget.option.size]
> The options of a group are of one width, each as wide as its group's longest words plus 24 pt,
> up to 120 pt each and 360 pt for the group, however wide the card, and at least 30 pt tall.
> Words longer than an option allows wrap onto more lines inside it, never cut with "…", and every
> option of the group grows to the same height.
>
> - tier: human (an option's size is judged by eye)
> - do: On the Design page, turn on "Show where the pointer lands", then look at "Style for" with the window at its widest
> - see: "Crossing" and "Shortcut and menu" are two options of the same width and height, the group compact rather than stretched across the card, each option's words whole

> r[ui.widget.option.stack]
> Where a group's options do not fit next to each other in the width they have, they stand one
> above the other, each as wide as the group, its words whole.
>
> - tier: human (an option's placement is judged by eye)
> - do: On the PC, narrow the window to its minimum and look at the Keyboard page's "Modifier keys"
> - see: "Same shortcuts", "Same positions" and "Mac keyboard layout" stack in one column, each showing all its words

> r[ui.widget.menu.keeps-choice]
> A menu shows the choice it has chosen in that choice's words as they are now: when its choices
> change while it is shown, closed or open, it redraws the chosen one's words in place, without
> closing an open list or showing words it had before. Which choice is chosen, and what it says, are
> viewmodel's (`viewmodel.design.follow-follows`, `viewmodel.camera.selector.unplugged`).
>
> - tier: human (a menu is judged by eye)
> - do: On the Design page, choose a peer under "Match design with", then rename that peer on the Crossing page and return to Design
> - see: The menu still shows that peer, under its new name

### Tiles and colours

A group of tiles is one choice of several shown as cards, each a preview over its name and a
detail, such as the Design page's styles. A group of colours is the same with a colour chip for
each.

> r[ui.widget.tiles.look]
> A tile is `ground` with a 1 pt `hairline` outline and 5 pt corners. Inside, 8 pt from its edges,
> its preview is at the top, its name under it in 13 pt semibold `text` and its detail under that in
> 11.5 pt `text-3`, the preview at least 10 pt tall; the name and detail wrap and never overlap the
> preview or each other, at every window width.
>
> - tier: human (a tile is judged by eye)
> - do: On the Design page, narrow the window step by step from its widest to its minimum
> - see: Each style tile keeps its preview above its name and detail, the words wrap within the tile and nothing overlaps or is cut off

> r[ui.widget.tiles.hover]
> Under the pointer, a tile's background turns `well` in Dark and `panel` in Light and its outline
> turns `text` at 18 % opacity.
>
> - tier: human (hover is judged by eye)
> - do: Move the pointer over the style tiles on the Design page
> - see: The tile under the pointer lifts slightly, its outline a little brighter

> r[ui.widget.tiles.chosen]
> The chosen tile or colour is ringed 2 pt in `text` with a 1 pt `ground` line inside the ring,
> its corners following the tile's, and a chosen tile shows a 16 pt round `signal` badge with a
> `ground` tick before its name, the name moving 22 pt along to make room while nothing else in the
> tile moves. A chosen colour's name turns `text` and semibold.
>
> - tier: human (the chosen tile is judged by eye)
> - parity: Python's PC also outlined the chosen tile 2 px in `signal` inside the `text` ring (ADR 0001: one Rune UI draws both machines; the Mac's)
> - do: On the Design page, choose another style tile
> - see: The new tile is ringed in the text colour with a cyan tick badge before its name, which shifts along slightly; the preview stays where it was

> r[ui.widget.tiles.columns]
> A group of style tiles puts 3 tiles to a line where the page column is at least 600 pt wide, 2 from
> 400 pt and 1 below; a group of colours puts 5 to a line from 600 pt, 3 from 400 pt and 2 below.
> The cards of a line share its width equally to within 1 pt, 12 pt apart (8 pt for colours), the
> first at the line's start and the last at its end, and a short last line keeps the same card
> width. The same width always gives the same placement, whether the window grew or shrank to it.
>
> - tier: human (the placement is judged by eye)
> - do: On the Design page, narrow the window slowly from its widest to its minimum and widen it again
> - see: The colours go from five to three to two a line and the styles from three to two to one, the same at each width in both directions, every line edge to edge

> r[ui.widget.tiles.one-chosen]
> Exactly one tile of a group, or one colour across all the groups of colours, is chosen at a time.
> Choosing the chosen one again, by a click, a key or an accessibility action, keeps it chosen.
>
> - tier: human (the chosen tile is judged by eye)
> - do: Click the chosen style tile on the Design page again, then press Space on it after Tab
> - see: It stays chosen each time, and no other tile becomes chosen

> r[ui.widget.tiles.accessible]
> Each tile and colour is an accessibility element in the role of a radio button, which reports
> whether it is chosen and which an accessibility press action chooses.
>
> - tier: human (what VoiceOver and Narrator say is heard)
> - do: With VoiceOver on the Mac or Narrator on the PC, move to a style tile that is not chosen and activate it
> - see: It is announced as a radio button, not selected; after the action it is announced as selected and the tile is chosen

> r[ui.widget.swatch.look]
> A colour is a chip 28 pt tall with 4 pt corners and a 1 pt `hairline` outline, filled with its
> colours as a gradient from left to right (one flat colour for a colour of one value), with its
> name under it in 11.5 pt `text-2`, centred. Where the page column is at least 600 pt wide
> (`ui.widget.tiles.columns`), the five colours of a group fill one line with no empty place at its
> end.
>
> - tier: human (a colour chip is judged by eye)
> - do: Open the Colour module on the Design page with the window at its widest
> - see: The Light and Mono groups each show five chips across, each a smooth band of its colours, names under them, with no blank space after the fifth

> r[ui.widget.swatch.visible]
> Every colour, White included, is visibly drawn in its chip and in every style tile, in
> Light and in Dark.
>
> - tier: human (visibility is judged by eye)
> - do: In Light, choose White, then hover each style tile; repeat in Dark
> - see: The White chip and each preview show the white light distinctly against their background in both appearances

### Lights

> r[ui.widget.light.look]
> A light is a round lamp 8 pt across in its tone's colour (`ui.look.tone.*`). A light in the live
> tone also glows: a soft halo of `signal` round it, 4 pt wide at 55 % opacity.
>
> - tier: human (a light is judged by eye)
> - parity: Python's PC drew the live light without a halo (ADR 0001: one Rune UI draws both machines; the Mac's)
> - do: Drive this machine from a peer and look at the light in the sidebar's link block
> - see: The small round light is cyan with a faint glow round it

> r[ui.widget.light.blink]
> A blinking light fades from full to 25 % opacity over 0.55 s and returns over 0.55 s, again and again
> until it stops blinking.
>
> - tier: human (a blink is judged by eye)
> - parity: Python's PC drew the light steady, never blinking (ADR 0001: one Rune UI draws both machines, the Mac's)
> - do: Quit Beamer on the peer, then quit and restart Beamer here and watch the sidebar's light at once, while the link status says "Waiting" (`viewmodel.status.looking`)
> - see: The amber light pulses gently, dimming and brightening about once a second

### Placing controls

> r[ui.widget.form.caption-track]
> A setting with a caption in a card puts the caption in a 180 pt column and starts its control
> 196 pt from the card's content edge, every captioned control of a card lining up at that place.
>
> - tier: human (alignment is judged by eye)
> - do: Open the Design page with the window at its widest
> - see: "Length", "Size", "Frame rate" and "This window" sit in one column, and their controls all start at the same line to the right of it

> r[ui.widget.form.stacked]
> Where a captioned setting has less than 528 pt for the caption and its control, the caption goes
> above and the control starts at the card's content edge, 8 pt under the caption. Growing or
> shrinking the window through 528 pt gives the same placement at the same width in either direction.
>
> - tier: human (placement is judged by eye)
> - do: On the Design page, narrow the window to its minimum and widen it again
> - see: The captions move above their controls when narrow and return to the left when wide, at the same width in both directions

> r[ui.widget.actions.trailing]
> The controls of a machine's line on Overview stand together at the line's trailing end, 12 pt
> apart and 12 pt in from the end of the card. Where the line is too narrow for them next to each
> other they stand one above the other, each still 12 pt in from the end, none overlapping another.
>
> - tier: human (placement is judged by eye)
> - do: Look at a peer's line on Overview at a wide window, then at the window's minimum width
> - see: Its controls sit together at the end of the line, evenly spaced, and when narrow they stack neatly at the end without overlapping

> r[ui.widget.grid]
> A grid of cells in a card, such as the link status's round trip and peer on Overview, is drawn as
> equal columns with 1 pt `hairline` lines between the cells and round them; below 350 pt it has one
> column. A last line with fewer cells than columns stretches its last cell to the end of the line,
> so no empty grey slot shows.
>
> - tier: human (a grid is judged by eye)
> - do: Look at the round trip and the peer in the link status module on Overview, at a wide window and at the minimum width
> - see: The two cells sit next to each other parted by a hairline when wide and one above the other when narrow, with no empty grey cell

## Text

How text fits the space it has.

> r[ui.text.reading-width]
> A note of running text is never wider than 560 pt, however wide its card; a narrower card wraps
> it to the card's width.
>
> - tier: human (line length is judged by eye)
> - do: Widen the settings window to the full width of a large display and read the notes on the Design page
> - see: The notes' lines stop at a comfortable reading length instead of running across the whole card

> r[ui.text.never-clipped]
> No text in the settings window is cut off at its edge, at any window width from the minimum up and
> in either appearance: text that may not fit wraps onto more lines, or is cut with "…" as
> `ui.text.elide.end` says, and its box grows to hold it. An option's words, a figure such as the
> time between taps or the round trip, a heading, a machine's name and address and a tile's caption
> each show whole.
>
> - tier: human (clipping is judged by eye)
> - do: Go through every page at the window's minimum width and again at its widest, in Light and in Dark
> - see: No word is sliced through at a box's edge anywhere: text wraps or ends in "…"

> r[ui.text.elide.end]
> A machine's label or a heading naming one that has one line of room and does not fit is cut at
> its end with "…" so that it fits; a text of several lines has each line cut on its own.
>
> - tier: human (eliding is judged by eye)
> - do: Name a peer "Studio MacBook Pro with an exceptionally long machine name" and look at the crossing ledger at a narrow window
> - see: The name stops at the column's end with "…", and nothing spills into the next column

> r[ui.text.elide.tooltip]
> While a text is cut with "…", hovering it shows its whole text as a tooltip, and its accessible
> name is the whole text.
>
> - tier: human (a tooltip is judged by eye)
> - do: Hover the cut name in the crossing ledger
> - see: The tooltip shows the whole name

> r[ui.text.elide.own-tooltip]
> A one-line text whose place has a tooltip of its own keeps that tooltip whether or not the text is
> cut with "…": the whole text does not take its place.
>
> - tier: human (a tooltip is judged by eye)
> - do: Rename the peer "A very long machine name that its audio map card cannot hold", open Audio & Camera, narrow the window until that name on its audio map card ends in "…", and rest the pointer on the name
> - see: The tooltip is the card's sentence (`viewmodel.audio.map.description`), the whole name followed by ", online." and the rest, not the name alone

## Keyboard focus

When a focus ring shows and how keyboard focus is drawn. Keyboard navigation is the state in which
the user moves keyboard focus with the keyboard.

> r[ui.focus.keys]
> Tab, Shift-Tab and the arrow keys start keyboard navigation, and from then on the control with
> keyboard focus draws its focus ring. A key pressed with Command (on a Mac) or with Ctrl, Alt or the
> Windows key (on a PC), and any other key, Space and letters included, does not start it.
>
> - tier: human (a focus ring is judged by eye)
> - do: Click a control on the Design page, then press a letter and Ctrl-Tab (Command-Tab on a Mac does nothing to the window), then press Tab
> - see: No ring shows after the letter or the modified Tab; after Tab the next control shows a ring

> r[ui.focus.click-ends]
> A mouse press anywhere in the settings window ends keyboard navigation: no control draws a focus
> ring until the next key that starts it, the control that still has keyboard focus included. A
> press on words that take no click leaves keyboard focus where it was, without its ring.
>
> - tier: human (a focus ring is judged by eye)
> - do: Tab to an option on the Design page, then click on a note's words, then click a switch
> - see: The ring disappears at the first click and does not show on the switch after the second

> r[ui.focus.ring]
> A focus ring is 2 pt of `signal` drawn 3 pt outside the control, its corners following the
> control's; a switch's ring goes round its track.
>
> - tier: human (a focus ring is judged by eye)
> - parity: Python's PC drew focus as the control's own border turned 1 or 2 px `signal`, inside the control, and its Mac ringed a switch's words and track together (ADR 0001: one Rune UI draws both machines)
> - do: Tab through the controls on the Design page
> - see: Each control in turn shows a cyan ring just outside it, following its rounded corners, and a switch's ring hugs its track

> r[ui.focus.thumb]
> A ruler or the resistance strip with keyboard focus during keyboard navigation draws its thumb in
> `signal` in place of a ring, and in `text` as soon as it loses keyboard focus or keyboard
> navigation ends.
>
> - tier: human (the thumb is judged by eye)
> - do: Tab to the resistance strip on Crossing, then Tab away, then Shift-Tab to it and click elsewhere
> - see: The thumb turns cyan when the strip has keyboard focus and returns to the text colour at once when it loses it or on the click

> r[ui.focus.click-then-arrows]
> A click or drag on a ruler or the resistance strip gives it keyboard focus without lighting its
> thumb, so its arrow keys move it next; the first arrow key starts keyboard navigation and lights
> the thumb.
>
> - tier: human (the thumb is judged by eye)
> - do: Click the Length ruler on the Design page, then press the right arrow key
> - see: The click leaves the thumb in the text colour; the arrow moves the ruler and its thumb turns cyan

> r[ui.focus.text-field]
> A text field with keyboard focus always draws its focus ring, whether a click, Tab or the window
> gave it keyboard focus, so the user sees where typing goes.
>
> - tier: human (a focus ring is judged by eye)
> - do: Click into the address field on the pairing sheet and type
> - see: The field shows its cyan ring while the caret is in it

> r[ui.focus.words-inert]
> Text in the settings window cannot be selected and takes no keyboard focus: a click on words, on
> their own or inside an option, tile or switch, shows no caret or selection and does not redraw
> them.
>
> - tier: human (selection is judged by eye)
> - do: Double-click and drag across a note and across a tile's name on the Design page
> - see: No text is selected, no caret appears, and the words keep their look

> r[ui.focus.wheel.unfocused]
> A scroll of the wheel or of a trackpad over a ruler, the resistance strip, a menu or a number
> field on a page that scrolls, while that control does not have keyboard focus, scrolls the page by
> the same distance as anywhere else on it and neither changes the control nor gives it keyboard
> focus.
>
> - tier: human (scrolling is felt by hand)
> - do: On the Design page, scroll the page with the wheel while the pointer passes over the Length and Size rulers
> - see: The page scrolls smoothly past them and both rulers keep their values

> r[ui.focus.wheel.focused]
> On a PC, once Tab or a click has given a ruler, the resistance strip, a menu or a number field
> keyboard focus, the wheel over it changes it and the page stays still; once keyboard focus moves
> elsewhere, the wheel over it scrolls the page again. On a Mac the wheel never changes a control.
>
> - tier: human (scrolling is felt by hand)
> - do: On the PC, click the Length ruler and turn the wheel over it, then click a note and turn the wheel over the ruler again
> - see: The first time the ruler's value changes and the page stays; the second time the page scrolls and the ruler keeps its value

## The settings window

How big the settings window is, where it opens and how it resizes. What it shows is
`spec/viewmodel.md`'s (`viewmodel.pages.*`); its colours, type and title bar are `ui.look.*`'s and
`ui.theme.*`'s. Sizes are in pt, Rune's logical units, which a PC's px equal at 100 % scaling.

> r[ui.window.minimum]
> The settings window can be made as small as 320 by 300 pt, and no smaller, on a Mac and on a PC.
> At that size every page still lays out as `ui.page.column` says, the sidebar collapsed
> (`ui.sidebar.collapsed`).
>
> - tier: human (window sizes are judged by hand)
> - parity: Python's Mac stopped the window at 640 by 540 pt while its PC went down to 320 by 300, and the Mac's pages were already laid out for 320 (ADR 0001: one Rune UI draws both machines, at the PC's smaller minimum)
> - do: Drag the settings window's corner inwards as far as it goes, on the Mac and on the PC
> - see: The window stops at about a third of its usual width and half its height, with the sidebar down to icons and the page still readable

> r[ui.window.remembered]
> The settings window first opens at 900 by 640 pt, centred on its display's visible area. Each
> later opening, in this run or the next, puts it where it last was and at the size it last had as
> an ordinary window, moved and shrunk as little as needed to lie inside its display's visible
> area. A window in full screen, filled by a title-bar double-click or tiled by the OS is not
> remembered: the frame it had before is.
>
> - tier: human (where a window opens is judged by eye)
> - parity: Python's PC opened the window at 900 by 640 each time, where its Mac remembered the frame (ADR 0001: one Rune UI draws both machines, the Mac's)
> - do: Move the settings window to a corner and make it larger, close it, quit and start Beamer, and open it again; then put it in full screen, close it and open it again
> - see: It opens in that corner at that size both times, never at the full-screen size, and never hanging off the display

> r[ui.window.no-width-cap]
> The settings window has no greatest width: it can be widened to its display's full width, filled
> from its title bar (`ui.look.titlebar.double-click`), maximised from its title bar's button on a
> PC and put in full screen on a Mac, and no page holds it narrower.
>
> - tier: human (window sizes are judged by hand)
> - do: On the PC, press the settings window's maximise button; on the Mac, double-click its title bar with "Fill" set
> - see: The window fills the whole width of the display, its pages centred in it (`ui.page.column`)

> r[ui.window.pages-keep-size]
> Nothing inside the settings window changes its size: showing another page, a module showing or
> hiding, or text growing leaves the window at the size the user gave it, and the page lays out in
> that size.
>
> - tier: human (window sizes are judged by hand)
> - do: Make the window small, then click through every page in the sidebar and open the pairing sheet
> - see: The window keeps its size and place on every page

> r[ui.window.drag-strip]
> On a Mac, an 18 pt strip runs across the top of the settings window's content, filled with `ground`
> with a 1 pt `hairline` along its bottom, under the transparent title bar. Pressing it and dragging
> moves the window, as the title bar does; a double-click on it is `ui.look.titlebar.double-click`'s.
>
> - tier: human (window behaviour is judged by hand)
> - do: On the Mac, press just above the sidebar's link block and drag
> - see: The window follows the pointer as when dragged by its title bar

> r[ui.window.scale]
> The settings window draws at its display's own scale, 100 %, 150 %, 200 % or any other: its text,
> lines, boxes and previews are drawn at that many device pixels per pt, sharp, and none is a
> smaller image scaled up; its sizes in pt stay as the rules give them.
>
> - tier: human (sharpness is judged by eye)
> - do: On the PC, set Windows' display scaling to 150 %, open the Design page and look closely at a style tile's preview and its words
> - see: The preview's light and the words have crisp edges, with no blur, and the page is laid out as at 100 %

> r[ui.window.resize-steady]
> Resizing the settings window changes a part's layout only as the width passes that part's
> threshold, once each time (`ui.page.narrow-layout`, `ui.page.side-by-side`), and settles as soon as
> the resize stops. Moving the window, resizing it at the same width, or leaving it alone changes
> nothing inside it.
>
> - tier: human (steadiness is judged by eye)
> - do: Open the Crossing page and drag the window's edge slowly from its narrowest to its widest and back; then drag the window round the display by its title bar
> - see: Each part jumps to its other layout once going narrower and once going wider, never flickering between the two; while the window is moved nothing inside it shifts

## Pages

How every page lays out in the width the window leaves it: one capped, centred column of modules,
scrolling only downwards. "The page's width" is the window's width less the sidebar
(`ui.sidebar.width`), its 1 pt divider and 16 pt for the scroll bar.

### The column

> r[ui.page.column]
> Every page sets its modules in one column, centred in the page's width, as wide as the page's
> width less 24 pt on each side but never wider than 880 pt. Each module spans the column exactly,
> its left and right edges on the column's. On a wider window the column stays 880 pt wide and
> centred, with `ground` on both sides of it.
>
> - tier: human (how a page is laid out is judged by eye)
> - do: Look at each page with the window at its minimum width, at its default width, and filling a large display
> - see: The modules form one column with an even margin either side, the same on every page; on the large display the column stops growing and sits in the middle

> r[ui.page.module-inset]
> A module's content starts 16 pt in from its left, right and top edges, and the lines of controls
> in a module are 12 pt apart, on every page.
>
> - tier: human (spacing is judged by eye)
> - do: Look at the modules of Overview and Design at a wide window
> - see: Each module's contents sit the same small distance in from its edges, with even gaps between its lines, alike on both pages

> r[ui.page.no-sideways]
> No page scrolls sideways or shows a sideways scroll bar, at any window size from the minimum
> (`ui.window.minimum`) up: whatever does not fit wraps, stacks or is cut with "…"
> (`ui.text.never-clipped`).
>
> - tier: human (scrolling is judged by hand)
> - do: At the window's minimum width, open each page and scroll sideways with the trackpad or Shift and the wheel
> - see: No page moves sideways and none shows a horizontal scroll bar

> r[ui.page.scrolls-to-end]
> Every page scrolls down to the end of its last module and no further, at any window size, and
> scrolled to its end shows that module's bottom whole above the footer. Only the page shown
> scrolls; showing another page starts it at its own top.
>
> - tier: human (scrolling is judged by hand)
> - do: With the window at its minimum height, open each page and scroll to the bottom; widen the window to a large display and do it again
> - see: Each page stops with the bottom of its last module just visible above the footer, nothing cut off

> r[ui.page.fit]
> At every window width from the minimum up, every control on every page, its words and anything
> inside it, lie inside its module and the page's column, and no two controls overlap: a machine's
> switches, a remove question's buttons, a row of options, "Identify displays" and the Crossing
> page's buttons included, in every state a page can show.
>
> - tier: human (overlap is judged by eye)
> - do: Go through every page at the minimum width, at 640 pt and at a wide window, with the pairing sheet open and with "Show where the pointer lands" on
> - see: Nothing pokes out of its module, nothing sits on top of anything else, and every control's words show whole

> r[ui.page.tab-reach]
> On every page, Tab and Shift-Tab reach every control that can be used, in the order the page
> reads, one stop each, the Camera module's "Camera your apps see" list, its "Quality" choice and
> its "Camera settings" button included; a group of options or tiles is one stop, taken at its
> chosen one, whose others the arrow keys reach.
>
> - tier: human (keyboard reach is judged by hand)
> - do: On the PC, with Beamer Camera installed, open Audio & Camera and choose the Mac's "Default camera" under "Camera your apps see" (`viewmodel.camera.quality.off`); click the page's first menu, then press Tab repeatedly until the focus comes round again, using the arrow keys inside "Quality"
> - see: The focus ring visits every menu, button and choice of the page once, in reading order, "Camera settings" included, and the arrow keys move it between "720p" and "1080p"

### Width thresholds

> r[ui.page.narrow-layout]
> A page has a wide and a narrow layout. It takes the narrow one once the page's width falls below
> 584 pt, and the wide one once it reaches 616 pt, keeping the one it has in between; it starts
> wide from 600 pt. In the narrow layout the page's title steps from 26 to 22 pt, the link status's
> word, readouts and numerals take their narrow sizes (`ui.look.font.sizes`), and the pairing sheet
> stacks its halves (`ui.sheet.halves`).
>
> - tier: human (a change of placement is judged by eye)
> - parity: Python's PC drew one layout at every width; its Mac switched at 600 pt with this 16 pt band (ADR 0001: one Rune UI draws both machines, the Mac's)
> - do: Open Overview and drag the window's edge slowly back and forth across the point where the link status's word shrinks
> - see: The word shrinks once going narrower and grows once going wider, a little further out, and never flickers while the edge hovers near that point

> r[ui.page.side-by-side]
> Two blocks a page can show next to each other, such as the profile tabs and their switch or a
> saved zones entry's words and its buttons, stand side by side while the width they share holds
> both blocks' least widths and the gap between them, and one above the other otherwise. Once side
> by side they stack only when that width falls 16 pt short of it, and once stacked they go side by
> side only when it is 16 pt more, so a width near the threshold never makes them flicker.
>
> - tier: human (a change of placement is judged by eye)
> - do: On the Crossing page with own zones on every machine off, drag the window's edge slowly back and forth where the profile tabs and the switch beside them stack
> - see: They stack once going narrower and go back side by side once going wider, a little further out, without flickering

> r[ui.page.menu-width]
> A menu on a page, such as "Match design with" or a camera menu, is never wider than 320 pt nor
> wider than its module. Its list shows each choice's whole words, a long machine name included,
> and its accessible value is the chosen choice's whole words.
>
> - tier: human (a menu is judged by eye)
> - do: Name a peer "A very long machine name kept in full in the menu", then open "Match design with" on the Design page at a wide window
> - see: The menu is no wider than about a third of the column, and its open list shows the whole long name

### Overview

> r[ui.page.overview.pair-button]
> "Pair a machine" stands at the head of the Machines list, above its rows or its note while it
> has none (`viewmodel.machines.list.empty`), as a plain button (`ui.widget.button.look`), never a
> primary one.
>
> - tier: human (placement is judged by eye)
> - do: Look at Overview with no peer paired, then with one
> - see: "Pair a machine" is an outlined button at the top of the Machines list both times, above the note or the machine's line

> r[ui.page.overview.address-face]
> An address on Overview, in a machine's line, on the pairing sheet's own address and in a heard
> machine's line, is set in 11.5 pt Hanken Grotesk, like the words beside it, never in B612 Mono.
>
> - tier: human (a typeface is judged by eye)
> - do: Open the pairing sheet with a machine heard nearby and look at the addresses in the Machines list and on the sheet
> - see: Each address is in the same face as its line's words, with ordinary spacing round its dots

> r[ui.page.overview.remove-question]
> A peer's remove question (`viewmodel.machines.remove.ask`) wraps its words at the reading width
> (`ui.text.reading-width`) and shows them whole, and its two buttons keep the width of their own
> words at every window width, the full screen's included, rather than stretching across the
> line.
>
> - tier: human (a question's layout is judged by eye)
> - do: Press Remove on a peer's line with the window in full screen, then at its minimum width
> - see: Both times the question is fully readable and its two buttons are snug round their words

### Crossing page

> r[ui.page.crossing.map-size]
> The crossing map is as wide as its module's content and 260 pt tall, or 220 pt while the window
> is 640 pt wide or narrower.
>
> - tier: human (a map's size is judged by eye)
> - do: Look at the Crossing page at a wide window, then narrow the window to 640 pt
> - see: The map fills its module's width at both sizes and is a little shorter at the narrow one

> r[ui.page.crossing.hint-gap]
> The map's hint (`viewmodel.map.hint`) stands 12 pt under the crossing map's bottom edge at every
> window width, whatever the map shows.
>
> - tier: human (spacing is judged by eye)
> - do: On the Crossing page, turn a corner zone on and off at a wide window and at 640 pt
> - see: The line under the map stays the same small distance below it every time

> r[ui.page.crossing.profile-header]
> The profile tabs and the switch for own zones on every machine stand side by side, the switch at
> the module's trailing end, while 396 pt holds them (the tabs' least 120 pt, the switch's 260 pt
> and a 16 pt gap, by `ui.page.side-by-side`), and otherwise one above the other, the switch at the
> leading edge. The switch's words sit next to its track (`ui.widget.switch.look`), never parted by
> the spare width.
>
> - tier: human (placement is judged by eye)
> - parity: Python's PC stacked them below 420 pt with no band (ADR 0001: one Rune UI draws both machines, the Mac's)
> - do: On the Crossing page with own zones on every machine off and a second profile, narrow the window from wide to its minimum
> - see: The tabs and the switch share one line with the switch at the end, then the switch moves under the tabs at the start, its words always beside its track

> r[ui.page.crossing.profile-tabs]
> Each profile tab is drawn with a 1 pt border, the active one marked as
> `viewmodel.profiles.tabs.active` says, and the tab that adds a profile is a borderless "+" at
> most 24 pt wide.
>
> - tier: human (a tab's look is judged by eye)
> - do: With own zones on every machine off, add a profile named "Sofa" on the Crossing page
> - see: Each profile is an outlined tab, and the add button after them is a small plain "+" with no box

> r[ui.page.crossing.saved-zones]
> The own-zones module and its saved zones (`viewmodel.profiles.saved.entry`) are as wide as the
> crossing ledger's module. In each saved entry the words take the spare width and the Restore and
> Delete buttons stand at the line's trailing end, borderless, keeping the same width at every
> window width.
>
> - tier: human (placement is judged by eye)
> - do: Open the saved zones on the Crossing page, then widen the window from 640 pt to a large display and back
> - see: Restore and Delete stay at the right end of each entry, the same size throughout, while the words take up the rest

### Design page

> r[ui.page.design.order]
> In the Style module, "Style for" and "Show at" come first, each across the module's full width,
> above the style tiles; the chosen style's name and its note, then Length and then Size, come after
> the last line of tiles.
>
> - tier: human (order is judged by eye)
> - do: On the Design page, turn on "Show where the pointer lands", then look at the Style module at a narrow and at a wide window
> - see: The two choices sit above the tiles across the whole module, and the style's description and the two rulers sit below the tiles

> r[ui.page.design.rulers]
> Length and Size stand one above the other, each ruler at most 240 pt wide and never wider than
> its module, at every window width.
>
> - tier: human (placement is judged by eye)
> - do: On the Design page, widen the window from its minimum to a large display
> - see: The Size ruler is always under the Length ruler, and neither stretches across a wide module

> r[ui.page.design.tile-size]
> A style tile is never narrower than 96 pt, so its name stays readable on one line wherever its
> name fits, the shortcut's style tiles included.
>
> - tier: human (a tile's size is judged by eye)
> - do: Turn on "Show where the pointer lands", choose "Shortcut and menu" under "Style for" and narrow the window to 640 pt
> - see: Every style tile is wide enough to show its name whole, inside the tile

### Audio & Camera

How the audio map (`viewmodel.audio.map.*`, `viewmodel.camera.map.*`) places its cards and arrows.
The order of arrows between two machines is `ui.map.audio.arrow-order`'s.

> r[ui.page.audio.map-fit]
> The audio map is 260 pt tall. Its machines' cards keep the places of their footprints, scaled
> alike to the largest size that fits the map less a 12 pt margin, at most 1.15 times, and centred;
> between two cards side by side, or one above the other, the map adds 52 pt, so cards whose
> footprints touch keep their order with room for an arrow between them.
>
> - tier: human (how the audio map is drawn is judged by eye)
> - parity: Python's Mac opened 40 pt across and 28 pt down between cards whose footprints touch, kept a 16 pt margin, scaled up to 1.6 times, and grew the map from 260 pt to as much as 420 pt for footprints in several rows (ADR 0001: one Rune UI draws both machines, the PC's, which leaves room for three arrows)
> - do: Open Audio & Camera with the peer's footprint touching This Mac's right edge on the crossing map, then move the peer's footprint under This Mac's on the Crossing page and open Audio & Camera again
> - see: Both times the two cards sit as their footprints do on the crossing map, side by side and then one above the other, both inside the map with a clear gap between them

> r[ui.page.audio.arrow-path]
> An arrow between two cards that share a line runs straight across the gap between their facing
> sides, level (or upright for cards one above the other), from 6 pt outside one card to 6 pt
> outside the other, through the middle of the stretch they share; between other cards it runs
> along the line between their centres, from 6 pt outside each. Two or more arrows between the
> same two cards never lie on one another.
>
> - tier: human (how the audio map is drawn is judged by eye)
> - do: Share the Mac's microphone with the PC and play the PC's sound on the Mac, their cards side by side
> - see: Two separate level arrows cross the gap between the cards, one each way, neither touching a card

> r[ui.page.audio.card-lines]
> A card on the audio map shows its camera line (`viewmodel.camera.map.badge`,
> `viewmodel.camera.map.watching`) 19 pt above its microphone line (`viewmodel.audio.map.mic-line`).
> A line shows only where it lies at least 48 pt below the card's top, or 30 pt for an important
> line: the shared microphone or camera, or a line saying who hears it or that an app watched from
> elsewhere uses the camera. Short of room, an important line takes the lower place first, the
> microphone's when both are important or neither is.
>
> - tier: human (how the audio map is drawn is judged by eye)
> - do: Share the Mac's camera to the PC while the PC's own microphone is not shared, and make the window narrow so the cards are small
> - see: The Mac's card keeps the camera's name and leaves out the microphone line; at a wide window both show, the camera's above the microphone's

### Keycaps

> r[ui.page.keycap.fits]
> A keycap that records a key, the shortcut key's on the Crossing page and "Add a key or button" on
> the Keyboard page, holds its key's words and its hint whole: both are set 12 pt in from its sides,
> the hint under the words across the whole width between, both wrap onto more lines rather than
> being cut, and the keycap grows to hold them with 8 pt under the hint.
>
> - tier: human (a keycap is judged by eye)
> - do: At the window's minimum width, at 150 % scaling on the PC, look at "Add a key or button" on the Keyboard page and the shortcut key on the Crossing page
> - see: Each keycap shows its words and its "Click, then press…" hint in full inside its border, wrapping if need be

## The sidebar

How the sidebar's width, rows and keyboard focus follow the window. Its pages, dots and words are
`viewmodel.pages.*`'s and `viewmodel.status.*`'s.

> r[ui.sidebar.width]
> The sidebar is 56 pt wide while the window is narrower than 640 pt, 144 pt while it is narrower
> than 800 pt, and 176 pt from 800 pt, with a 1 pt `hairline` divider between it and the page; it
> changes at once as the window passes 640 or 800 pt, in either direction.
>
> - tier: human (the sidebar's width is judged by eye)
> - do: Drag the window's edge from its widest to its minimum and back
> - see: The sidebar narrows a step at about 800 pt and to icons only at 640 pt, and widens again at the same widths

> r[ui.sidebar.collapsed]
> While the sidebar is 56 pt wide it is collapsed: each page's row is 44 by 36 pt, centred in the
> sidebar, showing only its 18 pt icon, with the page's name as its tooltip; the link block shows
> only its light and the short response time (`viewmodel.status.response-compact`); and the foot
> shows Beamer's icon in place of its words.
>
> - tier: human (the sidebar is judged by eye)
> - do: Narrow the window below 640 pt and hover each icon in the sidebar
> - see: The sidebar is a column of centred icons, each naming its page in a tooltip, with the link's light and its milliseconds at the top

> r[ui.sidebar.row-look]
> Each page's row is 36 pt tall, its icon then its name in 13 pt. The page shown has its row filled
> `well` with a 2 pt `signal` bar down its leading edge, its name semibold in `text` and its icon
> `text`; the other rows have their names in `text-2` and their icons in `text-3`.
>
> - tier: human (the sidebar is judged by eye)
> - parity: Python's PC set the shown page's name bold and its bar 7 pt short of each end (ADR 0001: one Rune UI draws both machines, the Mac's)
> - do: Click through the pages in the sidebar
> - see: The page shown is a lighter band with a cyan bar at its left and a bolder name; the others are quieter

> r[ui.sidebar.dot]
> A page's dot (`viewmodel.pages.dot-permissions`, `viewmodel.pages.dot-dormant`,
> `viewmodel.pages.dot-firewall`) is a 6 pt round mark in its tone (`ui.look.tone.*`), centred on
> the row's height 8 pt in from its trailing end, and while the sidebar is collapsed at least 2 pt
> clear of the row's icon.
>
> - tier: human (the sidebar is judged by eye)
> - parity: Python's PC set the dot 10 pt in from the row's end (ADR 0001: one Rune UI draws both machines, the Mac's)
> - do: On the Mac, with Accessibility not granted, look at Permissions in the sidebar at a wide window and below 640 pt
> - see: A small amber dot sits at the end of the row, and beside the icon, not on it, when collapsed

> r[ui.sidebar.selection]
> The sidebar's selected row is always the page shown, and at most that row has keyboard focus. A
> click selects a row without a focus ring; during keyboard navigation (`ui.focus.keys`) the arrow
> keys select the row above or below at once and keyboard focus moves with it, its ring 2 pt of
> `signal` drawn 2 pt inside the row; the window becoming active again leaves the same row
> selected.
>
> - tier: human (focus is judged by eye)
> - do: Click Design in the sidebar, then press Tab until the sidebar has focus and press the up arrow twice; bring another app to the front, then the settings window again
> - see: The click shows Design with no ring; each arrow shows the page above at once with the ring on its row only; after switching back the same page is selected

> r[ui.sidebar.tab-first]
> With nothing in the settings window holding keyboard focus, the first Tab gives it to the
> sidebar's selected row, and the next Tab enters the page shown.
>
> - tier: human (focus is judged by hand)
> - do: Open the settings window on Design, click an empty part of the page, and press Tab twice
> - see: The first Tab rings Design in the sidebar, the second rings the first control on the Design page

> r[ui.sidebar.above-footer]
> At every window size down to the minimum (`ui.window.minimum`), every page's row and the sidebar's
> foot end above the window's footer: none is covered or cut by it.
>
> - tier: human (overlap is judged by eye)
> - do: Make the window as small as it goes
> - see: Every icon in the sidebar and Beamer's icon at its foot show whole above the footer line

> r[ui.sidebar.response-wraps]
> While the sidebar is 144 or 176 pt wide, the response time (`viewmodel.status.response-time`)
> wraps onto a second line when it does not fit on one, and its line grows to show it whole.
>
> - tier: human (wrapping is judged by eye)
> - do: Connect to a peer over a slow network so the response time reads in thousands of milliseconds, with the window between 640 and 800 pt wide
> - see: "Response time: 3000 ms" shows whole under the link's word, on two lines if need be

## The pairing sheet

How the pairing sheet (`viewmodel.pairing-sheet.*`) is laid out, and how its code boxes are drawn
and feel. Which digits they take is `viewmodel.pairing-sheet.code-digits`,
`viewmodel.pairing-sheet.code-spread` and `viewmodel.pairing-sheet.code-replace`.

> r[ui.sheet.halves]
> The pairing sheet's two halves, showing a code and entering one, stand side by side at equal
> widths in the page's wide layout and one above the other in its narrow layout
> (`ui.page.narrow-layout`).
>
> - tier: human (placement is judged by eye)
> - parity: Python's PC stacked the halves at every width (ADR 0001: one Rune UI draws both machines, the Mac's)
> - do: Open the pairing sheet at a wide window, then narrow the window to its minimum
> - see: The two halves sit next to each other when wide and one under the other when narrow

> r[ui.sheet.boxes.look]
> A code, typed or shown (`viewmodel.pairing-sheet.code-shown`), is drawn in six boxes in one line,
> centred in the width the sheet gives it, in two groups of three: each box as wide as a digit plus
> 4 pt either side and at least 34 by 40 pt, 6 pt apart, 18 pt between the third and the fourth.
> Where the line is narrower than that, the boxes narrow alike to fit it and the digits step down in
> size, never cut. A box is filled `ground` with 4 pt corners and a 1 pt `outline` border; the box
> of the typed code that has keyboard focus has a `signal` border instead, in place of a focus
> ring. Digits are B612 Mono in `text`, 20 pt typed and 48 pt shown; an empty box draws no digit,
> and the shown boxes take no typing.
>
> - tier: human (the code boxes are judged by eye)
> - parity: Python's Mac drew the typed code at 20 pt and the shown code at 34 pt bold, each in six 48 pt tall boxes 4 pt apart filling the sheet's width, with no gap between the groups, and kept the `outline` border on the box with keyboard focus (ADR 0001: one Rune UI draws both machines, the PC's look, which groups the digits as the shown code reads and marks the box typing goes into)
> - do: Open the pairing sheet, press "Show a code", then type three digits into the code to enter
> - see: Both codes sit in a centred line of six boxes split three and three; the typed code's fourth box, where typing goes next, has a cyan border

> r[ui.sheet.boxes.centred]
> Each digit is drawn centred in its box to within one device pixel, across and up and down, for
> every digit from 0 to 9, typed, shown and selected, at 100 %, 150 % and 200 % scaling.
>
> - tier: human (centring is judged by eye)
> - do: Show the code and type a code of 1s and then of 8s, on the Mac's Retina display and on the PC at 150 %
> - see: Every digit sits in the exact middle of its box, a 1 as centred as an 8

> r[ui.sheet.boxes.caret]
> While a box of the typed code has keyboard focus (`viewmodel.pairing-sheet.code-spread`) and
> nothing in it is selected, a 1 pt `signal` caret stands in that box, from 8 pt under its top to
> 8 pt above its bottom: in an empty box at the left edge of where a digit sits centred in it, and in
> a box holding a digit just before or just after that digit, on the side a click or an arrow key
> put it. No other box draws a caret. An input method's window opens at the caret.
>
> - tier: human (the caret is judged by eye)
> - parity: Python's Mac drew each box's own caret in `text`, centred in an empty box; its PC took the code in one field drawn as six cells, with one `signal` caret in the cell the next digit went into (ADR 0001: one Rune UI draws both machines: the PC's caret, in the six boxes of `viewmodel.pairing-sheet.code-spread`, which are the Mac's)
> - do: Type 12, press Space, then paste "3-4"; then click the right half of the first box
> - see: The cyan caret stands at the left of the empty third box after 12 and stays there after Space; after the paste it stands in the empty fifth box; after the click it stands just after the 1

> r[ui.sheet.boxes.keys]
> In the typed code, a digit typed into a box moves keyboard focus to the next box, the sixth box
> keeping it. Backspace with the caret after a box's digit clears that digit and keeps keyboard
> focus in the box; Backspace in an empty box other than the first clears the box before it and
> moves keyboard focus there. A key the code does not take, such as a space, a hyphen or a letter,
> changes no box and leaves keyboard focus and the caret where they were.
>
> - tier: human (typing is felt by hand)
> - parity: Python's PC took the code in one field drawn as six cells, in which Backspace removed the digit before the caret; its Mac also moved keyboard focus on when a letter or a space was typed into a box holding a digit (ADR 0001: one Rune UI draws both machines, the Mac's six boxes, which `viewmodel.pairing-sheet.code-spread` takes)
> - do: Type 1234, press Backspace three times, then type 9; then press Space and a letter
> - see: The digits fill the first four boxes; each Backspace clears the last digit left and the cyan box steps back one; the 9 goes into the second box, so the code reads 1 9 with the third box cyan, and Space and the letter change nothing

> r[ui.sheet.boxes.select]
> In the typed code a selection stays within one box: a double-click on a box's digit, or a drag
> across it, selects that digit, and a drag that leaves the box selects nothing in the boxes it
> passes over. A box whose digit is selected is filled `signal-dim` inside its border and draws no
> caret.
>
> - tier: human (selection is judged by hand)
> - parity: Python's PC took the code in one field drawn as six cells, across which a press and drag, or a press then a Shift-press, selected the digits of several cells (ADR 0001: one Rune UI draws both machines, the Mac's, whose six boxes `viewmodel.pairing-sheet.code-spread` takes)
> - do: Type a full code, double-click its third box, then drag from the fourth box's digit across the fifth and sixth
> - see: The double-click fills the third box with the dim selection colour behind its digit; the drag fills the fourth box only, never the fifth or sixth

> r[ui.sheet.boxes.select-shown]
> The shown code can be selected across its six boxes, digit by digit: a press and drag, or a press
> then a Shift-press, selects the digits of the boxes between, and a double-click selects one digit.
> Each box whose digit is selected is filled `signal-dim` inside its border. Unlike the window's
> other words (`ui.focus.words-inert`), the shown code can be selected so, and it copies as it reads,
> "482 913".
>
> - tier: human (selection is judged by hand)
> - parity: Python's Mac drew the shown code in boxes that could not be selected (ADR 0001: one Rune UI draws both machines, the PC's, so the code can be copied)
> - do: Press "Show a code", drag across its six boxes, copy, and paste into a text editor; then double-click its third box
> - see: The drag highlights all six digits and pastes as the code reads, three digits, a space and three digits; the double-click highlights the third digit alone

## The crossing ledger

How the crossing ledger (`viewmodel.ledger.*`) is laid out at any width and how its states look.

> r[ui.ledger.table]
> The crossing ledger is a table, its column headers in a line above the rows and each row's
> cells side by side under them, while it is at least as wide as its columns' least widths, Machine
> 132, Edges 220, Corners 70, Notch 64 (on a Mac) and Keys 132 pt, and 16 pt between neighbours:
> 682 pt on a Mac, 602 pt on a PC. Machine, Edges and Keys share the width left over 1.3 to 2 to 1.
> Below that width the rows stack (`ui.ledger.stacked`) and the headers hide.
>
> - tier: human (how the ledger is laid out is judged by eye)
> - parity: Python's PC stacked below 620 pt with a 128 pt Keys column (ADR 0001: one Rune UI draws both machines, the Mac's column widths)
> - do: On the Crossing page with a peer, look at the ledger at a wide window, then at 640 pt
> - see: Wide, it is a table under the headers "Machine", "Edges", "Corners" and "Keys"; at 640 pt each peer is a block with no headers

> r[ui.ledger.stacked]
> In stacked rows, the Machine cell runs from 12 pt in to the row's end, 12 pt under the row's top;
> the Edges cell, 12 pt in from either side, comes 12 pt under it; and Corners (70 pt wide, 12 pt
> in), on a Mac the Notch cell (64 pt, 16 pt after it) and Keys (16 pt after the cell before it,
> the rest of the width, at least 128 pt) stand side by side 12 pt under the Edges cell, the row ending 12 pt under the
> tallest of them.
>
> - tier: human (how the ledger is laid out is judged by eye)
> - do: Narrow the window to 640 pt on the Crossing page
> - see: The peer's row is a block: its name on top, its edge chips across the block below, and the corners, notch and keys side by side at the bottom, nothing reaching past the ledger's edge

> r[ui.ledger.selected]
> Rows are parted by a 1 pt `hairline` along each row's top. The row of the peer selected on the
> map carries a 3 pt `signal` bar down its leading edge, from 12 pt under its top to 12 pt above its
> bottom, and every cell, the name, its status line and its dot included, starts at least 12 pt
> clear of that bar, in the table and in stacked rows.
>
> - tier: human (how the ledger is laid out is judged by eye)
> - do: Select a peer on the crossing map, at a wide window and at 640 pt
> - see: Its ledger row has a cyan bar at its left, and the name, dot and chips keep a clear gap from it

> r[ui.ledger.chips.flow]
> A row's chips (`viewmodel.ledger.edge.chips`) are 26 pt tall and 6 pt apart, each as wide as its
> words, icon and thirds need (its words plus 40 pt, and 28 pt more with thirds), but never wider
> than the Edges cell. They run from the cell's start and wrap onto further lines, 6 pt apart, where
> the next does not fit, and the cell and its row grow to hold every line.
>
> - tier: human (how the ledger is laid out is judged by eye)
> - do: Give a peer the top and bottom wrap-around edges and look at its row at a wide window, then at 640 pt
> - see: Four chips sit in the Edges cell, each showing its words whole, wrapping onto a second line when narrow, the row tall enough for them all

> r[ui.ledger.chips.states]
> A chip is drawn with a 1 pt border and 5 pt corners, an icon at its start and, on a `contact`
> chip, its three thirds at its end, each a short line, lit ones 3 pt thick in `signal` and the
> others 1.5 pt in `outline`. A chip that is on is filled `signal-dim` with a `signal` border,
> `text` words and a `signal` icon; a `contact` chip that is off has an `outline` border and
> `text-3` words; a `free` chip a dashed `outline` border and `text-2` words; a `kept` chip a dashed
> `signal` border at 45 % and `text-3` words; a `taken` chip is drawn at 45 % with a `hairline`
> border and `text-3` words. Under the pointer an enabled chip is filled `well` with a `text-2`
> border and `text` words, and while pressed it adds `signal` at 35 % and a `signal` border.
>
> - tier: human (a chip's look is judged by eye)
> - do: Give a peer a contact edge and a wrap-around edge, leave another edge free, and hover and press a chip
> - see: The on chips are tinted cyan, the free one has a dashed outline, a hovered chip lights up and a pressed one turns cyan

> r[ui.ledger.chips.disabled]
> A chip or a corner button that cannot be used, as in a row of a peer set aside or for a corner
> `taken` or `unavailable` (`viewmodel.ledger.corner.buttons`), is drawn with a solid 1 pt
> `hairline` border, no fill and its words, icon, thirds and bracket in `text-3`, whatever its
> state, and never with a dashed or `signal` border.
>
> - tier: human (a disabled control's look is judged by eye)
> - do: Press "Leave out" in the peer's ledger row and look at the row; then press "Put back", turn off any of its corners that is on, drag its footprint on the map so it touches nothing and look at its corner squares
> - see: First its chips and corner squares are plain grey outlines with grey words and brackets, none dashed or cyan; then its four corner squares are drawn the same way

> r[ui.ledger.corners.grid]
> A row's four corner buttons (`viewmodel.ledger.corner.buttons`) are 28 pt squares in a grid of two
> by two, 4 pt apart, top left first, filling a Corners cell 60 pt square, at every width and in
> every state.
>
> - tier: human (how the ledger is laid out is judged by eye)
> - do: Look at a peer's corners in the ledger at a wide window and at 640 pt
> - see: Four equal squares in two rows of two, the same size at both widths

> r[ui.ledger.corners.bracket]
> Each corner button draws a 7 pt L bracket, 1.8 pt wide, pointing to its corner and centred in its
> square, with the same margin on all four sides to within 1 pt and at least 3 pt. Free, it has a
> dashed `outline` border and a `text-2` bracket; on, it is filled `signal` with a `ground` bracket.
> A corner button disabled because its corner is `taken` or `unavailable`
> (`viewmodel.ledger.corner.buttons`) is drawn as `ui.ledger.chips.disabled` says.
>
> - tier: human (a bracket's place is judged by eye)
> - parity: Python's Mac drew an unavailable corner's disabled look at 40 % (ADR 0001: one Rune UI draws both machines, the PC's)
> - do: Turn a peer's top-left corner on and look at all four corner squares
> - see: Each bracket sits in the middle of its square pointing at its own corner, the top-left one dark on a cyan square

> r[ui.ledger.dimmed]
> A dimmed row (`viewmodel.ledger.dimmed`) draws its cells at 50 % opacity; its rule and selection
> bar stay at full strength.
>
> - tier: human (dimming is judged by eye)
> - do: Disconnect a peer and look at its ledger row
> - see: Its name, chips, corners and keys are faded to half strength, the row still in its place

> r[ui.ledger.keys]
> A row's Keys cell has the tick for the shortcut's cycle (`viewmodel.ledger.keys.cycle-tick`) on a
> 28 pt line, drawn as the map's tick is (`ui.map.cycle.look`), and under it the caption "Jump" with
> the jump key starting 36 pt in, beside it at every width. Clear stands 4 pt after the jump key, or
> under it where the cell is too narrow. Clear, Rename and Leave out are quiet buttons, 24 pt tall,
> each as wide as its words.
>
> - tier: human (how the ledger is laid out is judged by eye)
> - do: Tick a peer for the shortcut's cycle and give it a jump key, then look at its Keys cell at a wide window and at 640 pt
> - see: The tick shows a clear check mark, and the jump key sits right beside "Jump" with Clear next to or under it

> r[ui.ledger.jump-look]
> A peer's jump key with none set shows its words (`viewmodel.crossing.jump-key.empty`) as plain
> 12.5 pt Hanken Grotesk `text-2` words with no box; a set one shows the chord in B612 Mono `text`
> in a keycap box filled `well` with an `outline` border and 4 pt corners. While it records, its
> words are `signal` and its hint shows under them; when the recording ends, by a chord, a click or
> Esc, it shows the empty words or the keycap again and the hint hides.
>
> - tier: human (the jump key is judged by eye)
> - do: Click a peer's "Set a key", press Esc, click it again and press Option-F18
> - see: "Set a key" is plain grey words, turns cyan with a hint under it while recording, goes back after Esc, and after the chord shows "⌥ F18" in a small key box

> r[ui.ledger.rename]
> While a row is renamed (`viewmodel.ledger.rename.field`), the name field takes the name's place
> across the whole Machine cell and the hint "Return saves, Escape cancels" shows under it, the row
> growing 74 pt to hold them, inside the ledger at every width.
>
> - tier: human (how the ledger is laid out is judged by eye)
> - do: At 640 pt, press "Rename" in the peer's ledger row and type "Office studio PC"
> - see: The field spans the name's cell with "Return saves, Escape cancels" below it, and nothing spills out of the ledger

## The crossing map

How the crossing map on the Crossing page (F6) is drawn and felt. What it shows, enables and what
each click, drag, drop and key asks of `Core` are `viewmodel.map.*`'s, `viewmodel.profiles.picker.*`'s
and `viewmodel.profiles.word.*`'s; which contacts and zones a footprint makes are `spec/crossing.md`'s.
Sizes are in points (pt), the logical unit Rune draws in, which is a PC's pixel at 100 % scaling.
Colours are the look's colour roles (`ui.look.palette`), named in backticks (`signal`, `amber`, `text-2`…), in the light or
dark palette the window uses.

### The crossing map's fit

> r[ui.map.fit.centred]
> The map draws every footprint, and every display of every footprint, a display moved apart
> included, at one scale: the largest at which all of them fit inside the map's box less a 12 pt
> margin all round, and never more than 1 pt per map unit. The whole drawing is centred in the
> box, left to right and top to bottom. A column of three footprints 160 by 100 map units in a box
> 480 by 260 pt is drawn at 236 / 300 pt per map unit, centred both ways, whichever end this
> machine's footprint is at.
>
> - tier: human (how the crossing map is drawn is judged by eye)
> - do: On the Crossing page, place the peer's footprint to the right of This Mac's, then under it, then Option-drag one of the peer's displays (Alt-drag on a PC) far out and release it
> - see: Every time, the footprints and displays all fit in the map with an even margin and the group sits in the middle of it
> - parity: Python's Mac kept a 14 pt margin and drew up to 1.15 pt per map unit, its PC a 12 px margin and up to 1 px per map unit (ADR 0001: one Rune UI draws both machines)

> r[ui.map.fit.refit]
> When the crossing map changes, as when a footprint is dropped elsewhere or resized or a display is
> moved apart, the map fits and centres again (`ui.map.fit.centred`), its scale growing or
> shrinking to suit.
>
> - tier: human (how the crossing map is drawn is judged by eye)
> - do: Drag a peer's footprint well away from this machine's, below and to the right, and release it
> - see: Once released, the whole drawing shrinks and moves so that it is centred in the map again

> r[ui.map.fit.frozen-drag]
> From the press that starts a drag of a footprint, of a display or of a resize handle until its
> release, the map keeps the scale and placement it had at the press: this machine's footprint stays
> where it is on the page while the dragged footprint follows the pointer, even when it is dragged
> beyond the map's box, and even when the map's box changes size meanwhile, as when the selection
> the press makes changes the page. The map fits again only after the release.
>
> - tier: human (how a drag feels is judged by hand)
> - do: Press on a peer's footprint and drag it slowly to the map's edge and beyond before releasing; then do the same with a display Option-dragged (Alt-dragged on a PC) apart
> - see: During each drag nothing else moves or rescales and the footprint stays under the pointer; the map refits only after the release

> r[ui.map.fit.box]
> The map's box is filled with `ground`, with a 1 pt `outline` border and 4 pt rounded corners.
>
> - tier: human (how the crossing map is drawn is judged by eye)
> - do: Open the Crossing page in the light and then in the dark appearance
> - see: The map is a box with a thin grey border and slightly rounded corners, in the window's background colour, in both
> - parity: Python's Mac filled the map's box with `well` (ADR 0001: one Rune UI draws both machines)

### Footprints

How each machine's footprint, its displays, its name and its lines are drawn.

> r[ui.map.footprint.box]
> Each footprint is a rectangle with 5 pt rounded corners: this machine's is filled with `well` and
> bordered 1 pt in `outline`, and each peer's is filled with `panel` and bordered 1 pt in `outline`.
>
> - tier: human (how the crossing map is drawn is judged by eye)
> - do: Look at the map, then turn "Own zones on every machine" off and look again, This Mac viewed
> - see: This Mac's footprint and the peer's are filled in two different shades, both with softly rounded corners; This Mac's border is thin and grey at first, and the peer's is once the switch is off
> - parity: Python's Mac rounded footprints by 4 pt (ADR 0001: one Rune UI draws both machines)

> r[ui.map.footprint.selected]
> The footprint of the peer selected on the map is bordered 2 pt in `text-2`, a neutral colour and
> never `signal`, so that a lit seam along it stays apart from it. This machine's footprint keeps its
> 1 pt `outline` border whichever peer is selected.
>
> - tier: human (how the crossing map is drawn is judged by eye)
> - do: Click a peer's footprint whose edge crosses to this machine
> - see: Its border turns thicker and lighter grey, plainly not the colour of the lit seam beside it, and This Mac's border does not change

> r[ui.map.footprint.hover]
> A peer's footprint under the pointer that is not selected is bordered 1 pt in `text-3`, and returns
> to its own border when the pointer leaves it.
>
> - tier: human (how the crossing map is drawn is judged by eye; the border of a peer not selected under the pointer, which the desk's one peer can't show, stays out of the check)
> - do: Move the pointer over the peer's footprint, then over This Mac's, then off the map
> - see: Neither border changes while the pointer is over it

> r[ui.map.footprint.amber]
> A peer's footprint that is not selected and whose outcome is "Drag to connect" or "Overlaps"
> (`viewmodel.map.outcome`) is bordered 1 pt in `amber`.
>
> - tier: human (how the crossing map is drawn is judged by eye; the amber border of a peer not selected, which the desk's one peer can't show, stays out of the check)
> - do: Drag the peer's footprint away so that it touches nothing and release it
> - see: Its outcome reads "Drag to connect" in amber while its border stays the selected footprint's thick grey one, never amber
> - parity: Python's Mac drew that border 2 pt wide and the footprint's status line in `amber` as well (ADR 0001: one Rune UI draws both machines)

> r[ui.map.footprint.displays]
> A footprint of a machine with two or more displays shows each display (`viewmodel.map.displays`)
> as a rectangle 2 pt inside the display's place, with 2 pt rounded corners, filled with `well` and
> bordered 1 pt in `outline`. A footprint of a machine with one display, or whose displays are not
> known, shows no rectangle inside it.
>
> - tier: human (how the crossing map is drawn is judged by eye)
> - do: Look at the footprint of a machine with two displays, then at one with a single display
> - see: The first shows two display rectangles inside it with a thin gap around each; the second is a plain footprint
> - parity: Python's Mac drew a display's rectangle on the display's own place, for a single display too (ADR 0001: one Rune UI draws both machines)

> r[ui.map.footprint.name-above]
> A footprint's name and lines are drawn above its display rectangles and display numbers: the name
> stays fully readable on a footprint whose displays are known, this machine's included, with one
> display or several.
>
> - tier: human (how the crossing map is drawn is judged by eye)
> - do: Look at the footprints of this machine and of a peer, each with its displays known
> - see: Each name reads cleanly in full over the display rectangles, nothing drawn across it

> r[ui.map.footprint.name]
> A footprint's name is drawn in `text` at 13 pt, semibold, on one line, cut short with an ellipsis
> where it does not fit. On a footprint 56 pt tall or more its line is 22 pt tall, starting 7 pt
> below the footprint's top, and runs from 8 pt inside the left edge to 8 pt inside the right, the
> name centred on it. Where the footprint shows a tick for the shortcut's cycle, the name stays
> centred on the footprint while it fits between 30 pt in from each edge, and otherwise starts 8 pt
> in from the left edge and ends 6 pt short of the tick.
>
> - tier: human (how the crossing map is drawn is judged by eye)
> - do: On the Crossing page, under "Shortcut goes to", choose "Next among ticked"; rename the peer to a short name, then to one too long for its footprint
> - see: The short name sits centred on the footprint; the long one starts near the left edge and is cut with an ellipsis just before the tick, never under it
> - parity: Python's Mac drew the name's line 20 pt tall, 10 pt below the top (ADR 0001: one Rune UI draws both machines)

> r[ui.map.footprint.name-numbers]
> Where a display number lies on the name's line, the name takes the widest stretch of that line
> that stays 4 pt clear of every display number, and is cut with an ellipsis to fit it.
>
> - tier: human (how the crossing map is drawn is judged by eye)
> - do: Resize a peer with two displays next to each other down to its smallest footprint
> - see: The name never runs into a display number; it shrinks to the room beside them

> r[ui.map.footprint.numbers]
> Each display of a footprint carries the number identify displays gives it, in bold monospaced
> digits in `text`, on a box filled with `ground`, bordered 1 pt in `outline` with 3 pt rounded corners,
> 5 pt in from the display's left edge; a footprint whose displays are not known carries one number,
> placed on the footprint as on a display. On a footprint 56 pt tall or more the box is 18 by 16 pt
> with its bottom 5 pt above the display's bottom edge and 10 pt digits; 44 to 56 pt tall, 16 by
> 14 pt with its bottom 1 pt above the display's bottom and 10 pt digits; 32 to 44 pt tall, 14 by
> 10 pt, 1 pt below the display's top, with 8 pt digits; under 32 pt, 12 by 8 pt, 1 pt below the
> top, with 7 pt digits.
>
> - tier: human (how the crossing map is drawn is judged by eye)
> - do: Identify displays from the Crossing page and compare the numbers shown on the displays with the map, then resize a footprint down to its smallest
> - see: Each display on the map carries in its bottom-left corner the number its display showed, in a small bordered box that moves to the top-left and shrinks with the footprint
> - parity: Python's Mac drew the number in the window's own face at 12.5 pt in `text-2` on a 20 by 18 pt `panel` box without a border, its bottom 5 pt above the display's (ADR 0001: one Rune UI draws both machines)

> r[ui.map.footprint.status]
> On a footprint 72 pt tall or more, its status line (`viewmodel.map.status-own`,
> `viewmodel.map.status-peer`), or the profile word that takes its place
> (`viewmodel.profiles.word.beside-outcome`), is drawn under the name in `text-2` at 12.5 pt, left
> aligned, on a line from 31 to 47 pt below the footprint's top, starting 24 pt in from its left
> edge and ending 8 pt in from its right, beside display numbers as the name is
> (`ui.map.footprint.name-numbers`), and cut with an ellipsis where it does not fit. On a footprint
> under 72 pt tall it is not drawn.
>
> - tier: human (how the crossing map is drawn is judged by eye)
> - do: Look at a peer's footprint at its usual size, then resize it down until it is about half as tall
> - see: At the usual size "Windows · Linked" (or "macOS · Linked") reads under the name; once small, that line is gone and the name and outcome stay
> - parity: Python's Mac drew the status line in `text-3` at 10.5 pt, from 35 pt below the top; its PC drew it on footprints 56 px tall or more (ADR 0001: one Rune UI draws both machines)

> r[ui.map.footprint.status-dot]
> At the start of the status line, and drawn whenever it is, a 6 pt dot sits with its centre 15 pt
> in from the footprint's left edge and 39 pt below its top: `signal` on this machine's footprint
> and on the footprint of a peer with a link up, and `text-3` on a peer without one. While a profile
> word takes the status line's place, the dot's colour follows `ui.map.picker.word-dot`.
>
> - tier: human (how the crossing map is drawn is judged by eye)
> - do: Look at the map with the peer linked, then quit Beamer on the peer
> - see: This Mac's and the peer's status lines start with an accent-coloured dot; once Beamer on the peer has quit, the peer's dot is grey and This Mac's stays coloured
> - parity: Python's Mac drew this machine's dot in `text-3` and hid any dot that met a display number (ADR 0001: one Rune UI draws both machines)

> r[ui.map.footprint.outcome]
> On a peer's footprint 56 pt tall or more, its outcome (`viewmodel.map.outcome`) is drawn at
> 12.5 pt, centred left to right on a line from 24 to 8 pt above the footprint's bottom edge,
> 7 pt in from either edge and beside display numbers as the name is, cut with an ellipsis where it
> does not fit. On a footprint under 56 pt tall it is not drawn. The name, and the status line while
> it is drawn, always end above the outcome's line.
>
> - tier: human (how the crossing map is drawn is judged by eye)
> - do: Resize a peer's footprint down step by step from its usual size to its smallest
> - see: "Crosses" stays along the bottom, clear of the name, until the footprint gets quite small, and only then goes
> - parity: Python's PC drew the outcome only on footprints 78 px tall or more; its Mac drew it from 22 to 8 pt above the bottom, 8 pt in (ADR 0001: one Rune UI draws both machines)

> r[ui.map.footprint.outcome-colour]
> A footprint's outcome is drawn in `signal` when it is "Crosses", whether an edge, thirds, a corner,
> the notch or a wrap-around zone leads there; in `amber` when it is "Drag to connect" or
> "Overlaps"; and in `text-3` otherwise: "Offline", "Edge off", "Set elsewhere".
>
> - tier: human (how the crossing map is drawn is judged by eye)
> - do: Look at a peer that crosses, then turn its edge off, then drag it away so it touches nothing
> - see: "Crosses" shows in the accent colour, "Edge off" in grey and "Drag to connect" in amber

> r[ui.map.footprint.small]
> On a footprint under 56 pt tall the name's line is 20 pt tall and 8 pt in from each edge: 4 pt
> below the top from 44 to 56 pt tall, with the tick for the shortcut's cycle under it
> (`ui.map.cycle.place`), and from 32 to 44 pt tall at 12 pt below the top or 21 pt above the
> bottom, whichever is lower.
>
> - tier: human (how the crossing map is drawn is judged by eye)
> - do: With "next among ticked" chosen for where the shortcut goes, resize a peer's footprint down step by step to its smallest
> - see: The name stays readable and inside the footprint at every size, and the tick never covers it

> r[ui.map.footprint.tiny]
> On a footprint under 32 pt tall only the name, the display numbers and the tick for the shortcut's
> cycle are drawn: the name at 10 pt, 11 pt below the top, 5 pt in from each edge, clear of the
> display numbers; no status line, dot or outcome.
>
> - tier: human (how the crossing map is drawn is judged by eye)
> - do: Drag a peer's footprint far below the others so that the map shrinks its drawing a lot
> - see: Each footprint keeps a small readable name inside it, clear of the display numbers, and nothing else written

### The tick for the shortcut's cycle

How the tick on each peer's footprint (`viewmodel.map.cycle-tick`) is drawn and where it takes a
press.

> r[ui.map.cycle.look]
> Each tick for the shortcut's cycle is a 16 pt square with 3 pt rounded corners. Ticked, it is
> filled with `text-2` and carries a check mark in `ground`, 1.8 pt wide with round ends and joins: a
> short stroke down to its lowest point, left of the square's middle, then a longer one up to the
> right. Unticked, it is filled with `well`, bordered 1 pt in `outline`, and empty.
>
> - tier: human (how the crossing map is drawn is judged by eye)
> - do: Choose "next among ticked" for where the shortcut goes, then click one peer's tick to untick it
> - see: A ticked box is a filled square with a check mark (✓, not a caret ^); an unticked one is an empty outlined well

> r[ui.map.cycle.place]
> On a footprint 56 pt tall or more the tick sits 8 pt in from the right edge, centred on the name's
> line. On one 44 to 56 pt tall it sits under the name, 4 pt above the bottom edge and 20 pt in
> from the right, leaving the corner to the resize grip; on one 32 to 44 pt tall it is 20 pt in from
> the right, centred on the name's line. On one under 32 pt tall it is a 10 pt square, 5 pt in from
> the right and 1 pt below the top, its check mark scaled with it. It never overlaps the name.
>
> - tier: human (how the crossing map is drawn is judged by eye)
> - do: With "next among ticked" chosen, select a peer and resize its footprint down step by step to its smallest
> - see: The tick moves from the name's line to below it and back as the footprint shrinks, never on the name and never on the resize grip
> - parity: Python's Mac kept the tick 8 pt in from the right at every height from 32 pt, in the resize grip's corner on footprints 44 to 56 pt tall (ADR 0001: one Rune UI draws both machines; `viewmodel.map.press-priority` needs the tick clear of the grip)

> r[ui.map.cycle.hit]
> A press on the tick, the tick's tooltip and the tick's accessible element all cover the tick's
> square grown 4 pt in every direction, 24 pt square for a 16 pt tick, wherever the tick sits. A press
> just outside that square goes to the footprint.
>
> - tier: human (how a press lands is judged by hand)
> - do: On the Crossing page, under "Shortcut goes to", choose "Next among ticked"; click on the map just outside the corner of the peer's tick, then rest the pointer there
> - see: The click toggles the tick and the tooltip "In the shortcut's cycle" appears, while a click a little further away selects the footprint instead

### Seams and thirds

How the contacts between this machine's footprint and its peers' are drawn
(`viewmodel.map.seam-lit`, `viewmodel.map.tracks`), and where a press lands on a third.

> r[ui.map.seam.lit]
> Each lit stretch of a contact is drawn as a 4 pt line in `signal` with round ends, centred on the
> edge of this machine's display it lies on, from 2 pt inside each end of the stretch, or from its
> middle when the stretch is shorter than 4 pt. Half its width lies outside this machine's footprint,
> so it reads apart from the selected footprint's 2 pt `text-2` border beside it.
>
> - tier: human (how the crossing map is drawn is judged by eye)
> - do: Select a peer that crosses through this machine's whole right edge
> - see: A thick accent-coloured line runs along the shared edge, stopping just short of each end, and stands out from the grey border of the selected footprint next to it

> r[ui.map.seam.display-edge]
> While this machine's displays are known, a seam or an unlit track lies on the edge of the display
> of this machine that the peer touches, and runs only along the stretch where that display's edge
> touches the peer's footprint: under an external display and a shorter built-in display beside it,
> a peer reaching under both gets its seam on the external display's bottom edge, over the stretch
> where it touches it.
>
> - tier: human (how the crossing map is drawn is judged by eye)
> - do: Place a peer's footprint under a Mac whose external display is taller than its built-in one, reaching under both
> - see: The seam runs along the bottom of the external display only, where the peer's footprint touches it, not across the whole footprint

> r[ui.map.seam.join]
> Where two displays of a peer meet along a contact, the seam goes on unbroken across the join, and
> the breaks at the thirds stay where `ui.map.third.breaks` puts them.
>
> - tier: human (how the crossing map is drawn is judged by eye)
> - do: Place a peer with two displays next to each other flush under this machine's footprint, selected, all thirds lit
> - see: One unbroken seam runs along the whole contact, with no gap where the peer's two displays meet

> r[ui.map.seam.offline]
> The lit seams of a peer with no link are drawn at 0.55 opacity.
>
> - tier: human (how the crossing map is drawn is judged by eye)
> - do: Put a linked peer to sleep while its edge crosses
> - see: Its seam stays where it was, visibly fainter

> r[ui.map.third.breaks]
> For the peer selected on the map, a contact's lit thirds that meet are drawn as one continuous
> seam; where a lit third meets an unlit one, and where two unlit thirds meet, the line breaks with a
> 2 pt clear gap centred on the boundary between the two thirds. The boundaries are the thirds of
> this machine's display edge (`crossing.zone.thirds`), wherever the contact starts and ends. Each
> lit stretch of a peer that is not selected is drawn as one unbroken line, with no break at any
> third, from 2 pt inside one end of that stretch to 2 pt inside the other.
>
> - tier: human (how the crossing map is drawn is judged by eye; the seam of a peer not selected, which the desk's one peer can't show, stays out of the check)
> - do: Select a peer flush under this machine's footprint, all thirds lit, then click its middle third to leave it out
> - see: First one continuous seam; then a seam, a small gap, a thin faint track, a small gap and a seam, each gap exactly at a third of This Mac's edge
> - parity: Python's Mac broke the selected peer's seam at every third boundary, between two lit thirds too (ADR 0001: one Rune UI draws both machines; the PC's, from Beamer 1.5.6, is the later)

> r[ui.map.third.track]
> For the peer selected on the map, while its edge is on, each unlit third of its contact
> (`viewmodel.map.tracks`) is drawn as an unlit track: a 2 pt line in `signal` at 0.3 opacity with
> round ends, on the line a seam would take, with the same ends and breaks. While the peer's edge is
> off, or before its zone choices are known, nothing is drawn for its thirds.
>
> - tier: human (how the crossing map is drawn is judged by eye)
> - do: Select a peer, leave its middle third out, then turn its edge off in the ledger
> - see: The left-out third shows as a thin faint line in the seam's colour; with the edge off, nothing is drawn along the contact

> r[ui.map.third.band]
> For the peer selected on the map, each third of a contact along an edge has a click band: the
> stretch of that third (a third of this machine's display edge) that the contact covers, from 5 pt
> inside to 5 pt outside the edge's line, 10 pt across. A press in it goes to that third
> (`viewmodel.map.third-click`), and the bands are there while the peer's edge is off too. On a
> display edge 99 map units long, a contact over 0 to 50 has a band for the start third over 0 to 33
> and one for the middle third over 33 to 50. A contact at a corner has no band, and neither has any
> contact of a peer that is not selected.
>
> - tier: human (how a press lands is judged by hand; a contact of a peer not selected, which the desk's one peer can't show, stays out of the check)
> - do: Move the pointer slowly outward across the edge This Mac shares with the peer, then turn the peer's edge off in its ledger row and do the same again
> - see: Both times the pointer turns into a pointing hand a few points before the edge and stays one until a few points past it

> r[ui.map.third.band-over-grip]
> Where a click band overlaps a resize grip, a press in the band goes to the third and the pointer
> there is the pointing hand; the rest of the grip resizes. A footprint drawn smaller than its grip
> keeps a full 16 pt square press area for its grip from the grip's top-left corner, reaching out
> past the footprint's corner, so that the grip can still be taken outside the band.
>
> - tier: human (how a press lands is judged by hand)
> - do: Select a peer to the left of this machine whose bottom corner sits on the shared edge, and click its grip right on the edge and then a little inside
> - see: On the edge the click leaves out the end third; inside, a drag resizes the footprint

> r[ui.map.third.press]
> A press in a click band feels like a button, not a grab: until the release the pointer stays the
> pointing hand, no footprint follows it and nothing on the map changes; the third's change shows
> on the release (`viewmodel.map.third-click`, `viewmodel.map.third-click-cancel`).
>
> - tier: human (how a press feels is judged by hand)
> - do: Press on the selected peer's middle third, drag the pointer away and back with the button down, then release it there
> - see: Nothing moves while the button is down; on release the middle third goes out

> r[ui.map.seam.corner-contact]
> Where a peer's display meets one of this machine's displays only at a corner, nothing is drawn at
> that vertex but the corner pad the map shows there (`ui.map.corner.pad`): no seam and no dot. Where
> two displays meet at a corner beside a contact along an edge, as when a peer sits under one of this
> machine's displays and off the corner of the other, only the contact along the edge is drawn.
>
> - tier: human (how the crossing map is drawn is judged by eye)
> - do: Place a peer's footprint touching only the bottom-right corner of this machine's, then one under the left of two displays next to each other
> - see: The first shows only the corner pad at the corner; the second only a seam along the shared bottom edge, nothing at the corner
> - parity: Python's PC drew a 7 px `signal` dot at the vertex of a peer reached at a corner alone, under its corner pad (ADR 0001: one Rune UI draws both machines, taking the Mac's)

> r[ui.map.seam.between]
> A contact between two peers reported as crossing between each other (`viewmodel.map.between`) is
> drawn as a 2 pt dashed line in `amber`, 8 pt dashes and 4 pt gaps, with round ends, along the edge
> they share.
>
> - tier: human (how the crossing map is drawn is judged by eye; the dashed line itself, which needs two peers touching each other and so a third machine, stays out of the check)
> - do: Drag the peer's footprint so it touches This Mac's, then away so it touches nothing, releasing it each time
> - see: No dashed amber line is drawn anywhere on the map either time
> - parity: Python's Mac dashed it 4 pt on, 3 pt off (ADR 0001: one Rune UI draws both machines)

### Corner pads and the notch mark

How a corner pad (`viewmodel.map.corner-pad`) and, on a Mac, the notch mark
(`viewmodel.map.notch-mark`) are drawn.

> r[ui.map.corner.pad]
> Each corner pad is a 16 pt square with 4 pt rounded corners, filled with `signal`, centred on that
> corner of this machine's footprint, the footprint's own corner even where the corner zone is on
> one of its displays. On it, a 7 pt bracket in `ground`, 1.8 pt wide with round ends, is centred on
> the vertex: two strokes meeting at the corner that points out of the footprint, equally far from
> the pad's edges all round. Behind the pad a 22 pt square with 5 pt rounded corners in `ground`
> sets it off from the seams and borders under it. Pads are drawn above the footprints: one for each
> pad the map shows, and none left behind once its zone goes.
>
> - tier: human (how the crossing map is drawn is judged by eye)
> - do: In a peer's ledger row turn on its top-right corner, then turn it off
> - see: A small accent-coloured square appears centred on This Mac's top-right corner, with a bracket pointing out of the corner, sitting in a small clear ring; turned off, it is gone with nothing left

> r[ui.map.corner.opacity]
> The pads and the notch mark of the peer selected on the map are drawn at full opacity and above
> any other pad; those of other peers are drawn at 0.55 opacity.
>
> - tier: human (how the crossing map is drawn is judged by eye; the faded pads of a peer not selected, which the desk's one peer can't show, stay out of the check)
> - do: In the peer's ledger row turn on its top-right corner and tick "Notch"
> - see: The pad and the notch mark are solid, at full strength

> r[ui.map.corner.seam-stop]
> On an edge of this machine's footprint with a corner pad at one end, whichever peer's pad it is,
> a lit seam or an unlit track that reaches that corner stops 11 pt from the vertex, at the pad's
> clear ring.
>
> - tier: human (how the crossing map is drawn is judged by eye; a pad of another peer, which the desk's one peer can't show, stays out of the check)
> - do: Give the peer, on This Mac's right, both its whole edge and the top-right corner
> - see: The seam along the right edge stops just short of the pad's ring, never running under the pad

> r[ui.map.notch.mark]
> On a Mac, the notch mark is a 4 pt tall bar with 2 pt rounded corners in `signal`, centred on the
> top edge of the display that has the notch, 2 pt above it and 2 pt below, spanning the notch's
> stretch of that display: the display at the top of this Mac's displays whose width holds the notch. While this
> Mac's displays or the notch's stretch are not known, the bar is 14 pt wide, centred on the top edge
> of this Mac's footprint. Its opacity follows its peer as a pad's does (`ui.map.corner.opacity`).
>
> - tier: human (how the crossing map is drawn is judged by eye)
> - do: On the MacBook, place the external display to the left of the built-in one with their tops level (System Settings, Displays), then tick "Notch" in the peer's ledger row
> - see: A short accent-coloured bar sits across the top of the built-in display on the map, over where the notch is, not at the middle of the whole footprint

### Wrap-around seams

How a wrap-around zone's seam (`viewmodel.map.wrap-seam`), a kept seam (`viewmodel.map.wrap-kept`)
and their labels are drawn on this machine's footprint.

> r[ui.map.wrap.seam]
> A wrap-around seam is a 4 pt line in `signal` with round ends, centred on that edge of this
> machine's footprint, along the whole edge from 2 pt inside each end. It goes as soon as the
> wrap-around zone does.
>
> - tier: human (how the crossing map is drawn is judged by eye)
> - do: Turn on a wrap-around zone on this machine's left edge for a peer on its right, then turn it off
> - see: A thick accent-coloured line runs the full height of This Mac's left edge, and goes when the zone is turned off

> r[ui.map.wrap.pad-stop]
> A wrap-around seam stops 13 pt short of a corner of this machine's footprint that carries a corner
> pad, whichever peer's pad it is, measured along the edge from the vertex, and 2 pt short of a
> corner without one.
>
> - tier: human (how the crossing map is drawn is judged by eye; a pad of another peer, which the desk's one peer can't show, stays out of the check)
> - do: With the peer on This Mac's right, click "Left" and the top-left corner square in its ledger row
> - see: The seam along This Mac's left edge ends just before the top-left pad and never touches it; at the bottom-left corner, without a pad, it runs almost to the end

> r[ui.map.wrap.opacity]
> A wrap-around seam, its label and its arrow are drawn at full opacity for the peer selected on the
> map and at 0.7 opacity for every other peer, each multiplied by 0.55 for a peer with no link.
>
> - tier: human (how the crossing map is drawn is judged by eye; the 0.7 opacity of a peer not selected, which the desk's one peer can't show, stays out of the check)
> - do: With the peer on This Mac's right, click "Left" in its ledger row, then quit Beamer on the peer
> - see: The seam, its label and its arrow are at full strength, then fade to about half once Beamer on the peer has quit

> r[ui.map.wrap.kept]
> A kept seam is a 2 pt dashed line in `signal` at 0.35 opacity, 4 pt dashes and 5 pt gaps, on the
> line a wrap-around seam takes, and its label and arrow are drawn in `text-3` at 0.8 opacity.
>
> - tier: human (how the crossing map is drawn is judged by eye)
> - do: Turn on a wrap-around zone for a peer, then turn that peer's edge off
> - see: The seam becomes a thin faint dashed line and its label, now "wrap kept, Edge off", turns grey
> - parity: Python's Mac dashed it 3 pt on, 5 pt off, and drew its arrow at 0.7 opacity (ADR 0001: one Rune UI draws both machines)

> r[ui.map.wrap.label-lines]
> Each wrap label is drawn inside this machine's footprint at 11.5 pt, in its seam's colour and
> opacity, on a line of its own 16 pt tall below the name and the status line, so that no label ever
> lands on the name, the status line or another label, whatever edges wrap and for however many
> peers. Lines are at least 18 pt apart, from 51 pt below the footprint's top (34 pt while the
> status line is not drawn) to 22 pt above its bottom: a top edge's label and a merged one
> (`viewmodel.map.wrap-label-merged`) on the first line, a bottom edge's on the last, a left or
> right edge's nearest 10 pt below the footprint's middle. A left and a right edge's labels share a
> line when both fit with 26 pt from each edge and 8 pt between them. A label left without a line is
> not drawn, though its seam is, and on a footprint under 56 pt tall no wrap label is drawn.
>
> - tier: human (how the crossing map is drawn is judged by eye; a second peer's labels on lines of their own, which the desk's one peer can't show, stay out of the check)
> - do: With the peer on This Mac's right, click "Left" in its ledger row, then "Top", then resize This Mac's footprint smaller step by step
> - see: First the left edge's label sits a little below the middle of This Mac's footprint, then a single label naming the top and left edges sits under the status line; neither lands on the name or the status line, and as the footprint shrinks the label drops out while the seams stay
> - parity: Python's Mac drew wrap labels at 10.5 pt from 55 pt below the top; its PC drew none on footprints under 78 px tall (ADR 0001: one Rune UI draws both machines)

> r[ui.map.wrap.label-place]
> A left edge's wrap label starts 26 pt in from the footprint's left edge, a right edge's ends 26 pt
> in from its right edge, and a top, bottom or merged label ends 12 pt in from the right edge. A
> label is cut with an ellipsis to the footprint's width less 52 pt beside an arrow, and less 24 pt
> otherwise.
>
> - tier: human (how the crossing map is drawn is judged by eye)
> - do: With the peer under This Mac, click "Left" in its ledger row; then "Left, wraps" to remove it and "Right"; then "Right, wraps" and "Top"
> - see: The left label sits near the left edge, the right one near the right edge, and the top one towards the right, all inside the footprint

> r[ui.map.wrap.arrow]
> Beside a left or right edge's wrap label, an arrow points out through that edge: a 9 pt shaft and a
> head 4 pt long each way, 1.6 pt wide with round ends and joins, its tip 16 pt in from that edge,
> level with the middle of the label's line, in the label's colour and opacity. A top or bottom
> edge's label has no arrow.
>
> - tier: human (how the crossing map is drawn is judged by eye)
> - do: Turn on wrap-around zones on This Mac's left and top edges
> - see: The left label has a small arrow before it pointing out through the left edge; the top label has none

### Resize grip, pointer and keyboard focus

How the resize grip (`viewmodel.map.resize-offered`) is drawn, which pointer the map shows, where
its tooltips are, and its focus ring.

> r[ui.map.grip.look]
> A footprint's resize grip is drawn in its bottom-right corner as two parallel diagonal strokes, 4
> and 8 pt long, 3 pt in from the corner, 1.5 pt wide with round ends, in `text-2`, inside the 16 pt
> square in that corner, or a square as wide as the footprint's width or height where that is less. Its
> press area is always the full 16 pt square (`ui.map.third.band-over-grip`).
>
> - tier: human (how the crossing map is drawn is judged by eye)
> - do: Select a peer's footprint, then move the pointer over another one
> - see: Two short diagonal strokes appear in the bottom-right corner of each, and dragging there resizes the footprint
> - parity: Python's Mac's grip was a 12 pt square, drawn in `text-3` on a footprint that was only under the pointer (ADR 0001: one Rune UI draws both machines)

> r[ui.map.grip.split-display]
> A footprint's resize grip is not drawn, and does not take a press, where its square would lie over
> a display of another machine moved apart from its footprint; a press there goes to that display.
>
> - tier: human (how a press lands is judged by hand)
> - parity: Python's Mac drew the grip there and took a press on it for a resize (ADR 0001: one Rune UI draws both machines, the PC's)
> - do: On the MacBook, place the external display to the left of the built-in one with their tops level (System Settings, Displays), which leaves This Mac's footprint empty under the built-in display; Option-drag one of the peer's displays apart so that its top-left corner lies in that empty corner and release it; then press on This Mac's footprint's bottom-right corner, under that display
> - see: No grip is drawn at This Mac's bottom-right corner under the peer's display, and the press takes the display, not a resize of This Mac's footprint

> r[ui.map.pointer.shapes]
> Over the map the pointer is the diagonal resize pointer over a resize grip and through a resize;
> the pointing hand over a click band; the open hand over a peer's footprint that can be dragged;
> the closed hand while a footprint or a display is dragged; and the arrow over this machine's
> footprint, outside the footprints, and once it leaves the map.
>
> - tier: human (how the pointer looks is judged by eye)
> - do: Move the pointer over the map's ground, This Mac's footprint, a peer's footprint, the selected peer's shared edge and a grip, then drag a peer's footprint
> - see: An arrow, an arrow, an open hand, a pointing hand and a diagonal resize pointer in turn, and a closed hand during the drag

> r[ui.map.pointer.picker]
> While own zones on every machine is off, the pointer is the pointing hand over every footprint,
> this machine's and a peer's that could be dragged included, except over a resize grip, where it
> is the resize pointer.
>
> - tier: human (how the pointer looks is judged by eye)
> - do: Turn own zones on every machine off, then move the pointer over each footprint and over a grip
> - see: A pointing hand over every footprint, saying a click picks it, and the resize pointer over the grip
> - parity: Python's PC showed the open hand over a peer's footprint that could be dragged (ADR 0001: one Rune UI draws both machines, taking the Mac's)

> r[ui.map.pointer.tooltips]
> The map shows a third's tooltip (`viewmodel.map.third-tooltip`) while the pointer rests in that
> third's click band, the tick's tooltip while it rests in the tick's press area
> (`ui.map.cycle.hit`), and, while a footprint's name is cut short, the whole name while it rests
> on that name (`viewmodel.map.title`).
>
> - tier: human (how a tooltip appears is judged by eye)
> - do: On the Crossing page, under "Shortcut goes to", choose "Next among ticked"; rename the peer to a name too long for its footprint, then on the map rest the pointer on its name, on a third of the edge it shares with This Mac and on its tick
> - see: The full name, the third's tooltip and "In the shortcut's cycle" appear, each only where the pointer rests
> - parity: Python's PC showed a footprint's name as its tooltip anywhere over the footprint and over its grip (ADR 0001: one Rune UI draws both machines, taking the Mac's)

> r[ui.map.focus.ring]
> While the map has keyboard focus that came from the keyboard, a ring 2 pt wide in `text-2`, with
> 3 pt rounded corners, is drawn 3 pt outside the footprint Tab has reached: the selected footprint
> (`viewmodel.map.tab-order`), or, while the map picks the viewed machine, the footprint with
> keyboard focus, this machine's included (`viewmodel.profiles.keys.order`). It moves as Tab moves
> focus, is not drawn while the map has focus from a press, and goes as soon as keyboard focus leaves
> the map.
>
> - tier: human (how keyboard focus shows is judged by eye; Tab from one peer's footprint to another's while own zones on every machine is on, which the desk's one peer can't show, stays out of the check)
> - do: Turn "Own zones on every machine" off, Tab into the map, Tab once more, Tab again until focus leaves the map, then click a footprint
> - see: A ring hugs one footprint, jumps to the other at the next Tab, is gone once focus leaves the map, and does not show after the click
> - parity: Python's Mac rounded the ring by 4 pt (ADR 0001: one Rune UI draws both machines)

### Guides and the snap preview

The map's thirds guides and how the snap preview (`viewmodel.map.snap-preview`) is drawn.

> r[ui.map.guide.thirds]
> Behind the footprints, four faint 1 pt lines in `outline` at 0.22 opacity cross the whole map, 4 pt
> short of its border, through the thirds of this machine's footprint: two upright through the
> thirds of its width and two level through the thirds of its height.
>
> - tier: human (how the crossing map is drawn is judged by eye)
> - do: Look at the map, then resize This Mac's footprint
> - see: Faint lines cross the map at This Mac's thirds and follow it when it is resized
> - parity: Python's Mac drew them at 0.24 opacity from border to border and, while a footprint was dragged, short ticks 6 pt long just outside each third of this machine's footprint, which its PC never drew (ADR 0001: one Rune UI draws both machines)

> r[ui.map.snap.look]
> The snap preview is the outline of the footprint where it would settle, 1 pt dashed in `signal` at
> 0.4 opacity with a footprint's 5 pt rounded corners, and with the seam its contact there would make
> drawn at 0.5 opacity: a 4 pt line along the contact, or a 7 pt dot at the vertex where it would
> touch at a corner only.
>
> - tier: human (how the crossing map is drawn is judged by eye)
> - do: Drag a peer's footprint slowly towards the middle of This Mac's right edge without releasing it
> - see: A faint dashed outline appears where it would settle, with a faint seam along This Mac's edge
> - parity: Python's Mac drew the outline with square corners, dashed 4 pt on, 3 pt off (ADR 0001: one Rune UI draws both machines)

### The map while it picks the viewed machine

How the map is drawn while own zones on every machine is off and it picks the viewed machine
(`viewmodel.profiles.picker.click`, `viewmodel.profiles.word.this`, `viewmodel.profiles.word.peer`).

> r[ui.map.picker.border]
> While own zones on every machine is off, the viewed machine's footprint, this machine's included,
> is bordered 2 pt in `text-2`, and every other footprint 1 pt in `outline`.
>
> - tier: human (how the crossing map is drawn is judged by eye)
> - do: Turn own zones on every machine off, then click a peer's footprint, then This Mac's
> - see: The clicked footprint, This Mac's too, takes the thick light grey border, and only it
> - parity: Python's PC drew no 2 pt border while This PC was the viewed machine, only on another machine viewed, and kept the hover border on other footprints while picking; its Mac kept the hover border and the amber border on other footprints while picking (ADR 0001: one Rune UI draws both machines)

> r[ui.map.picker.all-peers]
> While own zones on every machine is off, every peer's contacts with the viewed machine's footprint
> are drawn as the selected peer's are on the Crossing page: with their breaks and unlit tracks
> (`ui.map.third.breaks`, `ui.map.third.track`), and their seams, pads, notch mark and wrap-around
> seams at full opacity.
>
> - tier: human (how the crossing map is drawn is judged by eye)
> - do: Turn own zones on every machine off and view This Mac, with one peer crossing only by its middle third
> - see: That peer's contact shows the lit middle third with faint tracks for the other two thirds, as on the Crossing page for a selected peer
> - parity: Python's PC drew breaks and tracks only for the selected peer while picking (ADR 0001: one Rune UI draws both machines, taking the Mac's)

> r[ui.map.picker.word-dot]
> While own zones on every machine is off, the dot at the start of a footprint's profile word is
> `signal` for "Desk" and "Own" and `text-3` for "Offline", whatever the machine's link.
>
> - tier: human (how the crossing map is drawn is judged by eye)
> - do: Turn "Own zones on every machine" off with the peer linked, then quit Beamer on the peer
> - see: The dots beside "Desk" and "Own" are in the accent colour; once Beamer on the peer has quit, the dot beside its "Offline" is grey

### The audio map's arrows

> r[ui.map.audio.arrow-order]
> On the audio map (`viewmodel.audio.map.mic-arrows`, `viewmodel.audio.map.sound-arrows`,
> `viewmodel.camera.map.arrows`), arrows between the same two machines run parallel, 12 pt apart,
> in the order camera, microphone, sound: where they run level the camera's arrow is the highest,
> where they run upright the leftmost, so that each arrow lines up with its own line on the
> machines' cards, on which the camera's line sits above the microphone's.
>
> - tier: human (how the audio map is drawn is judged by eye)
> - do: Share this Mac's camera and microphone with a PC whose card sits beside the Mac's, both streaming
> - see: The camera's dotted arrow runs just above the microphone's dashed one, each beside its own line on the cards
> - parity: Python's Mac set two arrows 22 pt apart and three 17.6 pt apart; its PC kept the order the arrows came in when none was a camera's (ADR 0001: one Rune UI draws both machines)

## Overlays

Beamer draws over the desktop in overlays: the edge glow and the Ring (`ui.feedback.one-display`
says which display they play on), the numbers of identify displays, and the pointer overlay. The
overlay layer and its frame clock ship for these (#11); the crossing effects (F30) and the notch
island (F31) would draw in the same layer, their room `ui.later.effects` and `ui.later.notch`.
Sizes are in pt, Rune's logical units, which a PC's px equal at 100 % scaling.

### The overlay window

> r[ui.overlay.transparent]
> Outside what it draws, an overlay is wholly transparent: it has no background, border, title bar
> or shadow, and the desktop and windows under it show through unchanged.
>
> - tier: human (what an overlay covers is judged by eye)
> - do: Open a window full of text, then press "Identify screens" on the Crossing page
> - see: Only the number cards cover the window; the text all round them shows through unchanged, with no box, frame or shadow round the display

> r[ui.overlay.no-input]
> An overlay takes no clicks, no scrolling and no keys, and never becomes the active window: a
> click on it reaches the window under it, and the app in front stays in front and keeps taking
> what is typed. Showing an overlay never brings Beamer to the front.
>
> - tier: human (where a click lands is judged by hand)
> - do: Click into a text editor, press "Identify screens" from the peer, then while the numbers show click on the editor through a number card and type
> - see: The click lands in the editor under the card, the typing appears there, and Beamer's window does not come to the front

> r[ui.overlay.on-top-mac]
> On a Mac, an overlay is above every app's windows, the menu bar and the Dock, on every Space and
> over an app in full screen, whether or not Beamer is the active app.
>
> - tier: human (what is on top is judged by eye)
> - do: On the Mac, put an app in full screen, push up against the top edge toward a peer, then leave full screen and push up again with the menu bar showing
> - see: The light shows over the full-screen app, and over the menu bar rather than under it

> r[ui.overlay.on-top-pc]
> On a PC, an overlay is above every window, those kept on top by other apps included, and above
> the taskbar for as long as it shows, also after the taskbar is hovered or clicked.
>
> - tier: human (what is on top is judged by eye)
> - do: On the PC, click the taskbar, then push along the bottom edge over the taskbar toward a peer
> - see: The light shows over the taskbar, not under it

> r[ui.overlay.not-listed]
> On a PC, an overlay has no taskbar button and is not listed in Alt+Tab or Task View.
>
> - tier: human (what the taskbar lists is judged by eye)
> - do: On the PC, press "Identify screens" and press Alt+Tab while the numbers show
> - see: Alt+Tab and the taskbar show no entry for the number cards

> r[ui.overlay.stationary]
> On a Mac, Mission Control and a change of Space leave an overlay where it is, and cycling
> through windows with Command-` never stops on one.
>
> - tier: human (what Mission Control moves is judged by eye)
> - do: On the Mac, press "Identify screens", then open Mission Control while the numbers show
> - see: The number cards stay where they were, centred on their displays, and are not shrunk among the windows

> r[ui.overlay.clip]
> What an overlay draws for one display shows whole, cut only at that display's edges: none of it
> shows on a display beside it, also where the two share an edge, and no part of it inside the
> display is cut off, the light that reaches past the stretch pushed included.
>
> - tier: human (where the drawing stops is judged by eye)
> - do: With two displays next to each other, put the pointer a few points from the edge they share and bring input home by the shortcut with "Show where the pointer lands" on
> - see: The Ring is cut off straight along the shared edge and nothing of it shows on the other display, while the half on the pointer's display is whole

> r[ui.overlay.geometry]
> What an overlay draws for a point of the desktop lands on that point however the displays are
> placed: on a display left of or above the main display, where the desktop's coordinates are
> negative, and on displays at different scaling beside each other, such as a 150 % display beside
> a 100 % one, each drawn sharp at its own scaling. Where an overlay window sits over its display
> never shifts what it draws.
>
> - tier: human (where drawing lands is judged by eye)
> - do: Set a second display to 150 % scaling and place it left of and above the main display, then bring input home by the shortcut with the pointer on it, with "Show where the pointer lands" on
> - see: The Ring closes onto the pointer's tip on that display, sharp, not offset or blurred

> r[ui.overlay.while-playing]
> An overlay shows from the first frame of what plays on it to the last, without a gap or flicker
> between frames, and is gone once nothing plays on it. Its frame clock runs only while something
> plays.
>
> - tier: human (flicker is judged by eye)
> - do: Choose Beam and push slowly against an edge toward a peer for a few seconds, then let go
> - see: The line and its comet stay visible without a flicker for the whole push and fade away after it, leaving nothing

> r[ui.overlay.no-trail]
> Each frame of an overlay replaces the last one: nothing of an earlier frame stays where the
> drawing has moved on, and nothing of the last frame stays once it has gone.
>
> - tier: human (leftover pixels are judged by eye)
> - do: Choose Beam and push steadily against an edge so the comet runs along it, then bring input home by the shortcut with "Show where the pointer lands" on
> - see: The comet leaves no smear behind it, the closing Ring leaves no ring of old pixels, and nothing is left once either is gone

> r[ui.overlay.display-gone]
> When a display goes away while an overlay plays on it, nothing more is drawn for it, none of it
> moves onto a display that is left, and what plays on the other displays ends on time.
>
> - tier: human (what is left behind is judged by eye)
> - do: With an external display attached, press "Identify screens" and unplug the external display while the numbers show
> - see: The built-in display keeps only its own number, gains no second card, and is clear of numbers within 3 seconds of the press

### The frame clock

> r[ui.overlay.frame-rate]
> An overlay draws its frames at the refresh rate of the display it draws on (60 a second when the
> OS gives no rate) divided by "Frame rate" (`viewmodel.design.frame-rate`): by 1 for "Match
> display", 2 for "Half" and 4 for "Quarter", rounded down to a whole number of frames a second. So
> a 60 Hz display draws 60, 30 or 15 frames a second, and a 144 Hz display 144, 72 or 36. The Design
> page's previews draw at the same rate for the display the window is on, capped at 60 before the
> division.
>
> - tier: human (smoothness is judged by eye)
> - do: On the Mac's built-in display, which runs at 60 Hz, choose Beam, set "Frame rate" to "Quarter" and push steadily against an edge toward the PC without crossing; then set it to "Match display" and push again
> - see: At "Quarter" the comet jumps along in clear steps, about 15 a second; at "Match display" it glides, 60 a second, over the same time
> - parity: Python's Mac paced every overlay to the fastest display's rate, wherever it drew (ADR 0001: one Rune UI draws both machines; the PC's, each overlay at the rate of the display it draws on)

> r[ui.overlay.paced]
> An overlay draws only on its frame clock: however often pointer reports arrive, as from a
> 1000 Hz mouse, it draws at most one frame for each frame of the clock, and the first frame of
> what plays shows at the clock's next frame after the report that starts it.
>
> - tier: human (lag is judged by eye and hand)
> - do: With a 1000 Hz gaming mouse, choose Glow and push hard and fast against an edge toward a peer, again and again
> - see: The band appears with the first push and follows each push without lag, and the pointer never stutters

### Drawing

What an overlay draws for Glow, Beam, the Ring and the identify cards, the same on a Mac and a PC.
Each applies wherever an overlay draws; its check looks at whichever of them shows it best. The
crossing effects (F30) and the notch island and beam (F31) will need more than this, which "Rooms
for what comes Later" names.

> r[ui.overlay.draw.shapes]
> An overlay fills and strokes the shapes Glow, Beam, the Ring and the identify cards are made of:
> rectangles, rectangles with rounded corners, and circles. Their edges are smooth at the display's
> scaling, and a circle is evenly round, with no flat spots or corners.
>
> - tier: human (shapes are judged by eye)
> - do: With "Show where the pointer lands" on, bring input home by the shortcut; then press "Identify screens"
> - see: The Ring is a smooth, even circle all round, and each number card's corners are smooth even curves, with no steps or kinks

> r[ui.overlay.draw.strokes]
> A stroke draws a shape's outline at the width asked for, centred on the outline: the Ring's line,
> the dark halo under it, and an identify card's border. A stroke painted with a gradient shows the
> gradient on the stroke alone.
>
> - tier: human (line widths are judged by eye)
> - do: Choose Colourful and bring input home by the shortcut with "Show where the pointer lands" on
> - see: The Ring's line is one even width all round, its colours on the line only, with nothing painted inside or outside it

> r[ui.overlay.draw.colour]
> A solid colour is drawn at its own opacity: red at an alpha of 0.5 draws red at half opacity over
> whatever is under it.
>
> - tier: human (opacity is judged by eye)
> - do: In light mode, over a white window, choose White and bring input home by the shortcut with "Show where the pointer lands" on
> - see: The Ring's dark halo is a soft translucent grey through which the window shows, not solid black

> r[ui.overlay.draw.linear]
> A linear gradient blends smoothly from each of its stops to the next, colour and opacity alike,
> along the line from its start point to its end point, with no visible bands or steps. Glow and
> Beam are drawn with linear gradients: the colour's stops along the stretch, and their light fading
> inward from the edge and, for Beam's comet, along it.
>
> - tier: human (colour is judged by eye)
> - do: Choose Colourful and Beam, and push steadily against a left edge toward the PC without crossing
> - see: The colours run smoothly from red-pink at the top to green at the bottom, the light fades smoothly inward from the edge, and the comet's tails fade smoothly, with no bands or steps anywhere

> r[ui.overlay.draw.radial]
> A radial gradient runs its stops outward from a centre point to a circle round it, the same in
> every direction: the soft glow that swells at the Ring's tip (`ui.ring.closes`).
>
> - tier: human (colour is judged by eye)
> - do: In dark mode, bring input home by the shortcut with "Show where the pointer lands" on
> - see: As the Ring closes, a soft round glow swells at the pointer's tip, brightest at its middle and fading evenly to nothing at its rim

> r[ui.overlay.draw.conic]
> A conic gradient runs its stops once round a centre point, clockwise from the angle it starts at,
> as the Ring's colours run round it (`ui.ring.colours`). One whose last stop is its first colour
> shows no seam, and drawn at part opacity, as the Ring fades in and out, it shows no spokes or
> brighter lines anywhere round it.
>
> - tier: human (colour is judged by eye)
> - do: Choose Colourful and bring input home by the shortcut with "Show where the pointer lands" on
> - see: The colours run evenly round the Ring as it closes and fades, with no seam and no spokes

> r[ui.overlay.draw.alpha]
> Each shape is drawn at its own opacity and with its own compositing, normal or additive, and
> neither carries over to the next shape. Additive drawing adds its light to what is under it, so
> it brightens and never darkens; normal drawing covers what is under it by its opacity. The glow at
> the Ring's tip is additive while the OS is in dark mode (`ui.palette.os-mode`), and normal in
> light mode; the Ring's line and halo are normal.
>
> - tier: human (brightness is judged by eye)
> - do: In dark mode, choose Colourful and bring input home by the shortcut with "Show where the pointer lands" on
> - see: The glow at the pointer's tip brightens the Ring's line where they meet, and the Ring keeps its full colours and its dark halo while the glow fades

## Identify displays

How the numbers of identify displays are drawn. What starts them is `viewmodel.identify.press`,
and that each display shows its number is `viewmodel.identify.number`. Which displays show a
number, which number, and for how long are `spec/display.md`'s (#61).

> r[ui.identify.card]
> Each display shows its number on a square card 220 pt wide, or as wide as the display's width or
> height where that is less, filled with `panel`, bordered 1 pt in `outline` with 6 pt rounded
> corners (`ui.look.palette`). The number is centred on the card in bold B612 Mono, `text`, 128 pt
> high, or 0.58 of the card's width where that is less.
>
> - tier: human (the card is judged by eye)
> - do: Press "Identify screens" on the Crossing page
> - see: With the window in Dark, each display shows a dark rounded square a few centimetres wide with one large bold monospaced number in the middle
> - parity: Python's PC drew a 208 px card with 12 px corners and the number 0.54 of its width, and its Mac put a window shadow under the card (ADR 0001: one Rune UI draws both machines; the Mac's card, without the shadow, as `ui.overlay.transparent` says)

> r[ui.identify.appearance]
> The cards are drawn in the settings window's appearance (`viewmodel.design.appearance`): in
> Dark's colours while it shows Dark, in Light's while it shows Light.
>
> - tier: human (colour is judged by eye)
> - do: Under Design, set "This window" to Light, then press "Identify screens"
> - see: The cards are near-white with a near-black number
> - parity: Python's PC drew the cards in Dark's colours whatever the appearance (ADR 0001: one Rune UI draws both machines; the Mac's)

> r[ui.identify.place]
> Each card is centred on the display whose number it shows, however the displays are
> placed and scaled, also where the OS lists the displays in another order than the crossing map numbers
> them.
>
> - tier: human (placement is judged by eye)
> - do: On a machine with two displays at different scaling, press "Identify screens" and compare with the crossing map
> - see: Each number sits in the middle of the display that carries the same number on the map

## Pointer overlay

How the pointer overlay is drawn. When it shows and where it follows the pointer is
`crossing.pointer.overlay.*`.

> r[ui.pointer-overlay.arrow]
> The pointer overlay draws the user's own Windows arrow, as their pointer scheme, size and colour
> make it, pixel for pixel at the scaling of the display it is on, with nothing drawn round it.
>
> - tier: human (the arrow is judged by eye)
> - do: On a PC with no mouse attached and a large, coloured pointer chosen in Windows' settings, drive it from the Mac and move the pointer over a light window and a dark one
> - see: The arrow on the PC is the large coloured one chosen, sharp, with no box round it on either window

> r[ui.pointer-overlay.hotspot]
> The pointer overlay is placed so that the arrow's hot spot, the point of its tip, is on the
> pointer's position, at any display scaling.
>
> - tier: human (where the arrow points is judged by eye)
> - do: Drive a PC with no mouse attached from the Mac and click a small button such as a window's close button
> - see: The click lands on the button the arrow's tip is on

> r[ui.pointer-overlay.scheme-change]
> The arrow is read again each time the pointer overlay is placed, so a change to the user's pointer
> scheme, size or colour shows on the next placing, also when Windows keeps the same arrow for it.
>
> - tier: human (the arrow is judged by eye)
> - do: While driving a PC with no mouse attached, change its pointer size in Windows' settings, then move the pointer
> - see: The arrow takes the new size as soon as the pointer moves

## Tray icon

How the tray icon is drawn. Which state it shows, its badge's state and its words are
`viewmodel.tray.icon`, `viewmodel.tray.badge` and `viewmodel.tray.icon-words`.

> r[ui.tray.shape]
> The tray icon's image is 22 by 16 pt: two upright rectangles 8 by 12 pt with 1.5 pt rounded
> corners, 4 pt apart, standing for this machine on the left and the other machine on the right, each filled
> or drawn as a 1.5 pt outline, with a chevron of a 1.4 pt line with round ends and corners, or a
> bar, between them.
>
> - tier: human (the icon is judged by eye)
> - do: Look at the tray icon in the Mac's menu bar and in the PC's notification area
> - see: Both show two small upright rectangles next to each other with a mark between them, drawn alike

> r[ui.tray.states]
> The tray icon draws each of its states (`viewmodel.tray.icon`) as its own image:
>
> | State | Left rectangle | Right rectangle | Between them |
> |---|---|---|---|
> | here | filled | outline | a chevron pointing left |
> | away | outline | filled | a chevron pointing right |
> | paused | outline | outline | a solid bar 2 by 16 pt, the icon's full height |
>
> - tier: human (the icon is judged by eye)
> - do: Watch the tray icon while you cross to a peer, come home, then turn on a crossing pause
> - see: The filled rectangle moves to the right while input is away and returns to the left at home, and the pause shows two empty rectangles split by a bar

> r[ui.tray.tint]
> On a Mac the tray icon is drawn in the menu bar's own colour, as macOS draws a template icon: dark
> on a light menu bar, light on a dark one, and inverted while its menu is open. On a PC it is black
> on a light taskbar and white on a dark one, following the colour mode Windows sets for the taskbar,
> whatever the mode set for apps and Beamer's own appearance.
>
> - tier: human (colour is judged by eye)
> - do: On the PC, set Windows' mode to Custom with a dark Windows mode and a light app mode, then change the Windows mode to light
> - see: The tray icon is white on the dark taskbar, then black on the light taskbar

> r[ui.tray.sharp]
> The tray icon is sharp at every display scaling: on a PC it is drawn for each size the
> notification area asks for, 16, 20, 24, 32, 48 and 64 px, so at 100 %, 125 %, 150 % and 200 %
> scaling the whole image shows at its full size, not scaled from another size.
>
> - tier: human (sharpness is judged by eye)
> - do: On the PC, set the display's scaling to 150 % and look at the tray icon
> - see: Both rectangles, the mark between them and the badge are crisp and whole

> r[ui.tray.badge]
> On a PC the tray icon's badge (`viewmodel.tray.badge`) is a dot 30 % of the icon's width in its
> lower-right corner: `signal` while linked, `amber` while waiting and `fault` at fault, in the
> settings window's appearance (`ui.look.palette`). A gap of 1/32 of the icon's width is cut out of
> the image round the dot.
>
> - tier: human (colour is judged by eye)
> - do: On the PC, look at the tray icon with a peer linked, then quit Beamer on every peer
> - see: The dot in the icon's corner is cyan, then turns amber, with a thin clear ring between it and the rectangles

> r[ui.tray.dimmed]
> A dimmed tray icon (`viewmodel.tray.badge`) draws its image at 0.4 opacity and its badge at full
> opacity.
>
> - tier: human (opacity is judged by eye)
> - do: On the PC, start another program on Beamer's port, then start Beamer
> - see: The rectangles in the tray icon are faint while the red dot in its corner is bright

## Follow banner

How the banner "Following {name}'s design" is drawn; when it shows and what it says is
`viewmodel.design.follow-lock`.

> r[ui.banner.look]
> The follow banner is a box filled with `well`, bordered 1 pt in `hairline` with 5 pt rounded
> corners (`ui.look.palette`), padded 10 pt at the top and bottom and 12 pt left and right. Its words are 13
> pt semibold in `text`, and "Stop following" is a small button 8 pt under them, at the box's left.
>
> - tier: human (the banner is judged by eye)
> - do: Follow a peer's design with the Design group of Same on all machines off, and open the Design page
> - see: A softly outlined box at the top of Style contains the bold line "Following {name}'s design" with a small "Stop following" button under it on the left
> - parity: Python's PC set the words at a regular weight (ADR 0001: one Rune UI draws both machines; the Mac's)

> r[ui.banner.wrap]
> At every window width, the banner's words wrap only between words, never inside one, and "Stop
> following" keeps its own width and its height of 30 pt.
>
> - tier: human (wrapping is judged by eye)
> - do: Follow a peer with a long name, open the Design page and narrow the window to its minimum width
> - see: The banner's line wraps onto more lines between words, and the button stays its usual size

## Edge glow

The edge glow is what plays along a zone while the pointer pushes it and as the push crosses (F29),
in the crossing style chosen on the Design page (`viewmodel.design.styles`): Glow, a band of light
that deepens with push pressure, or Beam, a thin line with a comet of light running along it. Both
draw in the colour chosen (`ui.palette.stops`), at the depth the Size gives the display pushed
(`ui.size.depth`), and play their crossing for the time the Length gives (`ui.length.play-time`).
When they play is said under "When feedback plays". The crossing effects (F30) are Later, their room `ui.later.effects`.

### Glow

> r[ui.glow.band]
> Glow is a band along the stretch being pushed, growing inward from the edge. Its depth is 35 % of
> the full depth (`ui.size.depth`, for the display pushed) at the lightest push and grows in
> proportion to push pressure, to the full depth at the resistance. Its opacity is 0.85 times the push
> pressure as a fraction of the resistance.
>
> - tier: human (the band's depth and brightness are judged by eye)
> - do: Choose Glow, then push slowly against an edge that leads to a peer, without crossing
> - see: A faint, shallow band appears along the edge and grows deeper and brighter the harder you push
> - parity: Python's Mac drew the band at an opacity of 0.3 + 0.7 times the push pressure, so it appeared at 30 % on the first touch; its PC drew 0.85 times the push pressure (ADR 0001: one Rune UI draws both machines; the PC's, which the Design previews also drew)

> r[ui.glow.falloff]
> Glow's light is strongest at the edge and falls off evenly inward, reaching nothing at the band's
> inner edge: halfway into the band it is half as strong as at the edge.
>
> - tier: human (the falloff is judged by eye)
> - do: Choose Glow and push hard against an edge without crossing
> - see: The band is brightest against the edge and fades evenly into the desktop

> r[ui.glow.flash]
> When a push crosses, Glow flashes: the band goes to the full depth at full opacity and fades out
> over 0.18 s.
>
> - tier: human (the flash is judged by eye)
> - do: Choose Glow and push through an edge to a peer
> - see: The band flashes bright and full-depth as the pointer goes through, then fades out within a moment
> - parity: Python's Mac flashed for 0.18 s and its PC for 0.14 s (ADR 0001: one Rune UI draws both machines; the Mac's, which the Design previews also played)

### Beam

> r[ui.beam.line]
> Beam is drawn at the full depth (`ui.size.depth`) whatever the push pressure: a thin bright line
> at the edge with a short glow behind it. Inward from the edge its light is at 1 at the edge, 0.85 at
> 8 % of the depth, 0.25 at 30 % and nothing at the full depth, straight between those stops. Its
> opacity is 0.85 times the push pressure as a fraction of the resistance.
>
> - tier: human (the line's shape is judged by eye)
> - do: Choose Beam and push against an edge without crossing
> - see: A thin bright line lies along the edge with a short soft glow behind it, brighter as you push harder
> - parity: Python's Mac drew Beam at an opacity equal to the push pressure; its PC drew 0.85 times the push pressure (ADR 0001: one Rune UI draws both machines; the PC's, as for Glow)

> r[ui.beam.comet]
> Along the stretch, Beam's line is dim, at 0.2 of its brightness, except round the comet: the
> comet's head is at full brightness, and its light falls away in both directions over 15 % of
> the stretch's length, as the square of the distance left to cover.
>
> - tier: human (the comet's shape is judged by eye)
> - do: Choose Beam and push steadily against an edge without crossing
> - see: A bright spot of light with soft tails travels along a dimmer line

> r[ui.beam.comet-speed]
> The comet runs along the stretch from its top end on a left or right edge, and from its left end on
> a top or bottom edge. Its head moves the stretch's length in 2.4 s at the lightest push and in
> 0.8 s at the resistance, in proportion to push pressure in between. Each push starts it off the
> starting end, and once it has run off the far end it starts again from the starting end.
>
> - tier: human (the comet's speed is judged by eye)
> - do: Choose Beam, push lightly against a left or right edge, then push harder without crossing
> - see: The comet runs from top to bottom, unhurried at first and clearly faster as you push harder, starting over at the top each time it leaves the bottom

> r[ui.beam.through]
> When a push crosses, Beam lights the whole stretch at full brightness, dimming to the line's base
> over 0.18 s, while the comet runs on off the far end at the stretch's length every 0.35 s without
> starting over, and the whole Beam fades out over 0.6 s.
>
> - tier: human (the breakthrough is judged by eye)
> - do: Choose Beam and push through an edge to a peer
> - see: The whole line flashes as the pointer goes through, the comet shoots off the end and does not come round again, and the line fades out in about half a second

### Where Glow and Beam draw

> r[ui.glow.colours]
> Glow and Beam lay the chosen colour's stops evenly along the stretch, in order: from the top on a
> left or right edge, from the left on a top or bottom edge. A colour of one stop, Signal, is drawn
> flat.
>
> - tier: human (colour is judged by eye)
> - do: Choose Colourful, then push against a left or right edge, then against a bottom edge
> - see: The colours run from red-pink at the top to green at the bottom, then from red-pink on the left to green on the right

> r[ui.glow.soft-end]
> Wherever Glow or Beam draws, along an edge, a third, the notch or either wall of a corner, at every
> Size and display scale and in every colour, its light reaches nothing at the inner edge of the
> band, with no visible step where the band ends. The colour chosen never changes how strongly or
> how far inward the light reaches, and neither does the display's colour profile.
>
> - tier: human (a hard edge is judged by eye)
> - do: With Size at its largest, push hard against an edge and into a corner, with Glow and then with Beam, in Signal and in White
> - see: The light fades out to nothing inward, with no line or step where it stops, and reaches equally far in both colours

> r[ui.glow.third]
> A stretch shorter than its display's edge, such as a third, fades in at its start and out at its
> end, each over 72 pt but never over more than a quarter of its length. A whole edge does not fade at
> its ends. This applies to a push of this machine's own pointer and of its driver's.
>
> - tier: human (the fade at the ends is judged by eye)
> - do: Turn on only the middle third of an edge toward a peer, push against it, then turn on the whole edge and push again
> - see: The light along the middle third fades out softly at both of its ends; along the whole edge it runs to the display's corners
> - parity: Python's Mac lit the whole edge for its driver's push at a third, and faded only its own pushes' thirds (#16: one crossing engine reports the stretch pushed, `crossing.method.report-pressure`)

> r[ui.glow.corner]
> A push into a corner lights a square at that corner of the display, 200 pt wide, or as
> wide as the display's width or height if that is less: Glow or Beam along both of the corner's walls, each as it
> is drawn along an edge, brightest where the walls meet and falling to nothing at the far end of each
> wall as (1 − u)^1.4, u being the distance from the corner as a fraction of the square's width.
> Nothing is drawn in the middle of the square.
>
> - tier: human (the corner's light is judged by eye)
> - do: Turn on a corner toward a peer and push diagonally into it, with Glow
> - see: Light runs along both walls from the corner, brightest in the corner and fading out about 200 pt along each, with nothing out in the desktop between them
> - parity: Python's PC faded each wall from 1 at the corner to 0.75 at 30 % of its length and to nothing at the end (ADR 0001: one Rune UI draws both machines; the Mac's, which the Design previews also drew)

> r[ui.beam.corner]
> In a corner, Beam's comet makes one run over both walls: in along the top or bottom wall from its
> far end into the corner over the first half of its run, and out along the left or right wall over
> the second.
>
> - tier: human (the comet's run is judged by eye)
> - do: Choose Beam, turn on a bottom corner toward a peer and push diagonally into it, without crossing
> - see: The comet travels along the bottom wall into the corner and turns up the right wall

> r[ui.glow.menu-bar]
> On a Mac, where the full depth is greater than the height of the menu bar across the top of the
> display pushed, Glow along the top edge, and along the top wall of a top corner, is scaled by the
> menu bar's height over the full depth, so a push at the resistance fills the menu bar exactly. Beam,
> the bottom edge, the bottom corners, the notch (`ui.glow.notch`), and a display whose menu bar is
> hidden or absent keep the depth of `ui.glow.band`. The menu bar is that display's own.
>
> - tier: human (filling the menu bar is judged by eye)
> - do: On a Mac with its menu bar showing, choose Glow, turn on a top corner toward a peer and push hard into it
> - see: The top wall's light fills the menu bar's height, matching the band along the right or left wall, rather than lying as a faint stripe in the bar's top half

> r[ui.glow.notch]
> At the notch, Glow and Beam light the notch's span of the top edge as they light a third
> (`ui.glow.third`), but hung from the notch's lower edge rather than from the top of the display,
> so that all of their light shows below the camera housing: Glow's band grows downward from the
> notch's lower edge, and Beam's bright line lies along it. The menu bar's height does not scale
> them there (`ui.glow.menu-bar`).
>
> - tier: human (the light at the notch is judged by eye)
> - do: On the Mac, turn on the notch toward the PC, choose Glow and push up into the notch without crossing; then choose Beam and push again
> - see: Glow's light hangs just below the notch, as wide as it, fading out at both ends and reaching further down the harder you push; Beam's thin bright line runs along the notch's lower edge with its comet travelling from left to right
> - parity: Python drew no Glow or Beam at the notch: the notch island or notch beam played there (#11: the notch island and beam, F31, are Later, their room `ui.later.notch`; hung below the notch, the light shows where the camera housing would hide it)

> r[ui.glow.drain]
> While pressure reports keep coming, Glow and Beam show the push pressure reported. After the last,
> they fall with the push pressure as it decays (crossing's 300 px per second), and are gone once it
> reaches nothing.
>
> - tier: human (the fade after a push is judged by eye)
> - do: Push hard against an edge without crossing, then stop moving the pointer
> - see: The light fades away smoothly in well under a second, rather than vanishing or staying lit
> - parity: Python's PC, and its Mac for its driver's push, drained the light from full to nothing in 0.4 s whatever the resistance; the Mac's own push followed its push pressure (#16: one crossing engine, whose push pressure the light follows)

> r[ui.glow.style-change]
> A change of crossing style applies from the next push: it is drawn wholly in the new style, its
> depth and inward falloff included.
>
> - tier: human (the style shown is judged by eye)
> - do: Choose Beam and push against an edge, then choose Glow and push again
> - see: The second push shows Glow's band fading evenly inward, with nothing of Beam's thin line left

> r[ui.glow.unknown-style]
> A crossing style other than Glow or Beam, such as one of the crossing effects (F30) saved by a later
> Beamer, plays as Glow in the chosen colour.
>
> - tier: human (the style shown is judged by eye)
> - do: With Beamer quit, set the crossing style in this machine's settings file to a crossing effect's id, such as `rupture`, then start Beamer and push against an edge
> - see: Glow plays along the edge in the chosen colour
> - parity: Python played the crossing effect, and Glow only once the effects had failed (#11: the crossing effects, F30, are Later; their room is `ui.later.effects`)

> r[ui.glow.after-length]
> At every Length, Glow and Beam follow push pressure alone while the push builds. The Length
> (`ui.length.play-time`) multiplies only what plays after the push crosses, the flash, the comet's
> run off the end and Beam's fade, and the Ring's timing (`ui.ring.closes`).
>
> - tier: human (timing is judged by eye)
> - do: Push through an edge with Length at its shortest, then at its longest
> - see: the push builds alike both times; at the longest Length the flash and fade after crossing last clearly longer

## Ring

The Ring is a style for "Shortcut and menu" (`viewmodel.design.switch-styles`): it shows where the
pointer is when input arrives with no landing point, while "Show where the pointer lands" is on
(`viewmodel.design.landing`).

> r[ui.ring.when]
> The Ring plays at the pointer on this machine as input arrives with no landing point: when this
> machine's own user brings input home by the shortcut, a jump key, a shake or the menu
> (`crossing.method.report-home`, `crossing.shake.home`), and when a driver takes this machine with
> no landing point, by the shortcut, a jump key or the menu (`crossing.landing.no-edge`). It plays on
> no other arrival: not when the driven machine sends input home
> (`crossing.method.report-arrival-no-point`), not when a crossing lands the pointer through an
> edge, and never on the machine input leaves.
>
> - tier: human (the Ring's appearance is judged by eye)
> - do: With "Show where the pointer lands" on, on both machines, cross from the Mac to the PC by the shortcut and come home by the shortcut; then cross to the PC by the shortcut again and come home by shaking the Mac's pointer
> - see: A ring closes onto the pointer on the PC each time input arrives there, and on the Mac each time input comes home, by the shortcut and by the shake; none plays on the machine input leaves
> - parity: Python's PC also played the Ring when the driven machine sent input home with no landing point (`win_app/sender.py:1538`, `win_app/kvm_bridge_win.py:5749`), where its Mac played none (`mac_app/kvm_bridge_app.py:5633–5638`); both started every Ring 0.12 s late and dropped it when a crossing landed the pointer meanwhile (`core/effects.py:1378–1386`), so a send home that came with a take through an edge showed only the take (ADR 0001: one Rune UI draws both machines; the Mac's, under which no Ring comes with a crossing, so none waits for one)

> r[ui.ring.off]
> With "Show where the pointer lands" off, or with crossings not animated on this machine
> (`viewmodel.design.animate`), no Ring plays on it.
>
> - tier: human (the Ring's absence is judged by eye)
> - do: Turn off "Show where the pointer lands", cross to a peer by the shortcut and come home by it
> - see: No ring appears at the pointer when input comes home

> r[ui.ring.same-as-crossing]
> As the style for "Shortcut and menu", "Ring" plays the Ring. "Same as crossing" plays the crossing style's own arrival,
> and Glow and Beam have none, so under either of them it plays the Ring too.
>
> - tier: human (what plays is judged by eye)
> - do: Choose Beam as the crossing style and "Same as crossing" as the style for "Shortcut and menu", then come home by the shortcut
> - see: The Ring closes onto the pointer

> r[ui.ring.closes]
> The Ring is one ring centred on the pointer's tip that closes onto it: from 220 pt across to 32 pt
> over 0.45 s, quickly at first and slowing as it closes (a cubic ease-out), its line thickening from
> 2 pt to 3 pt. It fades in over its first 0.08 s and out from 0.52 s to 0.72 s. From 0.38 s a
> soft glow 52 pt across, in the colour's first stop at up to 0.45 opacity, swells at the tip until
> 0.5 s and fades by 0.72 s. These sizes are at a depth of 36 pt and grow and shrink in proportion to
> the depth (`ui.size.depth`); the times are at a Length of 1.
>
> - tier: human (the Ring's motion is judged by eye)
> - do: With "Show where the pointer lands" on, come home by the shortcut
> - see: A ring a few centimetres across shrinks onto the pointer's tip, slowing as it closes, glows briefly at the tip and is gone in under a second

> r[ui.ring.colours]
> The Ring is drawn in the chosen colour: a colour of one stop strokes the whole ring in it, and a
> colour of several stops runs them round the ring evenly, clockwise from the top, the last stop
> blending into the first with no seam.
>
> - tier: human (colour is judged by eye)
> - do: Choose Colourful and come home by the shortcut, then choose Signal and do it again
> - see: The first ring shows Colourful's hues all round with no seam; the second is all Signal's blue

> r[ui.ring.halo]
> Under the Ring's line runs a dark halo 3 pt wider than it: black at 0.5 opacity when the OS is in
> light mode and 0.3 in dark mode (`ui.palette.os-mode`), so the Ring stays legible on a light
> wallpaper.
>
> - tier: human (legibility is judged by eye)
> - do: In light mode, over a white window, choose White and come home by the shortcut
> - see: The Ring shows clearly, outlined by a soft dark edge

> r[ui.ring.reduce-motion]
> With the OS's reduce motion on, the Ring keeps its size: two still rings round the pointer, 32 pt
> across with a 3 pt line and 60 pt across with a 2 pt line at 0.55 of the inner's opacity, fade in
> over 0.1 s, and out from 0.35 s to 0.7 s.
>
> - tier: human (motion is judged by eye)
> - do: Turn on Reduce Motion in the OS, then come home by the shortcut
> - see: Two small rings fade in round the pointer and out again without moving or changing size

## Colours

The ten colours the Design page offers (`viewmodel.design.colours`). Glow, Beam and the Ring all draw
the colour chosen. The crossing effects' colour packs are Later (F30).

> r[ui.palette.stops]
> Each colour has these stops, in order:
>
> | Colour | Stops |
> |---|---|
> | Signal | `#5fd4f4` |
> | Colourful | `#ff3264` `#ff7828` `#f032b4` `#b428f0` `#6446ff` `#288cff` `#1eb9aa` `#32c850` |
> | Ocean | `#288cff` `#6446ff` `#1eb9aa` `#3c6eff` |
> | Sunset | `#ff7828` `#ff3264` `#ffb43c` `#f03278` |
> | Aurora | `#53dc91` `#9572eb` `#43c5b4` |
> | White | `#ebebeb` `#969696` `#ebebeb` |
> | Silver | `#c9ced2` `#a8afb6` `#e1e5e8` |
> | Graphite | `#65696f` `#4a4f55` `#868c94` |
> | Ink | `#25282c` `#101215` `#41464d` |
> | Warm grey | `#b7ada0` `#8a8074` `#d8cfc2` |
>
> In light mode every colour but Aurora is drawn in these stops (`ui.palette.light-aurora`); dark
> mode lifts the darkest (`ui.palette.dark-lift`, `ui.palette.dark-ink`).
>
> - tier: human (colour is judged by eye)
> - do: In light mode, push against an edge in Sunset, then in Warm grey
> - see: Sunset runs orange, pink-red, gold and rose along the edge; Warm grey runs in beige-greys

> r[ui.palette.light-aurora]
> In light mode Aurora is drawn in deeper stops of the same hues, `#1d8d50` `#8962e9` `#2a897d`, so
> it stays legible on a light wallpaper.
>
> - tier: human (legibility is judged by eye)
> - do: In light mode, over a white window, choose Aurora and push against an edge
> - see: A clearly visible deep green, violet and teal band, not a washed-out pastel

> r[ui.palette.dark-lift]
> In dark mode every stop darker than an HLS lightness of 0.45 is drawn lifted to 0.45, keeping its
> hue and saturation, so it does not vanish on a dark wallpaper: Colourful's and Ocean's `#1eb9aa`
> draws as `#20c5b5`, and Graphite as `#6d7278` `#6b727b` `#868c94`. Ink is drawn as
> `ui.palette.dark-ink` says. Every other stop of the ten colours is drawn as given.
>
> - tier: human (legibility is judged by eye)
> - do: In dark mode, over a black window, choose Graphite and push against an edge
> - see: A visible mid-grey band, not one that disappears into the dark

> r[ui.palette.dark-ink]
> In dark mode Ink is drawn as a deep blue-black, `#2c3e66` `#1f2d4d` `#3b5287`, so it reads as ink
> on a dark wallpaper rather than as a grey.
>
> - tier: human (colour is judged by eye)
> - do: In dark mode, over a black window, choose Ink and push against an edge
> - see: A dark navy band that shows against the black, not a steel grey and not white

> r[ui.palette.os-mode]
> Light and dark mode, for the colours and the Ring's halo, are the OS's own: a Mac's appearance, and
> the app mode a PC sets for apps, whatever Beamer's own "This window" appearance
> (`viewmodel.design.appearance`). The mode is read as each edge glow or Ring starts.
>
> - tier: human (colour is judged by eye)
> - do: With the OS in dark mode and Beamer's window set to Light, choose Ink and push against an edge
> - see: The band is Ink's dark navy, as in dark mode, not its near-black

> r[ui.palette.unknown]
> A colour that is not one of the ten, such as one of the crossing effects' colour packs (F30) saved
> by a later Beamer, is drawn as Signal.
>
> - tier: human (colour is judged by eye)
> - do: With Beamer quit, set the colour in this machine's settings file to a colour pack's id, such as `ember`, then start Beamer and push against an edge
> - see: The light is Signal's blue
> - parity: Python drew a crossing effects' colour pack in the pack's own stops, and any other unknown colour as Signal (#11: the colour packs, F30, are Later; their room is `ui.later.effects`)

## When feedback plays

What the edge glow, the Ring and the trackpad do with each report Core makes for a crossing
(`spec/crossing.md`, "What a crossing reports"). The edge glow and the Ring play only on this
machine's displays, and only while crossings are animated on it.

> r[ui.feedback.push]
> While Core reports push pressure at a zone (`crossing.method.report-pressure`), the edge glow
> plays in the crossing style along the stretch reported, at the pressure reported: the whole edge, a
> third, a corner's square (`ui.glow.corner`) or the notch's span (`ui.glow.notch`). This applies to
> this machine's own pointer at its own zones, and for its driver's pointer at the zones this machine
> arms for it, its return zone included.
>
> - tier: human (where the light plays is judged by eye)
> - do: Push against an edge toward a peer; then, from the peer driving this machine, push the pointer out through the edge it came in by
> - see: The light plays along the edge being pushed both times, on the display the pointer is on

> r[ui.feedback.cross]
> A crossing report (`crossing.method.report-cross`) plays the crossing style's breakthrough
> (`ui.glow.flash`, `ui.beam.through`) along the stretch that was pushed, also when the push crossed on
> its first pressure report.
>
> - tier: human (the breakthrough is judged by eye)
> - do: With the resistance at 0, move the pointer through an edge to a peer
> - see: The edge still flashes as the pointer goes through

> r[ui.feedback.touch-alone]
> Touching a zone with no push pressure (`crossing.method.report-touching`) draws nothing.
>
> - tier: human (the absence of light is judged by eye)
> - do: Rest the pointer against an edge toward a peer without pushing
> - see: Nothing lights along the edge

> r[ui.feedback.arrival]
> Glow and Beam play nothing when input arrives on a machine: neither at an arrival
> (`crossing.method.report-arrival`, `crossing.method.report-arrival-no-point`) nor on the driven
> machine when a driver's take lands its pointer there through an edge. What plays when input
> arrives with no landing point is the Ring's (`ui.ring.when`).
>
> - tier: human (the absence of light is judged by eye)
> - do: Cross to a peer through an edge and come home through the edge you landed at
> - see: Light plays at the edge pushed on each machine, and none at the edge the pointer lands at

> r[ui.feedback.animate-off]
> With crossings not animated on this machine (`viewmodel.design.animate`), no edge glow and no Ring
> play on it. Its trackpad ticks are unchanged.
>
> - tier: human (the absence of light is judged by eye)
> - do: Turn off "Animate crossings on this Mac" and push through an edge to a peer on a trackpad
> - see: Nothing lights along the edge, and the ticks and the crossing's double tick are still felt

> r[ui.feedback.one-display]
> The edge glow and the Ring play on one display, measured as if it were the only one: the display
> the pointer is on, or for a push that goes on with no new report, the display it started on. A
> stretch that runs past that display, along an edge of the whole desktop, is cut to it, and the
> light stops at that display's edges (`ui.overlay.clip`). A reported point
> that lies on no display counts as on the nearest display, at its nearest point.
>
> - tier: human (which display lights is judged by eye)
> - do: With two displays of different heights next to each other, push against the outer edge of the shorter one, then bring input home by the shortcut with the pointer at the edge the displays share
> - see: The light runs only along the shorter display's edge; the Ring is cut off at the shared edge and never shows on the other display

> r[ui.feedback.haptic-ticks]
> On a Mac with "Tick as the push builds" on, each tick report (`crossing.method.report-tick`) is
> felt on the trackpad as one tick, as "Ticks at" says (`viewmodel.design.trackpad`): "Every quarter"
> at a quarter, a half and three quarters of the resistance, "Halfway" only at a half, and "Only when
> through" at none.
>
> - tier: human (ticks are felt)
> - do: On a Mac's trackpad, with "Ticks at" on "Every quarter", push slowly through an edge; then on "Halfway"
> - see: Three ticks are felt as the push builds, then only one, halfway

> r[ui.feedback.haptic-through]
> On a Mac with "Tick as the push builds" on, a crossing report (`crossing.method.report-cross`) and
> an arrival (`crossing.method.report-arrival`, `crossing.method.report-arrival-no-point`) are felt
> as two ticks 60 ms apart, whatever "Ticks at" says. Input coming home by this machine's own
> shortcut, jump key, shake or menu (`crossing.method.report-home`, `crossing.shake.home`) is not
> felt.
>
> - tier: human (ticks are felt)
> - do: On a Mac's trackpad, with "Ticks at" on "Only when through", push through an edge to a peer
> - see: No tick while the push builds, then a quick double tick as the pointer goes through

> r[ui.feedback.haptic-off]
> On a Mac with "Tick as the push builds" off, nothing is felt for a crossing: no ticks and no double
> tick.
>
> - tier: human (ticks are felt)
> - do: Turn off "Tick as the push builds" and push through an edge on the trackpad
> - see: Nothing is felt on the trackpad

## Size and Length

The Design page's "Size" and "Length" (`viewmodel.design.size`, `viewmodel.design.length`) set how
deep Glow, Beam and the Ring draw and how long they play. The rulers' ranges, steps, detents and
words are the view-model's; the depths and times they stand for, and how the rulers are drawn, are
here.

### Size

> r[ui.size.depth]
> Glow, Beam and the Ring are drawn at a depth set by the Size: the Size, from 2 % to 25 %, times the
> shorter edge of the display they are drawn on, in pt, and never less than 24 pt. The Size is 4.5 %
> until the user sets it; the ruler's detents at 3 %, 4.5 % and 6 % (`viewmodel.design.size`) are
> Python's Small, Medium and Large. At 4.5 % a display of 1512 × 982 pt gets a depth of 44.19 pt
> and one of 3840 × 2160 pt 97.2 pt; at 25 % the 3840 × 2160 pt display gets 540 pt; at 2 % a
> display of 640 × 480 pt gets 24 pt. The Size ruler's readout is this depth for this machine's
> main display, the whole display.
>
> - tier: human (the depth of the light is judged by eye)
> - parity: Python's PC measured the readout's depth on its main display less the taskbar (ADR 0001: one Rune UI draws both machines; the Mac's, the whole display, which the glow itself uses)
> - do: Choose Glow, set Size to 3 % and push hard against an edge without crossing; then set Size to 25 % and push again
> - see: The first band is a thin strip along the edge; the second reaches across about a quarter of the display's shorter edge, as the readout's depth says

> r[ui.size.scales]
> Every style grows in proportion to the depth: Glow's band and Beam's light reach the depth inward
> (`ui.glow.band`, `ui.beam.line`), and the Ring's sizes are those of `ui.ring.closes` and
> `ui.ring.reduce-motion` times the depth over 36 pt. At a depth of 90 pt each style's light reaches
> more than 2.8 times as far as at 24 pt, at 1 and at 2 device pixels per pt, with the OS's reduce
> motion on or off.
>
> - tier: human (sizes are judged by eye)
> - do: With "Show where the pointer lands" on, set Size to 2 %, push against an edge and come home by the shortcut; then set Size to 8 % and do the same
> - see: The band along the edge and the ring round the pointer are both several times larger the second time

> r[ui.size.applies]
> A Size chosen applies to every edge glow and Ring that starts after it, both for this machine's own
> pointer and for its driver's, and to the Design page's tiles at once (`ui.preview.size`).
>
> - tier: human (the depth of the light is judged by eye)
> - do: Push against an edge, then drag Size to its largest and push again
> - see: The second push draws a clearly deeper band, and the style tiles grew as the ruler moved

### Length

> r[ui.length.play-time]
> At a Length L, from 0.25 to 2.5 and 1 until the user sets it (Python's Short, Normal and Long were
> 0.5, 1 and 2), each style plays what follows the push in L times its own time: Glow's flash in
> 0.18 s (`ui.glow.flash`), Beam's breakthrough and fade in 0.6 s (`ui.beam.through`), and the Ring
> in 0.72 s (`ui.ring.closes`, `ui.ring.reduce-motion`), every time in those rules stretched by L.
> The push itself is not stretched (`ui.glow.after-length`). The Length ruler's readout
> (`viewmodel.design.length`) is this play time for the style the tiles show: the crossing style's
> under Crossing, and the Ring's under Shortcut and menu, where "Ring" and "Same as crossing" both
> play the Ring (`ui.ring.same-as-crossing`). Beam at a Length of 2 reads "1.2 s", and the Ring at 1
> reads "0.7 s".
>
> - tier: human (timing is judged by eye)
> - do: Choose Beam, set Length to 2 and push through an edge to a peer
> - see: The readout says "1.2 s", and the line flashes and fades out over about a second after the pointer goes through

> r[ui.length.rulers]
> The Length and Size rulers each stand with their readout to the right, 10 pt apart. A readout is
> set in 9.5 pt B612 Mono, right-aligned in a box 58 pt wide whatever it reads, so the ruler never
> moves or resizes as its value changes. Each ruler takes the width its line leaves beside the
> readout, up to 240 pt, and the two rulers are always the same width. At the window's minimum width
> the readout still shows whole and its ruler stays wider than it, neither leaving the line. With
> the captions above their controls (`ui.widget.form.stacked`), both lines start at the card's
> content edge and span its width.
>
> - tier: human (placement is judged by eye)
> - parity: Python's Mac stretched both rulers across the control column, wider than 240 pt in a wide window, where its PC capped each at 240 px (ADR 0001: one Rune UI draws both machines; the PC's, compact like a group of options, `ui.widget.option.size`)
> - do: With the window wide, drag Length from end to end; then narrow the window to its minimum width
> - see: The readout's figures change in place without nudging the ruler, Length and Size stay the same width, and at the minimum width both readouts still show whole beside a ruler wider than them

> r[ui.length.keys]
> With keyboard focus on the Length or Size ruler, the left and down arrow keys lower it and the
> right and up arrow keys raise it by one step, 0.05 for Length and 0.1 % for Size; Page Down and
> Page Up move it by five steps, 0.25 and 0.5 %; Home and End set it to its lowest and highest value.
> A move past an end stops at that end.
>
> - tier: human (keys are judged by hand)
> - do: Tab to the Length ruler at 1, press the right arrow key, then Page Up, then Home, then End
> - see: The thumb moves one small step right, then a larger jump, then to the start and to the end of the ruler, and the readout follows each move

## The Design page's previews

Each style tile on the Design page (`viewmodel.design.styles`, `viewmodel.design.switch-styles`)
previews its style above its name. How a tile is drawn round its preview, hovered and chosen, and
how the tiles wrap into lines, is the widget kit's (`ui.widget.tiles.look`,
`ui.widget.tiles.hover`, `ui.widget.tiles.chosen`, `ui.widget.tiles.columns`). The preview draws
Glow, Beam and the Ring as the desktop does (`ui.glow.*`, `ui.beam.*`, `ui.ring.*`), shrunk.

### What a preview shows

> r[ui.preview.scene]
> A crossing style's tile previews its style as a crossing between two displays, in a frame twice as
> wide as it is tall, across the tile's width: the two displays filled `ground` with a 1 pt
> `hairline` outline and 4 pt corners, meeting at a 1 pt `hairline` seam that a 10 pt tick in
> `text-2` marks where it leaves the frame's bottom (at the Notch, where it leaves the frame's
> left end); an arrow pointer 11 pt tall, white outlined in black; and the style's light. The light
> fades out over the outer tenth of the frame at each of its four borders, so light running past the
> frame thins away rather than stopping at a cut edge, and it casts a faint shadow 1 pt below it
> in a darker `ground`.
>
> - tier: human (a preview is judged by eye)
> - do: Open the Design page with "Show at" on "Edge", in Light and in Dark
> - see: Each crossing tile shows two displays in the window's own background colour meeting at a thin line, a white pointer at the line and the style's light along it, fading softly out toward the frame's borders

> r[ui.preview.place]
> Every crossing style's tile previews its style at the place "Show at" chooses
> (`viewmodel.design.show-at`): at Edge, where the right edge of the left display meets the left
> edge of the right one; at Corner, where the top-right corner of the left display meets the
> top-left corner of the right one; at the Notch, where the notch in the middle of the lower
> display's top edge meets the bottom edge of the display above. Choosing a place redraws every
> tile at once. At the Notch, Glow and Beam light the notch's span as `ui.glow.notch` says, and the
> notch is drawn black, with rounded lower corners, at this Mac's notch size, or 185 × 32 pt on a Mac
> without one, at the scale of the displays.
>
> - tier: human (a preview is judged by eye)
> - parity: Python's Mac showed the notch styles' own preview, the notch island or notch beam, in the Glow and Beam tiles at the Notch (#11: the notch island and beam, F31, are Later; Glow and Beam light the notch, `ui.glow.notch`)
> - do: On a Mac, choose "Edge", "Corner" and "Notch" in turn under "Show at"
> - see: Every tile changes at once to show its style at a straight edge, then in a corner, then round a black notch at the top of the lower display

> r[ui.preview.size]
> A tile's displays stand for displays of 1600 × 900 pt: its style is drawn as it would be on such a
> display, at the depth `ui.size.depth` gives it at the chosen Size and in the chosen colour, then
> shrunk whole to fit the frame, so a tile shows the real light scaled down. A change of Size redraws
> every tile at once, and the frame does not zoom out as the Size grows, so a larger Size shows as
> deeper light. At every Size and Length, a tile's still shows its light within the frame.
>
> - tier: human (a preview is judged by eye)
> - do: Drag Size from 2 % to 25 % while watching the style tiles
> - see: The light in each tile grows deeper as the ruler moves, while the displays and the pointer in the tile stay where they are

> r[ui.preview.framing]
> A crossing tile's frame is centred on the seam across it and fitted round its style's still and
> the pointer's whole journey, so the pointer is in view, its arrow whole, at every moment of the
> loop (`ui.preview.journey`). It spans at least 19 % of a display's width, and at most 43 % at Edge
> and Corner and 68 % at the Notch. It is fitted at a Size of 4.5 % and a Length of 1 and keeps that
> framing at every Size and Length.
>
> - tier: human (a preview is judged by eye)
> - do: Hover a crossing tile at each place under "Show at" and watch a whole loop
> - see: The pointer never leaves the frame or is cut by its edge, from where it starts to where it rests

> r[ui.preview.journey]
> Playing, a crossing tile runs a loop of 4.4 s. The pointer glides to the seam from inside the left
> display (the lower one, at the Notch) over 0.4 s, easing in and out; from 0.6 s it pushes, push
> pressure rising from nothing to the resistance along half a cosine, and crosses at 2.2 s; it
> appears just inside the other display and glides inward until 3.4 s; nothing then moves until the
> loop starts again at 4.4 s with the pointer where it began. The style plays as on the desktop: its
> push while the pointer pushes and its breakthrough as it crosses (`ui.glow.band`,
> `ui.glow.flash`, `ui.beam.comet`, `ui.beam.through`).
>
> - tier: human (motion is judged by eye)
> - do: Choose Beam's tile and keep the pointer over it for ten seconds
> - see: The pointer glides to the line, the comet runs faster as it pushes, the line flashes as the pointer passes to the other display, the pointer settles, and the same journey plays again

> r[ui.preview.still]
> At rest each tile shows a still: the one frame of its loop that shows its style best, drawn the
> same every time. Glow's still is 0.05 s before the crossing, its band near its full depth; Beam's
> is 0.05 s after it, the whole line flashing; the Ring's is 0.1 s after it starts. A still is the
> same at every Length.
>
> - tier: human (a preview is judged by eye)
> - do: Look at the Glow and Beam tiles, set Length from 0.25 to 2.5, and look again
> - see: Glow's tile shows a deep band at the seam and Beam's a bright line, and neither changes when the Length does

> r[ui.preview.notch-pointer]
> At the Notch, the pointer is drawn clear of the notch: while it pushes, just below the notch's
> lower edge rather than under it, and as it lands, with its whole arrow inside the upper display,
> above the seam. So it is for any notch from 150 × 24 pt to 220 × 48 pt.
>
> - tier: human (a preview is judged by eye)
> - do: On a Mac, choose "Notch" under "Show at" and hover Glow's tile through a whole loop
> - see: The pointer stays visible under the black notch as it pushes, and appears wholly in the upper display after the crossing

> r[ui.preview.shortcut]
> A tile under Shortcut and menu previews what plays round the pointer on one display. Playing, it
> loops every 2 s with the pointer still at 46 % across and 44 % down the display: nothing is drawn
> until 0.35 s in, when the Ring starts and closes onto the pointer (`ui.ring.closes`). Its still is
> the Ring alone, with no pointer and no display drawn.
>
> - tier: human (a preview is judged by eye)
> - do: Choose "Shortcut and menu" under "Style for" and keep the pointer over the Ring's tile for a few seconds
> - see: At rest the tile shows a ring; hovered, a pointer appears and a ring closes onto it about every two seconds

> r[ui.preview.same-as-crossing]
> The "Same as crossing" tile previews what "Same as crossing" plays for the crossing style chosen
> now: the Ring while it is Glow or Beam (`ui.ring.same-as-crossing`). A change of crossing style
> redraws it at once.
>
> - tier: human (a preview is judged by eye)
> - do: Choose Beam under Crossing, then look at the tiles under Shortcut and menu
> - see: "Same as crossing" shows the same ring as the "Ring" tile

### Playing on hover

> r[ui.preview.rest]
> While no tile is hovered, no tile plays: every tile shows its still, and the page draws no frames
> for them.
>
> - tier: human (motion is judged by eye)
> - do: Open the Design page and leave the pointer off the tiles
> - see: Every tile is still

> r[ui.preview.hover]
> Once the pointer has stayed over a tile for 0.1 s, that tile plays, and only that one: a crossing
> tile from the start of its loop, the pointer gliding in (`ui.preview.journey`), and a tile under
> Shortcut and menu on from its still. Before 0.1 s it keeps its still, so a pointer passing over
> tiles toward somewhere else starts none of them. It plays for as long as the pointer stays, looping.
>
> - tier: human (motion is judged by eye)
> - do: Sweep the pointer quickly across the style tiles, then rest it on one
> - see: The tiles swept over stay still; the one rested on starts playing almost at once, and no other tile plays with it

> r[ui.preview.leave]
> When the pointer leaves a playing tile, the tile cross-fades to its still over 200 ms
> (`ui.motion.ease`), whatever moment of its loop it had reached, a breakthrough included. Hovered
> again, it plays as `ui.preview.hover` says.
>
> - tier: human (motion is judged by eye)
> - do: Hover Beam's tile until the line flashes, and move the pointer off it at that moment
> - see: The tile dissolves to its usual still instead of freezing on the flash

> r[ui.preview.length]
> A playing tile plays its style at the chosen Length (`ui.length.play-time`): its breakthrough and
> the Ring take L times as long. The pointer's journey and the loop's 4.4 s and 2 s do not change
> with the Length, and the Length does not redraw the stills (`ui.preview.still`).
>
> - tier: human (timing is judged by eye)
> - do: Hover Beam's tile at a Length of 0.25, then at 2.5
> - see: After the pointer crosses, the line's flash and fade are over in a blink the first time and linger for well over a second the second time

> r[ui.preview.reduce-motion]
> With the OS's reduce motion on, hovering a tile plays nothing: it keeps its still. A tile playing
> when reduce motion turns on returns to its still; after it turns off, a hovered tile plays again.
>
> - tier: human (motion is judged by eye)
> - do: Turn on the OS's reduce-motion setting and hover the style tiles; turn it off and hover again
> - see: With it on, every tile stays on its still; with it off, the hovered tile plays

> r[ui.preview.unseen]
> A hovered tile that cannot be seen, because the window is hidden, minimised or covered or the tile
> is scrolled out of view, draws no frames until it can be seen again, and then shows the moment its
> loop has reached.
>
> - tier: human (motion is judged by eye)
> - do: Hover a style tile so it plays, then cover the settings window with another window for a few seconds and uncover it
> - see: The tile is playing on, midway through its loop, when it shows again

### Legible previews

> r[ui.preview.legible]
> Every preview's light stands out from its tile, in Light and in Dark, at 1 and at 2 device pixels
> per pt: of the pixels its light changes, the strongest tenth reach a contrast of 3:1 or more with
> `ground`, and together they cover at least 1 % of the frame, in every colour. To that end a preview
> draws its light 2.8 times as opaque as the desktop does, up to fully opaque, and its lines 1.6
> times as wide, none thinner than 1.25 pt in a frame 200 pt wide. In Light it also darkens each
> colour, keeping its hue, until its relative luminance is at most 0.22, and draws the Ring's dark
> halo (`ui.ring.halo`) at 0.3 of its opacity.
>
> - tier: human (legibility is judged by eye)
> - do: In Light, choose White and look at every style tile, under Crossing and under Shortcut and menu; then do the same in Dark
> - see: Every tile's light shows clearly against the tile in both appearances, the white light drawn as a visible grey on the light tiles

> r[ui.preview.same-on-both]
> A style's preview is the same on a Mac and on a PC, at the same place, colour, Size, Length and
> appearance, still and at every moment of its loop.
>
> - tier: human (previews are compared by eye)
> - do: Set the same style, colour, Size, Length, place and appearance on the Mac and the PC, and look at each one's tiles, still and hovered
> - see: The tiles look the same on both machines, Beam's line and its soft glow alike

## Rooms for what comes Later

The crossing effects (F30) and the notch island and notch beam (F31) are Later in #11: neither is
built in 2.0, and the Design page offers neither (`viewmodel.design.styles`). What ships for Glow,
Beam, the Ring and identify displays is the room left for them: the overlay, its frame clock and
its drawing (`ui.overlay.*`). They will need more of that drawing than 2.0 uses, which a later
version adds to it: for F30, shapes of any outline of lines and quadratic and cubic curves, with
holes cut by the even-odd rule, strokes with butt or square ends and mitred or bevelled corners,
and gradients whose stops come out of order, share an offset or are missing; for F31, a Gaussian
blur that softens what is drawn into a bloom.

> r[ui.later.effects]
> Glow, Beam and the Ring are drawn by one overlay drawing, the same on a Mac and a PC
> (`ui.overlay.draw.*`), on the overlay's frame clock (`ui.overlay.frame-rate`): on displays of the
> same size in pt, at the same colour, Size and Length, a push or a Ring draws the same light on
> either machine. This one drawing and its frame clock are the room left for the crossing effects
> (F30), Later in #11.
>
> - tier: human (drawing is judged by eye)
> - do: With GN10 at its full 2560 × 1440 on both machines and the same colour, Size and Length on both, choose Colourful and Beam and turn on "Show where the pointer lands"; with GN10 showing the Mac, push against its edge toward the PC without crossing, then, with the pointer on GN10, cross by the shortcut and come home by it; then do the same with GN10 showing the PC
> - see: The light along GN10's edge and the Ring round the pointer look the same from the Mac and from the PC: the same colours, depth, line and comet

> r[ui.later.notch]
> On a Mac with a notch, the overlay draws round the notch at its true size and place on its display,
> whatever resolution the display is set to: the light at the notch (`ui.glow.notch`) spans the
> notch's width and hangs from its lower edge. This, with the drawing of `ui.later.effects` and the
> overlay above the menu bar on every Space (`ui.overlay.on-top-mac`), is the room left for the
> notch island and notch beam (F31), Later in #11.
>
> - tier: human (placement is judged by eye)
> - do: On the Mac, with the notch on toward the PC and Glow chosen, set the built-in display to "More Space" in Displays settings and push up into the notch; then return it to "Default" and push again
> - see: Both times the light hangs just below the notch and spans exactly its width
