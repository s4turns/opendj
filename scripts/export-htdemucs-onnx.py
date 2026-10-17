#!/usr/bin/env python3
#
# This file is part of OpenDJ. See LICENSE for terms (GPLv3 or later).
# Copyright (C) 2026 The OpenDJ contributors.
#
# Exports htdemucs to ONNX for StemModel's CUDA backend.
#
# Only the middle of HTDemucs.forward is exported, the same cut as Intel's
# OpenVINO model (htdemucs_v4.xml): the STFT, the normalisation and the inverse
# STFT live in StemModel.cpp and StemDsp, so the graph takes the normalised
# spectrum `x` [1, 4, 2048, 336] and waveform `xt` [1, 2, 343980] and returns
# the decoders' outputs `x_out` and `xt_out` before anything is undone.
#
# Usage, in a venv with torch, demucs, onnx, onnxruntime and openvino:
#   python scripts/export-htdemucs-onnx.py [-o htdemucs_v4.onnx] [--verify path/to/htdemucs_v4.xml]
#
# --verify feeds the same random input to the export and to the OpenVINO model
# and prints how far apart they come out. A cut in the wrong place does not
# fail, it gives output that sounds almost right, so this is the check.

import argparse
import pathlib

import numpy as np
import torch
from demucs.pretrained import get_model

SPEC_SHAPE = (1, 4, 2048, 336)
WAVE_SHAPE = (1, 2, 343980)


class Middle(torch.nn.Module):
    """HTDemucs.forward from the normalised inputs to the decoders' outputs."""

    def __init__(self, model):
        super().__init__()
        self.m = model

    def forward(self, x, xt):
        m = self.m
        saved, saved_t, lengths, lengths_t = [], [], [], []

        for idx, encode in enumerate(m.encoder):
            lengths.append(x.shape[-1])
            inject = None
            if idx < len(m.tencoder):
                lengths_t.append(xt.shape[-1])
                tenc = m.tencoder[idx]
                xt = tenc(xt)
                if not tenc.empty:
                    saved_t.append(xt)
                else:
                    inject = xt
            x = encode(x, inject)
            if idx == 0 and m.freq_emb is not None:
                frs = torch.arange(x.shape[-2], device=x.device)
                emb = m.freq_emb(frs).t()[None, :, :, None].expand_as(x)
                x = x + m.freq_emb_scale * emb
            saved.append(x)

        if m.crosstransformer:
            if m.bottom_channels:
                b, c, f, t = x.shape
                x = x.reshape(b, c, f * t)
                x = m.channel_upsampler(x)
                x = x.reshape(b, -1, f, t)
                xt = m.channel_upsampler_t(xt)

            x, xt = m.crosstransformer(x, xt)

            if m.bottom_channels:
                b, c, f, t = x.shape
                x = x.reshape(b, c, f * t)
                x = m.channel_downsampler(x)
                x = x.reshape(b, -1, f, t)
                xt = m.channel_downsampler_t(xt)

        for idx, decode in enumerate(m.decoder):
            skip = saved.pop(-1)
            x, pre = decode(x, skip, lengths.pop(-1))
            offset = m.depth - len(m.tdecoder)
            if idx >= offset:
                tdec = m.tdecoder[idx - offset]
                length_t = lengths_t.pop(-1)
                if tdec.empty:
                    pre = pre[:, :, 0]
                    xt, _ = tdec(pre, None, length_t)
                else:
                    skip = saved_t.pop(-1)
                    xt, _ = tdec(xt, skip, length_t)

        return x, xt


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("-o", "--output", default="htdemucs_v4.onnx")
    parser.add_argument("--verify", metavar="IR_XML")
    args = parser.parse_args()

    model = get_model("htdemucs").models[0].eval()
    middle = Middle(model).eval()

    x = torch.randn(*SPEC_SHAPE)
    xt = torch.randn(*WAVE_SHAPE)

    # The fused attention kernel eval mode picks has no ONNX equivalent.
    torch.backends.mha.set_fastpath_enabled(False)

    with torch.no_grad():
        torch.onnx.export(middle, (x, xt), args.output,
                          input_names=["x", "xt"], output_names=["x_out", "xt_out"],
                          opset_version=17, dynamo=False)

    print(f"wrote {args.output}, {pathlib.Path(args.output).stat().st_size} bytes")

    if not args.verify:
        return

    import onnxruntime
    import openvino

    rng = np.random.default_rng(1)
    feed = {"x": rng.standard_normal(SPEC_SHAPE, dtype=np.float32),
            "xt": rng.standard_normal(WAVE_SHAPE, dtype=np.float32)}

    session = onnxruntime.InferenceSession(args.output, providers=["CPUExecutionProvider"])
    ours = session.run(None, feed)

    compiled = openvino.Core().compile_model(args.verify, "CPU")
    result = compiled([feed["x"], feed["xt"]])
    theirs = [result[o] for o in compiled.outputs]

    # Matched by rank, as StemModel does: the spectrum is 4-D, the waveform 3-D.
    theirs.sort(key=lambda a: -a.ndim)

    for name, a, b in zip(["x_out", "xt_out"], ours, theirs):
        b = b.reshape(a.shape)
        print(f"{name}: shape {a.shape}, max |diff| {np.abs(a - b).max():.2e}, "
              f"rms {np.sqrt(np.mean(a * a)):.3f}")


if __name__ == "__main__":
    main()
