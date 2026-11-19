# App

What the `beamer` binary does as a program: its command line, single instance and the second
launch (F37), the watchdog that relaunches Beamer after a crash, Quit and `beamer quit`, how
`beamer install`, `beamer uninstall` and the `--elevated` steps run the install steps, and refusing
to start below the OS floor. `app` is the one domain with no crate suffix: its crate is the bin
crate, `beamer`.

The rules come from the decisions #21 (packaging, signing, install and restart, ADR 0004 amended),
#17 (the OS floor), #16 (one process, ADR 0004) and #11 (F37 ships; F22, the update check, is
dropped), and from the Python app at `archive/python` (`win_app/kvm_bridge_win.py`,
`win_app/quit_signals.py`, `win_app/admin_mode.py`, `win_app/Invoke-InstallMaintenance.ps1`,
`mac_app/kvm_bridge_app.py` and `mac_app/instance_signal.py`), mined through its tests in
`spec/ledger/app.txt`.

What stays elsewhere: each install step itself is its domain's. The logon task, Start at login,
admin mode, the firewall rule and the elevated setup's steps are `spec/platform.md`'s
(`platform.task.*`, `platform.login.*`, `platform.admin.*`, `platform.firewall.setup*`,
`platform.install.*`); installing and removing the virtual microphone is `spec/audio.md`'s, and the
virtual camera `spec/camera.md`'s. The heartbeat the engine writes to the watchdog's pipe, the
data folder, the log and what a panic does are `spec/runtime.md`'s (`runtime.heartbeat.*`,
`runtime.files.*`, `runtime.log.*`, `runtime.crash.*`). What Quit does to the links, the data files
and the input while Beamer quits is each domain's own. Every word Beamer shows, the restarted
notification and the floor message included, is `spec/viewmodel.md`'s, and when closing the window
quits Beamer is its tray rules' (`viewmodel.tray.close`). Delivering a notification is
`spec/platform.md`'s.

Dropped, so no rule here (#11): the update check, its notice and its release notes (F22), the
portable build (F23), and the Linux start and its Wayland activation (#1). The OS version is checked
once, at the floor, and nowhere else (#17): above it, no part of Beamer reports unsupported here
because of the OS version.

A Mac is a machine running macOS, and a PC one running Windows. Every rule is `ci` unless it says
otherwise: the command line, the instance check and the watchdog's decisions are tested on a hosted
runner of each OS; what only a real install, a real login session or a person at the desk shows is
`machine` or `human`.

## Command line

r[app.command.forms]
`beamer` takes one of these command lines: no command, which launches Beamer's app with the launch
options of `app.command.launch-options`; `install`; `uninstall`, or `uninstall --purge`; `quit`;
`--watchdog` with the arguments the app gives it (`app.watchdog.started`); and `--elevated`
followed by one elevated step (`app.elevated.steps`).

> r[app.command.launch-options]
> A launch takes these options, each at most once and in any order: `--hidden`
> (`app.command.hidden`), `--relaunched` (`app.instance.relaunch-waits`) and the watchdog's record of
> recent crashes (`app.watchdog.relaunch-arguments`). On a PC, a launch from the logon task begins
> with `--logon-task`, followed by `$(Arg0)` as written or by `--launched` and the options it carries
> (`platform.task.definition`, `platform.task.launch-visible`, `platform.task.launch-options`).
>
> - parity: Python also took `--config` with the path of a settings file to use instead of its own (#16: the data folder and its two data files are fixed, `runtime.files.folder`)

> r[app.command.refused]
> A command line `app.command.forms` does not list, such as an unknown command or option, an option
> given twice or a command given an argument it does not take, writes what `beamer` takes to
> standard error and ends with a non-zero exit status. It starts nothing and changes nothing on the
> machine.
>
> - fuzz: command-line

r[app.command.hidden]
A launch with `--hidden` starts Beamer hidden, as a start at login does (`platform.login.mac-hidden`,
`platform.task.launch-visible`), and so does a start at login. Any other launch shows Beamer's
window once it has started. What a hidden start shows on a PC while Show in tray is off is
`viewmodel.tray.hidden-start`'s.

## Single instance

F37: one Beamer runs per login session, and a second launch hands over to it.

> r[app.instance.first]
> A launch looks for a running Beamer before it does anything but the OS floor check
> (`app.floor.pc`): before it runs the logon task (`platform.task.launch`), reads a data file, writes
> a file or changes anything on the machine.
>
> - parity: Python's PC first moved the logon task of Beamer 1.5.1 to the account's own, and went on to look for a running Beamer when that failed (#21: no compatibility code goes into the app, `platform.task.no-migration`)

r[app.instance.one]
In one login session, at most one Beamer's app runs. A launch that finds one running starts no
second one, also when two launches start at the same moment, as two logon tasks would.

r[app.instance.after-crash]
A Beamer that crashed or was killed keeps no claim on its login session: the next launch starts
Beamer, with no wait.

r[app.instance.same-account]
Only a launch by the account Beamer runs as can ask it to show its window or to Quit
(`app.quit-command.asks`): a launch by another account changes nothing in that Beamer.

r[app.instance.elevated-counts]
On a PC, a launch without administrator rights counts a Beamer running with them as running.

> r[app.instance.show]
> A second launch that was not asked to start hidden asks the running Beamer to show its window, and
> ends with exit status 0. The running Beamer then shows its window, restored if it was minimised
> and in front of other windows, also when the window was hidden or closed and while Show in tray
> is off.

> r[app.instance.hidden]
> A second launch asked to start hidden, with `--hidden` or as a start at login, asks the running
> Beamer nothing and ends with exit status 0, leaving its window as it was.
>
> - parity: Python's Mac asked the running Beamer to show its window from any second launch, hidden or not (one behaviour on both machines: the PC's, so a start at login never brings up a window)

r[app.instance.bounded]
A second launch whose request the running Beamer has not taken within 2 s of the launch finding it
running ends without it, with exit status 0.

> r[app.instance.request]
> The running Beamer shows its window only for the request a second launch sends. Anything else it
> receives where it listens for second launches, bytes that are not a whole request included,
> changes nothing.
>
> - fuzz: instance-request
> - parity: Python's PC showed its window for any line it received there, readable or not (one behaviour on both machines: the Mac's, which acted only on its own request)

r[app.instance.relaunch-waits]
A launch with `--relaunched` that finds a Beamer running waits for that Beamer to end, and starts
within 0.2 s of its ending, as the Beamer it replaces.

r[app.instance.relaunch-gives-up]
When the running Beamer has not ended 15 s after a launch with `--relaunched` found it, that launch
does as a second launch does (`app.instance.show`, `app.instance.hidden`).

## OS floor

The lowest OS versions Beamer runs on, #17: macOS 14.2, and Windows 11 build 22000.

> r[app.floor.mac]
> Beamer's Mac binary is built with macOS 14.2 as its deployment target, matching the lowest macOS
> Beamer.app declares (`platform.install.mac-info`), so that macOS refuses to start it on an older
> version.
>
> - parity: Python's app was built for macOS 13.0 (#17: macOS 14.2 is the floor)

> r[app.floor.mac-arm64]
> Beamer's Mac binary contains arm64 code only.
>
> - parity: Python's app was universal2, with an Intel slice (#17: arm64 only, since macOS 27 excludes Intel Macs)

r[app.floor.pc-x64]
`Beamer.exe` contains x86_64 code only.

> r[app.floor.pc]
> On a PC running a Windows build below 22000, a launch shows the floor message (`spec/viewmodel.md`)
> in a message box and ends with a non-zero exit status before it does anything else: no instance
> check, no logon task, no file read or written.
>
> - parity: Python's PC started on any Windows (#17: below the floor Beamer refuses to start and says why)

r[app.floor.pc-commands]
On a PC running a Windows build below 22000, every command line but a launch (`install`,
`uninstall`, `quit`, `--watchdog` and `--elevated`) writes the floor message to standard error and
ends with a non-zero exit status, having changed nothing.

## Watchdog

ADR 0004 (amended) and #21: a second process of the same binary that runs no Beamer logic and only
relaunches the app after a crash or a frozen engine.

r[app.watchdog.started]
When Beamer's app starts, it starts its watchdog: the same binary run as `beamer --watchdog`,
joined to the app by a pipe. Only a launch that goes on to run Beamer's app starts one: a second
launch, a PC launch that runs the logon task and ends (`platform.task.launch`), and every other
command line of `app.command.forms` start none.

r[app.watchdog.by-hand]
`beamer --watchdog` started without the pipe of the app that starts it ends at once with a non-zero
exit status, and launches nothing.

r[app.watchdog.no-logic]
The watchdog shows no window, tray icon or Dock icon, opens no network socket, captures and injects
no input, and reads no data file.

> r[app.watchdog.pipe]
> The watchdog reads its pipe as lines: `beat` is the engine's heartbeat
> (`runtime.heartbeat.every-second`), and `bye` says the app is quitting. Any other line, and a last
> line cut short, are ignored.
>
> - fuzz: watchdog-pipe

r[app.watchdog.bye]
On Quit, the app writes `bye` to the pipe, then ends with exit status 0. A watchdog that has read
`bye` ends when the pipe ends, and relaunches nothing.

> r[app.watchdog.crash]
> Any end of the pipe that `bye` did not come before is a crash: a panic (`runtime.crash.*`), a kill,
> Force Quit and End task included. After a crash, the watchdog waits as `app.watchdog.backoff`
> says, relaunches the app, and ends.
>
> - parity: Python had no crash restart: a Beamer that crashed stayed down (ADR 0004, amended by #21: a watchdog relaunches the app)

r[app.watchdog.crash-logged]
The watchdog adds one line to Beamer's log (`runtime.log.path`) for each crash, saying it was a
crash and, on a PC, giving the app's exit code.

> r[app.watchdog.relaunch-mac]
> On a Mac, the watchdog relaunches Beamer.app through LaunchServices, as a launch from Finder is.
>
> - tier: machine (only a real Mac opens an installed app through LaunchServices)

> r[app.watchdog.relaunch-pc]
> On a PC, the watchdog relaunches `Beamer.exe` at the crashed app's path with the watchdog's own
> token, so the relaunched Beamer runs with the crashed one's rights, and no UAC prompt is shown.
>
> - tier: machine (only a real PC starts a process with an elevated token kept from the logon task)

r[app.watchdog.relaunch-arguments]
The watchdog relaunches the app with the crashed app's own command-line arguments, `--relaunched`
and the watchdog's record of recent crashes, each once, and `--hidden` when the crashed app had
started hidden, a start at login included: a Beamer started hidden is relaunched hidden, and one the
logon task started is relaunched as one the logon task started.

r[app.watchdog.backoff]
The watchdog waits before it relaunches the app: 1 s after the first crash of a streak, then 2, 4, 8
and 16 s after the second to fifth, and 30 s after each crash of the streak after those.

r[app.watchdog.streak]
A run of 60 s or more of the app ends a streak of crashes: the next crash is the first of a new
streak.

r[app.watchdog.give-up]
A crash that is the fifth within 10 minutes is not relaunched: the watchdog adds a line to Beamer's
log saying it stopped relaunching, and ends. Beamer then runs again only from the next start at
login or launch by hand, which starts with no record of crashes.

r[app.watchdog.restarted]
A Beamer the watchdog relaunched raises the restarted notification (`spec/viewmodel.md`) once it has
started. A Beamer started otherwise raises none.

> r[app.watchdog.frozen]
> When 10 s pass with no `beat` on the pipe, counted from the watchdog's start for the first, the
> watchdog kills the app and relaunches it as after a crash, which this counts as
> (`app.watchdog.backoff`, `app.watchdog.give-up`). Only beats count: a window that stops answering
> while beats come kills nothing.
>
> - parity: Python never noticed a frozen Beamer, which could swallow this machine's keyboard and pointer for good (#21: a frozen engine with a stale capture word is killed and relaunched)

r[app.watchdog.sleep]
Time the machine spends asleep does not count toward the 10 s of `app.watchdog.frozen`: after a
wake, the watchdog waits 10 s more from the wake for a `beat` before it kills the app.

r[app.watchdog.replaced]
When the watchdog ends while the app runs, the app starts a new watchdog within 2 s.

## Quit

r[app.quit.ends]
Quit ends Beamer's app with exit status 0, and its watchdog with it (`app.watchdog.bye`): once Quit
is done, no Beamer process runs. Quit comes, among others, from the tray menu's "Quit Beamer", from
closing the window as `viewmodel.tray.close` says, from `beamer quit`, from a signal
(`app.quit.signal`) and from the end of the login session (`app.quit.session-end`).

> r[app.quit.signal]
> A SIGTERM or SIGINT that Beamer's app receives quits it as Quit does, once, however many arrive.
>
> - parity: Python's Mac app ended at once on SIGTERM, without quitting, while its PC and Linux apps quit through Quit (one behaviour for every app: the PC app's, so a `kill` is a Quit and not a crash the watchdog relaunches)

> r[app.quit.session-end]
> Logging out of the login session, restarting or shutting down the machine quits Beamer as Quit
> does, so the watchdog relaunches nothing.
>
> - tier: machine (only a real machine ends a login session)

> r[app.quit.kill-both]
> Ending the app and the watchdog together, with `killall Beamer` or `killall -KILL Beamer` on a Mac,
> or `taskkill /F /IM Beamer.exe` on a PC, leaves no Beamer running: nothing relaunches it.
>
> - tier: machine (only a real install relaunches through LaunchServices or the logon task's token)

## beamer quit

r[app.quit-command.asks]
`beamer quit` asks the running Beamer of its login session to Quit, also one running with
administrator rights, waits until that Beamer and its watchdog have ended, and ends with exit
status 0.

r[app.quit-command.none-running]
With no Beamer running in its login session, `beamer quit` ends with exit status 0 within 1 s. It
never starts Beamer.

r[app.quit-command.timeout]
When the running Beamer and its watchdog have not both ended 10 s after `beamer quit` asked, it
writes so to standard error and ends with a non-zero exit status.

## Install

#21: the binary installs itself, with no installer package. The dev script copies the files into
place and runs `beamer install`.

> r[app.install.steps]
> `beamer install` makes this machine's install as its steps' rules say: on a Mac, Start at login
> (`platform.login.default-on`) and the virtual microphone (`spec/audio.md`); on a PC, the logon task
> (`platform.install.task`) and the elevated setup (`platform.install.one-setup`): the firewall rule
> and the virtual camera (`spec/camera.md`). It starts no Beamer.
>
> - parity: Python was installed by an Inno Setup installer and its PowerShell scripts on a PC, and from a disk image on a Mac (#21: the binary installs itself)

r[app.install.installed-copy]
`beamer install` runs only from the installed copy (`platform.install.pc-path`,
`platform.install.mac-path`). Run from anywhere else, it writes the installed path to standard error,
installs nothing and ends with a non-zero exit status.

r[app.install.unchanged]
When every install step is already as its rule says, `beamer install` changes nothing and asks for
no administrator permission: no UAC prompt and no administrator password prompt.

r[app.install.elevated-once]
On a PC, `beamer install` runs `beamer --elevated setup` at most once, and only when one of its
steps differs (`platform.install.check`). On a Mac, it runs `beamer --elevated install-driver` only
when the virtual microphone in Beamer.app differs from the installed one.

r[app.install.failed]
When an install step fails, or administrator permission for it is declined, `beamer install` writes
which step to standard error and ends with a non-zero exit status. The steps it finished stay as
they are, and running `beamer install` again does only what is left.

r[app.install.done]
`beamer install` ends with exit status 0 when every install step is as its rule says.

## Uninstall

r[app.uninstall.quits-first]
`beamer uninstall` first quits Beamer as `beamer quit` does. When Beamer has not ended
(`app.quit-command.timeout`), uninstall removes nothing and ends with a non-zero exit status.

r[app.uninstall.removes]
`beamer uninstall` then removes everything install added, as `platform.install.uninstall` and the
audio and camera install steps say, running `beamer --elevated remove` once for what needs
administrator rights. It keeps the data folder and the identity key
(`platform.install.uninstall-keeps`).

r[app.uninstall.purge]
`beamer uninstall --purge` also removes the data folder and the identity key
(`platform.install.purge`).

r[app.uninstall.own-files]
On a PC, `beamer uninstall` run from the installed copy leaves none of the installed files within
10 s of its end, `Beamer.exe` included.

r[app.uninstall.failed]
When something install added could not be removed, or administrator permission for it is declined,
`beamer uninstall` writes what is left to standard error and ends with a non-zero exit status;
running it again removes what is left.

r[app.uninstall.done]
`beamer uninstall` ends with exit status 0 when it has removed everything install added, also when
nothing was installed.

## Elevated steps

ADR 0004: every step that needs administrator rights is an `--elevated` command line of the same
binary. GameInput recovery (F40), Later in #11, will be a step of the elevated setup, the room
`platform.install.one-setup` names, so it needs no new elevated step.

> r[app.elevated.steps]
> `beamer --elevated` takes exactly one of these steps. On a PC: `setup`, the elevated setup
> (`platform.install.one-setup`); `remove`, what uninstall removes with administrator rights;
> `admin-mode on` and `admin-mode off`, the logon task's run level for admin mode
> (`platform.admin.change`); and `start-at-login on` and `start-at-login off`, Start at login while
> admin mode is on (`platform.login.pc-elevated-change`). On a Mac: `install-driver`, which installs
> the virtual microphone (`spec/audio.md`), and `remove`.
>
> - parity: Python's PC ran its elevated changes as `--set-admin-mode on|off`, with an optional `--config` and the settings file's absolute path and an optional `--show`, and `--set-admin-startup on|off`, and its install and repair through a PowerShell script (ADR 0004: steps that need elevation are `--elevated` subcommands of the same binary; #21 names `setup` and `install-driver`)

> r[app.elevated.fixed]
> An `--elevated` command line with any other step, any other value, or any argument more, such as a
> path, an account or an option, ends with a non-zero exit status before it changes anything.
>
> - fuzz: command-line

> r[app.elevated.own-install]
> An elevated step acts on Beamer at the installed path of the account it runs as, whatever
> characters that path has (`platform.task.any-path`), and never on a path or an account given to
> it.
>
> - parity: Python's PC handed its elevated script the path of `Beamer.exe` and the signed-in account's SID, and refused a path holding a double quote before starting it (#21: the binary's own elevated steps find the installed path themselves)

r[app.elevated.other-account]
On a PC, an elevated step run as an account other than the one Windows names as signed in to its
session, as when another administrator approved its UAC prompt, changes nothing and ends with a
non-zero exit status (`platform.login.pc-account`).

r[app.elevated.needs-rights]
An elevated step run without administrator rights changes nothing and ends with a non-zero exit
status.

r[app.elevated.no-prompt]
Beamer, `beamer install` and `beamer uninstall` run an elevated step with no prompt when they already
run with administrator rights, as over SSH on a PC, and on a Mac when sudo runs the step without a
password, as #21's dev-only `beamer-hal` rule lets it for `install-driver`.

> r[app.elevated.prompt]
> When they cannot run an elevated step without a prompt (`app.elevated.no-prompt`), Beamer,
> `beamer install` and `beamer uninstall` run it through one UAC prompt on a PC, or one macOS
> administrator password prompt on a Mac, naming Beamer.
>
> - tier: human (a UAC or administrator password prompt is seen at the desk)
> - do: On the PC, signed in at the desk without an elevated shell, with something in the elevated setup differing (delete the "Beamer (UDP-In)" firewall rule), run `beamer install`; on the Mac, with the installed virtual microphone removed, run `beamer install`
> - see: Exactly one UAC prompt on the PC, and exactly one administrator password prompt on the Mac, each naming Beamer, and after approving it the install ends without another prompt
