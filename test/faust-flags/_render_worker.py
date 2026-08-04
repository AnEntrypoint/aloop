#!/usr/bin/env python3
import json
import sys

import numpy as np

import dawdreamer as daw


def main():
    code_path, flags_json, input_path, output_path, sample_rate, block_size = sys.argv[1:7]
    flags = json.loads(flags_json)
    sample_rate = int(sample_rate)
    block_size = int(block_size)

    code = open(code_path).read()
    input_audio = np.load(input_path)

    engine = daw.RenderEngine(sample_rate, block_size)
    playback = engine.make_playback_processor("in", input_audio)
    faust_processor = engine.make_faust_processor("fx")
    faust_processor.set_dsp_string(code)
    faust_processor.compile_flags = flags
    if not faust_processor.compile():
        print("COMPILE_FAILED", file=sys.stderr)
        return 2

    engine.load_graph([(playback, []), (faust_processor, ["in"])])
    duration_s = input_audio.shape[1] / sample_rate
    if not engine.render(duration_s):
        print("RENDER_FAILED", file=sys.stderr)
        return 3

    np.save(output_path, engine.get_audio())
    return 0


if __name__ == "__main__":
    sys.exit(main())
