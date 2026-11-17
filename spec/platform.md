# Platform

What a machine's operating system lets Beamer do, and how each part of it reports whether it can:
the capability contract, the Mac's permissions (F38), Start at login (F26), the PC's logon task and
admin mode (F24), the PC's firewall rule and its check (F25), the input blocker diagnostics (F40),
notification delivery (F36), trackpad haptics (F32), process tuning (F41), the identity key store,
power and wake, and the install steps that #21 decided. All of these ship (#11). GameInput recovery
(F40) is Later: `platform.install.one-setup` names the room left for it.

The rules come from the decisions #16 (the platform seam, ADR 0004), #21 (packaging, signing,
install and restart, ADR 0004 amended) and #8 (what Linux forces on the seam), and from the Python
app at `archive/python` (`win_app/firewall_win.py`, `win_app/autostart_win.py`,
`win_app/admin_mode.py`, `win_app/input_blocker.py`, `win_app/input_injector.py`,
`win_app/desktop_win.py`, `win_app/kvm_bridge_win.py`, `mac_app/login_item.py`,
`mac_app/notices.py`, `mac_app/kvm_bridge_app.py` and the Windows installer scripts), mined through
its tests in `spec/ledger/platform.txt`.

What stays elsewhere: what capture and injection do with input is `spec/input.md`'s; the UDP port
and the link are `spec/link.md`'s; where the identity key pair comes from, and that the Keychain or
a DPAPI file keeps it, is `spec/pairing.md`'s (`pairing.identity.*`), and only what the key store
adds to those rules is here; when a wake happens and what its packet carries is `spec/discovery.md`'s
(`discovery.wake.*`); the virtual microphone's and the virtual camera's install steps are
`spec/audio.md`'s and `spec/camera.md`'s; the `--elevated` command line, `beamer install`,
`beamer uninstall`, `beamer quit` and the watchdog are `spec/app.md`'s; persistence and logging are
`spec/runtime.md`'s; and what every page, tray menu and notification says is `spec/viewmodel.md`'s.
The capabilities here are #16's fourteen, one trait each in `beamer-platform`; admin mode is
reported through Start at login's (`platform.capability.admin-mode`). They report typed values: a notification's words, and the words that explain a
firewall check or an input blocker, are the view-model's (#16).

Dropped, so no rule here (#11): the portable build (F23), Windows unlock through the RoomDeck
credential provider and the lock screen (F21), US spelling at run time (F34), and the Linux
code for Start at login, permissions and portals (#1). The Python installer (Inno Setup and its PowerShell
maintenance scripts) is replaced by `beamer install` (#21).

A Mac is a machine running macOS, and a PC one running Windows. Every rule is `ci` unless it says
otherwise. Translation code (OS answers to capability states, task and rule definitions) is `ci` on
a hosted runner of its own OS (#15); a rule that only the real OS can show is `machine`.

## Capabilities

r[platform.capability.states]
Each capability of a machine reports exactly one of four states at a time: ready, needs
permission, refused, or unsupported here with its reason. The capabilities are the fourteen #16
lists: input capture, input injection, displays and the pointer, the clipboard, audio devices,
camera devices, display switching, gestures and haptics, the identity key store, Start at login,
the firewall, power and wake, notifications, and permissions.

r[platform.capability.admin-mode]
Admin mode is no capability of its own. On a PC, Start at login's capability, which keeps the
logon task whose run level admin mode is, reports admin mode beside its own state: whether it is
on, and one of the same four states. Where a rule says that admin mode reports a state, it is this
report.

> r[platform.capability.never-waits]
> Asking a capability for something (a firewall check, a change to Start at login, a permission
> prompt) never makes the rest of Beamer wait on the OS: its outcome is reported once the OS answers.
> While one such OS call takes 2 s or more, input goes on being captured and injected, and links go
> on being answered.

> r[platform.capability.error-is-state]
> When the OS refuses or fails a capability's call, the capability reports the state that follows
> from it, and the log gets the OS's own message and code, whatever the language Windows or macOS
> displays. Beamer goes on running, and no other capability's state changes for it.
>
> - parity: Python raised Windows' or macOS's own words as the error the window showed (#16: platform crates report typed values; the words a user reads are the view-model's)

> r[platform.capability.unanswered]
> A permission request may stay unanswered for any length of time. Until it is answered, the
> capability stays at needs permission, and nothing else in Beamer waits on the answer.

> r[platform.capability.unsupported-os]
> On a Mac, the firewall reports unsupported here, and so does admin mode
> (`platform.capability.admin-mode`). On a PC, haptics report unsupported here.
>
> - parity: Python's firewall check, run anywhere but Windows, reported a check that failed (#16: a capability the OS has no part for reports unsupported here)

## Permissions

> r[platform.permissions.mac-input]
> On a Mac, input capture needs two permissions, Accessibility and Input Monitoring, granted
> separately. While either is not granted, input capture reports needs permission, naming each one
> that is missing.

> r[platform.permissions.silent-check]
> Checking whether a Mac's permissions are granted never shows a macOS prompt.
>
> - tier: machine (only a real Mac shows its permission prompts)

> r[platform.permissions.ask]
> Asking for Accessibility shows macOS's Accessibility prompt for Beamer, and asking for Input
> Monitoring shows macOS's Input Monitoring prompt. Each asks for its own permission only.
>
> - tier: human (a macOS permission prompt is seen on the Mac's display)
> - do: On the Mac, run `tccutil reset Accessibility uk.co.kalkman.beamer` and `tccutil reset ListenEvent uk.co.kalkman.beamer`, relaunch Beamer, then ask for Accessibility and then for Input Monitoring from Beamer's permissions page, granting each so that Beamer has both again
> - see: Each time, macOS shows the prompt for exactly that permission, naming Beamer

> r[platform.permissions.relaunch-needed]
> When both permissions are granted but were not both granted when this run of Beamer started,
> input capture reports needs permission, with a relaunch as the one step left: macOS passes keys
> only to a process started after the grant.

> r[platform.permissions.relaunch]
> Relaunching Beamer for its permissions quits the running Beamer, then opens Beamer again through
> LaunchServices once the running one has ended.
>
> - tier: machine (only a real Mac relaunches an app through LaunchServices)

> r[platform.permissions.change-seen]
> A Mac permission granted or taken away in System Settings while Beamer runs is reported within
> 0.5 s, without Beamer being relaunched.
>
> - tier: machine (only a real Mac changes a permission in System Settings)

## Start at login

> r[platform.login.default-on]
> A fresh install turns Start at login on, on a Mac and on a PC. Reinstalling over an earlier
> install keeps the choice the user made.
>
> - parity: Python left Start at login off on both: its Windows installer unless the user ticked it, and its Mac app until the user turned on its switch, the one place that registered the login item (#21: Start at login is on by default on both OSes)

> r[platform.login.mac-login-item]
> On a Mac, Start at login is Beamer.app itself registered with macOS as a login item, which Login
> Items in System Settings lists under Beamer.
>
> - tier: machine (only a real Mac keeps login items)

> r[platform.login.mac-state]
> On a Mac, Start at login is on while macOS reports Beamer's login item enabled or waiting for
> the user's approval, and off while it is not registered or not found.

> r[platform.login.mac-approval]
> While Beamer's login item waits for the user's approval, Start at login reports needs permission,
> and the step it offers opens Login Items in System Settings.

> r[platform.login.mac-applications]
> On a Mac, Start at login can be turned on only by a Beamer.app in /Applications or
> ~/Applications, or a folder within either, that macOS has not moved to a translocated copy.
> Anywhere else, turning it on reports unsupported here, naming /Applications.

> r[platform.login.mac-moved]
> A Beamer.app outside /Applications and ~/Applications whose login item is on or waiting for
> approval can still turn Start at login off.

> r[platform.login.mac-off-idle]
> Turning Start at login off while it is already off changes nothing.

> r[platform.login.mac-hidden]
> A Mac launch that macOS reports as a login item's launch starts Beamer hidden, in the menu bar
> only. Any other launch, including one for which macOS gives no launch kind, shows Beamer's window.

r[platform.login.pc-trigger]
On a PC, Start at login is on exactly when Beamer's logon task is registered, is enabled, and has
an enabled logon trigger.

> r[platform.login.pc-toggle]
> On a PC with its logon task registered, turning Start at login on enables the task and its logon
> trigger, and turning it off disables the logon trigger: the task stays registered and enabled,
> with its run level and its action unchanged.
>
> - parity: Python deleted the task to turn Start at login off, and registered it again to turn it on (#21: one logon task, always registered, through which every launch goes)

r[platform.login.pc-register]
Turning Start at login on on a PC with no logon task registered registers it, as
`platform.task.definition` and `platform.task.run-level` say, with its logon trigger on.

> r[platform.login.pc-off-absent]
> Turning Start at login off on a PC with no logon task changes nothing.

> r[platform.login.pc-account]
> When Beamer runs as an account other than the one signed in to Windows, as when another
> administrator approved a UAC prompt for it, Start at login and admin mode report unsupported here,
> naming the signed-in account, and a request to change either changes nothing, turning Start at
> login off included.
>
> - parity: Python, run as another account, refused to turn Start at login on but turned it off, deleting the signed-in account's task (one rule for every change: a Beamer run as another account changes no task of the signed-in account's, `platform.task.foreign`)

> r[platform.login.pc-no-account]
> When Windows names no signed-in account, Start at login and admin mode report unsupported here.

> r[platform.login.pc-elevated-change]
> While admin mode is on and Beamer runs without administrator rights, a change to Start at login
> asks for administrator permission with one UAC prompt, and Beamer is not relaunched for it. When
> the prompt is declined, Start at login keeps its earlier state.

## The logon task

> r[platform.task.name]
> A PC's logon task is named `Beamer-` followed by the signed-in account as `DOMAIN\user`, with its
> backslash replaced by `-`: `Beamer-STUDIO-PC-alex` for `STUDIO-PC\alex`.

> r[platform.task.definition]
> The logon task starts `Beamer.exe` at its installed path with the arguments
> `--logon-task $(Arg0)`, in its own folder, as the signed-in account with its interactive token, at
> priority 4. It is enabled, and has one logon trigger, for that account. It ignores a new start
> while it runs, neither stops nor refuses to start on battery, has no time limit, and never
> restarts Beamer on failure.
>
> - parity: Python's task ran Beamer with the argument `--hidden` at priority 7, a below-normal process (#21: priority 4, and every launch runs the task, so the task's arguments carry what the launch asked for, `platform.task.launch-options`)

> r[platform.task.run-level]
> The logon task's run level is the highest the account has while admin mode is on, and least
> privilege while it is off. A task with no run level counts as least privilege.

> r[platform.task.any-path]
> The logon task starts Beamer at any installed path and for any account name, including ones with
> `&`, apostrophes or characters outside ASCII, such as `C:\A & B\` or `STUDIO-PC\Élodie`.

r[platform.task.unchanged]
Registering the logon task when the registered task is already as `platform.task.definition` and
`platform.task.run-level` say, whether its logon trigger is on or off, changes nothing, so a second
install or a second registration writes nothing.

> r[platform.task.rewrite]
> When the registered logon task differs from what `platform.task.definition` and
> `platform.task.run-level` say, other than in whether its logon trigger is on, registering it
> rewrites it as they say and keeps Start at login as `platform.login.pc-trigger` read it: the
> rewritten task is enabled, and its logon trigger is on exactly when Start at login was on.
>
> - parity: Python's installer changed only a task's run level, keeping its arguments, whether the task was enabled and its triggers as the user left them, and left a task whose run level already matched as it was (#21: every launch runs the logon task, so it must be enabled and start Beamer as `platform.task.definition` says; Start at login is the one choice in the task that is the user's)

> r[platform.task.foreign]
> Beamer never changes or deletes a task that starts a program other than `Beamer.exe` at its
> installed path (one named by a relative path included), runs as another account, runs with a
> logon type other than the interactive token, or has more than one action or principal, even one
> with the logon task's name. An install, or a change to Start at login or admin mode, that would
> need to reports refused, naming the task; uninstall leaves it as it is.
>
> - parity: Python's switch for Start at login overwrote and deleted a task with the logon task's name whatever it started, and its installer passed over such a task without a word; only its admin mode refused (one rule for every change to the logon task, as Python's admin mode had it)

> r[platform.task.no-migration]
> Beamer reads, changes and deletes no task but its own logon task: a task an earlier Beamer
> registered under another name, such as the global `Beamer` task, is left as it is.
>
> - parity: Python moved the global `Beamer` task of 1.5.1 to the per-account task, keeping its arguments and whether it was on, and deleted it, also on uninstall (#21: no compatibility code goes into the app; `dev/cutover.sh` removes the Python app's leftovers once)

> r[platform.task.launch]
> On a PC, a launch of Beamer's app that does not come from its logon task (its first argument is
> not `--logon-task`) runs the logon task (`platform.task.launch-options`) and ends, so Beamer
> always runs at admin mode's level and no UAC prompt is shown at launch. A launch from the task
> never shows a UAC prompt. The commands of `spec/app.md`, such as `beamer install` or
> `beamer --elevated setup`, are not launches of the app.
>
> - parity: Python, launched by hand without administrator rights while admin mode was on, asked for them with a UAC prompt and relaunched itself (#21: every launch that does not come from the task runs the task and exits)

r[platform.task.launch-visible]
A Beamer that the logon task starts with `$(Arg0)` as written, which is how Task Scheduler leaves
it when the logon trigger starts the task (MS-TSCH 2.5.9.2: a parameter given no value is not
replaced), starts hidden. One that it starts with `--launched` shows its window, unless the options
after `--launched` include `--hidden`.

r[platform.task.launch-options]
Whatever runs the logon task (a launch, or a change to admin mode) gives Task Scheduler one value
for `$(Arg0)`: `--launched`, then the options for the Beamer it starts, which for a launch are that
launch's own command-line options. Each option is quoted so that the Beamer the task starts
receives it as the same argument, with its spaces, quotes and characters outside ASCII.

> r[platform.task.launch-failed]
> When the logon task is missing, or Windows refuses to run it, a launch that does not come from
> the task goes on by itself with the rights it was started with, and shows no UAC prompt. While
> admin mode is on and that Beamer runs without administrator rights, admin mode reports refused for
> as long as it runs; otherwise nothing is reported for it. Task Scheduler passing over the run
> because the task's Beamer already runs (`platform.task.definition`) is no refusal: the launch
> ends, and what a launch does while Beamer runs is `spec/app.md`'s.

## Admin mode

r[platform.admin.opt-in]
On a PC, admin mode is off until the user turns it on.

r[platform.admin.local]
Admin mode belongs to the signed-in Windows account on this PC: it is never sent to a peer.

> r[platform.admin.kept]
> Admin mode stays as the user chose it across restarts of Beamer.
>
> - parity: Python kept the choice in the registry, under `HKCU\Software\KalkmanCode\Beamer` (#16: it lives in `local.json`; the fresh start reads nothing of the Python app's, so an earlier choice is not carried over)

> r[platform.admin.installed]
> Admin mode can be turned on only by Beamer running from its installed path. Run from anywhere
> else, admin mode reports unsupported here, naming the install.

> r[platform.admin.administrator]
> On a standard Windows account, admin mode reports unsupported here, naming an administrator account
> as what it needs.

> r[platform.admin.uac]
> On an administrator account whose rights are not split by User Account Control, admin mode
> reports unsupported here, naming User Account Control as what it needs.

> r[platform.admin.unchecked]
> When Windows does not let Beamer read the account's rights, admin mode reports unsupported here,
> saying they could not be checked.

> r[platform.admin.change]
> Turning admin mode on or off asks for administrator permission with one UAC prompt. Once it is
> given, Beamer sets the logon task's run level for the new choice, changing nothing else in the
> task, and records the choice. The running Beamer then quits as Quit does (`spec/app.md`), and once
> it has ended, the logon task is run with `--launched` (`platform.task.launch-options`), starting
> Beamer at the new level with its window shown.
>
> - parity: Python started the new Beamer itself, elevated directly and unelevated through Explorer's desktop window, and quit the running one once it had started (#21: every launch goes through the logon task, which ignores a new start while the running Beamer is its own, so the running one ends first)

> r[platform.admin.same]
> Choosing the state admin mode already has asks for nothing and relaunches nothing.

> r[platform.admin.cancelled]
> When the UAC prompt for admin mode is declined, admin mode keeps its earlier state, the logon task
> is unchanged, and the change reports that it was cancelled.

> r[platform.admin.task-refused]
> When Windows refuses the change to the logon task, admin mode keeps its earlier state, the
> choice recorded is unchanged, and the running Beamer is not replaced.

> r[platform.admin.rollback]
> When the logon task cannot start Beamer at the new level (Windows refuses to run it, or reports
> that the run did not start Beamer), the task's run level and the choice recorded are restored to
> what they were before the change, with no second UAC prompt; a task that the change registered
> (`platform.admin.no-task`) stays registered, with its logon trigger off. The logon task is then
> run again, starting Beamer at the earlier level with its window shown, given an option that makes
> it report that the change failed. Beamer tries no other means of starting itself: when that run
> fails too, the failure is logged, and no Beamer runs until the next launch.
>
> - parity: Python restored the task and the choice while the running Beamer still ran, which then reported the failure, and deleted a task that the change had registered (#21: the running Beamer has ended before the logon task can start the new one, `platform.admin.change`, and the one logon task stays registered)

> r[platform.admin.keeps-login]
> Changing admin mode never changes Start at login: a logon trigger that was off stays off.

r[platform.admin.no-task]
When no logon task is registered, a change to admin mode registers it, as `platform.task.definition`
says, with its logon trigger off.

## Firewall

> r[platform.firewall.rule]
> On a PC, Beamer's firewall rule is one enabled inbound allow rule named "Beamer (UDP-In)", for UDP
> port 24820 (`link.transport.port`), for `Beamer.exe` at its installed path, on the Private and
> Domain profiles.
>
> - parity: Python wrote three rules, TCP on the link port and UDP and TCP on the pairing port 24821, on the Private profile only (#21: one inbound rule for UDP 24820 on the Private and Domain profiles; #14: one UDP port for links, pairing and beacons)

> r[platform.firewall.never-public]
> Beamer never writes a firewall rule for the Public profile, and never changes the profile of a
> network.

> r[platform.firewall.check-times]
> A PC checks its firewall when Beamer starts and each time the user asks to check again. A check
> changes no firewall rule and no network's profile, even when Beamer runs with administrator
> rights.

> r[platform.firewall.counts]
> A firewall rule counts for Beamer when it is enabled and inbound, its program is `Beamer.exe`'s
> installed path, its protocol is UDP or any, and its local ports include 24820: the port itself, a
> range that contains it, or any. Paths compare with environment variables expanded, ignoring case and
> taking `/` for `\`.

> r[platform.firewall.profiles]
> A firewall rule for any profile covers every profile. A domain-authenticated network is on the
> Domain profile.

r[platform.firewall.lan]
The LAN is the networks Windows reports with the best connectivity of any, Internet before local
network before subnet; when no network has any of these, the LAN is every network. Coverage is
judged on the profiles the LAN is on: a network outside the LAN, such as a Hyper-V switch with no
traffic beside a Wi-Fi with Internet, plays no part in it. Block rules are judged on every connected
network (`platform.firewall.blocked`).

> r[platform.firewall.outcome]
> A firewall check reports the first of these that holds: the check failed, or was refused for lack
> of permission (`platform.firewall.check-failed`, `platform.firewall.check-permission`); a block
> rule blocks (`platform.firewall.blocked`); the firewall is ready (`platform.firewall.ready`);
> Beamer's rule is missing (`platform.firewall.missing`); the LAN's network is public
> (`platform.firewall.network-public`).
>
> - parity: Python checked for a public network before a missing rule, so a LAN on Public and on Private, with an allow rule only for Public, was named public although its Private network had no rule (#21: Repair covers every profile but Public, so it comes first; only the user can change a Public network)

r[platform.firewall.ready]
The firewall is ready when at least one allow rule counts and each profile the LAN is on is covered
by an allow rule that counts.

> r[platform.firewall.blocked]
> A block rule that counts blocks when it is on a profile some connected network is on, or on any
> profile when no network is connected. The firewall then reports needs permission, naming a block
> rule as the cause, whatever allow rules there are, and offers Repair, which deletes it
> (`platform.firewall.setup`). A block rule only on profiles no connected network is on blocks
> nothing, and a disabled one counts for nothing (`platform.firewall.counts`).
>
> - parity: Python told the user to ask an administrator to review Beamer's block rules, and offered only checking again (#21: the elevated setup deletes the block rules Windows writes when its prompt is dismissed)

> r[platform.firewall.missing]
> When no allow rule counts, or a profile the LAN is on other than Public is covered by no allow
> rule that counts, the firewall reports needs permission, naming Beamer's rule as missing, and
> offers Repair.
>
> - parity: Python told the user to run the Beamer installer again, and on a domain network to ask an administrator for Domain-profile rules (#21: the app's Repair runs the elevated setup, whose rule covers the Domain profile too)

r[platform.firewall.network-public]
When the LAN is on the Public profile and no allow rule that counts covers it, the firewall reports
refused, naming each network of the LAN on the Public profile and its interface, so that the user
can make it Private in Windows Settings. It offers no Repair for it.

> r[platform.firewall.port]
> The check is always for UDP port 24820.
>
> - parity: Python checked the link port the user chose, and on a port other than 24820 told the user to ask an administrator to allow it (#14: every machine takes links on UDP 24820, `link.transport.port`)

> r[platform.firewall.check-failed]
> When Windows cannot be asked, or its answer cannot be read, the firewall reports refused, naming
> a check that failed, and offers to check again. A check that failed never reports a rule missing
> or a block rule, and the OS's words reach only the log.

r[platform.firewall.check-permission]
When Windows refuses the check for lack of permission, told by its access-denied error code
whatever the language Windows displays, while Beamer runs without administrator rights, the
firewall reports needs permission, naming the check, and offers checking again. It offers no
Repair, which leaves the rights the check runs with as they were. The same refusal while Beamer runs
with administrator rights, and any other refusal, whatever words it carries, is a check that failed
(`platform.firewall.check-failed`).

> r[platform.firewall.repair]
> Repair runs Beamer's elevated setup, which asks for administrator permission with one UAC prompt
> at the desk, then checks the firewall again.
>
> - parity: Python's app never changed the firewall, and told the user to run the installer again (#21: the app's Repair calls the same elevated setup as install)

> r[platform.firewall.setup]
> The elevated setup deletes every inbound block rule whose program is `Beamer.exe` at its installed
> path, the ones Windows writes when its "allow access" prompt is dismissed included, and any rule
> named "Beamer (UDP-In)" for that program that differs from `platform.firewall.rule`, then writes
> that rule when it is missing.
>
> - parity: Python's app never changed the firewall; its installer's repair deleted only its own three named rules whose program was exactly the installed path, wrote them again, and left block rules for an administrator to review (#21: setup deletes the block rules Windows writes when its prompt is dismissed)

> r[platform.firewall.setup-unchanged]
> When Beamer's rule is as `platform.firewall.rule` says and no block rule for `Beamer.exe` at its
> installed path exists, the elevated setup changes no firewall rule.

r[platform.firewall.setup-scope]
The elevated setup and uninstall delete only rules whose program is `Beamer.exe` at its installed
path: never another program's rule, one named "Beamer (UDP-In)" included, and never every rule.

> r[platform.firewall.setup-gone]
> A rule already gone when the elevated setup deletes it does not stop the setup, which still
> writes Beamer's rule. Any other failure to delete a rule ends the setup before it writes anything,
> and reports it.

> r[platform.firewall.any-path]
> A `Beamer.exe` installed at a path with apostrophes or characters outside ASCII gets a rule for
> exactly that path, and the check finds it.

## Input blocker

> r[platform.blocker.reported]
> When Windows refuses an injection or the placing of the pointer on a PC, the PC reports an input
> problem naming the input blocker: the foreground window's process file name without its folder,
> its window class, whether its process runs as administrator, and whether it runs at system
> integrity, each as far as Windows let Beamer read it.

> r[platform.blocker.kind]
> An input problem says the first kind that is true of its input blocker: GameInput's system window
> (window class `GameInputServiceWindow`); a system-level window (system integrity or above); a
> window running as administrator while Beamer runs without administrator rights; a window whose
> process Beamer was not allowed to inspect; an ordinary window.

> r[platform.blocker.name]
> An input problem names its input blocker by its process file name, by its window class when the
> name could not be read, and as the foreground window when neither could.

> r[platform.blocker.no-guess]
> An input blocker is called administrator or system-level only when Windows confirmed it: one
> whose rights Beamer was not allowed to read is neither, and a confirmed system integrity stands
> when its elevation could not be read.

r[platform.blocker.no-foreground]
With no foreground window, Beamer inspects no process, and the input problem is of the ordinary
kind, naming the foreground window (`platform.blocker.name`). With a foreground window whose process
Windows does not name, Beamer opens no process, keeps the window's class, and counts the window as
one whose process Beamer was not allowed to inspect (`platform.blocker.kind`).

> r[platform.blocker.log]
> The PC logs one warning for a refusal when the input blocker differs from the last refusal's, or
> when an injection or placement succeeded since: the operation refused, Windows' error code from
> that refusal or none, and the input blocker's name, process id, window class, rights and
> integrity. The error code is never one left from an earlier call.
>
> - parity: Python logged a refused injection again only for a different blocker, a success between them notwithstanding (one reading for both: the pointer placement's)

r[platform.blocker.notify]
An input problem raises a notification, unless one was already raised in this run for the same input
blocker window, a window being its process id, handle and class, or one was raised for any input
problem in the last 30 s. A later success does not let a window's notification come again.

> r[platform.blocker.success]
> An injection or a pointer placement that Windows accepts reports no input problem.

> r[platform.blocker.gameinput-front]
> On a driven PC, when a pointer move from the driver finds GameInput's system window in front, the
> PC reports an input problem for it at once, before any injection is refused. Another window in
> front reports nothing.

> r[platform.blocker.placed]
> A pointer placement reports the pointer placed only when Windows accepted it: a refused placement
> reports it not placed.

> r[platform.blocker.no-recovery]
> A PC never restarts GameInput's service itself: with GameInput's system window in front, Beamer
> reports the input problem and goes on injecting the driver's input as Windows lets it.
>
> - parity: Python ran the installer's `Beamer-GameInputRecovery` task, at most once a minute, to restart GameInput's service, and placed the pointer again once GameInput's window had left the front (#11: GameInput recovery is Later; only the diagnostics ship)

## Notifications

> r[platform.notify.mac-centre]
> On a Mac, a notification shows through macOS's notification centre as Beamer's, with its title,
> its body and the default sound, delivered at once.
>
> - tier: machine (only a real Mac's notification centre shows what it delivered)

> r[platform.notify.each]
> Each notification is a notification of its own: a second one never replaces an earlier one still
> shown.
>
> - tier: machine (only a real Mac's notification centre shows what it delivered)

> r[platform.notify.mac-ask]
> On a Mac, Beamer asks macOS for permission to show alerts and play sounds once in a run, before
> its first notification if nothing asked earlier.

> r[platform.notify.front]
> On a Mac, a notification shows as a banner and in Notification Centre's list, with its sound, also
> while Beamer is the app in front.
>
> - tier: machine (only a real Mac's notification centre shows what it delivered)

> r[platform.notify.undelivered]
> While macOS refuses Beamer's notifications, or cannot deliver them, notifications report refused,
> and a notification is logged with its title and body instead. Nothing else in Beamer fails for it.
>
> - parity: Python never noticed macOS refusing its notifications: it ignored the answer to its request for permission and posted with no completion handler, logging a notification only when posting it raised (#16: errors are values that set a capability's state)

> r[platform.notify.pc]
> On a PC, a notification shows as a Windows notification from Beamer, with its title and body.
>
> - tier: human (a Windows notification is seen on the PC's display)
> - do: On the PC, signed in to an administrator account (so that Task Manager runs as administrator), with Do not disturb off and admin mode off, open Task Manager so that it is in front; then, from the Mac, drive the PC, move the pointer over Task Manager and click
> - see: A Windows notification from Beamer, with a title and a body, naming `Taskmgr.exe` (`platform.blocker.notify`)

## Haptics

> r[platform.haptics.tick]
> On a Mac with a Force Touch trackpad, each tick a zone push reports
> (`crossing.method.report-tick`) gives one level-change click while haptics are on and set to feel
> that tick, felt under a finger resting on the trackpad.
>
> - tier: human (a haptic click is felt)
> - do: With haptics on and every tick felt (the defaults), rest a finger on the Mac's trackpad and push the pointer slowly into a zone until input crosses
> - see: Three single clicks under the finger as the push builds, before the crossing's own clicks

> r[platform.haptics.breakthrough]
> With haptics on, a zone push that crosses (`crossing.method.report-cross`) gives two level-change
> clicks 60 ms apart.
>
> - tier: human (a haptic click is felt)
> - do: With haptics on, rest a finger on the Mac's trackpad and push the pointer through a zone until input crosses
> - see: As input crosses, two clicks close together, unlike a single tick

r[platform.haptics.arrival]
With haptics on, input arriving home, through a zone or sent home by the driven machine
(`crossing.method.report-arrival`, `crossing.method.report-arrival-no-point`), gives the same two
clicks as a zone push that crosses (`platform.haptics.breakthrough`).

> r[platform.haptics.never-delays]
> Haptics never delay or stop a crossing: on a Mac with no Force Touch trackpad nothing is felt, and
> the crossing is as it would be with haptics off.

r[platform.haptics.failure]
When macOS fails a haptic, haptics report refused for the rest of the run, an error is logged with
what macOS raised, and no later haptic is tried.

## Process tuning

> r[platform.tuning.mac-nap]
> On a Mac, for as long as it runs, Beamer keeps a user-initiated, latency-critical activity, so
> macOS neither naps it nor coalesces its timers. The Mac may still sleep when idle.
>
> - tier: machine (only a real Mac lists the activities its processes keep)

> r[platform.tuning.pc-throttle]
> On a PC, when it starts and before it shows any window, Beamer opts out of Windows' power
> throttling of its execution speed, whatever the power plan.
>
> - tier: machine (only a real PC reports a process's power throttling)

r[platform.tuning.refused]
When the OS refuses one of process tuning's requests (`platform.tuning.mac-nap`,
`platform.tuning.pc-throttle`, `platform.tuning.pc-priority`, `platform.tuning.top-priority`),
Beamer logs a warning naming the request and goes on running at what the OS gave.

> r[platform.tuning.pc-priority]
> On a PC, Beamer runs in the above-normal priority class.
>
> - tier: machine (only a real PC reports a process's priority class)

> r[platform.tuning.top-priority]
> Capture and the engine run at the OS's top priority for interactive work: user-interactive on a
> Mac, time-critical on a PC.
>
> - tier: machine (only the real OS reports the priority Beamer runs at)
> - parity: Python's PC captured at the highest thread priority, below time-critical, and the rest of its work ran at normal priority; its Mac set no priority class for capture or any other work (#16: capture and the engine run at the top)

## Identity key store

> r[platform.key-store.mac]
> On a Mac, the Keychain that keeps the private key (`pairing.identity.keychain`) is the user's
> login Keychain: the key is one item there, readable by Beamer signed as "Beamer Dev", kept on this
> Mac only and never synced to another device.
>
> - tier: machine (the Keychain is reached only on a real Mac, by the signed app)

> r[platform.key-store.pc]
> On a PC, the DPAPI-protected file that keeps the private key (`pairing.identity.dpapi`) is in
> Beamer's data folder, `%LOCALAPPDATA%\Beamer\`, and no other Windows account can read the key
> from it.
>
> - tier: machine (DPAPI is reached only on a real Windows machine, under the user's account)

r[platform.key-store.unreadable]
When a stored private key exists but the OS will not hand it over (a locked Keychain, access
refused, DPAPI failing), the identity key store reports refused, Beamer makes no new key pair
(`pairing.identity.key-pair`), and it neither takes nor dials a link for the rest of the run. It
tries to read the key again at its next start, and at no other time.

> r[platform.key-store.not-a-key]
> A stored private key that is not a valid Ed25519 private key counts as lost
> (`pairing.identity.no-export`): Beamer logs it, and makes a new key pair.
>
> - fuzz: key-store

## Power and wake

> r[platform.power.sleep]
> A machine reports the OS going to sleep when the OS announces it, before the machine sleeps, and
> reports the wake within 1 s of the OS announcing it.
>
> - tier: machine (only a real machine sleeps and wakes)

r[platform.power.wake-packet]
A machine sends a wake's packet to each destination `discovery.wake.packet` names. When the OS
refuses the send to 255.255.255.255, power and wake reports it, and the wake ends as
`discovery.wake.send-failed` says. A send the OS refuses to any other destination does not end the
wake.

## Install steps

> r[platform.install.pc-path]
> On a PC, Beamer is installed for the signed-in account at `%LOCALAPPDATA%\Beamer\Beamer.exe`, with
> no installer package.
>
> - tier: machine (only a real PC has an installed Beamer)
> - parity: Python was installed by an Inno Setup installer and its PowerShell maintenance scripts (#21: the binary installs itself)

> r[platform.install.pc-manifest]
> `Beamer.exe` asks Windows for no elevation and no UIAccess when it launches, and declares
> per-monitor DPI awareness (version 2), Common Controls 6, the UTF-8 code page and long paths.

> r[platform.install.mac-path]
> On a Mac, Beamer is installed at `/Applications/Beamer.app`, with the bundle id
> `uk.co.kalkman.beamer`.
>
> - tier: machine (only a real Mac has an installed Beamer)

> r[platform.install.mac-signing]
> Beamer.app is signed as "Beamer Dev" with the hardened runtime and exactly two entitlements:
> audio input and camera.
>
> - tier: machine (only a real Mac's codesign reads an installed app's signature)
> - parity: Python's app also allowed unsigned executable memory and turned off library validation (#21: the Rust binary links everything statically but system frameworks)

> r[platform.install.mac-info]
> Beamer.app declares macOS 14.2 as its lowest macOS, and gives macOS its reasons for Local Network,
> Microphone, system audio capture and Camera access.
>
> - tier: machine (only a real Mac shows an installed app's declarations)
> - parity: Python's app declared macOS 13.0 as its lowest (#21: `LSMinimumSystemVersion` 14.2)

> r[platform.install.check]
> Each install step tells, without administrator rights, whether what it would write differs from
> what the machine has.

> r[platform.install.one-setup]
> On a PC, every install step that needs administrator rights is a step of Beamer's one elevated
> setup, which runs each of its steps only when that step differs: one UAC prompt at the desk, and
> none when nothing differs.
>
> - parity: Python's installer ran its maintenance script with administrator rights whenever it ran, and registered the GameInput recovery task there (#21: one elevated step that does nothing when nothing changed; GameInput recovery is Later, #11)

> r[platform.install.setup-starts-nothing]
> The elevated setup never starts Beamer and reads none of Beamer's settings: it runs its own fixed
> steps and ends.

r[platform.install.task]
On a PC, install registers the logon task (`platform.task.definition`), its logon trigger set as
`platform.login.default-on` says, and a task already registered is kept or rewritten as
`platform.task.unchanged` and `platform.task.rewrite` say.

r[platform.install.uninstall]
Uninstall removes Start at login (the Mac's login item, or the PC's logon task), Beamer's firewall
rule, and the installed files.

r[platform.install.uninstall-keeps]
Uninstall keeps Beamer's data folder and the identity key, so that a reinstall is still paired.

r[platform.install.purge]
Uninstall with `--purge` also removes Beamer's data folder and the identity key.

> r[platform.install.quit-keeps]
> Quitting Beamer leaves every install step as it is: Start at login and the firewall rule stay.
