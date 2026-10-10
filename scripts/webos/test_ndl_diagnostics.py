#!/usr/bin/env python3
"""Preprocessor/build-gate fixture; no decoder/media or device execution."""
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
MODULE = ROOT / 'third_party/ss4s/modules/webos/ndl/webos5'
CASES = {
    'ndl_player.c': {
        'diagnostic': ['"/tmp/aurora-dropthresh"', '"/tmp/aurora-ndl-load.log"', 'ApplyFrameDropThreshold', 'NDL_DirectVideoSetFrameDropThreshold'],
        'ordinary': ['NDL_DirectMediaInit', 'NDL_DirectMediaLoad(&info, LoadCallback)', 'NDL_DirectAudioPlay', 'context->mediaLoaded = true'],
    },
    'ndl_video.c': {
        'diagnostic': ['"/tmp/aurora-agetrace"', '"/tmp/aurora-ndl-trace.csv"', 'ndl_trace_row', 'ndl_trace_f'],
        'ordinary': ['NDL_DirectVideoPlay', 'NDL_DirectVideoGetRenderBufferLength', 'VideoStats.ReportFrame', 'context->lastFrameTime = now', 'float latency = bufLen'],
    },
}
for filename, tokens in CASES.items():
    # Preserve the whole actual translation unit and its conditional code. Removing
    # includes avoids pretending this fixture is a full vendor SDK compile/link.
    source = re.sub(r'^\s*#\s*include[^\n]*', '', (MODULE / filename).read_text(), flags=re.M)
    for diagnostic in [False, True]:
        command = ['cc', '-E', '-P', '-x', 'c', '-']
        if diagnostic:
            command.insert(1, '-DAURORA_NDL_DIAGNOSTICS=1')
        output = subprocess.run(command, input=source, text=True, capture_output=True, check=True).stdout
        for token in tokens['diagnostic']:
            assert (token in output) == diagnostic, (filename, diagnostic, token)
        for token in tokens['ordinary']:
            assert token in output, (filename, diagnostic, token)
        print('PASS', filename, 'diagnostic' if diagnostic else 'release', 'probe gate and ordinary media/statistics preserved')

cmake = (ROOT / 'CMakeLists.txt').read_text()
pattern = r'    # Separate NDL module: developer filesystem probes never ship in release\.\n(    if \(TARGET ss4s-module-ndl-webos5.*?    endif \(\))'
block = re.search(pattern, cmake, flags=re.S)
assert block, 'Missing actual module-only CMake gate'
with tempfile.TemporaryDirectory(prefix='eclipse-ndl-gate-') as directory:
    root = Path(directory)
    (root / 'CMakeLists.txt').write_text('cmake_minimum_required(VERSION 3.20)\nproject(gate NONE)\nadd_library(ss4s-module-ndl-webos5 INTERFACE)\n' + block[1].replace(' PRIVATE ', ' INTERFACE ') + '\nget_target_property(result ss4s-module-ndl-webos5 INTERFACE_COMPILE_DEFINITIONS)\nfile(WRITE "${CMAKE_BINARY_DIR}/definitions.txt" "${result}")\n')
    for index, (app, vendor, expected) in enumerate([
        ('com.aurora.gamestream', 'OFF', False),
        ('com.eclipse.release.clienttest', 'OFF', False),
        ('com.aurora.gamestream.beta', 'OFF', True),
        ('com.aurora.gamestream', 'ON', True),
    ]):
        build = root / str(index)
        subprocess.run(['cmake', '-S', str(root), '-B', str(build), '-DWEBOS_APPINFO_ID='+app, '-DAURORA_VENDOR_DIAG='+vendor], check=True, capture_output=True, text=True)
        definitions = (build / 'definitions.txt').read_text()
        assert ('AURORA_NDL_DIAGNOSTICS=1' in definitions) == expected, (app, vendor, definitions)
        print('PASS actual CMake gate', app, vendor, expected)
print('PASS diagnostic-only NDL probe gate; full SDK compile/native media remain separate')
