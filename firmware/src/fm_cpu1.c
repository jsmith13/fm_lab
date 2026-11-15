/* SPDX-License-Identifier: GPL-3.0-only */
/* Allocation, controllers and global envelopes stay on CPU0; only copied DX voice state is shared. */
typedef struct {
    dxv_t voice[4];
    uint32_t index[4], count, os, peg_off, ge;
    int32_t pitch, g0[4], g1[4];
    int32_t mix[2 * DX_N], bus[2][2 * DX_N];
    uint32_t ticks;
} fm_cpu1_job_t;
static fm_cpu1_job_t fm_cpu1_job;
static int32_t fm_cpu1_local_bus[2][2 * DX_N];
static volatile uint32_t fm_cpu1_enabled = 1;
static volatile uint32_t fm_cpu1_jobs, fm_cpu1_failures, fm_cpu1_skipped;
static volatile uint32_t fm_cpu1_dsp_peak, fm_cpu1_wait_peak;
static uint32_t fm_cpu1_deadline;

static void fm_cpu1_begin(uint32_t ticks, uint32_t frames)
{
    uint32_t budget = frames*1000000u/44100u;
    fm1_cpu1_check();
    fm_cpu1_deadline = ticks+(budget > 1250u ? budget-1250u : 0u)*FM1_TICKS_PER_US;
}

static void fm_cpu1_voice(dxv_t *v, int32_t *out, int32_t g0, int32_t g1, uint32_t ge,
                          int32_t pitch, uint32_t os, uint32_t peg_off,
                          int32_t bus[2][2 * DX_N])
{
    int32_t one[2 * DX_N];
    uint32_t i, m = DX_N << os, lg = DX_LG_N+os;
    int32_t lv = dx_lfo_sample(&v->lfo), ld = dx_lfo_delay(&v->lfo);
    if (!ge) {
        dx_compute_state(v,out,lv,ld,pitch,bus,os,peg_off);
        return;
    }
    for (i = 0; i < m; i++) one[i] = 0;
    dx_compute_state(v,one,lv,ld,pitch,bus,os,peg_off);
    {
        int32_t g = g0 << lg, dg = g1-g0;
        for (i = 0; i < m; i++) {
            out[i] += ((one[i] >> 8)*(g >> lg)) >> 4;
            g += dg;
        }
    }
}

static void fm_cpu1_work(uint32_t arg)
{
    fm_cpu1_job_t *job = &fm_cpu1_job;
    uint32_t i, start = fm1_ticks();
    (void)arg;
    for (i = 0; i < job->count; i++)
        fm_cpu1_voice(&job->voice[i],job->mix,job->g0[i],job->g1[i],job->ge,
                      job->pitch,job->os,job->peg_off,job->bus);
    job->ticks = fm1_ticks()-start;
}

static void fm_cpu1_render(dxv_t voices[8], int32_t *out, uint32_t active,
                           const int32_t g0[8], const int32_t g1[8], uint32_t ge,
                           int32_t pitch, uint32_t os, uint32_t peg_off)
{
    fm_cpu1_job_t *job = &fm_cpu1_job;
    uint32_t i, k, mask = 0, ordinal = 0, dispatched = 0, start, us, m = DX_N << os;
    job->count = 0; job->os = os; job->peg_off = peg_off; job->ge = ge; job->pitch = pitch;
    job->ticks = 0;
    for (i = 0; i < m; i++) out[i] = job->mix[i] = 0;
    if (fm_cpu1_enabled && fm1_cpu1_on) {
        for (i = 0; i < 8u; i++) {
            if (!(active >> i & 1u)) continue;
            if (ordinal++ & 1u) {
                k = job->count++;
                job->voice[k] = voices[i]; job->index[k] = i;
                job->g0[k] = g0[i]; job->g1[k] = g1[i]; mask |= 1u << i;
            }
        }
        if (job->count) {
            if ((int32_t)(fm_cpu1_deadline-fm1_ticks()) >
                (int32_t)(FM1_CPU1_TIMEOUT_US*FM1_TICKS_PER_US))
                dispatched = fm1_cpu1_run(fm_cpu1_work,0);
            else fm_cpu1_skipped++;
        }
    }
    if (!dispatched) mask = 0;
    for (i = 0; i < 8u; i++)
        if ((active >> i & 1u) && !(mask >> i & 1u))
            fm_cpu1_voice(&voices[i],out,g0[i],g1[i],ge,pitch,os,peg_off,fm_cpu1_local_bus);
    if (!dispatched) return;
    fm_cpu1_jobs++; start = fm1_ticks();
    if (fm1_cpu1_wait_until(fm_cpu1_deadline)) {
        fm_cpu1_failures++;
        for (k = 0; k < job->count; k++) {
            i = job->index[k];
            fm_cpu1_voice(&voices[i],out,g0[i],g1[i],ge,pitch,os,peg_off,fm_cpu1_local_bus);
        }
    } else {
        for (k = 0; k < job->count; k++) voices[job->index[k]] = job->voice[k];
        for (i = 0; i < m; i++) out[i] += job->mix[i];
    }
    us = (fm1_ticks()-start)/FM1_TICKS_PER_US;
    if (us > fm_cpu1_wait_peak) fm_cpu1_wait_peak = us;
    us = job->ticks/FM1_TICKS_PER_US;
    if (us > fm_cpu1_dsp_peak) fm_cpu1_dsp_peak = us;
}
