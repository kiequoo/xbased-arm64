/*
 * xbased native core — the GP's DSP56300 program compiled to x86-64
 * (dsp_rec.cpp), word for word what the interpreter (dsp_cpu.c) does.
 */
#ifndef DSP_REC_H
#define DSP_REC_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct DSPState DSPState;
typedef struct dsp_core_s dsp_core_t;

/* dsp_run's work on the compiled program; false: not available here (no
 * executable memory), the interpreter runs it */
bool dsp_rec_run(DSPState *dsp, int cycles);
/* the recompiler runs this DSP's program (not off, executable memory) */
bool dsp_rec_ready(DSPState *dsp);
/* its executable memory there (dsp_rec.cpp) */
bool dsp_rec_available(DSPState *dsp);
/* the program changed under it (a savestate, a reset): compiled again */
void dsp_rec_invalidate(DSPState *dsp);
void dsp_rec_destroy(DSPState *dsp);
/* dev: the counters, logged once a second (XBASED_APU_STATS) */
void dsp_rec_frame_stats(DSPState *dsp);
/* random programs on both, compared word for word: the differences found
 * (logged), -1 when it can't run */
int dsp_rec_selftest(int programs, uint32_t seed);
/* dsp_run's loop on the interpreter alone */
void dsp_run_interp(DSPState *dsp, int cycles);

/* the interpreter's pieces it calls (dsp_cpu.c) */
void *dsp56k_rec_alu_fn(int op);
void dsp56k_rec_alu_sm(dsp_core_t *dsp, int op);
int dsp56k_rec_opcode_count(void);
const char *dsp56k_rec_opcode_template(int i);
const char *dsp56k_rec_opcode_name(int i);
int dsp56k_rec_opcode_any(uint32_t op);
int dsp56k_rec_opcode_index(uint32_t op);
void dsp56k_rec_update_rn(dsp_core_t *dsp, uint32_t numreg, uint32_t modifier);
int dsp56k_rec_read_accu24(dsp_core_t *dsp, int numreg, uint32_t *dest);
void dsp56k_rec_stack_push(dsp_core_t *dsp, uint32_t curpc, uint32_t cursr);
uint32_t dsp56k_rec_stack_pop_pc(dsp_core_t *dsp);
void dsp56k_rec_post_interrupts(dsp_core_t *dsp);
void dsp56k_rec_loop_check(dsp_core_t *dsp);
void dsp56k_rec_loop_end(dsp_core_t *dsp);
void dsp56k_rec_write_reg(dsp_core_t *dsp, uint32_t numreg, uint32_t value);
int dsp56k_rec_register_mask(int numreg);

#ifdef __cplusplus
}
#endif

#endif
