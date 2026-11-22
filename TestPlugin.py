import ctypes as ct
from array import array
import math
import mmap
import pathlib
import shutil
import struct
import sys
import wave


Dispatch = ct.CFUNCTYPE(ct.c_ssize_t, ct.c_void_p, ct.c_int32, ct.c_int32,
                        ct.c_ssize_t, ct.c_void_p, ct.c_float)
Planes = ct.POINTER(ct.c_float) * 8
Process = ct.CFUNCTYPE(None, ct.c_void_p, Planes, Planes, ct.c_int32)


class Effect(ct.Structure):
    _fields_ = [
        ("magic", ct.c_int32), ("dispatch", Dispatch), ("process", ct.c_void_p),
        ("set", ct.c_void_p), ("get", ct.c_void_p), ("programs", ct.c_int32),
        ("parameters", ct.c_int32), ("inputs", ct.c_int32), ("outputs", ct.c_int32),
        ("flags", ct.c_int32), ("reserved1", ct.c_ssize_t), ("reserved2", ct.c_ssize_t),
        ("delay", ct.c_int32), ("real", ct.c_int32), ("offline", ct.c_int32),
        ("ratio", ct.c_float), ("object", ct.c_void_p), ("user", ct.c_void_p),
        ("id", ct.c_int32), ("version", ct.c_int32), ("replace", Process),
        ("double", ct.c_void_p), ("reserved", ct.c_char * 56),
    ]


@Dispatch
def host(_effect, opcode, _index, _value, _pointer, _option):
    return 2400 if opcode == 1 else 0


def process(effect, channels, block_size=157, in_place=False):
    frames = len(channels[0])
    in_buffers = [(ct.c_float * frames)(*channel) for channel in channels]
    out_buffers = in_buffers if in_place else [(ct.c_float * frames)() for _ in channels]
    for start in range(0, frames, block_size):
        def planes(buffers):
            return Planes(*(ct.cast(ct.byref(buffer, start * 4), ct.POINTER(ct.c_float))
                            for buffer in buffers))
        effect.contents.replace(effect, planes(in_buffers), planes(out_buffers),
                                min(block_size, frames - start))
    return [list(buffer) for buffer in out_buffers]


def reset(effect, rate):
    for opcode, value, option in [(72, 0, 0), (12, 0, 0), (10, 0, rate),
                                   (11, 512, 0), (12, 1, 0), (71, 0, 0)]:
        effect.contents.dispatch(effect, opcode, 0, value, None, option)
    assert effect.contents.delay == math.floor(rate * 0.005 + 0.5)
    return effect.contents.delay


def set_target(view, encoded):
    struct.pack_into("<i", view, 0, encoded)
    view.flush()


def amplitude(db):
    return 10 ** (db / 20)


def level(samples):
    return 10 * math.log10(sum(sample * sample for sample in samples) / len(samples))


def tone(rate, db, seconds):
    peak = math.sqrt(2) * amplitude(db)
    return [ct.c_float(peak * math.sin(2 * math.pi * 440 * i / rate)).value
            for i in range(round(rate * seconds))]


def stereo(sample, right_gain=0.0):
    return [sample, [value * right_gain for value in sample]] + [[0.0] * len(sample)] * 6


def check_speech(effect, shared, speech_path):
    with wave.open(str(speech_path), "rb") as speech:
        assert speech.getnchannels() == 1 and speech.getsampwidth() == 2
        rate = speech.getframerate()
        pcm = array("h", speech.readframes(speech.getnframes()))
    samples = [item / 32768 for item in pcm]
    block_size = rate // 4
    active = [
        start for start in range(0, len(samples) - block_size, block_size)
        if sum(sample * sample for sample in samples[start:start + block_size]) / block_size > 1e-4
    ]
    assert len(active) > 8
    start_at = active[0]
    end_at = active[-1] + block_size
    samples = samples[start_at:end_at]
    active = [index - start_at for index in active]
    reference = level([sample for index in active for sample in samples[index:index + block_size]])
    detected = []
    set_target(shared, 152)
    for input_level in (-36, -14):
        delay = reset(effect, rate)
        gain = amplitude(input_level - reference)
        sample = [value * gain for value in samples] * 2
        output = process(effect, stereo(sample))[0]
        offset = len(samples) + delay
        active_output = [value for start in active[:-1]
                         for value in output[offset + start:offset + start + block_size]]
        detected.append(level(active_output))
    assert detected[0] - (-36) > 6, detected
    assert (-14) - detected[1] > 4, detected
    assert abs(detected[1] - detected[0]) < 12, detected
    print(f"Speech sample: -36 / -14 dBFS input -> {detected[0]:.1f} / {detected[1]:.1f} dBFS")


def main(delivery, staging, speech_path=None):
    staging.mkdir(parents=True, exist_ok=True)
    shutil.copy2(delivery / "VolumeLeveler.dll", staging / "VolumeLeveler.dll")
    settings = staging / "VolumeLeveler.settings"
    settings.write_bytes(struct.pack("<i", 152))
    dll = ct.CDLL(str(staging / "VolumeLeveler.dll"))
    factory = dll.VSTPluginMain
    factory.argtypes = [Dispatch]
    factory.restype = ct.POINTER(Effect)
    effect = factory(host)
    assert effect and effect.contents.magic == 0x56737450
    assert effect.contents.inputs == effect.contents.outputs == 8
    assert effect.contents.flags & 16
    try:
        with settings.open("r+b") as file, mmap.mmap(file.fileno(), 4) as shared:
            for rate in (8000, 44100, 48000, 96000, 384000):
                set_target(shared, 152)
                delay = reset(effect, rate)
                dry = tone(rate, -24, 0.35)
                result = process(effect, stereo(dry), in_place=True)
                assert result[0] == [0.0] * delay + dry[:-delay]

                reset(effect, rate)
                impulse = [1.5] + [0.0] * delay
                result = process(effect, stereo(impulse))
                assert abs(result[0][delay] - amplitude(-3)) < 1e-6
                print(f"{rate} Hz: delay {delay} samples, dead band and -3 dBFS peak protection OK")

            rate = 48000
            reset(effect, rate)
            quiet = tone(rate, -38, 4)
            soft = process(effect, stereo(quiet, 0.25))
            soft_level = level(soft[0][-rate // 2:])
            assert -27 < soft_level < -26, soft_level
            assert all(abs(a * 0.25 - b) < 1e-7 for a, b in zip(*soft[:2]))

            loud = tone(rate, -12, 4)
            loud[0] = 1.8
            result = process(effect, stereo(loud))
            loud_level = level(result[0][-rate // 2:])
            assert -22.1 < loud_level < -21.9, loud_level
            assert max(map(abs, result[0])) <= amplitude(-3) + 1e-7
            quiet_again = process(effect, stereo(quiet))[0]
            assert -28 < level(quiet_again[-rate // 2:]) < -26
            print(f"Same stream, soft -> loud -> soft: {soft_level:.2f} / {loud_level:.2f} / "
                  f"{level(quiet_again[-rate // 2:]):.2f} dBFS")

            set_target(shared, 146)
            louder_target = process(effect, stereo(quiet))[0]
            assert level(louder_target[-rate // 2:]) > soft_level + 2
            # A quiet noise floor must return to unity after previously boosted content.
            noise = tone(rate, -60, 2)
            noise_output = process(effect, stereo(noise))[0]
            assert abs(level(noise_output[-rate // 2:]) + 60) < 0.1
            silence = process(effect, stereo([0.0] * rate))[0]
            assert not any(silence[-rate // 2:])
            print("Live target, noise gate and silence OK")

            process(effect, stereo(quiet))
            set_target(shared, 18)
            bypass = [ct.c_float(0.63).value] * (rate * 2)
            result = process(effect, stereo(bypass))[0]
            assert max(map(abs, result)) <= amplitude(-3) + 1e-7
            assert result[-rate // 2:] == bypass[-rate // 2:]
            print("Bypass transition and dry signal OK")

            if speech_path:
                check_speech(effect, shared, speech_path)
    finally:
        effect.contents.dispatch(effect, 1, 0, 0, None, 0)


if __name__ == "__main__":
    main(*(pathlib.Path(arg).resolve() for arg in sys.argv[1:]))
