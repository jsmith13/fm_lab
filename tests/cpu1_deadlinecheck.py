#!/usr/bin/env python3
"""Compile the target's actual wait function against a deterministic register/timer model."""
from pathlib import Path
import subprocess

root = Path(__file__).resolve().parents[1]
source = (root / "firmware" / "hal" / "fm1_cpu1.h").read_text()
start = source.index("static int fm1_cpu1_wait_until(")
brace = source.index("{", start)
depth = 1
end = brace + 1
while depth:
    depth += (source[end] == "{") - (source[end] == "}")
    end += 1
function = source[start:end]
fixture = r"""
#include <stdint.h>
#include <stdio.h>
#define FM1_CPU1_SYNC() ((void)0)
#define FM1_CPU1_TIMEOUT_US 600u
#define FM1_TICKS_PER_US 24u
enum { FM1_CPU1_MISREAD=1, FM1_CPU1_LATE=2, FM1_CPU1_FAULT=4, FM1_CPU1_DEADLINE=7 };
static uint32_t fm1_cpu1_on, fm1_cpu1_requested_at, now, reason;
static uint32_t fm1_cpu1_late_age;
static int32_t fm1_cpu1_late_left;
static struct { uint32_t count; } fm1_cpu1_fault;
static struct { uint32_t rejected, complete, request; } fm1_cpu1_mb;
static uint32_t fm1_ticks(void) { return now++; }
static void fm1_cpu1_retire(uint32_t why) { reason=why; fm1_cpu1_on=0; }
"""
main = r"""
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"deadline line %d\n",__LINE__); return 1; } } while (0)
static void reset(void) {
    fm1_cpu1_on=1; now=reason=fm1_cpu1_requested_at=0;
    fm1_cpu1_late_age=0; fm1_cpu1_late_left=0;
    fm1_cpu1_fault.count=fm1_cpu1_mb.rejected=fm1_cpu1_mb.complete=0;
    fm1_cpu1_mb.request=1;
}
int main(void) {
    reset(); now=700*24; fm1_cpu1_mb.complete=1; CHECK(!fm1_cpu1_wait_until(100) && !reason);
    reset(); now=700*24; CHECK(fm1_cpu1_wait_until(100000)==-1 && reason==FM1_CPU1_LATE);
    CHECK(fm1_cpu1_late_age==700*24 && fm1_cpu1_late_left==100000-700*24);
    reset(); CHECK(fm1_cpu1_wait_until(12)==-1 && reason==FM1_CPU1_DEADLINE && now<20);
    CHECK(fm1_cpu1_late_age==12 && fm1_cpu1_late_left==0);
    reset(); now=0xfffffff0u; fm1_cpu1_requested_at=now;
    CHECK(fm1_cpu1_wait_until(12)==-1 && reason==FM1_CPU1_DEADLINE && now<20);
    CHECK(fm1_cpu1_late_age==28 && fm1_cpu1_late_left==0);
    reset(); now=0xfffffff0u; fm1_cpu1_requested_at=now-700*24;
    CHECK(fm1_cpu1_wait_until(now+100000)==-1 && reason==FM1_CPU1_LATE);
    CHECK(fm1_cpu1_late_age==700*24 && fm1_cpu1_late_left==100000);
    reset(); now=700*24;
    CHECK(fm1_cpu1_wait_until(1)==-1 && reason==FM1_CPU1_LATE && fm1_cpu1_late_left<0);
    reset(); fm1_cpu1_fault.count=1;
    CHECK(fm1_cpu1_wait_until(100)==-1 && reason==FM1_CPU1_FAULT);
    reset(); fm1_cpu1_mb.rejected=1;
    CHECK(fm1_cpu1_wait_until(100)==-1 && reason==FM1_CPU1_MISREAD);
    reset(); fm1_cpu1_on=0; CHECK(fm1_cpu1_wait_until(100)==-1 && !reason);
    puts("WT CPU1 target wait: distinct timeout reasons, captured age/slack, completion, wrap, fault and rejection passed");
    return 0;
}
"""
out = root / "build" / "cpu1-tests"
out.mkdir(parents=True, exist_ok=True)
path = out / "deadline-model.c"
path.write_text(fixture + function + main)
subprocess.run(["gcc", "-std=c11", "-O1", "-Wall", "-Werror", str(path), "-o", str(out / "deadline-model")],
               check=True)
subprocess.run([str(out / "deadline-model")], check=True)
