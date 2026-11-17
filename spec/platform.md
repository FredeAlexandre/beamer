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
and the link are `spec/link.md`'s; where the identity key pair comes from is `spec/pairing.md`'s
(`pairing.identity.*`); when a wake happens and what its packet carries is `spec/discovery.md`'s
(`discovery.wake.*`); the virtual microphone's and the virtual camera's install steps are
`spec/audio.md`'s and `spec/camera.md`'s; the `--elevated` command line, `beamer install`,
`beamer uninstall`, `beamer quit` and the watchdog are `spec/app.md`'s; persistence and logging are
`spec/runtime.md`'s; and what every page, tray menu and notification says is `spec/viewmodel.md`'s.
The capabilities here report typed values: a notification's words, and the words that explain a
firewall check or an input blocker, are the view-model's (#16).

Dropped, so no rule here (#11): the portable build (F23), Windows unlock through the RoomDeck
credential provider and the lock screen (F21), US spelling at run time (F34), and the Linux
code for Start at login, permissions and portals (#1). The Python installer (Inno Setup and its PowerShell
maintenance scripts) is replaced by `beamer install` (#21).

A Mac is a machine running macOS, and a PC one running Windows. Every rule is `ci` unless it says
otherwise. Translation code (OS answers to capability states, task and rule definitions) is `ci` on
a hosted runner of its own OS (#15); a rule that only the real OS can show is `machine`.

## Capabilities

> r[platform.capability.states]
> Each capability of a machine reports exactly one of four states at a time: ready, needs
> permission, refused, or unsupported here with its reason. The capabilities are input capture,
> input injection, displays and the pointer, the clipboard, audio devices, camera devices, display
> switching, gestures and haptics, the identity key store, Start at login, the firewall, admin mode,
> power and wake, notifications, and permissions.

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
> On a Mac, the firewall and admin mode report unsupported here. On a PC, haptics report unsupported
> here.
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
> - do: On a Mac where Beamer has neither permission, ask for Accessibility, then for Input Monitoring, from Beamer's permissions page
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
> 5 s, without Beamer being relaunched.
>
> - tier: machine (only a real Mac changes a permission in System Settings)

## Start at login

> r[platform.login.default-on]
> A fresh install turns Start at login on, on a Mac and on a PC. Reinstalling over an earlier
> install keeps the choice the user made.
>
> - parity: Python's Windows installer left Start at login off unless the user ticked it (#21: Start at login is on by default on both OSes)

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

> r[platform.login.pc-trigger]
> On a PC, Start at login is on exactly when Beamer's logon task exists, is enabled, and has an
> enabled logon trigger.

> r[platform.login.pc-toggle]
> Turning Start at login on or off on a PC changes only the enabled state of the logon task's logon
> trigger: the task stays registered, with its run level and its action unchanged.
>
> - parity: Python deleted the task to turn Start at login off, and registered it again to turn it on (#21: one logon task, always registered, through which every launch goes)

> r[platform.login.pc-off-absent]
> Turning Start at login off on a PC with no logon task changes nothing.

> r[platform.login.pc-account]
> When Beamer runs as an account other than the one signed in to Windows, as when another
> administrator approved a UAC prompt for it, Start at login and admin mode report unsupported here,
> naming the signed-in account, and a request to change either changes nothing.

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
> The logon task starts `Beamer.exe` at its installed path with `--hidden`, in its own folder,
> as the signed-in account with its interactive token, at priority 4. It has one logon trigger,
> for that account. It ignores a new start while it runs, neither stops nor refuses to start on
> battery, has no time limit, and never restarts Beamer on failure.
>
> - parity: Python's task ran Beamer at priority 7, a below-normal process (#21: priority 4)

> r[platform.task.run-level]
> The logon task's run level is the highest the account has while admin mode is on, and least
> privilege while it is off. A task with no run level counts as least privilege.

> r[platform.task.any-path]
> The logon task starts Beamer at any installed path and for any account name, including ones with
> `&`, apostrophes or characters outside ASCII, such as `C:\A & B\` or `STUDIO-PC\Élodie`.

> r[platform.task.unchanged]
> Registering the logon task when the registered task already starts this Beamer.exe as
> `platform.task.definition` and `platform.task.run-level` say changes nothing, so a second install
> or a second registration writes nothing.

> r[platform.task.rewrite]
> When the registered logon task differs from what Beamer would write, Beamer rewrites it and keeps
> the enabled state of its logon trigger.

> r[platform.task.foreign]
> Beamer never changes or deletes a task that starts another program, belongs to another account,
> or has more than one action or principal, even one with the logon task's name: a change that
> would need to reports refused, naming the task.

> r[platform.task.no-migration]
> Beamer reads, changes and deletes no task but its own logon task: a task an earlier Beamer
> registered under another name, such as the global `Beamer` task, is left as it is.
>
> - parity: Python moved the global `Beamer` task of 1.5.1 to the per-account task, keeping its arguments and whether it was on, and deleted it, also on uninstall (#21: no compatibility code goes into the app; `dev/cutover.sh` removes the Python app's leftovers once)

> r[platform.task.launch]
> On a PC, a launch of Beamer that does not come from its logon task runs the logon task and ends,
> so Beamer always runs at admin mode's level and no UAC prompt is shown at launch. A launch from
> the task never shows a UAC prompt.
>
> - parity: Python, launched by hand without administrator rights while admin mode was on, asked for them with a UAC prompt and relaunched itself (#21: every launch that does not come from the task runs the task and exits)

> r[platform.task.launch-visible]
> The Beamer that the logon task starts for another launch shows its window when that launch would
> have, as a double-click does, and starts hidden for a launch with `--hidden` or a logon.

> r[platform.task.launch-options]
> The Beamer that the logon task starts for another launch is given that launch's other
> command-line options.

> r[platform.task.launch-failed]
> When the logon task is missing, or Windows refuses to run it, a launch goes on by itself with the
> rights it was started with, and admin mode reports refused until install registers the task again.

## Admin mode

> r[platform.admin.opt-in]
> On a PC, admin mode is off until the user turns it on. It belongs to the signed-in Windows account
> on this PC: it is never sent to a peer, and it stays as chosen across restarts of Beamer.
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
> Turning admin mode on or off asks for administrator permission with one UAC prompt. Once given,
> Beamer sets the logon task's run level for the new choice, changing nothing else in the task,
> records the choice, then ends the running Beamer, and the logon task starts Beamer again at the
> new level, showing its window.
>
> - parity: Python started the elevated Beamer itself, and the unelevated one through Explorer's desktop window (#21: every launch goes through the logon task, which starts Beamer at admin mode's level)

> r[platform.admin.same]
> Choosing the state admin mode already has asks for nothing and relaunches nothing.

> r[platform.admin.cancelled]
> When the UAC prompt for admin mode is declined, admin mode keeps its earlier state, the logon task
> is unchanged, and the change reports that it was cancelled.

> r[platform.admin.task-refused]
> When Windows refuses the change to the logon task, admin mode keeps its earlier state, the
> choice recorded is unchanged, and the running Beamer is not replaced.

> r[platform.admin.rollback]
> When the Beamer at the new level cannot be started, the logon task and the choice recorded are
> restored to what they were before the change, the running Beamer goes on, and the change reports
> that it failed. Beamer tries no other means of starting itself.

> r[platform.admin.keeps-login]
> Changing admin mode never changes Start at login: a logon trigger that was off stays off.

> r[platform.admin.no-task]
> When no logon task is registered, turning admin mode on registers it with its logon trigger off.

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

> r[platform.firewall.lan]
> The LAN is the networks Windows reports with the best connectivity of any, Internet before local
> network before subnet; when no network has any of these, the LAN is every network. A network
> outside the LAN, such as a Hyper-V switch with no traffic beside a Wi-Fi with Internet, plays no
> part in the check.

> r[platform.firewall.ready]
> The firewall reports ready when an allow rule that counts covers every profile the LAN is on, and
> no block rule blocks (`platform.firewall.blocked`).

> r[platform.firewall.blocked]
> A block rule that counts and is on a profile some network is connected on blocks: the firewall
> reports needs permission, naming a block rule as the cause, whatever allow rules there are. A
> block rule only on profiles no network is on, or a disabled one, is not reported.

> r[platform.firewall.missing]
> When no allow rule that counts covers every profile the LAN is on, and the LAN is not on the
> Public profile, the firewall reports needs permission, naming Beamer's rule as missing, and offers
> Repair.
>
> - parity: Python told the user to run the Beamer installer again, and on a domain network to ask an administrator for Domain-profile rules (#21: the app's Repair runs the elevated setup, whose rule covers the Domain profile too)

> r[platform.firewall.network-public]
> When the LAN is on the Public profile and an allow rule that counts does not cover it, the
> firewall reports refused, naming the LAN's network as public and its interface, so that the user
> can make it Private in Windows Settings. It offers no Repair for it.

> r[platform.firewall.port]
> The check is always for UDP port 24820.
>
> - parity: Python checked the link port the user chose, and on a port other than 24820 told the user to ask an administrator to allow it (#14: every machine takes links on UDP 24820, `link.transport.port`)

> r[platform.firewall.check-failed]
> When Windows cannot be asked, or its answer cannot be read, the firewall reports refused, naming
> a check that failed, and offers to check again. A check that failed never reports a rule missing
> or a block rule, and the OS's words reach only the log.

> r[platform.firewall.check-permission]
> When Windows refuses the check for lack of permission, told by its access-denied error code
> whatever the language Windows displays, the firewall reports needs permission, naming the check,
> and offers Repair and checking again. Any other refusal is a check that failed
> (`platform.firewall.check-failed`), whatever words it carries.

> r[platform.firewall.repair]
> Repair runs Beamer's elevated setup, which asks for administrator permission with one UAC prompt
> at the desk, then checks the firewall again.
>
> - parity: Python's app never changed the firewall, and told the user to run the installer again (#21: the app's Repair calls the same elevated setup as install)

> r[platform.firewall.setup]
> The elevated setup deletes every inbound block rule for `Beamer.exe` at its installed path, the
> ones Windows writes when its "allow access" prompt is dismissed included, and any rule named
> "Beamer (UDP-In)" that differs from `platform.firewall.rule`, then writes that rule when it is
> missing.
>
> - parity: Python's repair deleted every inbound rule for any `Beamer.exe`, wherever it was, and its installer left block rules for an administrator to review (#21: setup deletes the block rules Windows writes when its prompt is dismissed)

> r[platform.firewall.setup-unchanged]
> When Beamer's rule is as `platform.firewall.rule` says and no block rule for `Beamer.exe` exists,
> the elevated setup changes no firewall rule.

> r[platform.firewall.setup-scope]
> The elevated setup and uninstall delete only rules for `Beamer.exe` at its installed path or
> named "Beamer (UDP-In)": never another program's rule, and never every rule.

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

> r[platform.blocker.no-foreground]
> With no foreground window, or one whose process Windows does not name, Beamer inspects no process,
> and the input blocker is unknown.

> r[platform.blocker.log]
> The PC logs one warning for a refusal when the input blocker differs from the last refusal's, or
> when an injection or placement succeeded since: the operation refused, Windows' error code from
> that refusal or none, and the input blocker's name, process id, window class, rights and
> integrity. The error code is never one left from an earlier call.
>
> - parity: Python logged a refused injection again only for a different blocker, a success between them notwithstanding (one reading for both: the pointer placement's)

> r[platform.blocker.notify]
> An input problem raises a notification at most once for each input blocker window in a run, a
> window being its process id, handle and class, and at most once in 30 s whatever the window. A
> later success does not let a window's notification come again.

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

> r[platform.notify.pc]
> On a PC, a notification shows as a Windows notification from Beamer, with its title and body.
>
> - tier: human (a Windows notification is seen on the PC's display)
> - do: On the PC, make a crossing to a peer that refuses it
> - see: A Windows notification from Beamer says the crossing was refused, with a title and a body

## Haptics

> r[platform.haptics.tick]
> On a Mac with a Force Touch trackpad, a haptic tick is one level-change click, felt under a
> finger resting on the trackpad.
>
> - tier: human (a haptic click is felt)
> - do: Rest a finger on the Mac's trackpad and push the pointer slowly into a zone
> - see: Each step of push pressure is one click under the finger

> r[platform.haptics.breakthrough]
> The haptic for a breakthrough is two level-change clicks 60 ms apart.
>
> - tier: human (a haptic click is felt)
> - do: Rest a finger on the Mac's trackpad and push the pointer through a zone
> - see: As input crosses, two clicks close together, unlike a single step

> r[platform.haptics.never-delays]
> Haptics never delay or stop a crossing: on a Mac with no Force Touch trackpad nothing is felt, and
> the crossing is as it would be with haptics off.

> r[platform.haptics.failure]
> When macOS fails a haptic, haptics report refused for the rest of the run, one warning is logged,
> and no later haptic is tried.

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

> r[platform.tuning.refused]
> When the OS refuses either opt-out, Beamer logs a warning and goes on starting.

> r[platform.tuning.pc-priority]
> On a PC, Beamer runs in the above-normal priority class.
>
> - tier: machine (only a real PC reports a process's priority class)

> r[platform.tuning.top-priority]
> Capture and the engine run at the OS's top priority for interactive work: user-interactive on a
> Mac, time-critical on a PC.
>
> - tier: machine (only the real OS reports the priority Beamer runs at)
> - parity: Python's PC captured at the highest thread priority, below time-critical, and the rest of its work ran at normal priority (#16: capture and the engine run at the top)

## Identity key store

> r[platform.key-store.mac]
> On a Mac, the private key is one item in the user's login Keychain, readable by Beamer signed as
> "Beamer Dev", kept on this Mac only and never synced to another device.
>
> - tier: machine (the Keychain is reached only on a real Mac, by the signed app)

> r[platform.key-store.pc]
> On a PC, the private key is in a file in Beamer's data folder, `%LOCALAPPDATA%\Beamer\`, protected
> by DPAPI for the signed-in account, so that no other account and no other PC can read it.
>
> - tier: machine (DPAPI is reached only on a real Windows machine, under the user's account)

> r[platform.key-store.unreadable]
> When a stored private key exists but the OS will not hand it over (a locked Keychain, access
> refused, DPAPI failing), the identity key store reports refused, Beamer makes no new key pair,
> and it neither takes nor dials a link until it has read the key.

> r[platform.key-store.not-a-key]
> A stored private key that is not a valid Ed25519 private key counts as lost
> (`pairing.identity.no-export`): Beamer logs it, and makes a new key pair.
>
> - fuzz: key-store

## Power and wake

> r[platform.power.sleep]
> A machine reports the OS going to sleep, and waking from it, as each happens.
>
> - tier: machine (only a real machine sleeps and wakes)

> r[platform.power.wake-packet]
> A machine sends a wake's packet (`discovery.wake.packet`) as a UDP broadcast. When the OS refuses
> the broadcast, power and wake reports it, and the wake ends as `discovery.wake.send-failed` says.

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

> r[platform.install.task]
> Install registers the logon task (`platform.task.definition`), with its logon trigger on for a
> fresh install, and keeps the logon trigger's state when a task is already registered.

> r[platform.install.uninstall]
> Uninstall removes Start at login (the Mac's login item, or the PC's logon task), Beamer's firewall
> rule and the block rules for `Beamer.exe`, and the installed files.

> r[platform.install.uninstall-keeps]
> Uninstall keeps Beamer's data folder and the identity key, so that a reinstall is still
> paired. Uninstall with `--purge` removes them too.

> r[platform.install.quit-keeps]
> Quitting Beamer leaves every install step as it is: Start at login and the firewall rule stay.
