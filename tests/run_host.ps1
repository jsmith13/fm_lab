# SPDX-License-Identifier: GPL-3.0-only
# Run firmware/src/fmlab.c on Windows with MSVC (tests/run_host.sh is the cc/sh version).
#   pwsh tests\run_host.ps1 [script.txt]
# Out: build\host\<script>.wav and build\host\<script>-<ms>.ppm ("fmlab" with no script).
$ErrorActionPreference = 'Stop'
Set-Location (Join-Path $PSScriptRoot '..')
New-Item -ItemType Directory -Force -Path build\host | Out-Null

$vcvars = Get-ChildItem 'C:\Program Files*\Microsoft Visual Studio\*\*\VC\Auxiliary\Build\vcvars64.bat' |
          Select-Object -First 1 -ExpandProperty FullName
if (-not $vcvars) { throw 'vcvars64.bat not found: install the Visual Studio Build Tools (C++ workload).' }

$ver = if ($env:APP_VER) { $env:APP_VER } else { 'DEV' }
# msvc.h is force-included: cl is not gcc, and the host half of platform.c still carries one gcc attribute.
$cl = "cl /nologo /TC /O2 /W3 /DDINGHY_HOST /DAPP_VER=\""$ver\"" /Itests /FImsvc.h " +
      "/Ifirmware\src /Ifirmware\gen firmware\src\fmlab.c /Fo:build\host\ /Fe:build\host\fmlab.exe"
& $env:ComSpec /c "call `"$vcvars`" >nul && $cl"
if ($LASTEXITCODE) { throw 'build failed' }
if ($args.Count -and [IO.Path]::GetFileName($args[0]) -in 'controllers.txt', 'diagnostics.txt') {
    $unit = 'cl /nologo /TC /O2 /W3 /Itests /FImsvc.h /Ifirmware\src /Ifirmware\gen ' +
            'tests\midi_host.c /Fo:build\host\midi_host.obj /Fe:build\host\midi_host.exe'
    & $env:ComSpec /c "call `"$vcvars`" >nul && $unit && build\host\midi_host.exe"
    if ($LASTEXITCODE) { throw 'MIDI controller unit checks failed' }
}

# a script's output is named after it, so the runs do not overwrite each other
$out = 'build\host\fmlab'
if ($args.Count) { $out = Join-Path 'build\host' ([IO.Path]::GetFileNameWithoutExtension($args[0])) }
Remove-Item "$out-*.ppm" -ErrorAction SilentlyContinue
# the bank file outlives a run, the way the flash sector does: clear it so a run starts from nothing
Remove-Item "$out-bank.bin", "$out-out.syx" -ErrorAction SilentlyContinue

if ($args.Count -and [IO.Path]::GetFileName($args[0]) -in 'bank.txt', 'alias.txt') {
    python tests\mksyx.py build\host          # the .syx files the script sends itself
    if ($LASTEXITCODE) { throw 'could not make the test .syx files' }
}

& build\host\fmlab.exe $out @args | Tee-Object "$out.log"
if ($LASTEXITCODE) { throw 'run failed' }
if ($args.Count -and [IO.Path]::GetFileName($args[0]) -eq 'controllers.txt') {
    python tests\controllercheck.py $out
    if ($LASTEXITCODE) { throw 'MIDI controller audio/editor checks failed' }
}
if ($args.Count -and [IO.Path]::GetFileName($args[0]) -eq 'diagnostics.txt') {
    python tests\diagnosticcheck.py $out
    if ($LASTEXITCODE) { throw 'GLOBAL diagnostic UI/audio checks failed' }
}

if ($args.Count -and [IO.Path]::GetFileName($args[0]) -eq 'operators.txt') {
    python tests\wavcheck.py "$out.wav"      # the switches have to agree with the sound
    if ($LASTEXITCODE) { throw 'the sound does not match the operator switches' }
}
if ($args.Count -and [IO.Path]::GetFileName($args[0]) -eq 'bank.txt') {
    python tests\syxcheck.py $out            # and what was stored with what was sent
    if ($LASTEXITCODE) { throw 'the bank does not match what was sent to it' }
}
if ($args.Count -and [IO.Path]::GetFileName($args[0]) -eq 'panel.txt') {
    python tests\panelcheck.py $out          # and the panel with what it is supposed to do
    if ($LASTEXITCODE) { throw 'the panel does not behave' }
}
if ($args.Count -and [IO.Path]::GetFileName($args[0]) -eq 'spectrum.txt') {
    python tests\speccheck.py $out           # and the analyser with the arithmetic
    if ($LASTEXITCODE) { throw 'the spectrum is not where it should be' }
}
if ($args.Count -and [IO.Path]::GetFileName($args[0]) -eq 'levels.txt') {
    python tests\levelcheck.py $out          # and the operator boxes with the envelopes
    if ($LASTEXITCODE) { throw 'the operator boxes do not follow the sound' }
}
if ($args.Count -and [IO.Path]::GetFileName($args[0]) -eq 'edit.txt') {
    python tests\editcheck.py $out           # and RATIO with the bytes underneath it
    if ($LASTEXITCODE) { throw 'the ratio does not match the bytes it writes' }
}
if ($args.Count -and [IO.Path]::GetFileName($args[0]) -eq 'home.txt') {
    python tests\homecheck.py $out           # and the four knobs HOME puts under your fingers
    if ($LASTEXITCODE) { throw 'the knobs on HOME are not the operators they name' }
}
if ($args.Count -and [IO.Path]::GetFileName($args[0]) -eq 'env.txt') {
    python tests\envcheck.py $out            # and ADSR against the eight bytes it writes
    if ($LASTEXITCODE) { throw 'ADSR does not match the segments underneath it' }
}
if ($args.Count -and [IO.Path]::GetFileName($args[0]) -eq 'focus.txt') {
    python tests\focuscheck.py $out          # and a tap of EDIT or ENV against a hold of it
    if ($LASTEXITCODE) { throw 'a tap and a hold are not telling themselves apart' }
}
if ($args.Count -and [IO.Path]::GetFileName($args[0]) -eq 'fast.txt') {
    python tests\fastcheck.py $out           # and four clicks against four clicks with SEL held
    if ($LASTEXITCODE) { throw 'SEL is not shifting the knobs' }
}
if ($args.Count -and [IO.Path]::GetFileName($args[0]) -eq 'global.txt') {
    python tests\globalcheck.py $out         # and the one envelope over all six operators
    if ($LASTEXITCODE) { throw 'the global envelope is not what it should be' }
}
if ($args.Count -and [IO.Path]::GetFileName($args[0]) -eq 'lfo.txt') {
    python tests\lfocheck.py $out            # and the depth and the sensitivity it needs
    if ($LASTEXITCODE) { throw 'the LFO is not what it should be' }
}
if ($args.Count -and [IO.Path]::GetFileName($args[0]) -eq 'play.txt') {
    python tests\playcheck.py $out           # the arpeggiator and the sequencer, heard
    if ($LASTEXITCODE) { throw 'the arpeggiator or the sequencer is not what it should be' }
}
if ($args.Count -and [IO.Path]::GetFileName($args[0]) -eq 'midi.txt') {
    python tests\midicheck.py $out           # the channel, and program change
    if ($LASTEXITCODE) { throw 'MIDI is not what it should be' }
}
if ($args.Count -and [IO.Path]::GetFileName($args[0]) -eq 'pitch.txt') {
    python tests\pitchcheck.py $out          # the PIT key, and the pitch envelope it opens
    if ($LASTEXITCODE) { throw 'the pitch envelope is not what it should be' }
}
if ($args.Count -and [IO.Path]::GetFileName($args[0]) -eq 'fx.txt') {
    python tests\fxcheck.py $out             # the filter and the effects' knobs
    if ($LASTEXITCODE) { throw 'the filter is not what it should be' }
}
if ($args.Count -and [IO.Path]::GetFileName($args[0]) -eq 'effects.txt') {
    python tests\effectscheck.py $out        # chorus, phaser, delay and reverb, heard
    if ($LASTEXITCODE) { throw 'the effects are not what they should be' }
}
if ($args.Count -and [IO.Path]::GetFileName($args[0]) -eq 'waves.txt') {
    python tests\wavescheck.py $out          # the operator waves, heard, drawn and saved
    if ($LASTEXITCODE) { throw 'the operator waves are not what they should be' }
}
if ($args.Count -and [IO.Path]::GetFileName($args[0]) -eq 'scope.txt') {
    python tests\scopecheck.py $out          # the waveform screen, by its trace
    if ($LASTEXITCODE) { throw 'the waveform screen is not what it should be' }
}
if ($args.Count -and [IO.Path]::GetFileName($args[0]) -eq 'corners.txt') {
    python tests\cornercheck.py $out         # and the battery and the pips at the top
    if ($LASTEXITCODE) { throw 'the corners are not saying what they should' }
}
if ($args.Count -and [IO.Path]::GetFileName($args[0]) -eq 'depth.txt') {
    python tests\depthcheck.py build\host       # and the 24 bits the DAC is given
    if ($LASTEXITCODE) { throw 'the sound is not reaching the DAC with its 24 bits' }
}
if ($args.Count -and [IO.Path]::GetFileName($args[0]) -eq 'alias.txt') {
    python tests\aliascheck.py build\host       # and 88.2K, against what folds back
    if ($LASTEXITCODE) { throw 'SAMPLE RATE 88.2K is not keeping the fold-back out' }
}
if ($args.Count -and [IO.Path]::GetFileName($args[0]) -eq 'bits.txt') {
    python tests\bitscheck.py build\host        # and BIT DEPTH, against its grid
    if ($LASTEXITCODE) { throw 'BIT DEPTH is not cutting the voices to its bits' }
}
if ($args.Count -and [IO.Path]::GetFileName($args[0]) -eq 'mono.txt') {
    python tests\monocheck.py build\host        # and OUTPUT, stereo and mono
    if ($LASTEXITCODE) { throw 'OUTPUT MONO is not putting the same sound on both sides' }
}
if ($args.Count -and [IO.Path]::GetFileName($args[0]) -eq 'srate.txt') {
    python tests\sratecheck.py build\host       # and SAMPLE RATE, held at each step
    if ($LASTEXITCODE) { throw 'SAMPLE RATE is not holding the voices at its rate' }
}
