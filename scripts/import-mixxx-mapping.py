#!/usr/bin/env python3
"""Turn Mixxx controller mappings into OpenDJ ones.

    python scripts/import-mixxx-mapping.py [--source DIR] [--out DIR] [model ...]

This is a developer's tool. OpenDJ reads the JSON it writes and never runs this,
and the JSON is committed, so a change to a generated mapping belongs here rather
than in the file.

A Mixxx mapping is XML that binds MIDI messages to Mixxx's own controls, plus
JavaScript for whatever a plain binding cannot do. The XML can be translated
mechanically. The JavaScript cannot, so each model below has a table saying what
the script functions it uses amount to in OpenDJ, written by reading that script.
Anything with no translation is listed in the mapping's description rather than
quietly dropped.

Mixxx's mappings are GPLv2 or later, which OpenDJ's GPLv3 or later can take in.
Their authors are credited in each file.

Files come from the commit named below, so the output is reproducible. Give
--source a checkout's res/controllers folder to read from that instead.
"""

import argparse
import json
import re
import sys
import urllib.parse
import urllib.request
import xml.etree.ElementTree as ET
from dataclasses import dataclass, field
from pathlib import Path

MIXXX_COMMIT = "64c1cbec91a86bf72fdbaedd5cf750275b29a729"
MIXXX_URL = "https://raw.githubusercontent.com/mixxxdj/mixxx/" + MIXXX_COMMIT + "/res/controllers/"

# The lengths OpenDJ's loop and beat jump slots stand for: loopBeatsForSlot().
BEAT_SLOTS = {"0.25": 0, "0.5": 1, "1": 2, "2": 3, "4": 4, "8": 5, "16": 6, "32": 7}

# Actions that belong to the whole mix, so "deck" would mean nothing.
GLOBAL_ACTIONS = {
    "modifier.shift", "mixer.crossfader", "mixer.master", "mixer.phones", "mixer.cue_mix",
    "browse.scroll", "sampler.trigger", "sampler.stop", "sampler.gain",
}

# Which of Mixxx's outputs reports what, for the actions OpenDJ can light.
# A key may contain {n}, the hot cue number.
LIGHTS = {
    "deck.play_toggle": ["play_indicator", "play"],
    "deck.cue": ["cue_indicator"],
    "deck.sync": ["sync_enabled"],
    "deck.keylock_toggle": ["keylock"],
    "deck.quantize_toggle": ["quantize"],
    "mixer.cue_toggle": ["pfl"],
    "pad.hotcue": ["hotcue_{n}_enabled", "hotcue_{n}_status"],
}

CHANNEL_KEYS = {
    "play": ("deck.play_toggle", "play"),
    "cue_default": ("deck.cue", "cue"),
    "sync_enabled": ("deck.sync", "sync"),
    "keylock": ("deck.keylock_toggle", "key lock"),
    "quantize": ("deck.quantize_toggle", "quantize"),
    "slip_enabled": ("deck.slip_toggle", "slip"),
    "reverseroll": ("deck.censor", "censor"),
    "bpm_tap": ("deck.tap_tempo", "tap tempo"),
    "LoadSelectedTrack": ("deck.load", "load"),
    "volume": ("mixer.fader", "fader"),
    "pregain": ("deck.trim", "trim"),
    "rate": ("deck.tempo", "tempo fader"),
    "pfl": ("mixer.cue_toggle", "headphone cue"),
    "loop_in": ("loop.in", "loop in"),
    "loop_out": ("loop.out", "loop out"),
    "loop_halve": ("loop.halve", "loop halve"),
    "loop_double": ("loop.double", "loop double"),
    # Not loop.reloop, which can only start a loop. This button has to end one too.
    "reloop_toggle": ("loop.toggle", "loop on/off"),
    "reloop_exit": ("loop.toggle", "loop on/off"),
}

MASTER_KEYS = {
    "crossfader": ("mixer.crossfader", "Crossfader"),
    "gain": ("mixer.master", "Master level"),
    "headGain": ("mixer.phones", "Headphone level"),
    "headMix": ("mixer.cue_mix", "Headphone mix"),
}


def number(text):
    return int(text.strip(), 0)


def deck_of(group):
    match = re.fullmatch(r"\[Channel(\d)\]", group or "")
    return int(match[1]) - 1 if match else None


def translate_direct(group, key):
    """What a plain, script-free Mixxx binding means in OpenDJ, or None."""
    deck = deck_of(group)

    if deck is not None:
        if key in CHANNEL_KEYS:
            action, label = CHANNEL_KEYS[key]
            return {"action": action, "label": label, "deck": deck}

        if match := re.fullmatch(r"hotcue_(\d+)_(activate|clear)", key):
            if int(match[1]) <= 8:
                activate = match[2] == "activate"
                return {"action": "pad.hotcue" if activate else "pad.hotcue_clear",
                        "label": ("hot cue " if activate else "clear hot cue ") + match[1],
                        "deck": deck, "slot": int(match[1]) - 1, "hotcue": int(match[1])}

        if match := re.fullmatch(r"beatloop_([\d.]+)_(toggle|activate)", key):
            if match[1] in BEAT_SLOTS:
                return {"action": "loop.beats", "label": "loop " + match[1],
                        "deck": deck, "slot": BEAT_SLOTS[match[1]]}

        if match := re.fullmatch(r"beatlooproll_([\d.]+)_activate", key):
            if match[1] in BEAT_SLOTS:
                return {"action": "loop.roll", "label": "roll " + match[1],
                        "deck": deck, "slot": BEAT_SLOTS[match[1]]}

        if match := re.fullmatch(r"beatjump_([\d.]+)_(forward|backward)", key):
            if match[1] in BEAT_SLOTS:
                forward = match[2] == "forward"
                return {"action": "deck.beatjump_forward" if forward else "deck.beatjump_back",
                        "label": "jump " + ("forward " if forward else "back ") + match[1],
                        "deck": deck, "slot": BEAT_SLOTS[match[1]]}

        return None

    if match := re.fullmatch(r"\[EqualizerRack1_\[Channel(\d)\]_Effect1\]", group or ""):
        if band := re.fullmatch(r"parameter([123])", key):
            slot = int(band[1]) - 1
            return {"action": "mixer.eq", "label": ("EQ low", "EQ mid", "EQ high")[slot],
                    "deck": int(match[1]) - 1, "slot": slot}
        return None

    if match := re.fullmatch(r"\[QuickEffectRack1_\[Channel(\d)\]\]", group or ""):
        if key == "super1":
            return {"action": "mixer.filter", "label": "filter", "deck": int(match[1]) - 1}
        return None

    if group in ("[Master]", "[Main]") and key in MASTER_KEYS:
        action, label = MASTER_KEYS[key]
        return {"action": action, "label": label}

    if group == "[Library]" and key in ("MoveVertical", "ScrollVertical"):
        # Mixxx reads these as signed seven-bit however the binding is marked.
        return {"action": "browse.scroll", "label": "Browse", "mode": "relative_twos"}

    if match := re.fullmatch(r"\[Sampler(\d+)\]", group or ""):
        # OpenDJ has eight pads to a bank, so a second row reaches the same eight.
        slot = (int(match[1]) - 1) % 8
        if key == "cue_gotoandplay":
            return {"action": "sampler.trigger", "label": f"Sampler {slot + 1}", "slot": slot}
        if key in ("cue_gotoandstop", "stop"):
            return {"action": "sampler.stop", "label": f"Sampler {slot + 1} stop", "slot": slot}
        return None

    # Mixxx has racks of three effects. OpenDJ has one effect in each of two
    # units, so the first effect of the first two units is what lines up.
    if match := re.fullmatch(r"\[EffectRack1_EffectUnit([12])_Effect1\]", group or ""):
        unit = int(match[1]) - 1
        if key == "enabled":
            return {"action": "master_fx.toggle", "label": f"FX {unit + 1} on/off", "deck": unit}
        if key == "next_effect":
            return {"action": "master_fx.next_type", "label": f"FX {unit + 1} next", "deck": unit}
        if key == "meta":
            return {"action": "master_fx.param", "label": f"FX {unit + 1} amount", "deck": unit, "slot": 0}
        return None

    if match := re.fullmatch(r"\[EffectRack1_EffectUnit([12])\]", group or ""):
        unit = int(match[1]) - 1
        if key == "mix":
            return {"action": "master_fx.wet", "label": f"FX {unit + 1} wet", "deck": unit}
        if key == "super1":
            return {"action": "master_fx.param", "label": f"FX {unit + 1} amount", "deck": unit, "slot": 0}

    return None


#==============================================================================
# What the scripts do
#==============================================================================

JOG_TOUCH = {"action": "jog.touch", "label": "platter touch"}
JOG_OFFSET = {"action": "jog.turn", "label": "platter", "mode": "relative_offset"}
JOG_TWOS = {"action": "jog.turn", "label": "platter", "mode": "relative_twos"}
SHIFT = {"action": "modifier.shift", "label": "Shift"}


def pioneer_pad(control):
    """Pads whose script works out what to do from which pad it was."""
    if control.function == "beatjumpPadPressed":
        pad = control.number - 0x20
        slot = 2 + pad // 2             # pads run 1, 1, 2, 2, 4, 4, 8, 8 beats
        forward = pad % 2 == 1
        return {"action": "deck.beatjump_forward" if forward else "deck.beatjump_back",
                "label": "jump " + ("forward " if forward else "back ") + str(2 ** (pad // 2)),
                "deck": 0 if control.status == 0x97 else 1, "slot": slot}

    if control.function == "samplerPadPressed":
        return translate_direct(control.group, "cue_gotoandplay")

    if control.function == "samplerPadShiftPressed":
        return translate_direct(control.group, "cue_gotoandstop")

    if control.function == "stemMutePadPressed":
        slot = control.number - 0x40    # Mixxx and OpenDJ both number stems drums, bass, other, vocals
        return {"action": "deck.stem_toggle", "label": f"stem {slot + 1} mute",
                "deck": 0 if control.status == 0x97 else 1, "slot": slot}

    return None


PIONEER = {
    "shiftPressed": SHIFT,
    "jogTouch": JOG_TOUCH,      # both notes: the shifted one is what a release sends if Shift went down meanwhile
    "jogTurn": JOG_OFFSET,
    "jogSearch": JOG_OFFSET,
    "tempoSliderMSB": {"action": "deck.tempo", "label": "tempo fader", "pair": "msb", "inverted": True},
    "tempoSliderLSB": {"action": "deck.tempo", "label": "tempo fader", "pair": "lsb", "inverted": True},
    "syncPressed": {"action": "deck.sync", "label": "sync"},
    "toggleQuantize": {"action": "deck.quantize_toggle", "label": "quantize"},
    "quickJumpForward": {"action": "deck.beatjump_forward", "label": "jump forward 32", "slot": 7},
    "quickJumpBack": {"action": "deck.beatjump_back", "label": "jump back 32", "slot": 7},
    "cueLoopCallLeft": {"action": "loop.halve", "label": "loop halve"},
    "cueLoopCallRight": {"action": "loop.double", "label": "loop double"},
    "beatFxOnOffPressed": {"action": "master_fx.toggle", "label": "Beat FX on/off", "deck": 0},
    "beatFxSelectPressed": {"action": "master_fx.next_type", "label": "Beat FX next", "deck": 0},
    "beatFxLevelDepthRotate": {"action": "master_fx.wet", "label": "Beat FX level", "deck": 0},
    "beatjumpPadPressed": pioneer_pad,
    "samplerPadPressed": pioneer_pad,
    "samplerPadShiftPressed": pioneer_pad,
    "stemMutePadPressed": pioneer_pad,
}

HERCULES = {
    "wheelTouch": JOG_TOUCH,
    "wheelTouchShift": JOG_TOUCH,
    "scratchWheel": JOG_TWOS,
    "scratchWheelShift": JOG_TWOS,
    "bendWheel": JOG_TWOS,
    "jogWheel": JOG_TWOS,
}

# Hercules sends each deck on two MIDI channels, the second while Shift is held.
HERCULES_DECKS = {1: 0, 2: 1, 4: 0, 5: 1}

MC4000 = {
    "platterTouch": JOG_TOUCH,
    "jogWheel": JOG_TWOS,
    "samplerLevel": {"action": "sampler.gain", "label": "Sampler level"},
}

MIXAGE = {
    "handleShift": SHIFT,
    "wheelTouch": JOG_TOUCH,
    "wheelTurn": JOG_OFFSET,
    "handlePlay": {"action": "deck.play_toggle", "label": "play"},
    "handleTrackLoading": {"action": "deck.load", "label": "load"},
    "handleLoop": {"action": "loop.beats", "label": "loop", "slot": BEAT_SLOTS["4"]},
    "handleLoopIn": {"action": "loop.in", "label": "loop in"},
    "handleLoopOut": {"action": "loop.out", "label": "loop out"},
    "handleReloop": {"action": "loop.toggle", "label": "loop on/off"},
    "handleFilter": {"action": "mixer.filter", "label": "filter"},
    "handleFxPress": {"action": "master_fx.toggle", "label": "FX on/off", "unit_from_deck": True},
    "nextEffect": {"action": "master_fx.next_type", "label": "FX next", "unit_from_deck": True},
    "handleFxAmount": {"action": "master_fx.param", "label": "FX amount", "slot": 0, "unit_from_deck": True},
    "selectTrack": {"action": "browse.scroll", "label": "Browse", "mode": "relative_offset"},
}


#==============================================================================
# The models
#==============================================================================

@dataclass
class Model:
    filename: str               # what this is written as
    source: str                 # the Mixxx file
    name: str
    hints: list
    ticks: int
    script: dict
    decks_by_channel: dict = field(default_factory=dict)
    init: list = field(default_factory=list)
    keep_alive: tuple = None
    behaviour: str = ""         # what a reader needs to know that the numbers do not say
    lights: list = field(default_factory=list)  # (group, Mixxx output key, status, note) a script drives


FLX4_HELLO = "F0 00 40 05 00 00 04 05 00 50 02 F7"

# From Mixage.ledMap, in place of the outputs the XML does not list.
MIXAGE_LIGHTS = [
    (group, key, 0x90, note)
    for group, notes in (("[Channel1]", (0x0B, 0x0C, 0x0E, 0x09)), ("[Channel2]", (0x19, 0x1A, 0x1C, 0x17)))
    for key, note in zip(("cue_indicator", "play_indicator", "pfl", "sync_enabled"), notes)
]

NO_VINYL = ("Touching the platter always scratches: OpenDJ has no separate vinyl mode, "
            "so the button that switches one is left alone. ")

MODELS = [
    Model("pioneer-ddj-flx4", "Pioneer-DDJ-FLX4.midi.xml", "Pioneer DDJ-FLX4", ["DDJ-FLX4"], 720, PIONEER,
          init=[FLX4_HELLO], keep_alive=(FLX4_HELLO, 200),
          behaviour="The controller is sent a message every 200 ms to stay talking to the computer. "
                    "The right hand pads reach the same eight sampler slots as the left. "),
    Model("pioneer-ddj-400", "Pioneer-DDJ-400.midi.xml", "Pioneer DDJ-400", ["DDJ-400"], 720, PIONEER,
          init=["F0 00 40 05 00 00 02 06 00 03 01 F7"],
          behaviour="The opening message asks the controller to report where its knobs are. "
                    "The right hand pads reach the same eight sampler slots as the left. "),
    Model("hercules-djcontrol-inpulse-200", "Hercules_DJControl_Inpulse_200.midi.xml",
          "Hercules DJControl Inpulse 200", ["DJControl Inpulse 200"], 248, HERCULES,
          decks_by_channel=HERCULES_DECKS, init=["B0 7F 7F"], behaviour=NO_VINYL),
    Model("hercules-djcontrol-inpulse-300", "Hercules_DJControl_Inpulse_300.midi.xml",
          "Hercules DJControl Inpulse 300", ["DJControl Inpulse 300"], 248,
          {**HERCULES,
           "loopInButton": {"action": "loop.in", "label": "loop in"},
           "loopOutButton": {"action": "loop.out", "label": "loop out"}},
          decks_by_channel=HERCULES_DECKS, init=["B0 7F 7F"], behaviour=NO_VINYL),
    Model("hercules-djcontrol-starlight", "Hercules DJControl Starlight.midi.xml",
          "Hercules DJControl Starlight", ["DJControl Starlight"], 248,
          {**HERCULES, "shiftButton": SHIFT},
          decks_by_channel=HERCULES_DECKS, init=["B0 7F 7F"],
          behaviour=NO_VINYL + "Mixxx's mapping binds no browse or load controls, so this one has none either. "),
    Model("denon-mc4000", "Denon MC4000.midi.xml", "Denon MC4000", ["MC4000"], 605, MC4000,
          init=["F0 00 02 0B 7F 01 60 00 04 04 01 00 00 F7"],
          behaviour="The opening message asks the controller to report where its knobs are. " + NO_VINYL),
    Model("reloop-mixage", "Reloop-Mixage.midi.xml", "Reloop Mixage", ["Mixage"], 620, MIXAGE,
          behaviour="Touching the platter always scratches, where Mixxx waits for the disc button. "
                    "The loop button makes a loop of four beats. "
                    "Each side's FX controls reach OpenDJ's master effect unit of the same number. ",
          lights=MIXAGE_LIGHTS),
]


#==============================================================================
# Translation
#==============================================================================

@dataclass
class Binding:
    group: str
    key: str
    function: str               # the script function, or "" for a plain binding
    status: int
    number: int
    tags: set


def read_bindings(root):
    bindings = []

    for control in root.findall(".//controls/control"):
        status = number(control.findtext("status"))

        # A note off finds its note on in the engine, so naming it is redundant.
        if status & 0xF0 == 0x80:
            continue

        midino = control.findtext("midino")
        options = control.find("options")
        tags = {option.tag.lower() for option in options} if options is not None else set()
        key = (control.findtext("key") or "").strip()
        scripted = "script-binding" in tags

        bindings.append(Binding(group=(control.findtext("group") or "").strip(),
                                key=key,
                                function=key.rsplit(".", 1)[-1] if scripted else "",
                                status=status,
                                number=number(midino) if midino else 0,
                                tags=tags))

    return bindings


def read_outputs(root):
    outputs = []

    for output in root.findall(".//outputs/output"):
        on = output.findtext("on")
        off = output.findtext("off")
        outputs.append({"group": (output.findtext("group") or "").strip(),
                        "key": (output.findtext("key") or "").strip(),
                        "status": number(output.findtext("status")),
                        "number": number(output.findtext("midino")),
                        "on": number(on) if on else 0x7F,
                        "off": number(off) if off else 0x00})

    return outputs


def light_for(spec, binding, outputs):
    """The output that lights this control, preferring its own button."""
    names = LIGHTS.get(spec["action"])

    if names is None:
        return None

    names = [name.format(n=spec.get("hotcue", "")) for name in names]
    wanted = [o for o in outputs if o["group"] == binding.group and o["key"] in names]

    for output in wanted:
        if (output["status"], output["number"]) == (binding.status, binding.number):
            return output

    # OpenDJ answers on the channel the control arrived on.
    for output in wanted:
        if output["status"] == binding.status:
            return output

    return None


def hex2(value):
    return f"0x{value:02X}"


def spec_for(model, binding):
    if binding.function:
        entry = model.script.get(binding.function)

        if callable(entry):
            return entry(binding)

        return dict(entry) if entry is not None else None

    return translate_direct(binding.group, binding.key)


def resolve_deck(model, spec, binding):
    """Fills in the deck of a script control, whose table entry cannot know it."""
    if spec["action"] in GLOBAL_ACTIONS or "deck" in spec:
        spec.pop("unit_from_deck", None)
        return True

    channel = binding.status & 0x0F
    deck = model.decks_by_channel.get(channel, deck_of(binding.group))

    if deck is None:
        return False

    spec["deck"] = deck

    if spec.pop("unit_from_deck", False):
        spec["label"] = spec["label"].replace("FX", f"FX {deck + 1}", 1)

    return True


def convert(model, root):
    outputs = read_outputs(root) + [{"group": group, "key": key, "status": status, "number": note,
                                     "on": 0x7F, "off": 0x00}
                                    for group, key, status, note in model.lights]
    controls = []
    taken = set()
    pairs = {}
    left_out = []

    def add(spec, binding, fine=None):
        if (binding.status, binding.number) in taken:
            left_out.append(binding.function or binding.key)
            return

        taken.add((binding.status, binding.number))

        deck = spec.get("deck")

        # A master effect's "deck" is which unit it is, so it is not named after one.
        if deck is None or spec["action"].startswith("master_fx."):
            name = spec["label"]
        else:
            name = f"Deck {'ABCD'[deck]} {spec['label']}"

        control = {"name": name, "status": hex2(binding.status), "number": hex2(binding.number)}

        if fine is not None:
            taken.add((binding.status, fine))
            control["fineNumber"] = hex2(fine)

        control["action"] = spec["action"]

        if deck is not None:
            control["deck"] = deck

        if "slot" in spec:
            control["slot"] = spec["slot"]

        # Written only where the default would be wrong, as a person would.
        if fine is not None:
            control["mode"] = "absolute14"
        elif binding.status & 0xF0 == 0xE0:
            control["mode"] = "pitch14"
        elif "mode" in spec:
            control["mode"] = spec["mode"]
        elif "selectknob" in binding.tags:
            control["mode"] = "relative_twos"

        if spec.get("inverted") or "invert" in binding.tags:
            control["inverted"] = True

        if light := light_for(spec, binding, outputs):
            control["feedbackNumber"] = hex2(light["number"])

            if light["on"] != 0x7F:
                control["feedbackOnVelocity"] = hex2(light["on"])

            if light["off"] != 0x00:
                control["feedbackOffVelocity"] = hex2(light["off"])

        controls.append(control)

    for binding in read_bindings(root):
        spec = spec_for(model, binding)

        if spec is None or not resolve_deck(model, spec, binding):
            left_out.append(binding.function or binding.key)
            continue

        half = spec.get("pair") or ("msb" if "fourteen-bit-msb" in binding.tags
                                    else "lsb" if "fourteen-bit-lsb" in binding.tags else None)

        if half is None:
            add(spec, binding)
            continue

        waiting = pairs.setdefault((binding.status, spec["action"], spec.get("deck"), spec.get("slot")), {})
        waiting[half] = (spec, binding)

        if len(waiting) == 2:
            add(waiting["msb"][0], waiting["msb"][1], fine=waiting["lsb"][1].number)

    return controls, sorted(set(left_out), key=str.lower)


def description(model, info, left_out):
    return ("Translated from the Mixxx mapping by " + info.findtext("author").strip()
            + " (https://github.com/mixxxdj/mixxx/tree/" + MIXXX_COMMIT + "/res/controllers), which is GPLv2 or "
            "later and so compatible with this project, by scripts/import-mixxx-mapping.py. Not yet tried on the "
            "hardware. " + model.behaviour
            + "Mixxx controls with nothing to reach in OpenDJ, and so not mapped: " + ", ".join(left_out) + ".")


def read_source(source, name):
    if source is not None:
        return (source / name).read_bytes()

    request = urllib.request.Request(MIXXX_URL + urllib.parse.quote(name),
                                     headers={"User-Agent": "opendj-import-mixxx-mapping"})

    with urllib.request.urlopen(request, timeout=30) as response:
        return response.read()


def main():
    parser = argparse.ArgumentParser(description="Turn Mixxx controller mappings into OpenDJ ones.")
    parser.add_argument("--source", type=Path, help="a Mixxx res/controllers folder, rather than downloading")
    parser.add_argument("--out", type=Path, default=Path(__file__).resolve().parent.parent / "mappings")
    parser.add_argument("models", nargs="*", help="file names to write, all of them if none are given")
    arguments = parser.parse_args()

    chosen = [m for m in MODELS if not arguments.models or m.filename in arguments.models]

    if len(chosen) != len(arguments.models) and arguments.models:
        sys.exit("Known models: " + ", ".join(m.filename for m in MODELS))

    for model in chosen:
        root = ET.fromstring(read_source(arguments.source, model.source))
        controls, left_out = convert(model, root)

        mapping = {
            "name": model.name,
            "author": "The OpenDJ contributors, from the Mixxx mapping by " + root.find("info").findtext("author").strip(),
            "description": description(model, root.find("info"), left_out),
            "deviceNameHints": model.hints,
            "jogTicksPerRevolution": model.ticks,
        }

        if model.init:
            mapping["initMessages"] = model.init

        if model.keep_alive:
            mapping["keepAlive"] = {"message": model.keep_alive[0], "intervalMs": model.keep_alive[1]}

        mapping["controls"] = controls

        path = arguments.out / (model.filename + ".json")
        path.write_text(json.dumps(mapping, indent=2, ensure_ascii=False) + "\n", encoding="utf-8", newline="\n")

        lit = sum(1 for control in controls if "feedbackNumber" in control)
        print(f"{model.filename}: {len(controls)} controls, {lit} with lights, {len(left_out)} left out")
        print("    left out: " + ", ".join(left_out))


if __name__ == "__main__":
    main()
