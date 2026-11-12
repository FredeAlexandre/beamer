# SPDX-License-Identifier: GPL-3.0-only
"""Shared displays in the menus: what the Mac menu bar and the PC tray show (core/display/part.py).

One entry per shared display: a title naming the display and who has it, and a choice per machine
that has it plus "Not managed". Pure apart from tr()."""

import dataclasses
from typing import Optional, Tuple

from core.i18n import tr


@dataclasses.dataclass(frozen=True)
class MenuChoice:
    label: str
    machine: Optional[str]
    checked: bool


@dataclasses.dataclass(frozen=True)
class DisplayMenu:
    key: str
    title: str
    choices: Tuple[MenuChoice, ...]


def menus(snapshot, own_label: str) -> Tuple[DisplayMenu, ...]:
    """The menu entries for a DisplaySnapshot (None: no feature, no entries). `own_label` names this
    machine ("This Mac", "This PC")."""
    if snapshot is None:
        return ()
    result = []
    for view in snapshot.shared:
        def label(choice):
            if choice.machine is None:
                return tr("Not managed")
            return own_label if choice.machine == snapshot.own else choice.name
        owner = next((choice for choice in view.choices if choice.chosen and choice.machine is not None), None)
        if owner is None:
            title = tr("Screen {name}: not managed").format(name=view.name)
        else:
            title = tr("Screen {name}: {owner}").format(name=view.name, owner=label(owner))
        choices = tuple(MenuChoice(label(choice), choice.machine, choice.chosen) for choice in view.choices)
        result.append(DisplayMenu(view.key, title, choices))
    return tuple(result)
