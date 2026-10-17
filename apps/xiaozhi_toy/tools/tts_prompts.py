"""
Synthesize the device's own Chinese voice prompts (those xiaozhi-esp32 does not have) into
assets/sounds/*.ogg, then run gen_prompts.py to embed them.

    python tools/tts_prompts.py [--all]  (Windows only, needs: pip install soundfile numpy)

Only prompts whose .ogg is missing are made (the Ogg serial number is random, so re-encoding
changes every file); --all regenerates everything, e.g. after changing the voice.
    python tools/gen_prompts.py

Voice: Windows built-in "Microsoft Huihui Desktop" (zh-CN, System.Speech), offline.
Format: Ogg/Opus, 16 kHz mono, loudness matched to the xiaozhi-esp32 prompts.
"""

import os
import sys
import subprocess
import tempfile

import numpy as np
import soundfile as sf

APP_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SOUND_DIR = os.path.join(APP_DIR, 'assets', 'sounds')

PROMPTS = {
    'charging.ogg': '开始充电',
    'charger_removed.ogg': '充电器已拔出',
    # provisioning: "请用手机连接热点，小智杠" + the SSID suffix spelled out (Xiaozhi-5C90)
    'connect_ap.ogg': '请用手机连接热点，小智杠',
    # network
    'net_lost.ogg': '网络已断开',
    'net_restored.ogg': '网络已恢复',
    'net_failed.ogg': '网络连接失败',
}
# single characters of the SSID suffix (MAC hex digits), same voice as connect_ap
for _i, _t in enumerate('零一二三四五六七八九'):
    PROMPTS[f'ssid_{_i}.ogg'] = _t
for _c in 'ABCDEF':
    PROMPTS[f'ssid_{_c.lower()}.ogg'] = _c

VOICE = 'Microsoft Huihui Desktop'
RATE = 16000
REFERENCE = 'wificonfig.ogg'  # a xiaozhi-esp32 voice prompt to match the loudness of

PS_TEMPLATE = r'''
Add-Type -AssemblyName System.Speech
$s = New-Object System.Speech.Synthesis.SpeechSynthesizer
$s.SelectVoice('{voice}')
$s.Rate = 0
$s.Volume = 100
$fmt = New-Object System.Speech.AudioFormat.SpeechAudioFormatInfo({rate}, [System.Speech.AudioFormat.AudioBitsPerSample]::Sixteen, [System.Speech.AudioFormat.AudioChannel]::Mono)
$s.SetOutputToWaveFile('{wav}', $fmt)
$s.Speak([IO.File]::ReadAllText('{txt}', [Text.Encoding]::UTF8))
$s.Dispose()
'''


def synthesize(text, wav):
    with tempfile.NamedTemporaryFile('w', suffix='.txt', delete=False, encoding='utf-8') as f:
        f.write(text)
        txt = f.name
    try:
        script = PS_TEMPLATE.format(voice=VOICE, rate=RATE, wav=wav, txt=txt)
        subprocess.run(['powershell', '-NoProfile', '-NonInteractive', '-Command', script], check=True)
    finally:
        os.remove(txt)


def active_rms(x):
    """RMS over the samples that are not silence."""
    loud = np.abs(x) > 0.02
    return float(np.sqrt(np.mean(x[loud] ** 2))) if loud.any() else 0.0


def trim(x, thr=0.01, pad=int(0.05 * RATE)):
    idx = np.where(np.abs(x) > thr)[0]
    if len(idx) == 0:
        return x
    return x[max(0, idx[0] - pad):min(len(x), idx[-1] + pad)]


def main():
    sys.stdout.reconfigure(encoding='utf-8')
    ref, ref_rate = sf.read(os.path.join(SOUND_DIR, REFERENCE), dtype='float32')
    target = active_rms(ref)
    print(f'reference {REFERENCE}: {ref_rate} Hz, active rms {target:.3f}')

    regen_all = '--all' in sys.argv[1:]
    for name, text in PROMPTS.items():
        if not regen_all and os.path.exists(os.path.join(SOUND_DIR, name)):
            continue
        wav = os.path.join(tempfile.gettempdir(), 'xz_tts_' + name.replace('.ogg', '.wav'))
        synthesize(text, wav)
        x, rate = sf.read(wav, dtype='float32')
        os.remove(wav)
        assert rate == RATE, rate
        # single characters get more room so a spelled out SSID is easy to follow
        x = trim(x, pad=int((0.12 if name.startswith('ssid_') else 0.05) * RATE))
        # match the reference loudness, but never push the peak above -0.5 dBFS (no clipping)
        gain = min(target / max(active_rms(x), 1e-6), 0.95 / max(float(np.max(np.abs(x))), 1e-6))
        x = np.clip(x * gain, -0.98, 0.98)
        out = os.path.join(SOUND_DIR, name)
        sf.write(out, x, RATE, format='OGG', subtype='OPUS')
        print(f'{name}: "{text}", {len(x) / RATE:.2f} s, gain {gain:.2f}, {os.path.getsize(out)} bytes')


if __name__ == '__main__':
    main()
