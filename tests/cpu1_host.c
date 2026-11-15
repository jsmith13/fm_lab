/* SPDX-License-Identifier: GPL-3.0-only */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <stdatomic.h>
#define DX_LG_N 6
#include "dx7_voice.h"
#include "dx7_core.c"
#define FM1_TICKS_PER_US 24u
#define FM1_CPU1_TIMEOUT_US 600u
static atomic_uint ticks;
static uint32_t fm1_ticks(void) { return atomic_fetch_add(&ticks,1); }
static uint32_t fm1_cpu1_on, fm1_cpu1_gave_up;
static uint32_t mode, partial, jobs, failed;
static pthread_t thread;
static void (*work)(uint32_t);
static void *host_work(void *arg);
static void fm1_cpu1_check(void) {}
static int fm1_cpu1_run(void (*fn)(uint32_t), uint32_t arg)
{
    int rc;
    (void)arg; work = fn; jobs++;
    if (!mode) { host_work(0); return 1; }
    rc = pthread_create(&thread,0,host_work,0);
    if (rc) { fprintf(stderr,"pthread_create: %d\n",rc); exit(2); }
    return 1;
}
static int fm1_cpu1_wait_until(uint32_t deadline)
{
    int rc;
    (void)deadline;
    if (mode) {
        rc = pthread_join(thread,0);
        if (rc) { fprintf(stderr,"pthread_join: %d\n",rc); exit(2); }
    }
    if (!failed) return 0;
    fm1_cpu1_on = 0; fm1_cpu1_gave_up = 2; return -1;
}
#include "fm_cpu1.c"
void fm_cpu1_reference_compute(void *state, int32_t *out, int32_t pitch, uint32_t os, uint32_t peg_off);
static void *host_work(void *arg)
{
    uint32_t i;
    (void)arg;
    if (jobs != 3u || partial == UINT32_MAX) { work(0); return 0; }
    for (i = 0; i < fm_cpu1_job.count && i < partial; i++)
        fm_cpu1_voice(&fm_cpu1_job.voice[i],fm_cpu1_job.mix,
                      fm_cpu1_job.g0[i],fm_cpu1_job.g1[i],fm_cpu1_job.ge,
                      fm_cpu1_job.pitch,fm_cpu1_job.os,fm_cpu1_job.peg_off,fm_cpu1_job.bus);
    failed = 1;
    return 0;
}

static void serial(dxv_t voices[8], int32_t *out, uint32_t active,
                   const int32_t g0[8], const int32_t g1[8], uint32_t ge, int32_t pitch)
{
    uint32_t i, v, m = DX_N << dx_os, lg = DX_LG_N+dx_os;
    int32_t one[2*DX_N];
    for (i = 0; i < m; i++) out[i] = 0;
    for (v = 0; v < 8u; v++) {
        if (!(active >> v & 1u)) continue;
        if (!ge) { fm_cpu1_reference_compute(&voices[v],out,pitch,dx_os,dx_peg_off); continue; }
        for (i = 0; i < m; i++) one[i] = 0;
        fm_cpu1_reference_compute(&voices[v],one,pitch,dx_os,dx_peg_off);
        {
            int32_t g = g0[v] << lg, dg = g1[v]-g0[v];
            for (i = 0; i < m; i++) {
                out[i] += ((one[i] >> 8)*(g >> lg)) >> 4;
                g += dg;
            }
        }
    }
}

int main(void)
{
    uint32_t alg, count, variant, step, i, comparisons = 0, recoveries = 0;
    for (alg = 0; alg < 32u; alg++)
        for (count = 0; count <= 8u; count++)
            for (variant = 0; variant < 7u; variant++) {
                dxv_t reference[8] = {0}, candidate[8];
                uint8_t patch[DX_VSZ];
                dx_init_voice(patch);
                patch[134] = alg; patch[135] = alg%8u; patch[142] = alg%6u;
                for (i = 0; i < 6u; i++) {
                    patch[21u*i+16u] = 70;
                    patch[21u*i+18u] = i+1u;
                    patch[DX_WAVE+i] = (alg+i)%8u;
                }
                for (i = 0; i < count; i++) dx_init(&reference[i],patch,48u+i,100);
                memcpy(candidate,reference,sizeof candidate);
                mode = variant != 0; partial = variant < 2u ? UINT32_MAX : variant-2u;
                fm1_cpu1_on = fm_cpu1_enabled = 1; jobs = failed = fm_cpu1_failures = 0;
                for (step = 0; step < 24u; step++) {
                    int32_t a[2*DX_N], b[2*DX_N], g0[8], g1[8];
                    uint32_t ge = step%3u != 0;
                    int32_t pitch = ((int32_t)(step%5u)-2)*65536;
                    dx_os = step&1u; dx_peg_off = (step/2u)&1u;
                    for (i = 0; i < count; i++) {
                        if (step == 4u) { dx_keyup(&reference[i]); dx_keyup(&candidate[i]); }
                        if (step == 8u) {
                            dx_init(&reference[i],patch,60u+i,110);
                            dx_init(&candidate[i],patch,60u+i,110);
                        }
                        if (step == 12u) {
                            dx_retune(&reference[i],patch,64u+i,90);
                            dx_retune(&candidate[i],patch,64u+i,90);
                        }
                        if (step == 16u) {
                            dx_update(&reference[i],patch,64u+i,90);
                            dx_update(&candidate[i],patch,64u+i,90);
                        }
                        reference[i].bright = candidate[i].bright = ((int32_t)(step%3u)-1)*65536;
                        reference[i].controller_vibrato = candidate[i].controller_vibrato = step*65536;
                        reference[i].controller_bias = candidate[i].controller_bias = step*32768;
                        reference[i].controller_mutes = candidate[i].controller_mutes = step%64u;
                    }
                    for (i = 0; i < 8u; i++) {
                        g0[i] = (step*347u+i*113u)%4096u;
                        g1[i] = (step*619u+i*97u)%4096u;
                    }
                    /* Include late/exact admission boundaries and wrap without changing timing limits. */
                    atomic_store(&ticks,step%6u == 0 ? 0xfffffff0u : 0);
                    fm_cpu1_begin(fm1_ticks(),256);
                    if (step%6u == 1) fm_cpu1_deadline = atomic_load(&ticks)+599u*24u;
                    if (step%6u == 2) fm_cpu1_deadline = atomic_load(&ticks)+600u*24u;
                    serial(reference,a,(1u << count)-1u,g0,g1,ge,pitch);
                    fm_cpu1_render(candidate,b,(1u << count)-1u,g0,g1,ge,pitch,dx_os,dx_peg_off);
                    if (memcmp(a,b,(DX_N << dx_os)*sizeof *a) ||
                        memcmp(reference,candidate,sizeof reference)) {
                        fprintf(stderr,"FM mismatch alg=%u voices=%u variant=%u step=%u\n",alg,count,variant,step);
                        return 1;
                    }
                    comparisons++;
                }
                if (fm_cpu1_failures) {
                    if (fm_cpu1_failures != 1 || fm1_cpu1_on) return 1;
                    recoveries++;
                }
            }
    if (!recoveries || !fm_cpu1_skipped || !fm_cpu1_jobs) return 1;
    printf("FM CPU1: %u output/full-state comparisons, %u retirement recoveries; all algorithms, waves, rates, envelopes and late admission passed\n",
           comparisons,recoveries);
    return 0;
}
