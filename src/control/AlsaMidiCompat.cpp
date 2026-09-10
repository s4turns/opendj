/*
    This file is part of OpenDJ. See LICENSE for terms (GPLv3 or later).
    Copyright (C) 2026 The OpenDJ contributors.
*/

/*
    Keeps the ALSA sequencer talking MIDI 1.0 to us on Linux.

    JUCE registers its sequencer client as MIDI 2.0:

        if (snd_seq_set_client_midi_version != nullptr)
            snd_seq_set_client_midi_version (handle, SND_SEQ_CLIENT_UMP_MIDI_2_0);

    On a kernel new enough to know about UMP, that makes the sequencer translate
    every legacy MIDI 1.0 event into MIDI 2.0 before handing it over -- and MIDI
    2.0 reserves controller 6 as Data Entry, the middle of an RPN or NRPN
    sequence, rather than a controller in its own right. A device that sends a
    bare controller 6 therefore has it swallowed in translation. Demonstrated
    with nothing but aseqdump:

        aseqdump -u 0   ->  controller 6 arrives, controller 7 arrives
        aseqdump -u 2   ->  controller 6 is gone, controller 7 arrives

    The Roland DJ-202's platters report on controller 6, so on such a kernel the
    jog wheels are invisible to any JUCE application: the touch is seen, the
    turning is not, and the deck stops dead under the hand.

    JUCE declares that function [[gnu::weak]] so that it can detect an older
    libasound that lacks it. A strong definition here satisfies the same symbol
    at link time, so JUCE calls this instead and the client is left at the
    version a new one gets by default, which is legacy MIDI 1.0.

    The cost is that a genuine MIDI 2.0 controller is spoken to in MIDI 1.0,
    losing its higher resolution. For DJ hardware that is a trade worth making:
    every controller this application has met speaks MIDI 1.0, and one that
    speaks MIDI 2.0 still works, just at seven bits.

    Remove this once JUCE offers a way to choose the client's MIDI version.
*/

#if defined (__linux__) || defined (__FreeBSD__)

extern "C"
{
    // The real signature is int (snd_seq_t*, int). Only the name matters for
    // linking under extern "C", and the pointer is passed straight through.
    int snd_seq_set_client_midi_version (void* seq, int midiVersion);

    int snd_seq_set_client_midi_version (void*, int)
    {
        // Deliberately does nothing: a new client is already legacy MIDI 1.0.
        return 0;
    }
}

#endif
