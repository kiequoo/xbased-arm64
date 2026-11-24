/*
 * DSP56300 instruction routines
 *
 * Copyright (c) 2015 espes
 *
 * Adapted from Hatari DSP M56001 emulation
 * (C) 2003-2008 ARAnyM developer team
 * Adaption to Hatari (C) 2008 by Thomas Huth
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
*/

typedef void (*emu_func_t)(dsp_core_t* dsp);


static void emu_undefined(dsp_core_t* dsp)
{
    /* xbased: the PC stays, so a frame meets it again and again: logged once */
    static bool said;
    dsp->cur_inst_len = 0;
    if (!__atomic_exchange_n(&said, true, __ATOMIC_RELAXED)) {
        nc_log("xbased-apu", "dsp: 0x%04x: 0x%06x illegal instruction (logged once)",
               dsp->pc, dsp->cur_inst);
    }
    /* Add some artificial CPU cycles to avoid being stuck in an infinite loop */
    dsp->instr_cycle += 100;
    if (dsp->exception_debugging) {
        assert(false);
    }
}


/* xbased: a program's write of SR (all 24 bits, before the 16 SR keeps):
 * bits 16-23 kept in sr_ext, SM (bit 20, arithmetic saturation) emulated;
 * DM (14) FV (16) SA (17) RM (21) aren't, said once when set */
#define DSP_SR_SM_BIT (1u << 20)
static void emu_sr_note(dsp_core_t* dsp, uint32_t value)
{
    static bool said;
    dsp->sr_ext = value & 0xff0000;
    if ((value & 0x234000) && !__atomic_exchange_n(&said, true, __ATOMIC_RELAXED)) {
        nc_log("xbased-apu", "dsp: 0x%04x: SR written with %06x, its DM/FV/SA/RM mode bits aren't "
               "emulated (logged once)", dsp->pc, value);
    }
}

/* SM: a Data ALU result (after rounding) saturated to 48 bits when its bits
 * 55, 48 and 47 differ: $00:7FFFFF:FFFFFF ($00:7FFFFF:000000 when rounded)
 * or $FF:800000:000000; V and L then (V: not representable in 48 bits).
 * v: ext, msp, lsp. Returns the V and L bits to set */
static uint16_t emu_sm_sat3(dsp_core_t* dsp, uint32_t *v, int rounded)
{
    if (!(dsp->sr_ext & DSP_SR_SM_BIT)) {
        return 0;
    }
    const uint32_t b55 = (v[0] >> 7) & 1, b48 = v[0] & 1, b47 = (v[1] >> 23) & 1;
    if (b55 == b48 && b48 == b47) {
        return 0;
    }
    if (b55) {
        v[0] = 0xff; v[1] = 0x800000; v[2] = 0;
    } else {
        v[0] = 0; v[1] = 0x7fffff; v[2] = rounded ? 0 : 0xffffff;
    }
    return (1 << DSP_SR_V) | (1 << DSP_SR_L);
}

static void emu_ccr_update_e_u_n_z(dsp_core_t* dsp, uint32_t reg0, uint32_t reg1, uint32_t reg2);

/* SM on accumulator d's result, its E U N Z then the saturated value's */
static void emu_sm_acc(dsp_core_t* dsp, int d, int rounded)
{
    if (!(dsp->sr_ext & DSP_SR_SM_BIT)) {
        return;
    }
    uint32_t v[3] = {dsp->registers[DSP_REG_A2+d], dsp->registers[DSP_REG_A1+d], dsp->registers[DSP_REG_A0+d]};
    const uint16_t f = emu_sm_sat3(dsp, v, rounded);
    if (!f) {
        return;
    }
    dsp->registers[DSP_REG_A2+d] = v[0];
    dsp->registers[DSP_REG_A1+d] = v[1];
    dsp->registers[DSP_REG_A0+d] = v[2];
    emu_ccr_update_e_u_n_z(dsp, v[0], v[1], v[2]);
    dsp->registers[DSP_REG_SR] |= f;
}

/* the parallel ALU ops SM saturates (the manual: not TFR, Tcc, the
 * logical ones, MAX, CLR, TST; CMP and CMPM saturate their own result):
 * 0 no, 1 the result in d, 2 the rounded result in d */
static int emu_sm_kind(int op)
{
    if (op >= 0x80) {
        return (op & 1) ? 2 : 1;
    }
    if (op >= 0x40) {
        return ((op & 7) == 0 || (op & 7) == 4) ? 1 : 0;
    }
    switch (op) {
    case 0x02: case 0x0a: case 0x06: case 0x0e: /* addr subr */
    case 0x10: case 0x18: case 0x14: case 0x1c: /* add sub */
    case 0x12: case 0x1a: case 0x16: case 0x1e: /* addl subl */
    case 0x20: case 0x28: case 0x30: case 0x38: /* add X/Y */
    case 0x21: case 0x29: case 0x31: case 0x39: /* adc */
    case 0x24: case 0x2c: case 0x34: case 0x3c: /* sub X/Y */
    case 0x25: case 0x2d: case 0x35: case 0x3d: /* sbc */
    case 0x22: case 0x2a: case 0x32: case 0x3a: /* asr asl */
    case 0x26: case 0x2e: case 0x36: case 0x3e: /* abs neg */
        return 1;
    case 0x11: case 0x19:                       /* rnd */
        return 2;
    default:
        return 0;
    }
}

/* after a parallel ALU op the interpreter ran: SM's saturation */
static void emu_alu_sm(dsp_core_t* dsp, int op)
{
    if (dsp->sr_ext & DSP_SR_SM_BIT) {
        const int k = emu_sm_kind(op);
        if (k) {
            emu_sm_acc(dsp, (op >> 3) & 1, k == 2);
        }
    }
}

/**********************************
 *  Effective address calculation
 **********************************/


static void emu_update_rn_bitreverse(dsp_core_t* dsp, uint32_t numreg)
{
    int revbits, i;
    uint32_t value, r_reg;

    /* Check how many bits to reverse */
    value = dsp->registers[DSP_REG_N0+numreg];
    for (revbits=0;revbits<16;revbits++) {
        if (value & (1<<revbits)) {
            break;
        }
    }
    revbits++;

    /* Reverse Rn bits */
    r_reg = dsp->registers[DSP_REG_R0+numreg];
    value = r_reg & (BITMASK(16)-BITMASK(revbits));
    for (i=0;i<revbits;i++) {
        if (r_reg & (1<<i)) {
            value |= 1<<(revbits-i-1);
        }
    }

    /* Increment */
    value++;
    value &= BITMASK(revbits);

    /* Reverse Rn bits */
    r_reg &= (BITMASK(16)-BITMASK(revbits));
    r_reg |= value;

    value = r_reg & (BITMASK(16)-BITMASK(revbits));
    for (i=0;i<revbits;i++) {
        if (r_reg & (1<<i)) {
            value |= 1<<(revbits-i-1);
        }
    }

    dsp->registers[DSP_REG_R0+numreg] = value;
}

static void emu_update_rn_modulo(dsp_core_t* dsp, uint32_t numreg, int16_t modifier)
{
    uint16_t bufsize, modulo, lobound, hibound, bufmask;
    int16_t r_reg, orig_modifier=modifier;

    modulo = dsp->registers[DSP_REG_M0+numreg]+1;
    bufsize = 1;
    bufmask = BITMASK(16);
    while (bufsize < modulo) {
        bufsize <<= 1;
        bufmask <<= 1;
    }

    lobound = dsp->registers[DSP_REG_R0+numreg] & bufmask;
    hibound = lobound + modulo - 1;

    r_reg = (int16_t) dsp->registers[DSP_REG_R0+numreg];

    if (orig_modifier>modulo) {
        while (modifier>bufsize) {
            r_reg += bufsize;
            modifier -= bufsize;
        }
        while (modifier<-bufsize) {
            r_reg -= bufsize;
            modifier += bufsize;
        }
    }

    r_reg += modifier;

    if (orig_modifier!=modulo) {
        if (r_reg>hibound) {
            r_reg -= modulo;
        } else if (r_reg<lobound) {
            r_reg += modulo;
        }
    }

    dsp->registers[DSP_REG_R0+numreg] = ((uint32_t) r_reg) & BITMASK(16);
}

static void emu_update_rn(dsp_core_t* dsp, uint32_t numreg, int16_t modifier)
{
    int16_t value;
    uint16_t m_reg;

    m_reg = (uint16_t) dsp->registers[DSP_REG_M0+numreg];
    if (m_reg == 65535) {
        /* Linear addressing mode */
        value = (int16_t) dsp->registers[DSP_REG_R0+numreg];
        value += modifier;
        dsp->registers[DSP_REG_R0+numreg] = ((uint32_t) value) & BITMASK(16);
    } else if (m_reg == 0) {
        /* Bit reversed carry update */
        emu_update_rn_bitreverse(dsp, numreg);
    } else if (m_reg<=32767) {
        /* Modulo update */
        emu_update_rn_modulo(dsp, numreg, modifier);
    } else {
        /* Undefined */
    }
}

static int emu_calc_ea(dsp_core_t* dsp, uint32_t ea_mode, uint32_t *dst_addr)
{
    uint32_t value, numreg, curreg;

    value = (ea_mode >> 3) & BITMASK(3);
    numreg = ea_mode & BITMASK(3);
    switch (value) {
        case 0:
            /* (Rx)-Nx */
            *dst_addr = dsp->registers[DSP_REG_R0+numreg];
            emu_update_rn(dsp, numreg, -dsp->registers[DSP_REG_N0+numreg]);
            break;
        case 1:
            /* (Rx)+Nx */
            *dst_addr = dsp->registers[DSP_REG_R0+numreg];
            emu_update_rn(dsp, numreg, dsp->registers[DSP_REG_N0+numreg]);
            break;
        case 2:
            /* (Rx)- */
            *dst_addr = dsp->registers[DSP_REG_R0+numreg];
            emu_update_rn(dsp, numreg, -1);
            break;
        case 3:
            /* (Rx)+ */
            *dst_addr = dsp->registers[DSP_REG_R0+numreg];
            emu_update_rn(dsp, numreg, +1);
            break;
        case 4:
            /* (Rx) */
            *dst_addr = dsp->registers[DSP_REG_R0+numreg];
            break;
        case 5:
            /* (Rx+Nx) */
            dsp->instr_cycle += 2;
            curreg = dsp->registers[DSP_REG_R0+numreg];
            emu_update_rn(dsp, numreg, dsp->registers[DSP_REG_N0+numreg]);
            *dst_addr = dsp->registers[DSP_REG_R0+numreg];
            dsp->registers[DSP_REG_R0+numreg] = curreg;
            break;
        case 6:
            /* aa */
            dsp->instr_cycle += 2;
            *dst_addr = read_memory_p(dsp, dsp->pc+1);
            dsp->cur_inst_len++;
            if (numreg != 0) {
                return 1; /* immediate value */
            }
            break;
        case 7:
            /* -(Rx) */
            dsp->instr_cycle += 2;
            emu_update_rn(dsp, numreg, -1);
            *dst_addr = dsp->registers[DSP_REG_R0+numreg];
            break;
    }
    /* address */
    return 0;
}

/**********************************
 *  Condition code test
 **********************************/

static int emu_calc_cc(dsp_core_t* dsp, uint32_t cc_code)
{
    uint16_t value1, value2, value3;

    switch (cc_code) {
        case 0:  /* CC (HS) */
            value1 = dsp->registers[DSP_REG_SR] & (1<<DSP_SR_C);
            return (value1==0);
        case 1: /* GE */
            value1 = (dsp->registers[DSP_REG_SR] >> DSP_SR_N) & 1;
            value2 = (dsp->registers[DSP_REG_SR] >> DSP_SR_V) & 1;
            return ((value1 ^ value2) == 0);
        case 2: /* NE */
            value1 = dsp->registers[DSP_REG_SR] & (1<<DSP_SR_Z);
            return (value1==0);
        case 3: /* PL */
            value1 = dsp->registers[DSP_REG_SR] & (1<<DSP_SR_N);
            return (value1==0);
        case 4: /* NN */
            value1 = (dsp->registers[DSP_REG_SR] >> DSP_SR_Z) & 1;
            value2 = (~(dsp->registers[DSP_REG_SR] >> DSP_SR_U)) & 1;
            value3 = (~(dsp->registers[DSP_REG_SR] >> DSP_SR_E)) & 1;
            return ((value1 | (value2 & value3)) == 0);
        case 5: /* EC */
            value1 = dsp->registers[DSP_REG_SR] & (1<<DSP_SR_E);
            return (value1==0);
        case 6: /* LC */
            value1 = dsp->registers[DSP_REG_SR] & (1<<DSP_SR_L);
            return (value1==0);
        case 7: /* GT */
            value1 = (dsp->registers[DSP_REG_SR] >> DSP_SR_N) & 1;
            value2 = (dsp->registers[DSP_REG_SR] >> DSP_SR_V) & 1;
            value3 = (dsp->registers[DSP_REG_SR] >> DSP_SR_Z) & 1;
            return ((value3 | (value1 ^ value2)) == 0);
        case 8: /* CS (LO) */
            value1 = dsp->registers[DSP_REG_SR] & (1<<DSP_SR_C);
            return (value1==1);
        case 9: /* LT */
            value1 = (dsp->registers[DSP_REG_SR] >> DSP_SR_N) & 1;
            value2 = (dsp->registers[DSP_REG_SR] >> DSP_SR_V) & 1;
            return ((value1 ^ value2) == 1);
        case 10: /* EQ */
            value1 = (dsp->registers[DSP_REG_SR] >> DSP_SR_Z) & 1;
            return (value1==1);
        case 11: /* MI */
            value1 = (dsp->registers[DSP_REG_SR] >> DSP_SR_N) & 1;
            return (value1==1);
        case 12: /* NR */
            value1 = (dsp->registers[DSP_REG_SR] >> DSP_SR_Z) & 1;
            value2 = (~(dsp->registers[DSP_REG_SR] >> DSP_SR_U)) & 1;
            value3 = (~(dsp->registers[DSP_REG_SR] >> DSP_SR_E)) & 1;
            return ((value1 | (value2 & value3)) == 1);
        case 13: /* ES */
            value1 = (dsp->registers[DSP_REG_SR] >> DSP_SR_E) & 1;
            return (value1==1);
        case 14: /* LS */
            value1 = (dsp->registers[DSP_REG_SR] >> DSP_SR_L) & 1;
            return (value1==1);
        case 15: /* LE */
            value1 = (dsp->registers[DSP_REG_SR] >> DSP_SR_N) & 1;
            value2 = (dsp->registers[DSP_REG_SR] >> DSP_SR_V) & 1;
            value3 = (dsp->registers[DSP_REG_SR] >> DSP_SR_Z) & 1;
            return ((value3 | (value1 ^ value2)) == 1);
    }
    return 0;
}

/**********************************
 *  Set/clear ccr bits
 **********************************/

/* reg0 has bits 55..48 */
/* reg1 has bits 47..24 */
/* reg2 has bits 23..0 */

static void emu_ccr_update_e_u_n_z(dsp_core_t* dsp, uint32_t reg0, uint32_t reg1, uint32_t reg2)
{
    uint32_t scaling, value_e, value_u;

    /* Initialize SR register */
    dsp->registers[DSP_REG_SR] &= BITMASK(16)-((1<<DSP_SR_E) | (1<<DSP_SR_U) | (1<<DSP_SR_N) | (1<<DSP_SR_Z));

    scaling = (dsp->registers[DSP_REG_SR]>>DSP_SR_S0) & BITMASK(2);
    switch(scaling) {
        case 0:
            /* Extension Bit (E) */
            value_e = (reg0<<1) + (reg1>>23);
            if ((value_e != 0) && (value_e != BITMASK(9)))
                dsp->registers[DSP_REG_SR] |= 1 << DSP_SR_E;

            /* Unnormalized bit (U) */
            if ((reg1 & 0xc00000) == 0 || (reg1 & 0xc00000) == 0xc00000)
                dsp->registers[DSP_REG_SR] |= 1 << DSP_SR_U;
            break;
        case 1:
            /* Extension Bit (E) */
            if ((reg0 != 0) && (reg0 != BITMASK(8)))
                dsp->registers[DSP_REG_SR] |= 1 << DSP_SR_E;

            /* Unnormalized bit (U) */
            value_u = ((reg0<<1) + (reg1>>23)) & 3;
            if (value_u == 0 || value_u == 3)
                dsp->registers[DSP_REG_SR] |= 1 << DSP_SR_U;
            break;
        case 2:
            /* Extension Bit (E) */
            value_e = (reg0<<2) + (reg1>>22);
            if ((value_e != 0) && (value_e != BITMASK(10)))
                dsp->registers[DSP_REG_SR] |= 1 << DSP_SR_E;

            /* Unnormalized bit (U) */
            if ((reg1 & 0x600000) == 0 || (reg1 & 0x600000) == 0x600000)
                dsp->registers[DSP_REG_SR] |= 1 << DSP_SR_U;
            break;
        default:
            return;
            break;
    }

    /* Zero Flag (Z) */
    if ((reg1 == 0) && (reg2 == 0) && (reg0 == 0))
        dsp->registers[DSP_REG_SR] |= 1 << DSP_SR_Z;

    /* Negative Flag (N) */
    dsp->registers[DSP_REG_SR] |= (reg0>>4) & 0x8;
}

/**********************************
 *  ALU instructions
 **********************************/

static void emu_abs_a(dsp_core_t* dsp)
{
    uint32_t dest[3], overflowed;

    dest[0] = dsp->registers[DSP_REG_A2];
    dest[1] = dsp->registers[DSP_REG_A1];
    dest[2] = dsp->registers[DSP_REG_A0];

    overflowed = ((dest[2]==0) && (dest[1]==0) && (dest[0]==0x80));

    dsp_abs56(dest);

    dsp->registers[DSP_REG_A2] = dest[0];
    dsp->registers[DSP_REG_A1] = dest[1];
    dsp->registers[DSP_REG_A0] = dest[2];

    dsp->registers[DSP_REG_SR] &= BITMASK(16)-(1<<DSP_SR_V);
    dsp->registers[DSP_REG_SR] |= (overflowed<<DSP_SR_L)|(overflowed<<DSP_SR_V);

    emu_ccr_update_e_u_n_z(dsp, dest[0], dest[1], dest[2]);
}

static void emu_abs_b(dsp_core_t* dsp)
{
    uint32_t dest[3], overflowed;

    dest[0] = dsp->registers[DSP_REG_B2];
    dest[1] = dsp->registers[DSP_REG_B1];
    dest[2] = dsp->registers[DSP_REG_B0];

    overflowed = ((dest[2]==0) && (dest[1]==0) && (dest[0]==0x80));

    dsp_abs56(dest);

    dsp->registers[DSP_REG_B2] = dest[0];
    dsp->registers[DSP_REG_B1] = dest[1];
    dsp->registers[DSP_REG_B0] = dest[2];

    dsp->registers[DSP_REG_SR] &= BITMASK(16)-(1<<DSP_SR_V);
    dsp->registers[DSP_REG_SR] |= (overflowed<<DSP_SR_L)|(overflowed<<DSP_SR_V);

    emu_ccr_update_e_u_n_z(dsp, dest[0], dest[1], dest[2]);
}

static void emu_adc_x_a(dsp_core_t* dsp)
{
    uint32_t source[3], dest[3], curcarry;
    uint16_t newsr;

    curcarry = (dsp->registers[DSP_REG_SR]>>DSP_SR_C) & 1;

    dest[0] = dsp->registers[DSP_REG_A2];
    dest[1] = dsp->registers[DSP_REG_A1];
    dest[2] = dsp->registers[DSP_REG_A0];

    source[2] = dsp->registers[DSP_REG_X0];
    source[1] = dsp->registers[DSP_REG_X1];
    source[0] = source[1] & (1<<23) ? 0xff : 0x0;

    newsr = dsp_add56(source, dest);

    if (curcarry) {
        source[0]=0; source[1]=0; source[2]=1;
        newsr |= dsp_add56(source, dest);
    }

    dsp->registers[DSP_REG_A2] = dest[0];
    dsp->registers[DSP_REG_A1] = dest[1];
    dsp->registers[DSP_REG_A0] = dest[2];

    emu_ccr_update_e_u_n_z(dsp, dest[0], dest[1], dest[2]);

    dsp->registers[DSP_REG_SR] &= BITMASK(16)-((1<<DSP_SR_V)|(1<<DSP_SR_C));
    dsp->registers[DSP_REG_SR] |= newsr;
}

static void emu_adc_x_b(dsp_core_t* dsp)
{
    uint32_t source[3], dest[3], curcarry;
    uint16_t newsr;

    curcarry = (dsp->registers[DSP_REG_SR]>>DSP_SR_C) & 1;

    dest[0] = dsp->registers[DSP_REG_B2];
    dest[1] = dsp->registers[DSP_REG_B1];
    dest[2] = dsp->registers[DSP_REG_B0];

    source[2] = dsp->registers[DSP_REG_X0];
    source[1] = dsp->registers[DSP_REG_X1];
    source[0] = source[1] & (1<<23) ? 0xff : 0x0;

    newsr = dsp_add56(source, dest);

    if (curcarry) {
        source[0]=0; source[1]=0; source[2]=1;
        newsr |= dsp_add56(source, dest);
    }

    dsp->registers[DSP_REG_B2] = dest[0];
    dsp->registers[DSP_REG_B1] = dest[1];
    dsp->registers[DSP_REG_B0] = dest[2];

    emu_ccr_update_e_u_n_z(dsp, dest[0], dest[1], dest[2]);

    dsp->registers[DSP_REG_SR] &= BITMASK(16)-((1<<DSP_SR_V)|(1<<DSP_SR_C));
    dsp->registers[DSP_REG_SR] |= newsr;
}

static void emu_adc_y_a(dsp_core_t* dsp)
{
    uint32_t source[3], dest[3], curcarry;
    uint16_t newsr;

    curcarry = (dsp->registers[DSP_REG_SR]>>DSP_SR_C) & 1;

    dest[0] = dsp->registers[DSP_REG_A2];
    dest[1] = dsp->registers[DSP_REG_A1];
    dest[2] = dsp->registers[DSP_REG_A0];

    source[2] = dsp->registers[DSP_REG_Y0];
    source[1] = dsp->registers[DSP_REG_Y1];
    source[0] = source[1] & (1<<23) ? 0xff : 0x0;

    newsr = dsp_add56(source, dest);

    if (curcarry) {
        source[0]=0; source[1]=0; source[2]=1;
        newsr |= dsp_add56(source, dest);
    }

    dsp->registers[DSP_REG_A2] = dest[0];
    dsp->registers[DSP_REG_A1] = dest[1];
    dsp->registers[DSP_REG_A0] = dest[2];

    emu_ccr_update_e_u_n_z(dsp, dest[0], dest[1], dest[2]);

    dsp->registers[DSP_REG_SR] &= BITMASK(16)-((1<<DSP_SR_V)|(1<<DSP_SR_C));
    dsp->registers[DSP_REG_SR] |= newsr;
}

static void emu_adc_y_b(dsp_core_t* dsp)
{
    uint32_t source[3], dest[3], curcarry;
    uint16_t newsr;

    curcarry = (dsp->registers[DSP_REG_SR]>>DSP_SR_C) & 1;

    dest[0] = dsp->registers[DSP_REG_B2];
    dest[1] = dsp->registers[DSP_REG_B1];
    dest[2] = dsp->registers[DSP_REG_B0];

    source[2] = dsp->registers[DSP_REG_Y0];
    source[1] = dsp->registers[DSP_REG_Y1];
    source[0] = source[1] & (1<<23) ? 0xff : 0x0;

    newsr = dsp_add56(source, dest);

    if (curcarry) {
        source[0]=0; source[1]=0; source[2]=1;
        newsr |= dsp_add56(source, dest);
    }

    dsp->registers[DSP_REG_B2] = dest[0];
    dsp->registers[DSP_REG_B1] = dest[1];
    dsp->registers[DSP_REG_B0] = dest[2];

    emu_ccr_update_e_u_n_z(dsp, dest[0], dest[1], dest[2]);

    dsp->registers[DSP_REG_SR] &= BITMASK(16)-((1<<DSP_SR_V)|(1<<DSP_SR_C));
    dsp->registers[DSP_REG_SR] |= newsr;
}

static void emu_add_b_a(dsp_core_t* dsp)
{
    uint32_t source[3], dest[3];
    uint16_t newsr;

    dest[0] = dsp->registers[DSP_REG_A2];
    dest[1] = dsp->registers[DSP_REG_A1];
    dest[2] = dsp->registers[DSP_REG_A0];

    source[0] = dsp->registers[DSP_REG_B2];
    source[1] = dsp->registers[DSP_REG_B1];
    source[2] = dsp->registers[DSP_REG_B0];

    newsr = dsp_add56(source, dest);

    dsp->registers[DSP_REG_A2] = dest[0];
    dsp->registers[DSP_REG_A1] = dest[1];
    dsp->registers[DSP_REG_A0] = dest[2];

    emu_ccr_update_e_u_n_z(dsp, dest[0], dest[1], dest[2]);

    dsp->registers[DSP_REG_SR] &= BITMASK(16)-((1<<DSP_SR_V)|(1<<DSP_SR_C));
    dsp->registers[DSP_REG_SR] |= newsr;
}

static void emu_add_a_b(dsp_core_t* dsp)
{
    uint32_t source[3], dest[3];
    uint16_t newsr;

    dest[0] = dsp->registers[DSP_REG_B2];
    dest[1] = dsp->registers[DSP_REG_B1];
    dest[2] = dsp->registers[DSP_REG_B0];

    source[0] = dsp->registers[DSP_REG_A2];
    source[1] = dsp->registers[DSP_REG_A1];
    source[2] = dsp->registers[DSP_REG_A0];

    newsr = dsp_add56(source, dest);

    dsp->registers[DSP_REG_B2] = dest[0];
    dsp->registers[DSP_REG_B1] = dest[1];
    dsp->registers[DSP_REG_B0] = dest[2];

    emu_ccr_update_e_u_n_z(dsp, dest[0], dest[1], dest[2]);

    dsp->registers[DSP_REG_SR] &= BITMASK(16)-((1<<DSP_SR_V)|(1<<DSP_SR_C));
    dsp->registers[DSP_REG_SR] |= newsr;
}

static void emu_add_x_a(dsp_core_t* dsp)
{
    uint32_t source[3], dest[3];
    uint16_t newsr;

    dest[0] = dsp->registers[DSP_REG_A2];
    dest[1] = dsp->registers[DSP_REG_A1];
    dest[2] = dsp->registers[DSP_REG_A0];

    source[1] = dsp->registers[DSP_REG_X1];
    source[2] = dsp->registers[DSP_REG_X0];
    source[0] = source[1] & (1<<23) ? 0xff : 0x0;

    newsr = dsp_add56(source, dest);

    dsp->registers[DSP_REG_A2] = dest[0];
    dsp->registers[DSP_REG_A1] = dest[1];
    dsp->registers[DSP_REG_A0] = dest[2];

    emu_ccr_update_e_u_n_z(dsp, dest[0], dest[1], dest[2]);

    dsp->registers[DSP_REG_SR] &= BITMASK(16)-((1<<DSP_SR_V)|(1<<DSP_SR_C));
    dsp->registers[DSP_REG_SR] |= newsr;
}

static void emu_add_x_b(dsp_core_t* dsp)
{
    uint32_t source[3], dest[3];
    uint16_t newsr;

    dest[0] = dsp->registers[DSP_REG_B2];
    dest[1] = dsp->registers[DSP_REG_B1];
    dest[2] = dsp->registers[DSP_REG_B0];

    source[1] = dsp->registers[DSP_REG_X1];
    source[2] = dsp->registers[DSP_REG_X0];
    source[0] = source[1] & (1<<23) ? 0xff : 0x0;

    newsr = dsp_add56(source, dest);

    dsp->registers[DSP_REG_B2] = dest[0];
    dsp->registers[DSP_REG_B1] = dest[1];
    dsp->registers[DSP_REG_B0] = dest[2];

    emu_ccr_update_e_u_n_z(dsp, dest[0], dest[1], dest[2]);

    dsp->registers[DSP_REG_SR] &= BITMASK(16)-((1<<DSP_SR_V)|(1<<DSP_SR_C));
    dsp->registers[DSP_REG_SR] |= newsr;
}

static void emu_add_y_a(dsp_core_t* dsp)
{
    uint32_t source[3], dest[3];
    uint16_t newsr;

    dest[0] = dsp->registers[DSP_REG_A2];
    dest[1] = dsp->registers[DSP_REG_A1];
    dest[2] = dsp->registers[DSP_REG_A0];

    source[1] = dsp->registers[DSP_REG_Y1];
    source[2] = dsp->registers[DSP_REG_Y0];
    source[0] = source[1] & (1<<23) ? 0xff : 0x0;

    newsr = dsp_add56(source, dest);

    dsp->registers[DSP_REG_A2] = dest[0];
    dsp->registers[DSP_REG_A1] = dest[1];
    dsp->registers[DSP_REG_A0] = dest[2];

    emu_ccr_update_e_u_n_z(dsp, dest[0], dest[1], dest[2]);

    dsp->registers[DSP_REG_SR] &= BITMASK(16)-((1<<DSP_SR_V)|(1<<DSP_SR_C));
    dsp->registers[DSP_REG_SR] |= newsr;
}

static void emu_add_y_b(dsp_core_t* dsp)
{
    uint32_t source[3], dest[3];
    uint16_t newsr;

    dest[0] = dsp->registers[DSP_REG_B2];
    dest[1] = dsp->registers[DSP_REG_B1];
    dest[2] = dsp->registers[DSP_REG_B0];

    source[1] = dsp->registers[DSP_REG_Y1];
    source[2] = dsp->registers[DSP_REG_Y0];
    source[0] = source[1] & (1<<23) ? 0xff : 0x0;

    newsr = dsp_add56(source, dest);

    dsp->registers[DSP_REG_B2] = dest[0];
    dsp->registers[DSP_REG_B1] = dest[1];
    dsp->registers[DSP_REG_B0] = dest[2];

    emu_ccr_update_e_u_n_z(dsp, dest[0], dest[1], dest[2]);

    dsp->registers[DSP_REG_SR] &= BITMASK(16)-((1<<DSP_SR_V)|(1<<DSP_SR_C));
    dsp->registers[DSP_REG_SR] |= newsr;
}

static void emu_add_x0_a(dsp_core_t* dsp)
{
    uint32_t source[3], dest[3];
    uint16_t newsr;

    dest[0] = dsp->registers[DSP_REG_A2];
    dest[1] = dsp->registers[DSP_REG_A1];
    dest[2] = dsp->registers[DSP_REG_A0];

    source[2] = 0;
    source[1] = dsp->registers[DSP_REG_X0];
    source[0] = source[1] & (1<<23) ? 0xff : 0x0;

    newsr = dsp_add56(source, dest);

    dsp->registers[DSP_REG_A2] = dest[0];
    dsp->registers[DSP_REG_A1] = dest[1];
    dsp->registers[DSP_REG_A0] = dest[2];

    emu_ccr_update_e_u_n_z(dsp, dest[0], dest[1], dest[2]);

    dsp->registers[DSP_REG_SR] &= BITMASK(16)-((1<<DSP_SR_V)|(1<<DSP_SR_C));
    dsp->registers[DSP_REG_SR] |= newsr;
}

static void emu_add_x0_b(dsp_core_t* dsp)
{
    uint32_t source[3], dest[3];
    uint16_t newsr;

    dest[0] = dsp->registers[DSP_REG_B2];
    dest[1] = dsp->registers[DSP_REG_B1];
    dest[2] = dsp->registers[DSP_REG_B0];

    source[2] = 0;
    source[1] = dsp->registers[DSP_REG_X0];
    source[0] = source[1] & (1<<23) ? 0xff : 0x0;

    newsr = dsp_add56(source, dest);

    dsp->registers[DSP_REG_B2] = dest[0];
    dsp->registers[DSP_REG_B1] = dest[1];
    dsp->registers[DSP_REG_B0] = dest[2];

    emu_ccr_update_e_u_n_z(dsp, dest[0], dest[1], dest[2]);

    dsp->registers[DSP_REG_SR] &= BITMASK(16)-((1<<DSP_SR_V)|(1<<DSP_SR_C));
    dsp->registers[DSP_REG_SR] |= newsr;
}

static void emu_add_y0_a(dsp_core_t* dsp)
{
    uint32_t source[3], dest[3];
    uint16_t newsr;

    dest[0] = dsp->registers[DSP_REG_A2];
    dest[1] = dsp->registers[DSP_REG_A1];
    dest[2] = dsp->registers[DSP_REG_A0];

    source[2] = 0;
    source[1] = dsp->registers[DSP_REG_Y0];
    source[0] = source[1] & (1<<23) ? 0xff : 0x0;

    newsr = dsp_add56(source, dest);

    dsp->registers[DSP_REG_A2] = dest[0];
    dsp->registers[DSP_REG_A1] = dest[1];
    dsp->registers[DSP_REG_A0] = dest[2];

    emu_ccr_update_e_u_n_z(dsp, dest[0], dest[1], dest[2]);

    dsp->registers[DSP_REG_SR] &= BITMASK(16)-((1<<DSP_SR_V)|(1<<DSP_SR_C));
    dsp->registers[DSP_REG_SR] |= newsr;
}

static void emu_add_y0_b(dsp_core_t* dsp)
{
    uint32_t source[3], dest[3];
    uint16_t newsr;

    dest[0] = dsp->registers[DSP_REG_B2];
    dest[1] = dsp->registers[DSP_REG_B1];
    dest[2] = dsp->registers[DSP_REG_B0];

    source[2] = 0;
    source[1] = dsp->registers[DSP_REG_Y0];
    source[0] = source[1] & (1<<23) ? 0xff : 0x0;

    newsr = dsp_add56(source, dest);

    dsp->registers[DSP_REG_B2] = dest[0];
    dsp->registers[DSP_REG_B1] = dest[1];
    dsp->registers[DSP_REG_B0] = dest[2];

    emu_ccr_update_e_u_n_z(dsp, dest[0], dest[1], dest[2]);

    dsp->registers[DSP_REG_SR] &= BITMASK(16)-((1<<DSP_SR_V)|(1<<DSP_SR_C));
    dsp->registers[DSP_REG_SR] |= newsr;
}

static void emu_add_x1_a(dsp_core_t* dsp)
{
    uint32_t source[3], dest[3];
    uint16_t newsr;

    dest[0] = dsp->registers[DSP_REG_A2];
    dest[1] = dsp->registers[DSP_REG_A1];
    dest[2] = dsp->registers[DSP_REG_A0];

    source[2] = 0;
    source[1] = dsp->registers[DSP_REG_X1];
    source[0] = source[1] & (1<<23) ? 0xff : 0x0;

    newsr = dsp_add56(source, dest);

    dsp->registers[DSP_REG_A2] = dest[0];
    dsp->registers[DSP_REG_A1] = dest[1];
    dsp->registers[DSP_REG_A0] = dest[2];

    emu_ccr_update_e_u_n_z(dsp, dest[0], dest[1], dest[2]);

    dsp->registers[DSP_REG_SR] &= BITMASK(16)-((1<<DSP_SR_V)|(1<<DSP_SR_C));
    dsp->registers[DSP_REG_SR] |= newsr;
}

static void emu_add_x1_b(dsp_core_t* dsp)
{
    uint32_t source[3], dest[3];
    uint16_t newsr;

    dest[0] = dsp->registers[DSP_REG_B2];
    dest[1] = dsp->registers[DSP_REG_B1];
    dest[2] = dsp->registers[DSP_REG_B0];

    source[2] = 0;
    source[1] = dsp->registers[DSP_REG_X1];
    source[0] = source[1] & (1<<23) ? 0xff : 0x0;

    newsr = dsp_add56(source, dest);

    dsp->registers[DSP_REG_B2] = dest[0];
    dsp->registers[DSP_REG_B1] = dest[1];
    dsp->registers[DSP_REG_B0] = dest[2];

    emu_ccr_update_e_u_n_z(dsp, dest[0], dest[1], dest[2]);

    dsp->registers[DSP_REG_SR] &= BITMASK(16)-((1<<DSP_SR_V)|(1<<DSP_SR_C));
    dsp->registers[DSP_REG_SR] |= newsr;
}

static void emu_add_y1_a(dsp_core_t* dsp)
{
    uint32_t source[3], dest[3];
    uint16_t newsr;

    dest[0] = dsp->registers[DSP_REG_A2];
    dest[1] = dsp->registers[DSP_REG_A1];
    dest[2] = dsp->registers[DSP_REG_A0];

    source[2] = 0;
    source[1] = dsp->registers[DSP_REG_Y1];
    source[0] = source[1] & (1<<23) ? 0xff : 0x0;

    newsr = dsp_add56(source, dest);

    dsp->registers[DSP_REG_A2] = dest[0];
    dsp->registers[DSP_REG_A1] = dest[1];
    dsp->registers[DSP_REG_A0] = dest[2];

    emu_ccr_update_e_u_n_z(dsp, dest[0], dest[1], dest[2]);

    dsp->registers[DSP_REG_SR] &= BITMASK(16)-((1<<DSP_SR_V)|(1<<DSP_SR_C));
    dsp->registers[DSP_REG_SR] |= newsr;
}

static void emu_add_y1_b(dsp_core_t* dsp)
{
    uint32_t source[3], dest[3];
    uint16_t newsr;

    dest[0] = dsp->registers[DSP_REG_B2];
    dest[1] = dsp->registers[DSP_REG_B1];
    dest[2] = dsp->registers[DSP_REG_B0];

    source[2] = 0;
    source[1] = dsp->registers[DSP_REG_Y1];
    source[0] = source[1] & (1<<23) ? 0xff : 0x0;

    newsr = dsp_add56(source, dest);

    dsp->registers[DSP_REG_B2] = dest[0];
    dsp->registers[DSP_REG_B1] = dest[1];
    dsp->registers[DSP_REG_B0] = dest[2];

    emu_ccr_update_e_u_n_z(dsp, dest[0], dest[1], dest[2]);

    dsp->registers[DSP_REG_SR] &= BITMASK(16)-((1<<DSP_SR_V)|(1<<DSP_SR_C));
    dsp->registers[DSP_REG_SR] |= newsr;
}

static void emu_addl_b_a(dsp_core_t* dsp)
{
    uint32_t source[3], dest[3];
    uint16_t newsr;

    dest[0] = dsp->registers[DSP_REG_A2];
    dest[1] = dsp->registers[DSP_REG_A1];
    dest[2] = dsp->registers[DSP_REG_A0];
    newsr = dsp_asl56(dest, 1) & ~(1<<DSP_SR_C); /* xbased: C the add's */

    source[0] = dsp->registers[DSP_REG_B2];
    source[1] = dsp->registers[DSP_REG_B1];
    source[2] = dsp->registers[DSP_REG_B0];
    newsr |= dsp_add56(source, dest);

    dsp->registers[DSP_REG_A2] = dest[0];
    dsp->registers[DSP_REG_A1] = dest[1];
    dsp->registers[DSP_REG_A0] = dest[2];

    emu_ccr_update_e_u_n_z(dsp, dest[0], dest[1], dest[2]);

    dsp->registers[DSP_REG_SR] &= BITMASK(16)-((1<<DSP_SR_V)|(1<<DSP_SR_C));
    dsp->registers[DSP_REG_SR] |= newsr;
}

static void emu_addl_a_b(dsp_core_t* dsp)
{
    uint32_t source[3], dest[3];
    uint16_t newsr;

    dest[0] = dsp->registers[DSP_REG_B2];
    dest[1] = dsp->registers[DSP_REG_B1];
    dest[2] = dsp->registers[DSP_REG_B0];
    newsr = dsp_asl56(dest, 1) & ~(1<<DSP_SR_C); /* xbased: C the add's */

    source[0] = dsp->registers[DSP_REG_A2];
    source[1] = dsp->registers[DSP_REG_A1];
    source[2] = dsp->registers[DSP_REG_A0];
    newsr |= dsp_add56(source, dest);

    dsp->registers[DSP_REG_B2] = dest[0];
    dsp->registers[DSP_REG_B1] = dest[1];
    dsp->registers[DSP_REG_B0] = dest[2];

    emu_ccr_update_e_u_n_z(dsp, dest[0], dest[1], dest[2]);

    dsp->registers[DSP_REG_SR] &= BITMASK(16)-((1<<DSP_SR_V)|(1<<DSP_SR_C));
    dsp->registers[DSP_REG_SR] |= newsr;
}

static void emu_addr_b_a(dsp_core_t* dsp)
{
    uint32_t source[3], dest[3];
    uint16_t newsr;

    dest[0] = dsp->registers[DSP_REG_A2];
    dest[1] = dsp->registers[DSP_REG_A1];
    dest[2] = dsp->registers[DSP_REG_A0];
    dsp_asr56(dest, 1); newsr = 0; /* xbased: C the add's / subtraction's */

    source[0] = dsp->registers[DSP_REG_B2];
    source[1] = dsp->registers[DSP_REG_B1];
    source[2] = dsp->registers[DSP_REG_B0];
    newsr |= dsp_add56(source, dest);

    dsp->registers[DSP_REG_A2] = dest[0];
    dsp->registers[DSP_REG_A1] = dest[1];
    dsp->registers[DSP_REG_A0] = dest[2];

    emu_ccr_update_e_u_n_z(dsp, dest[0], dest[1], dest[2]);

    dsp->registers[DSP_REG_SR] &= BITMASK(16)-((1<<DSP_SR_V)|(1<<DSP_SR_C));
    dsp->registers[DSP_REG_SR] |= newsr;
}

static void emu_addr_a_b(dsp_core_t* dsp)
{
    uint32_t source[3], dest[3];
    uint16_t newsr;

    dest[0] = dsp->registers[DSP_REG_B2];
    dest[1] = dsp->registers[DSP_REG_B1];
    dest[2] = dsp->registers[DSP_REG_B0];
    dsp_asr56(dest, 1); newsr = 0; /* xbased: C the add's / subtraction's */

    source[0] = dsp->registers[DSP_REG_A2];
    source[1] = dsp->registers[DSP_REG_A1];
    source[2] = dsp->registers[DSP_REG_A0];
    newsr |= dsp_add56(source, dest);

    dsp->registers[DSP_REG_B2] = dest[0];
    dsp->registers[DSP_REG_B1] = dest[1];
    dsp->registers[DSP_REG_B0] = dest[2];

    emu_ccr_update_e_u_n_z(dsp, dest[0], dest[1], dest[2]);

    dsp->registers[DSP_REG_SR] &= BITMASK(16)-((1<<DSP_SR_V)|(1<<DSP_SR_C));
    dsp->registers[DSP_REG_SR] |= newsr;
}

static void emu_and_x0_a(dsp_core_t* dsp)
{
    dsp->registers[DSP_REG_A1] &= dsp->registers[DSP_REG_X0];

    dsp->registers[DSP_REG_SR] &= BITMASK(16)-((1<<DSP_SR_N)|(1<<DSP_SR_Z)|(1<<DSP_SR_V));
    dsp->registers[DSP_REG_SR] |= ((dsp->registers[DSP_REG_A1]>>23) & 1)<<DSP_SR_N;
    dsp->registers[DSP_REG_SR] |= (dsp->registers[DSP_REG_A1]==0)<<DSP_SR_Z;
}

static void emu_and_x0_b(dsp_core_t* dsp)
{
    dsp->registers[DSP_REG_B1] &= dsp->registers[DSP_REG_X0];

    dsp->registers[DSP_REG_SR] &= BITMASK(16)-((1<<DSP_SR_N)|(1<<DSP_SR_Z)|(1<<DSP_SR_V));
    dsp->registers[DSP_REG_SR] |= ((dsp->registers[DSP_REG_B1]>>23) & 1)<<DSP_SR_N;
    dsp->registers[DSP_REG_SR] |= (dsp->registers[DSP_REG_B1]==0)<<DSP_SR_Z;
}

static void emu_and_y0_a(dsp_core_t* dsp)
{
    dsp->registers[DSP_REG_A1] &= dsp->registers[DSP_REG_Y0];

    dsp->registers[DSP_REG_SR] &= BITMASK(16)-((1<<DSP_SR_N)|(1<<DSP_SR_Z)|(1<<DSP_SR_V));
    dsp->registers[DSP_REG_SR] |= ((dsp->registers[DSP_REG_A1]>>23) & 1)<<DSP_SR_N;
    dsp->registers[DSP_REG_SR] |= (dsp->registers[DSP_REG_A1]==0)<<DSP_SR_Z;
}

static void emu_and_y0_b(dsp_core_t* dsp)
{
    dsp->registers[DSP_REG_B1] &= dsp->registers[DSP_REG_Y0];

    dsp->registers[DSP_REG_SR] &= BITMASK(16)-((1<<DSP_SR_N)|(1<<DSP_SR_Z)|(1<<DSP_SR_V));
    dsp->registers[DSP_REG_SR] |= ((dsp->registers[DSP_REG_B1]>>23) & 1)<<DSP_SR_N;
    dsp->registers[DSP_REG_SR] |= (dsp->registers[DSP_REG_B1]==0)<<DSP_SR_Z;
}

static void emu_and_x1_a(dsp_core_t* dsp)
{
    dsp->registers[DSP_REG_A1] &= dsp->registers[DSP_REG_X1];

    dsp->registers[DSP_REG_SR] &= BITMASK(16)-((1<<DSP_SR_N)|(1<<DSP_SR_Z)|(1<<DSP_SR_V));
    dsp->registers[DSP_REG_SR] |= ((dsp->registers[DSP_REG_A1]>>23) & 1)<<DSP_SR_N;
    dsp->registers[DSP_REG_SR] |= (dsp->registers[DSP_REG_A1]==0)<<DSP_SR_Z;
}

static void emu_and_x1_b(dsp_core_t* dsp)
{
    dsp->registers[DSP_REG_B1] &= dsp->registers[DSP_REG_X1];

    dsp->registers[DSP_REG_SR] &= BITMASK(16)-((1<<DSP_SR_N)|(1<<DSP_SR_Z)|(1<<DSP_SR_V));
    dsp->registers[DSP_REG_SR] |= ((dsp->registers[DSP_REG_B1]>>23) & 1)<<DSP_SR_N;
    dsp->registers[DSP_REG_SR] |= (dsp->registers[DSP_REG_B1]==0)<<DSP_SR_Z;
}

static void emu_and_y1_a(dsp_core_t* dsp)
{
    dsp->registers[DSP_REG_A1] &= dsp->registers[DSP_REG_Y1];

    dsp->registers[DSP_REG_SR] &= BITMASK(16)-((1<<DSP_SR_N)|(1<<DSP_SR_Z)|(1<<DSP_SR_V));
    dsp->registers[DSP_REG_SR] |= ((dsp->registers[DSP_REG_A1]>>23) & 1)<<DSP_SR_N;
    dsp->registers[DSP_REG_SR] |= (dsp->registers[DSP_REG_A1]==0)<<DSP_SR_Z;
}

static void emu_and_y1_b(dsp_core_t* dsp)
{
    dsp->registers[DSP_REG_B1] &= dsp->registers[DSP_REG_Y1];

    dsp->registers[DSP_REG_SR] &= BITMASK(16)-((1<<DSP_SR_N)|(1<<DSP_SR_Z)|(1<<DSP_SR_V));
    dsp->registers[DSP_REG_SR] |= ((dsp->registers[DSP_REG_B1]>>23) & 1)<<DSP_SR_N;
    dsp->registers[DSP_REG_SR] |= (dsp->registers[DSP_REG_B1]==0)<<DSP_SR_Z;
}

static void emu_asl_a(dsp_core_t* dsp)
{
    uint32_t dest[3];
    uint16_t newsr;

    dest[0] = dsp->registers[DSP_REG_A2];
    dest[1] = dsp->registers[DSP_REG_A1];
    dest[2] = dsp->registers[DSP_REG_A0];

    newsr = dsp_asl56(dest, 1);

    dsp->registers[DSP_REG_A2] = dest[0];
    dsp->registers[DSP_REG_A1] = dest[1];
    dsp->registers[DSP_REG_A0] = dest[2];

    dsp->registers[DSP_REG_SR] &= BITMASK(16)-((1<<DSP_SR_C)|(1<<DSP_SR_V));
    dsp->registers[DSP_REG_SR] |= newsr;

    emu_ccr_update_e_u_n_z(dsp, dest[0], dest[1], dest[2]);
}

static void emu_asl_b(dsp_core_t* dsp)
{
    uint32_t dest[3];
    uint16_t newsr;

    dest[0] = dsp->registers[DSP_REG_B2];
    dest[1] = dsp->registers[DSP_REG_B1];
    dest[2] = dsp->registers[DSP_REG_B0];

    newsr = dsp_asl56(dest, 1);

    dsp->registers[DSP_REG_B2] = dest[0];
    dsp->registers[DSP_REG_B1] = dest[1];
    dsp->registers[DSP_REG_B0] = dest[2];

    dsp->registers[DSP_REG_SR] &= BITMASK(16)-((1<<DSP_SR_C)|(1<<DSP_SR_V));
    dsp->registers[DSP_REG_SR] |= newsr;

    emu_ccr_update_e_u_n_z(dsp, dest[0], dest[1], dest[2]);
}

static void emu_asr_a(dsp_core_t* dsp)
{
    uint32_t dest[3];
    uint16_t newsr;

    dest[0] = dsp->registers[DSP_REG_A2];
    dest[1] = dsp->registers[DSP_REG_A1];
    dest[2] = dsp->registers[DSP_REG_A0];

    newsr = dsp_asr56(dest, 1);

    dsp->registers[DSP_REG_A2] = dest[0];
    dsp->registers[DSP_REG_A1] = dest[1];
    dsp->registers[DSP_REG_A0] = dest[2];

    dsp->registers[DSP_REG_SR] &= BITMASK(16)-((1<<DSP_SR_C)|(1<<DSP_SR_V));
    dsp->registers[DSP_REG_SR] |= newsr;

    emu_ccr_update_e_u_n_z(dsp, dest[0], dest[1], dest[2]);
}

static void emu_asr_b(dsp_core_t* dsp)
{
    uint32_t dest[3];
    uint16_t newsr;

    dest[0] = dsp->registers[DSP_REG_B2];
    dest[1] = dsp->registers[DSP_REG_B1];
    dest[2] = dsp->registers[DSP_REG_B0];

    newsr = dsp_asr56(dest, 1);

    dsp->registers[DSP_REG_B2] = dest[0];
    dsp->registers[DSP_REG_B1] = dest[1];
    dsp->registers[DSP_REG_B0] = dest[2];

    dsp->registers[DSP_REG_SR] &= BITMASK(16)-((1<<DSP_SR_C)|(1<<DSP_SR_V));
    dsp->registers[DSP_REG_SR] |= newsr;

    emu_ccr_update_e_u_n_z(dsp, dest[0], dest[1], dest[2]);
}

static void emu_clr_a(dsp_core_t* dsp)
{
    dsp->registers[DSP_REG_A2] = 0;
    dsp->registers[DSP_REG_A1] = 0;
    dsp->registers[DSP_REG_A0] = 0;

    dsp->registers[DSP_REG_SR] &= BITMASK(16)-((1<<DSP_SR_E)|(1<<DSP_SR_N)|(1<<DSP_SR_V));
    dsp->registers[DSP_REG_SR] |= (1<<DSP_SR_U)|(1<<DSP_SR_Z);
}

static void emu_clr_b(dsp_core_t* dsp)
{
    dsp->registers[DSP_REG_B2] = 0;
    dsp->registers[DSP_REG_B1] = 0;
    dsp->registers[DSP_REG_B0] = 0;

    dsp->registers[DSP_REG_SR] &= BITMASK(16)-((1<<DSP_SR_E)|(1<<DSP_SR_N)|(1<<DSP_SR_V));
    dsp->registers[DSP_REG_SR] |= (1<<DSP_SR_U)|(1<<DSP_SR_Z);
}

static void emu_cmp_b_a(dsp_core_t* dsp)
{
    uint32_t source[3], dest[3];
    uint16_t newsr;

    dest[0] = dsp->registers[DSP_REG_A2];
    dest[1] = dsp->registers[DSP_REG_A1];
    dest[2] = dsp->registers[DSP_REG_A0];

    source[0] = dsp->registers[DSP_REG_B2];
    source[1] = dsp->registers[DSP_REG_B1];
    source[2] = dsp->registers[DSP_REG_B0];

    newsr = dsp_sub56(source, dest);
    newsr |= emu_sm_sat3(dsp, dest, 0); /* SM */

    emu_ccr_update_e_u_n_z(dsp, dest[0], dest[1], dest[2]);

    dsp->registers[DSP_REG_SR] &= BITMASK(16)-((1<<DSP_SR_V)|(1<<DSP_SR_C));
    dsp->registers[DSP_REG_SR] |= newsr;
}

static void emu_cmp_a_b(dsp_core_t* dsp)
{
    uint32_t source[3], dest[3];
    uint16_t newsr;

    dest[0] = dsp->registers[DSP_REG_B2];
    dest[1] = dsp->registers[DSP_REG_B1];
    dest[2] = dsp->registers[DSP_REG_B0];

    source[0] = dsp->registers[DSP_REG_A2];
    source[1] = dsp->registers[DSP_REG_A1];
    source[2] = dsp->registers[DSP_REG_A0];

    newsr = dsp_sub56(source, dest);
    newsr |= emu_sm_sat3(dsp, dest, 0); /* SM */

    emu_ccr_update_e_u_n_z(dsp, dest[0], dest[1], dest[2]);

    dsp->registers[DSP_REG_SR] &= BITMASK(16)-((1<<DSP_SR_V)|(1<<DSP_SR_C));
    dsp->registers[DSP_REG_SR] |= newsr;
}

static void emu_cmp_x0_a(dsp_core_t* dsp)
{
    uint32_t source[3], dest[3];
    uint16_t newsr;

    dest[2] = dsp->registers[DSP_REG_A0];
    dest[1] = dsp->registers[DSP_REG_A1];
    dest[0] = dsp->registers[DSP_REG_A2];

    source[2] = 0;
    source[1] = dsp->registers[DSP_REG_X0];
    source[0] = source[1] & (1<<23) ? 0xff : 0x0;

    newsr = dsp_sub56(source, dest);
    newsr |= emu_sm_sat3(dsp, dest, 0); /* SM */

    emu_ccr_update_e_u_n_z(dsp, dest[0], dest[1], dest[2]);

    dsp->registers[DSP_REG_SR] &= BITMASK(16)-((1<<DSP_SR_V)|(1<<DSP_SR_C));
    dsp->registers[DSP_REG_SR] |= newsr;
}

static void emu_cmp_x0_b(dsp_core_t* dsp)
{
    uint32_t source[3], dest[3];
    uint16_t newsr;

    dest[0] = dsp->registers[DSP_REG_B2];
    dest[1] = dsp->registers[DSP_REG_B1];
    dest[2] = dsp->registers[DSP_REG_B0];

    source[2] = 0;
    source[1] = dsp->registers[DSP_REG_X0];
    source[0] = source[1] & (1<<23) ? 0xff : 0x0;

    newsr = dsp_sub56(source, dest);
    newsr |= emu_sm_sat3(dsp, dest, 0); /* SM */

    emu_ccr_update_e_u_n_z(dsp, dest[0], dest[1], dest[2]);

    dsp->registers[DSP_REG_SR] &= BITMASK(16)-((1<<DSP_SR_V)|(1<<DSP_SR_C));
    dsp->registers[DSP_REG_SR] |= newsr;
}

static void emu_cmp_y0_a(dsp_core_t* dsp)
{
    uint32_t source[3], dest[3];
    uint16_t newsr;

    dest[2] = dsp->registers[DSP_REG_A0];
    dest[1] = dsp->registers[DSP_REG_A1];
    dest[0] = dsp->registers[DSP_REG_A2];

    source[2] = 0;
    source[1] = dsp->registers[DSP_REG_Y0];
    source[0] = source[1] & (1<<23) ? 0xff : 0x0;

    newsr = dsp_sub56(source, dest);
    newsr |= emu_sm_sat3(dsp, dest, 0); /* SM */

    emu_ccr_update_e_u_n_z(dsp, dest[0], dest[1], dest[2]);

    dsp->registers[DSP_REG_SR] &= BITMASK(16)-((1<<DSP_SR_V)|(1<<DSP_SR_C));
    dsp->registers[DSP_REG_SR] |= newsr;
}

static void emu_cmp_y0_b(dsp_core_t* dsp)
{
    uint32_t source[3], dest[3];
    uint16_t newsr;

    dest[0] = dsp->registers[DSP_REG_B2];
    dest[1] = dsp->registers[DSP_REG_B1];
    dest[2] = dsp->registers[DSP_REG_B0];

    source[2] = 0;
    source[1] = dsp->registers[DSP_REG_Y0];
    source[0] = source[1] & (1<<23) ? 0xff : 0x0;

    newsr = dsp_sub56(source, dest);
    newsr |= emu_sm_sat3(dsp, dest, 0); /* SM */

    emu_ccr_update_e_u_n_z(dsp, dest[0], dest[1], dest[2]);

    dsp->registers[DSP_REG_SR] &= BITMASK(16)-((1<<DSP_SR_V)|(1<<DSP_SR_C));
    dsp->registers[DSP_REG_SR] |= newsr;
}
static void emu_cmp_x1_a(dsp_core_t* dsp)
{
    uint32_t source[3], dest[3];
    uint16_t newsr;

    dest[2] = dsp->registers[DSP_REG_A0];
    dest[1] = dsp->registers[DSP_REG_A1];
    dest[0] = dsp->registers[DSP_REG_A2];

    source[2] = 0;
    source[1] = dsp->registers[DSP_REG_X1];
    source[0] = source[1] & (1<<23) ? 0xff : 0x0;

    newsr = dsp_sub56(source, dest);
    newsr |= emu_sm_sat3(dsp, dest, 0); /* SM */

    emu_ccr_update_e_u_n_z(dsp, dest[0], dest[1], dest[2]);

    dsp->registers[DSP_REG_SR] &= BITMASK(16)-((1<<DSP_SR_V)|(1<<DSP_SR_C));
    dsp->registers[DSP_REG_SR] |= newsr;
}

static void emu_cmp_x1_b(dsp_core_t* dsp)
{
    uint32_t source[3], dest[3];
    uint16_t newsr;

    dest[0] = dsp->registers[DSP_REG_B2];
    dest[1] = dsp->registers[DSP_REG_B1];
    dest[2] = dsp->registers[DSP_REG_B0];

    source[2] = 0;
    source[1] = dsp->registers[DSP_REG_X1];
    source[0] = source[1] & (1<<23) ? 0xff : 0x0;

    newsr = dsp_sub56(source, dest);
    newsr |= emu_sm_sat3(dsp, dest, 0); /* SM */

    emu_ccr_update_e_u_n_z(dsp, dest[0], dest[1], dest[2]);

    dsp->registers[DSP_REG_SR] &= BITMASK(16)-((1<<DSP_SR_V)|(1<<DSP_SR_C));
    dsp->registers[DSP_REG_SR] |= newsr;
}

static void emu_cmp_y1_a(dsp_core_t* dsp)
{
    uint32_t source[3], dest[3];
    uint16_t newsr;

    dest[2] = dsp->registers[DSP_REG_A0];
    dest[1] = dsp->registers[DSP_REG_A1];
    dest[0] = dsp->registers[DSP_REG_A2];

    source[2] = 0;
    source[1] = dsp->registers[DSP_REG_Y1];
    source[0] = source[1] & (1<<23) ? 0xff : 0x0;

    newsr = dsp_sub56(source, dest);
    newsr |= emu_sm_sat3(dsp, dest, 0); /* SM */

    emu_ccr_update_e_u_n_z(dsp, dest[0], dest[1], dest[2]);

    dsp->registers[DSP_REG_SR] &= BITMASK(16)-((1<<DSP_SR_V)|(1<<DSP_SR_C));
    dsp->registers[DSP_REG_SR] |= newsr;
}

static void emu_cmp_y1_b(dsp_core_t* dsp)
{
    uint32_t source[3], dest[3];
    uint16_t newsr;

    dest[0] = dsp->registers[DSP_REG_B2];
    dest[1] = dsp->registers[DSP_REG_B1];
    dest[2] = dsp->registers[DSP_REG_B0];

    source[2] = 0;
    source[1] = dsp->registers[DSP_REG_Y1];
    source[0] = source[1] & (1<<23) ? 0xff : 0x0;

    newsr = dsp_sub56(source, dest);
    newsr |= emu_sm_sat3(dsp, dest, 0); /* SM */

    emu_ccr_update_e_u_n_z(dsp, dest[0], dest[1], dest[2]);

    dsp->registers[DSP_REG_SR] &= BITMASK(16)-((1<<DSP_SR_V)|(1<<DSP_SR_C));
    dsp->registers[DSP_REG_SR] |= newsr;
}

static void emu_cmpm_b_a(dsp_core_t* dsp)
{
    uint32_t source[3], dest[3];
    uint16_t newsr;

    dest[0] = dsp->registers[DSP_REG_A2];
    dest[1] = dsp->registers[DSP_REG_A1];
    dest[2] = dsp->registers[DSP_REG_A0];
    dsp_abs56(dest);

    source[0] = dsp->registers[DSP_REG_B2];
    source[1] = dsp->registers[DSP_REG_B1];
    source[2] = dsp->registers[DSP_REG_B0];
    dsp_abs56(source);

    newsr = dsp_sub56(source, dest);
    newsr |= emu_sm_sat3(dsp, dest, 0); /* SM */

    emu_ccr_update_e_u_n_z(dsp, dest[0], dest[1], dest[2]);

    dsp->registers[DSP_REG_SR] &= BITMASK(16)-((1<<DSP_SR_V)|(1<<DSP_SR_C));
    dsp->registers[DSP_REG_SR] |= newsr;
}

static void emu_cmpm_a_b(dsp_core_t* dsp)
{
    uint32_t source[3], dest[3];
    uint16_t newsr;

    dest[0] = dsp->registers[DSP_REG_B2];
    dest[1] = dsp->registers[DSP_REG_B1];
    dest[2] = dsp->registers[DSP_REG_B0];
    dsp_abs56(dest);

    source[0] = dsp->registers[DSP_REG_A2];
    source[1] = dsp->registers[DSP_REG_A1];
    source[2] = dsp->registers[DSP_REG_A0];
    dsp_abs56(source);

    newsr = dsp_sub56(source, dest);
    newsr |= emu_sm_sat3(dsp, dest, 0); /* SM */

    emu_ccr_update_e_u_n_z(dsp, dest[0], dest[1], dest[2]);

    dsp->registers[DSP_REG_SR] &= BITMASK(16)-((1<<DSP_SR_V)|(1<<DSP_SR_C));
    dsp->registers[DSP_REG_SR] |= newsr;
}

static void emu_cmpm_x0_a(dsp_core_t* dsp)
{
    uint32_t source[3], dest[3];
    uint16_t newsr;

    dest[2] = dsp->registers[DSP_REG_A0];
    dest[1] = dsp->registers[DSP_REG_A1];
    dest[0] = dsp->registers[DSP_REG_A2];
    dsp_abs56(dest);

    source[2] = 0;
    source[1] = dsp->registers[DSP_REG_X0];
    source[0] = source[1] & (1<<23) ? 0xff : 0x0;
    dsp_abs56(source);

    newsr = dsp_sub56(source, dest);
    newsr |= emu_sm_sat3(dsp, dest, 0); /* SM */

    emu_ccr_update_e_u_n_z(dsp, dest[0], dest[1], dest[2]);

    dsp->registers[DSP_REG_SR] &= BITMASK(16)-((1<<DSP_SR_V)|(1<<DSP_SR_C));
    dsp->registers[DSP_REG_SR] |= newsr;
}

static void emu_cmpm_x0_b(dsp_core_t* dsp)
{
    uint32_t source[3], dest[3];
    uint16_t newsr;

    dest[0] = dsp->registers[DSP_REG_B2];
    dest[1] = dsp->registers[DSP_REG_B1];
    dest[2] = dsp->registers[DSP_REG_B0];
    dsp_abs56(dest);

    source[2] = 0;
    source[1] = dsp->registers[DSP_REG_X0];
    source[0] = source[1] & (1<<23) ? 0xff : 0x0;
    dsp_abs56(source);

    newsr = dsp_sub56(source, dest);
    newsr |= emu_sm_sat3(dsp, dest, 0); /* SM */

    emu_ccr_update_e_u_n_z(dsp, dest[0], dest[1], dest[2]);

    dsp->registers[DSP_REG_SR] &= BITMASK(16)-((1<<DSP_SR_V)|(1<<DSP_SR_C));
    dsp->registers[DSP_REG_SR] |= newsr;
}

static void emu_cmpm_y0_a(dsp_core_t* dsp)
{
    uint32_t source[3], dest[3];
    uint16_t newsr;

    dest[2] = dsp->registers[DSP_REG_A0];
    dest[1] = dsp->registers[DSP_REG_A1];
    dest[0] = dsp->registers[DSP_REG_A2];
    dsp_abs56(dest);

    source[2] = 0;
    source[1] = dsp->registers[DSP_REG_Y0];
    source[0] = source[1] & (1<<23) ? 0xff : 0x0;
    dsp_abs56(source);

    newsr = dsp_sub56(source, dest);
    newsr |= emu_sm_sat3(dsp, dest, 0); /* SM */

    emu_ccr_update_e_u_n_z(dsp, dest[0], dest[1], dest[2]);

    dsp->registers[DSP_REG_SR] &= BITMASK(16)-((1<<DSP_SR_V)|(1<<DSP_SR_C));
    dsp->registers[DSP_REG_SR] |= newsr;
}

static void emu_cmpm_y0_b(dsp_core_t* dsp)
{
    uint32_t source[3], dest[3];
    uint16_t newsr;

    dest[0] = dsp->registers[DSP_REG_B2];
    dest[1] = dsp->registers[DSP_REG_B1];
    dest[2] = dsp->registers[DSP_REG_B0];
    dsp_abs56(dest);

    source[2] = 0;
    source[1] = dsp->registers[DSP_REG_Y0];
    source[0] = source[1] & (1<<23) ? 0xff : 0x0;
    dsp_abs56(source);

    newsr = dsp_sub56(source, dest);
    newsr |= emu_sm_sat3(dsp, dest, 0); /* SM */

    emu_ccr_update_e_u_n_z(dsp, dest[0], dest[1], dest[2]);

    dsp->registers[DSP_REG_SR] &= BITMASK(16)-((1<<DSP_SR_V)|(1<<DSP_SR_C));
    dsp->registers[DSP_REG_SR] |= newsr;
}

static void emu_cmpm_x1_a(dsp_core_t* dsp)
{
    uint32_t source[3], dest[3];
    uint16_t newsr;

    dest[2] = dsp->registers[DSP_REG_A0];
    dest[1] = dsp->registers[DSP_REG_A1];
    dest[0] = dsp->registers[DSP_REG_A2];
    dsp_abs56(dest);

    source[2] = 0;
    source[1] = dsp->registers[DSP_REG_X1];
    source[0] = source[1] & (1<<23) ? 0xff : 0x0;
    dsp_abs56(source);

    newsr = dsp_sub56(source, dest);
    newsr |= emu_sm_sat3(dsp, dest, 0); /* SM */

    emu_ccr_update_e_u_n_z(dsp, dest[0], dest[1], dest[2]);

    dsp->registers[DSP_REG_SR] &= BITMASK(16)-((1<<DSP_SR_V)|(1<<DSP_SR_C));
    dsp->registers[DSP_REG_SR] |= newsr;
}

static void emu_cmpm_x1_b(dsp_core_t* dsp)
{
    uint32_t source[3], dest[3];
    uint16_t newsr;

    dest[0] = dsp->registers[DSP_REG_B2];
    dest[1] = dsp->registers[DSP_REG_B1];
    dest[2] = dsp->registers[DSP_REG_B0];
    dsp_abs56(dest);

    source[2] = 0;
    source[1] = dsp->registers[DSP_REG_X1];
    source[0] = source[1] & (1<<23) ? 0xff : 0x0;
    dsp_abs56(source);

    newsr = dsp_sub56(source, dest);
    newsr |= emu_sm_sat3(dsp, dest, 0); /* SM */

    emu_ccr_update_e_u_n_z(dsp, dest[0], dest[1], dest[2]);

    dsp->registers[DSP_REG_SR] &= BITMASK(16)-((1<<DSP_SR_V)|(1<<DSP_SR_C));
    dsp->registers[DSP_REG_SR] |= newsr;
}

static void emu_cmpm_y1_a(dsp_core_t* dsp)
{
    uint32_t source[3], dest[3];
    uint16_t newsr;

    dest[2] = dsp->registers[DSP_REG_A0];
    dest[1] = dsp->registers[DSP_REG_A1];
    dest[0] = dsp->registers[DSP_REG_A2];
    dsp_abs56(dest);

    source[2] = 0;
    source[1] = dsp->registers[DSP_REG_Y1];
    source[0] = source[1] & (1<<23) ? 0xff : 0x0;
    dsp_abs56(source);

    newsr = dsp_sub56(source, dest);
    newsr |= emu_sm_sat3(dsp, dest, 0); /* SM */

    emu_ccr_update_e_u_n_z(dsp, dest[0], dest[1], dest[2]);

    dsp->registers[DSP_REG_SR] &= BITMASK(16)-((1<<DSP_SR_V)|(1<<DSP_SR_C));
    dsp->registers[DSP_REG_SR] |= newsr;
}

static void emu_cmpm_y1_b(dsp_core_t* dsp)
{
    uint32_t source[3], dest[3];
    uint16_t newsr;

    dest[0] = dsp->registers[DSP_REG_B2];
    dest[1] = dsp->registers[DSP_REG_B1];
    dest[2] = dsp->registers[DSP_REG_B0];
    dsp_abs56(dest);

    source[2] = 0;
    source[1] = dsp->registers[DSP_REG_Y1];
    source[0] = source[1] & (1<<23) ? 0xff : 0x0;
    dsp_abs56(source);

    newsr = dsp_sub56(source, dest);
    newsr |= emu_sm_sat3(dsp, dest, 0); /* SM */

    emu_ccr_update_e_u_n_z(dsp, dest[0], dest[1], dest[2]);

    dsp->registers[DSP_REG_SR] &= BITMASK(16)-((1<<DSP_SR_V)|(1<<DSP_SR_C));
    dsp->registers[DSP_REG_SR] |= newsr;
}

static void emu_eor_x0_a(dsp_core_t* dsp)
{
    dsp->registers[DSP_REG_A1] ^= dsp->registers[DSP_REG_X0];
    dsp->registers[DSP_REG_A1] &= BITMASK(24); /* FIXME: useless ? */

    dsp->registers[DSP_REG_SR] &= BITMASK(16)-((1<<DSP_SR_N)|(1<<DSP_SR_Z)|(1<<DSP_SR_V));
    dsp->registers[DSP_REG_SR] |= ((dsp->registers[DSP_REG_A1]>>23) & 1)<<DSP_SR_N;
    dsp->registers[DSP_REG_SR] |= (dsp->registers[DSP_REG_A1]==0)<<DSP_SR_Z;
}

static void emu_eor_x0_b(dsp_core_t* dsp)
{
    dsp->registers[DSP_REG_B1] ^= dsp->registers[DSP_REG_X0];
    dsp->registers[DSP_REG_B1] &= BITMASK(24); /* FIXME: useless ? */

    dsp->registers[DSP_REG_SR] &= BITMASK(16)-((1<<DSP_SR_N)|(1<<DSP_SR_Z)|(1<<DSP_SR_V));
    dsp->registers[DSP_REG_SR] |= ((dsp->registers[DSP_REG_B1]>>23) & 1)<<DSP_SR_N;
    dsp->registers[DSP_REG_SR] |= (dsp->registers[DSP_REG_B1]==0)<<DSP_SR_Z;
}

static void emu_eor_y0_a(dsp_core_t* dsp)
{
    dsp->registers[DSP_REG_A1] ^= dsp->registers[DSP_REG_Y0];
    dsp->registers[DSP_REG_A1] &= BITMASK(24); /* FIXME: useless ? */

    dsp->registers[DSP_REG_SR] &= BITMASK(16)-((1<<DSP_SR_N)|(1<<DSP_SR_Z)|(1<<DSP_SR_V));
    dsp->registers[DSP_REG_SR] |= ((dsp->registers[DSP_REG_A1]>>23) & 1)<<DSP_SR_N;
    dsp->registers[DSP_REG_SR] |= (dsp->registers[DSP_REG_A1]==0)<<DSP_SR_Z;
}

static void emu_eor_y0_b(dsp_core_t* dsp)
{
    dsp->registers[DSP_REG_B1] ^= dsp->registers[DSP_REG_Y0];
    dsp->registers[DSP_REG_B1] &= BITMASK(24); /* FIXME: useless ? */

    dsp->registers[DSP_REG_SR] &= BITMASK(16)-((1<<DSP_SR_N)|(1<<DSP_SR_Z)|(1<<DSP_SR_V));
    dsp->registers[DSP_REG_SR] |= ((dsp->registers[DSP_REG_B1]>>23) & 1)<<DSP_SR_N;
    dsp->registers[DSP_REG_SR] |= (dsp->registers[DSP_REG_B1]==0)<<DSP_SR_Z;
}

static void emu_eor_x1_a(dsp_core_t* dsp)
{
    dsp->registers[DSP_REG_A1] ^= dsp->registers[DSP_REG_X1];
    dsp->registers[DSP_REG_A1] &= BITMASK(24); /* FIXME: useless ? */

    dsp->registers[DSP_REG_SR] &= BITMASK(16)-((1<<DSP_SR_N)|(1<<DSP_SR_Z)|(1<<DSP_SR_V));
    dsp->registers[DSP_REG_SR] |= ((dsp->registers[DSP_REG_A1]>>23) & 1)<<DSP_SR_N;
    dsp->registers[DSP_REG_SR] |= (dsp->registers[DSP_REG_A1]==0)<<DSP_SR_Z;
}

static void emu_eor_x1_b(dsp_core_t* dsp)
{
    dsp->registers[DSP_REG_B1] ^= dsp->registers[DSP_REG_X1];
    dsp->registers[DSP_REG_B1] &= BITMASK(24); /* FIXME: useless ? */

    dsp->registers[DSP_REG_SR] &= BITMASK(16)-((1<<DSP_SR_N)|(1<<DSP_SR_Z)|(1<<DSP_SR_V));
    dsp->registers[DSP_REG_SR] |= ((dsp->registers[DSP_REG_B1]>>23) & 1)<<DSP_SR_N;
    dsp->registers[DSP_REG_SR] |= (dsp->registers[DSP_REG_B1]==0)<<DSP_SR_Z;
}

static void emu_eor_y1_a(dsp_core_t* dsp)
{
    dsp->registers[DSP_REG_A1] ^= dsp->registers[DSP_REG_Y1];
    dsp->registers[DSP_REG_A1] &= BITMASK(24); /* FIXME: useless ? */

    dsp->registers[DSP_REG_SR] &= BITMASK(16)-((1<<DSP_SR_N)|(1<<DSP_SR_Z)|(1<<DSP_SR_V));
    dsp->registers[DSP_REG_SR] |= ((dsp->registers[DSP_REG_A1]>>23) & 1)<<DSP_SR_N;
    dsp->registers[DSP_REG_SR] |= (dsp->registers[DSP_REG_A1]==0)<<DSP_SR_Z;
}

static void emu_eor_y1_b(dsp_core_t* dsp)
{
    dsp->registers[DSP_REG_B1] ^= dsp->registers[DSP_REG_Y1];
    dsp->registers[DSP_REG_B1] &= BITMASK(24); /* FIXME: useless ? */

    dsp->registers[DSP_REG_SR] &= BITMASK(16)-((1<<DSP_SR_N)|(1<<DSP_SR_Z)|(1<<DSP_SR_V));
    dsp->registers[DSP_REG_SR] |= ((dsp->registers[DSP_REG_B1]>>23) & 1)<<DSP_SR_N;
    dsp->registers[DSP_REG_SR] |= (dsp->registers[DSP_REG_B1]==0)<<DSP_SR_Z;
}

static void emu_lsl_a(dsp_core_t* dsp)
{
    uint32_t newcarry = (dsp->registers[DSP_REG_A1]>>23) & 1;

    dsp->registers[DSP_REG_A1] <<= 1;
    dsp->registers[DSP_REG_A1] &= BITMASK(24);

    dsp->registers[DSP_REG_SR] &= BITMASK(16)-((1<<DSP_SR_C)|(1<<DSP_SR_N)|(1<<DSP_SR_Z)|(1<<DSP_SR_V));
    dsp->registers[DSP_REG_SR] |= newcarry;
    dsp->registers[DSP_REG_SR] |= ((dsp->registers[DSP_REG_A1]>>23) & 1)<<DSP_SR_N;
    dsp->registers[DSP_REG_SR] |= (dsp->registers[DSP_REG_A1]==0)<<DSP_SR_Z;
}

static void emu_lsl_b(dsp_core_t* dsp)
{
    uint32_t newcarry = (dsp->registers[DSP_REG_B1]>>23) & 1;

    dsp->registers[DSP_REG_B1] <<= 1;
    dsp->registers[DSP_REG_B1] &= BITMASK(24);

    dsp->registers[DSP_REG_SR] &= BITMASK(16)-((1<<DSP_SR_C)|(1<<DSP_SR_N)|(1<<DSP_SR_Z)|(1<<DSP_SR_V));
    dsp->registers[DSP_REG_SR] |= newcarry;
    dsp->registers[DSP_REG_SR] |= ((dsp->registers[DSP_REG_B1]>>23) & 1)<<DSP_SR_N;
    dsp->registers[DSP_REG_SR] |= (dsp->registers[DSP_REG_B1]==0)<<DSP_SR_Z;
}

static void emu_lsx(dsp_core_t* dsp, uint32_t d, uint32_t n, int left);

static void emu_lsl_imm(dsp_core_t* dsp)
{
    /* xbased: one multi-bit shift (A-6.55); the single-bit loop left the
     * flags alone for #0 */
    emu_lsx(dsp, dsp->cur_inst & 1, (dsp->cur_inst >> 1) & BITMASK(5), 1);
}

static void emu_lsr_a(dsp_core_t* dsp)
{
    uint32_t newcarry = dsp->registers[DSP_REG_A1] & 1;
    dsp->registers[DSP_REG_A1] >>= 1;

    dsp->registers[DSP_REG_SR] &= BITMASK(16)-((1<<DSP_SR_C)|(1<<DSP_SR_N)|(1<<DSP_SR_Z)|(1<<DSP_SR_V));
    dsp->registers[DSP_REG_SR] |= newcarry;
    dsp->registers[DSP_REG_SR] |= (dsp->registers[DSP_REG_A1]==0)<<DSP_SR_Z;
}

static void emu_lsr_b(dsp_core_t* dsp)
{
    uint32_t newcarry = dsp->registers[DSP_REG_B1] & 1;
    dsp->registers[DSP_REG_B1] >>= 1;

    dsp->registers[DSP_REG_SR] &= BITMASK(16)-((1<<DSP_SR_C)|(1<<DSP_SR_N)|(1<<DSP_SR_Z)|(1<<DSP_SR_V));
    dsp->registers[DSP_REG_SR] |= newcarry;
    dsp->registers[DSP_REG_SR] |= (dsp->registers[DSP_REG_B1]==0)<<DSP_SR_Z;
}

static void emu_mac_p_x0_x0_a(dsp_core_t* dsp)
{
    uint32_t source[3], dest[3];
    uint16_t newsr;

    dsp_mul56(dsp->registers[DSP_REG_X0], dsp->registers[DSP_REG_X0], source, SIGN_PLUS);

    dest[0] = dsp->registers[DSP_REG_A2];
    dest[1] = dsp->registers[DSP_REG_A1];
    dest[2] = dsp->registers[DSP_REG_A0];
    newsr = dsp_add56(source, dest);

    dsp->registers[DSP_REG_A2] = dest[0];
    dsp->registers[DSP_REG_A1] = dest[1];
    dsp->registers[DSP_REG_A0] = dest[2];

    emu_ccr_update_e_u_n_z(dsp, dest[0], dest[1], dest[2]);

    dsp->registers[DSP_REG_SR] &= BITMASK(16)-(1<<DSP_SR_V);
    dsp->registers[DSP_REG_SR] |= newsr & 0xfe;
}

static void emu_mac_m_x0_x0_a(dsp_core_t* dsp)
{
    uint32_t source[3], dest[3];
    uint16_t newsr;

    dsp_mul56(dsp->registers[DSP_REG_X0], dsp->registers[DSP_REG_X0], source, SIGN_MINUS);

    dest[0] = dsp->registers[DSP_REG_A2];
    dest[1] = dsp->registers[DSP_REG_A1];
    dest[2] = dsp->registers[DSP_REG_A0];
    newsr = dsp_add56(source, dest);

    dsp->registers[DSP_REG_A2] = dest[0];
    dsp->registers[DSP_REG_A1] = dest[1];
    dsp->registers[DSP_REG_A0] = dest[2];

    emu_ccr_update_e_u_n_z(dsp, dest[0], dest[1], dest[2]);

    dsp->registers[DSP_REG_SR] &= BITMASK(16)-(1<<DSP_SR_V);
    dsp->registers[DSP_REG_SR] |= newsr & 0xfe;
}
static void emu_mac_p_x0_x0_b(dsp_core_t* dsp)
{
    uint32_t source[3], dest[3];
    uint16_t newsr;

    dsp_mul56(dsp->registers[DSP_REG_X0], dsp->registers[DSP_REG_X0], source, SIGN_PLUS);

    dest[0] = dsp->registers[DSP_REG_B2];
    dest[1] = dsp->registers[DSP_REG_B1];
    dest[2] = dsp->registers[DSP_REG_B0];
    newsr = dsp_add56(source, dest);

    dsp->registers[DSP_REG_B2] = dest[0];
    dsp->registers[DSP_REG_B1] = dest[1];
    dsp->registers[DSP_REG_B0] = dest[2];

    emu_ccr_update_e_u_n_z(dsp, dest[0], dest[1], dest[2]);

    dsp->registers[DSP_REG_SR] &= BITMASK(16)-(1<<DSP_SR_V);
    dsp->registers[DSP_REG_SR] |= newsr & 0xfe;
}

static void emu_mac_m_x0_x0_b(dsp_core_t* dsp)
{
    uint32_t source[3], dest[3];
    uint16_t newsr;

    dsp_mul56(dsp->registers[DSP_REG_X0], dsp->registers[DSP_REG_X0], source, SIGN_MINUS);

    dest[0] = dsp->registers[DSP_REG_B2];
    dest[1] = dsp->registers[DSP_REG_B1];
    dest[2] = dsp->registers[DSP_REG_B0];
    newsr = dsp_add56(source, dest);

    dsp->registers[DSP_REG_B2] = dest[0];
    dsp->registers[DSP_REG_B1] = dest[1];
    dsp->registers[DSP_REG_B0] = dest[2];

    emu_ccr_update_e_u_n_z(dsp, dest[0], dest[1], dest[2]);

    dsp->registers[DSP_REG_SR] &= BITMASK(16)-(1<<DSP_SR_V);
    dsp->registers[DSP_REG_SR] |= newsr & 0xfe;
}

static void emu_mac_p_y0_y0_a(dsp_core_t* dsp)
{
    uint32_t source[3], dest[3];
    uint16_t newsr;

    dsp_mul56(dsp->registers[DSP_REG_Y0], dsp->registers[DSP_REG_Y0], source, SIGN_PLUS);

    dest[0] = dsp->registers[DSP_REG_A2];
    dest[1] = dsp->registers[DSP_REG_A1];
    dest[2] = dsp->registers[DSP_REG_A0];
    newsr = dsp_add56(source, dest);

    dsp->registers[DSP_REG_A2] = dest[0];
    dsp->registers[DSP_REG_A1] = dest[1];
    dsp->registers[DSP_REG_A0] = dest[2];

    emu_ccr_update_e_u_n_z(dsp, dest[0], dest[1], dest[2]);

    dsp->registers[DSP_REG_SR] &= BITMASK(16)-(1<<DSP_SR_V);
    dsp->registers[DSP_REG_SR] |= newsr & 0xfe;
}

static void emu_mac_m_y0_y0_a(dsp_core_t* dsp)
{
    uint32_t source[3], dest[3];
    uint16_t newsr;

    dsp_mul56(dsp->registers[DSP_REG_Y0], dsp->registers[DSP_REG_Y0], source, SIGN_MINUS);

    dest[0] = dsp->registers[DSP_REG_A2];
    dest[1] = dsp->registers[DSP_REG_A1];
    dest[2] = dsp->registers[DSP_REG_A0];
    newsr = dsp_add56(source, dest);

    dsp->registers[DSP_REG_A2] = dest[0];
    dsp->registers[DSP_REG_A1] = dest[1];
    dsp->registers[DSP_REG_A0] = dest[2];

    emu_ccr_update_e_u_n_z(dsp, dest[0], dest[1], dest[2]);

    dsp->registers[DSP_REG_SR] &= BITMASK(16)-(1<<DSP_SR_V);
    dsp->registers[DSP_REG_SR] |= newsr & 0xfe;
}
static void emu_mac_p_y0_y0_b(dsp_core_t* dsp)
{
    uint32_t source[3], dest[3];
    uint16_t newsr;

    dsp_mul56(dsp->registers[DSP_REG_Y0], dsp->registers[DSP_REG_Y0], source, SIGN_PLUS);

    dest[0] = dsp->registers[DSP_REG_B2];
    dest[1] = dsp->registers[DSP_REG_B1];
    dest[2] = dsp->registers[DSP_REG_B0];
    newsr = dsp_add56(source, dest);

    dsp->registers[DSP_REG_B2] = dest[0];
    dsp->registers[DSP_REG_B1] = dest[1];
    dsp->registers[DSP_REG_B0] = dest[2];

    emu_ccr_update_e_u_n_z(dsp, dest[0], dest[1], dest[2]);

    dsp->registers[DSP_REG_SR] &= BITMASK(16)-(1<<DSP_SR_V);
    dsp->registers[DSP_REG_SR] |= newsr & 0xfe;
}

static void emu_mac_m_y0_y0_b(dsp_core_t* dsp)
{
    uint32_t source[3], dest[3];
    uint16_t newsr;

    dsp_mul56(dsp->registers[DSP_REG_Y0], dsp->registers[DSP_REG_Y0], source, SIGN_MINUS);

    dest[0] = dsp->registers[DSP_REG_B2];
    dest[1] = dsp->registers[DSP_REG_B1];
    dest[2] = dsp->registers[DSP_REG_B0];
    newsr = dsp_add56(source, dest);

    dsp->registers[DSP_REG_B2] = dest[0];
    dsp->registers[DSP_REG_B1] = dest[1];
    dsp->registers[DSP_REG_B0] = dest[2];

    emu_ccr_update_e_u_n_z(dsp, dest[0], dest[1], dest[2]);

    dsp->registers[DSP_REG_SR] &= BITMASK(16)-(1<<DSP_SR_V);
    dsp->registers[DSP_REG_SR] |= newsr & 0xfe;
}

static void emu_mac_p_x1_x0_a(dsp_core_t* dsp)
{
    uint32_t source[3], dest[3];
    uint16_t newsr;

    dsp_mul56(dsp->registers[DSP_REG_X1], dsp->registers[DSP_REG_X0], source, SIGN_PLUS);

    dest[0] = dsp->registers[DSP_REG_A2];
    dest[1] = dsp->registers[DSP_REG_A1];
    dest[2] = dsp->registers[DSP_REG_A0];
    newsr = dsp_add56(source, dest);

    dsp->registers[DSP_REG_A2] = dest[0];
    dsp->registers[DSP_REG_A1] = dest[1];
    dsp->registers[DSP_REG_A0] = dest[2];

    emu_ccr_update_e_u_n_z(dsp, dest[0], dest[1], dest[2]);

    dsp->registers[DSP_REG_SR] &= BITMASK(16)-(1<<DSP_SR_V);
    dsp->registers[DSP_REG_SR] |= newsr & 0xfe;
}

static void emu_mac_m_x1_x0_a(dsp_core_t* dsp)
{
    uint32_t source[3], dest[3];
    uint16_t newsr;

    dsp_mul56(dsp->registers[DSP_REG_X1], dsp->registers[DSP_REG_X0], source, SIGN_MINUS);

    dest[0] = dsp->registers[DSP_REG_A2];
    dest[1] = dsp->registers[DSP_REG_A1];
    dest[2] = dsp->registers[DSP_REG_A0];
    newsr = dsp_add56(source, dest);

    dsp->registers[DSP_REG_A2] = dest[0];
    dsp->registers[DSP_REG_A1] = dest[1];
    dsp->registers[DSP_REG_A0] = dest[2];

    emu_ccr_update_e_u_n_z(dsp, dest[0], dest[1], dest[2]);

    dsp->registers[DSP_REG_SR] &= BITMASK(16)-(1<<DSP_SR_V);
    dsp->registers[DSP_REG_SR] |= newsr & 0xfe;
}

static void emu_mac_p_x1_x0_b(dsp_core_t* dsp)
{
    uint32_t source[3], dest[3];
    uint16_t newsr;

    dsp_mul56(dsp->registers[DSP_REG_X1], dsp->registers[DSP_REG_X0], source, SIGN_PLUS);

    dest[0] = dsp->registers[DSP_REG_B2];
    dest[1] = dsp->registers[DSP_REG_B1];
    dest[2] = dsp->registers[DSP_REG_B0];
    newsr = dsp_add56(source, dest);

    dsp->registers[DSP_REG_B2] = dest[0];
    dsp->registers[DSP_REG_B1] = dest[1];
    dsp->registers[DSP_REG_B0] = dest[2];

    emu_ccr_update_e_u_n_z(dsp, dest[0], dest[1], dest[2]);

    dsp->registers[DSP_REG_SR] &= BITMASK(16)-(1<<DSP_SR_V);
    dsp->registers[DSP_REG_SR] |= newsr & 0xfe;
}

static void emu_mac_m_x1_x0_b(dsp_core_t* dsp)
{
    uint32_t source[3], dest[3];
    uint16_t newsr;

    dsp_mul56(dsp->registers[DSP_REG_X1], dsp->registers[DSP_REG_X0], source, SIGN_MINUS);

    dest[0] = dsp->registers[DSP_REG_B2];
    dest[1] = dsp->registers[DSP_REG_B1];
    dest[2] = dsp->registers[DSP_REG_B0];
    newsr = dsp_add56(source, dest);

    dsp->registers[DSP_REG_B2] = dest[0];
    dsp->registers[DSP_REG_B1] = dest[1];
    dsp->registers[DSP_REG_B0] = dest[2];

    emu_ccr_update_e_u_n_z(dsp, dest[0], dest[1], dest[2]);

    dsp->registers[DSP_REG_SR] &= BITMASK(16)-(1<<DSP_SR_V);
    dsp->registers[DSP_REG_SR] |= newsr & 0xfe;
}

static void emu_mac_p_y1_y0_a(dsp_core_t* dsp)
{
    uint32_t source[3], dest[3];
    uint16_t newsr;

    dsp_mul56(dsp->registers[DSP_REG_Y1], dsp->registers[DSP_REG_Y0], source, SIGN_PLUS);

    dest[0] = dsp->registers[DSP_REG_A2];
    dest[1] = dsp->registers[DSP_REG_A1];
    dest[2] = dsp->registers[DSP_REG_A0];
    newsr = dsp_add56(source, dest);

    dsp->registers[DSP_REG_A2] = dest[0];
    dsp->registers[DSP_REG_A1] = dest[1];
    dsp->registers[DSP_REG_A0] = dest[2];

    emu_ccr_update_e_u_n_z(dsp, dest[0], dest[1], dest[2]);

    dsp->registers[DSP_REG_SR] &= BITMASK(16)-(1<<DSP_SR_V);
    dsp->registers[DSP_REG_SR] |= newsr & 0xfe;
}

static void emu_mac_m_y1_y0_a(dsp_core_t* dsp)
{
    uint32_t source[3], dest[3];
    uint16_t newsr;

    dsp_mul56(dsp->registers[DSP_REG_Y1], dsp->registers[DSP_REG_Y0], source, SIGN_MINUS);

    dest[0] = dsp->registers[DSP_REG_A2];
    dest[1] = dsp->registers[DSP_REG_A1];
    dest[2] = dsp->registers[DSP_REG_A0];
    newsr = dsp_add56(source, dest);

    dsp->registers[DSP_REG_A2] = dest[0];
    dsp->registers[DSP_REG_A1] = dest[1];
    dsp->registers[DSP_REG_A0] = dest[2];

    emu_ccr_update_e_u_n_z(dsp, dest[0], dest[1], dest[2]);

    dsp->registers[DSP_REG_SR] &= BITMASK(16)-(1<<DSP_SR_V);
    dsp->registers[DSP_REG_SR] |= newsr & 0xfe;
}

static void emu_mac_p_y1_y0_b(dsp_core_t* dsp)
{
    uint32_t source[3], dest[3];
    uint16_t newsr;

    dsp_mul56(dsp->registers[DSP_REG_Y1], dsp->registers[DSP_REG_Y0], source, SIGN_PLUS);

    dest[0] = dsp->registers[DSP_REG_B2];
    dest[1] = dsp->registers[DSP_REG_B1];
    dest[2] = dsp->registers[DSP_REG_B0];
    newsr = dsp_add56(source, dest);

    dsp->registers[DSP_REG_B2] = dest[0];
    dsp->registers[DSP_REG_B1] = dest[1];
    dsp->registers[DSP_REG_B0] = dest[2];

    emu_ccr_update_e_u_n_z(dsp, dest[0], dest[1], dest[2]);

    dsp->registers[DSP_REG_SR] &= BITMASK(16)-(1<<DSP_SR_V);
    dsp->registers[DSP_REG_SR] |= newsr & 0xfe;
}

static void emu_mac_m_y1_y0_b(dsp_core_t* dsp)
{
    uint32_t source[3], dest[3];
    uint16_t newsr;

    dsp_mul56(dsp->registers[DSP_REG_Y1], dsp->registers[DSP_REG_Y0], source, SIGN_MINUS);

    dest[0] = dsp->registers[DSP_REG_B2];
    dest[1] = dsp->registers[DSP_REG_B1];
    dest[2] = dsp->registers[DSP_REG_B0];
    newsr = dsp_add56(source, dest);

    dsp->registers[DSP_REG_B2] = dest[0];
    dsp->registers[DSP_REG_B1] = dest[1];
    dsp->registers[DSP_REG_B0] = dest[2];

    emu_ccr_update_e_u_n_z(dsp, dest[0], dest[1], dest[2]);

    dsp->registers[DSP_REG_SR] &= BITMASK(16)-(1<<DSP_SR_V);
    dsp->registers[DSP_REG_SR] |= newsr & 0xfe;
}

static void emu_mac_p_x0_y1_a(dsp_core_t* dsp)
{
    uint32_t source[3], dest[3];
    uint16_t newsr;

    dsp_mul56(dsp->registers[DSP_REG_X0], dsp->registers[DSP_REG_Y1], source, SIGN_PLUS);

    dest[0] = dsp->registers[DSP_REG_A2];
    dest[1] = dsp->registers[DSP_REG_A1];
    dest[2] = dsp->registers[DSP_REG_A0];
    newsr = dsp_add56(source, dest);

    dsp->registers[DSP_REG_A2] = dest[0];
    dsp->registers[DSP_REG_A1] = dest[1];
    dsp->registers[DSP_REG_A0] = dest[2];

    emu_ccr_update_e_u_n_z(dsp, dest[0], dest[1], dest[2]);

    dsp->registers[DSP_REG_SR] &= BITMASK(16)-(1<<DSP_SR_V);
    dsp->registers[DSP_REG_SR] |= newsr & 0xfe;
}

static void emu_mac_m_x0_y1_a(dsp_core_t* dsp)
{
    uint32_t source[3], dest[3];
    uint16_t newsr;

    dsp_mul56(dsp->registers[DSP_REG_X0], dsp->registers[DSP_REG_Y1], source, SIGN_MINUS);

    dest[0] = dsp->registers[DSP_REG_A2];
    dest[1] = dsp->registers[DSP_REG_A1];
    dest[2] = dsp->registers[DSP_REG_A0];
    newsr = dsp_add56(source, dest);

    dsp->registers[DSP_REG_A2] = dest[0];
    dsp->registers[DSP_REG_A1] = dest[1];
    dsp->registers[DSP_REG_A0] = dest[2];

    emu_ccr_update_e_u_n_z(dsp, dest[0], dest[1], dest[2]);

    dsp->registers[DSP_REG_SR] &= BITMASK(16)-(1<<DSP_SR_V);
    dsp->registers[DSP_REG_SR] |= newsr & 0xfe;
}

static void emu_mac_p_x0_y1_b(dsp_core_t* dsp)
{
    uint32_t source[3], dest[3];
    uint16_t newsr;

    dsp_mul56(dsp->registers[DSP_REG_X0], dsp->registers[DSP_REG_Y1], source, SIGN_PLUS);

    dest[0] = dsp->registers[DSP_REG_B2];
    dest[1] = dsp->registers[DSP_REG_B1];
    dest[2] = dsp->registers[DSP_REG_B0];
    newsr = dsp_add56(source, dest);

    dsp->registers[DSP_REG_B2] = dest[0];
    dsp->registers[DSP_REG_B1] = dest[1];
    dsp->registers[DSP_REG_B0] = dest[2];

    emu_ccr_update_e_u_n_z(dsp, dest[0], dest[1], dest[2]);

    dsp->registers[DSP_REG_SR] &= BITMASK(16)-(1<<DSP_SR_V);
    dsp->registers[DSP_REG_SR] |= newsr & 0xfe;
}

static void emu_mac_m_x0_y1_b(dsp_core_t* dsp)
{
    uint32_t source[3], dest[3];
    uint16_t newsr;

    dsp_mul56(dsp->registers[DSP_REG_X0], dsp->registers[DSP_REG_Y1], source, SIGN_MINUS);

    dest[0] = dsp->registers[DSP_REG_B2];
    dest[1] = dsp->registers[DSP_REG_B1];
    dest[2] = dsp->registers[DSP_REG_B0];
    newsr = dsp_add56(source, dest);

    dsp->registers[DSP_REG_B2] = dest[0];
    dsp->registers[DSP_REG_B1] = dest[1];
    dsp->registers[DSP_REG_B0] = dest[2];

    emu_ccr_update_e_u_n_z(dsp, dest[0], dest[1], dest[2]);

    dsp->registers[DSP_REG_SR] &= BITMASK(16)-(1<<DSP_SR_V);
    dsp->registers[DSP_REG_SR] |= newsr & 0xfe;
}

static void emu_mac_p_y0_x0_a(dsp_core_t* dsp)
{
    uint32_t source[3], dest[3];
    uint16_t newsr;

    dsp_mul56(dsp->registers[DSP_REG_Y0], dsp->registers[DSP_REG_X0], source, SIGN_PLUS);

    dest[0] = dsp->registers[DSP_REG_A2];
    dest[1] = dsp->registers[DSP_REG_A1];
    dest[2] = dsp->registers[DSP_REG_A0];
    newsr = dsp_add56(source, dest);

    dsp->registers[DSP_REG_A2] = dest[0];
    dsp->registers[DSP_REG_A1] = dest[1];
    dsp->registers[DSP_REG_A0] = dest[2];

    emu_ccr_update_e_u_n_z(dsp, dest[0], dest[1], dest[2]);

    dsp->registers[DSP_REG_SR] &= BITMASK(16)-(1<<DSP_SR_V);
    dsp->registers[DSP_REG_SR] |= newsr & 0xfe;
}

static void emu_mac_m_y0_x0_a(dsp_core_t* dsp)
{
    uint32_t source[3], dest[3];
    uint16_t newsr;

    dsp_mul56(dsp->registers[DSP_REG_Y0], dsp->registers[DSP_REG_X0], source, SIGN_MINUS);

    dest[0] = dsp->registers[DSP_REG_A2];
    dest[1] = dsp->registers[DSP_REG_A1];
    dest[2] = dsp->registers[DSP_REG_A0];
    newsr = dsp_add56(source, dest);

    dsp->registers[DSP_REG_A2] = dest[0];
    dsp->registers[DSP_REG_A1] = dest[1];
    dsp->registers[DSP_REG_A0] = dest[2];

    emu_ccr_update_e_u_n_z(dsp, dest[0], dest[1], dest[2]);

    dsp->registers[DSP_REG_SR] &= BITMASK(16)-(1<<DSP_SR_V);
    dsp->registers[DSP_REG_SR] |= newsr & 0xfe;
}

static void emu_mac_p_y0_x0_b(dsp_core_t* dsp)
{
    uint32_t source[3], dest[3];
    uint16_t newsr;

    dsp_mul56(dsp->registers[DSP_REG_Y0], dsp->registers[DSP_REG_X0], source, SIGN_PLUS);

    dest[0] = dsp->registers[DSP_REG_B2];
    dest[1] = dsp->registers[DSP_REG_B1];
    dest[2] = dsp->registers[DSP_REG_B0];
    newsr = dsp_add56(source, dest);

    dsp->registers[DSP_REG_B2] = dest[0];
    dsp->registers[DSP_REG_B1] = dest[1];
    dsp->registers[DSP_REG_B0] = dest[2];

    emu_ccr_update_e_u_n_z(dsp, dest[0], dest[1], dest[2]);

    dsp->registers[DSP_REG_SR] &= BITMASK(16)-(1<<DSP_SR_V);
    dsp->registers[DSP_REG_SR] |= newsr & 0xfe;
}

static void emu_mac_m_y0_x0_b(dsp_core_t* dsp)
{
    uint32_t source[3], dest[3];
    uint16_t newsr;

    dsp_mul56(dsp->registers[DSP_REG_Y0], dsp->registers[DSP_REG_X0], source, SIGN_MINUS);

    dest[0] = dsp->registers[DSP_REG_B2];
    dest[1] = dsp->registers[DSP_REG_B1];
    dest[2] = dsp->registers[DSP_REG_B0];
    newsr = dsp_add56(source, dest);

    dsp->registers[DSP_REG_B2] = dest[0];
    dsp->registers[DSP_REG_B1] = dest[1];
    dsp->registers[DSP_REG_B0] = dest[2];

    emu_ccr_update_e_u_n_z(dsp, dest[0], dest[1], dest[2]);

    dsp->registers[DSP_REG_SR] &= BITMASK(16)-(1<<DSP_SR_V);
    dsp->registers[DSP_REG_SR] |= newsr & 0xfe;
}

static void emu_mac_p_x1_y0_a(dsp_core_t* dsp)
{
    uint32_t source[3], dest[3];
    uint16_t newsr;

    dsp_mul56(dsp->registers[DSP_REG_X1], dsp->registers[DSP_REG_Y0], source, SIGN_PLUS);

    dest[0] = dsp->registers[DSP_REG_A2];
    dest[1] = dsp->registers[DSP_REG_A1];
    dest[2] = dsp->registers[DSP_REG_A0];
    newsr = dsp_add56(source, dest);

    dsp->registers[DSP_REG_A2] = dest[0];
    dsp->registers[DSP_REG_A1] = dest[1];
    dsp->registers[DSP_REG_A0] = dest[2];

    emu_ccr_update_e_u_n_z(dsp, dest[0], dest[1], dest[2]);

    dsp->registers[DSP_REG_SR] &= BITMASK(16)-(1<<DSP_SR_V);
    dsp->registers[DSP_REG_SR] |= newsr & 0xfe;
}

static void emu_mac_m_x1_y0_a(dsp_core_t* dsp)
{
    uint32_t source[3], dest[3];
    uint16_t newsr;

    dsp_mul56(dsp->registers[DSP_REG_X1], dsp->registers[DSP_REG_Y0], source, SIGN_MINUS);

    dest[0] = dsp->registers[DSP_REG_A2];
    dest[1] = dsp->registers[DSP_REG_A1];
    dest[2] = dsp->registers[DSP_REG_A0];
    newsr = dsp_add56(source, dest);

    dsp->registers[DSP_REG_A2] = dest[0];
    dsp->registers[DSP_REG_A1] = dest[1];
    dsp->registers[DSP_REG_A0] = dest[2];

    emu_ccr_update_e_u_n_z(dsp, dest[0], dest[1], dest[2]);

    dsp->registers[DSP_REG_SR] &= BITMASK(16)-(1<<DSP_SR_V);
    dsp->registers[DSP_REG_SR] |= newsr & 0xfe;
}

static void emu_mac_p_x1_y0_b(dsp_core_t* dsp)
{
    uint32_t source[3], dest[3];
    uint16_t newsr;

    dsp_mul56(dsp->registers[DSP_REG_X1], dsp->registers[DSP_REG_Y0], source, SIGN_PLUS);

    dest[0] = dsp->registers[DSP_REG_B2];
    dest[1] = dsp->registers[DSP_REG_B1];
    dest[2] = dsp->registers[DSP_REG_B0];
    newsr = dsp_add56(source, dest);

    dsp->registers[DSP_REG_B2] = dest[0];
    dsp->registers[DSP_REG_B1] = dest[1];
    dsp->registers[DSP_REG_B0] = dest[2];

    emu_ccr_update_e_u_n_z(dsp, dest[0], dest[1], dest[2]);

    dsp->registers[DSP_REG_SR] &= BITMASK(16)-(1<<DSP_SR_V);
    dsp->registers[DSP_REG_SR] |= newsr & 0xfe;
}

static void emu_mac_m_x1_y0_b(dsp_core_t* dsp)
{
    uint32_t source[3], dest[3];
    uint16_t newsr;

    dsp_mul56(dsp->registers[DSP_REG_X1], dsp->registers[DSP_REG_Y0], source, SIGN_MINUS);

    dest[0] = dsp->registers[DSP_REG_B2];
    dest[1] = dsp->registers[DSP_REG_B1];
    dest[2] = dsp->registers[DSP_REG_B0];
    newsr = dsp_add56(source, dest);

    dsp->registers[DSP_REG_B2] = dest[0];
    dsp->registers[DSP_REG_B1] = dest[1];
    dsp->registers[DSP_REG_B0] = dest[2];

    emu_ccr_update_e_u_n_z(dsp, dest[0], dest[1], dest[2]);

    dsp->registers[DSP_REG_SR] &= BITMASK(16)-(1<<DSP_SR_V);
    dsp->registers[DSP_REG_SR] |= newsr & 0xfe;
}

static void emu_mac_p_y1_x1_a(dsp_core_t* dsp)
{
    uint32_t source[3], dest[3];
    uint16_t newsr;

    dsp_mul56(dsp->registers[DSP_REG_Y1], dsp->registers[DSP_REG_X1], source, SIGN_PLUS);

    dest[0] = dsp->registers[DSP_REG_A2];
    dest[1] = dsp->registers[DSP_REG_A1];
    dest[2] = dsp->registers[DSP_REG_A0];
    newsr = dsp_add56(source, dest);

    dsp->registers[DSP_REG_A2] = dest[0];
    dsp->registers[DSP_REG_A1] = dest[1];
    dsp->registers[DSP_REG_A0] = dest[2];

    emu_ccr_update_e_u_n_z(dsp, dest[0], dest[1], dest[2]);

    dsp->registers[DSP_REG_SR] &= BITMASK(16)-(1<<DSP_SR_V);
    dsp->registers[DSP_REG_SR] |= newsr & 0xfe;
}

static void emu_mac_m_y1_x1_a(dsp_core_t* dsp)
{
    uint32_t source[3], dest[3];
    uint16_t newsr;

    dsp_mul56(dsp->registers[DSP_REG_Y1], dsp->registers[DSP_REG_X1], source, SIGN_MINUS);

    dest[0] = dsp->registers[DSP_REG_A2];
    dest[1] = dsp->registers[DSP_REG_A1];
    dest[2] = dsp->registers[DSP_REG_A0];
    newsr = dsp_add56(source, dest);

    dsp->registers[DSP_REG_A2] = dest[0];
    dsp->registers[DSP_REG_A1] = dest[1];
    dsp->registers[DSP_REG_A0] = dest[2];

    emu_ccr_update_e_u_n_z(dsp, dest[0], dest[1], dest[2]);

    dsp->registers[DSP_REG_SR] &= BITMASK(16)-(1<<DSP_SR_V);
    dsp->registers[DSP_REG_SR] |= newsr & 0xfe;
}

static void emu_mac_p_y1_x1_b(dsp_core_t* dsp)
{
    uint32_t source[3], dest[3];
    uint16_t newsr;

    dsp_mul56(dsp->registers[DSP_REG_Y1], dsp->registers[DSP_REG_X1], source, SIGN_PLUS);

    dest[0] = dsp->registers[DSP_REG_B2];
    dest[1] = dsp->registers[DSP_REG_B1];
    dest[2] = dsp->registers[DSP_REG_B0];
    newsr = dsp_add56(source, dest);

    dsp->registers[DSP_REG_B2] = dest[0];
    dsp->registers[DSP_REG_B1] = dest[1];
    dsp->registers[DSP_REG_B0] = dest[2];

    emu_ccr_update_e_u_n_z(dsp, dest[0], dest[1], dest[2]);

    dsp->registers[DSP_REG_SR] &= BITMASK(16)-(1<<DSP_SR_V);
    dsp->registers[DSP_REG_SR] |= newsr & 0xfe;
}

static void emu_mac_m_y1_x1_b(dsp_core_t* dsp)
{
    uint32_t source[3], dest[3];
    uint16_t newsr;

    dsp_mul56(dsp->registers[DSP_REG_Y1], dsp->registers[DSP_REG_X1], source, SIGN_MINUS);

    dest[0] = dsp->registers[DSP_REG_B2];
    dest[1] = dsp->registers[DSP_REG_B1];
    dest[2] = dsp->registers[DSP_REG_B0];
    newsr = dsp_add56(source, dest);

    dsp->registers[DSP_REG_B2] = dest[0];
    dsp->registers[DSP_REG_B1] = dest[1];
    dsp->registers[DSP_REG_B0] = dest[2];

    emu_ccr_update_e_u_n_z(dsp, dest[0], dest[1], dest[2]);

    dsp->registers[DSP_REG_SR] &= BITMASK(16)-(1<<DSP_SR_V);
    dsp->registers[DSP_REG_SR] |= newsr & 0xfe;
}

static void emu_macr_p_x0_x0_a(dsp_core_t* dsp)
{
    uint32_t source[3], dest[3];
    uint16_t newsr;

    dsp_mul56(dsp->registers[DSP_REG_X0], dsp->registers[DSP_REG_X0], source, SIGN_PLUS);

    dest[0] = dsp->registers[DSP_REG_A2];
    dest[1] = dsp->registers[DSP_REG_A1];
    dest[2] = dsp->registers[DSP_REG_A0];
    newsr = dsp_add56(source, dest);

    newsr ^= dsp_rnd56(dsp, dest); /* V: of D + P + r */

    dsp->registers[DSP_REG_A2] = dest[0];
    dsp->registers[DSP_REG_A1] = dest[1];
    dsp->registers[DSP_REG_A0] = dest[2];

    emu_ccr_update_e_u_n_z(dsp, dest[0], dest[1], dest[2]);

    dsp->registers[DSP_REG_SR] &= BITMASK(16)-(1<<DSP_SR_V);
    dsp->registers[DSP_REG_SR] |= newsr & 0xfe;
}

static void emu_macr_m_x0_x0_a(dsp_core_t* dsp)
{
    uint32_t source[3], dest[3];
    uint16_t newsr;

    dsp_mul56(dsp->registers[DSP_REG_X0], dsp->registers[DSP_REG_X0], source, SIGN_MINUS);

    dest[0] = dsp->registers[DSP_REG_A2];
    dest[1] = dsp->registers[DSP_REG_A1];
    dest[2] = dsp->registers[DSP_REG_A0];
    newsr = dsp_add56(source, dest);

    newsr ^= dsp_rnd56(dsp, dest); /* V: of D + P + r */

    dsp->registers[DSP_REG_A2] = dest[0];
    dsp->registers[DSP_REG_A1] = dest[1];
    dsp->registers[DSP_REG_A0] = dest[2];

    emu_ccr_update_e_u_n_z(dsp, dest[0], dest[1], dest[2]);

    dsp->registers[DSP_REG_SR] &= BITMASK(16)-(1<<DSP_SR_V);
    dsp->registers[DSP_REG_SR] |= newsr & 0xfe;
}
static void emu_macr_p_x0_x0_b(dsp_core_t* dsp)
{
    uint32_t source[3], dest[3];
    uint16_t newsr;

    dsp_mul56(dsp->registers[DSP_REG_X0], dsp->registers[DSP_REG_X0], source, SIGN_PLUS);

    dest[0] = dsp->registers[DSP_REG_B2];
    dest[1] = dsp->registers[DSP_REG_B1];
    dest[2] = dsp->registers[DSP_REG_B0];
    newsr = dsp_add56(source, dest);

    newsr ^= dsp_rnd56(dsp, dest); /* V: of D + P + r */

    dsp->registers[DSP_REG_B2] = dest[0];
    dsp->registers[DSP_REG_B1] = dest[1];
    dsp->registers[DSP_REG_B0] = dest[2];

    emu_ccr_update_e_u_n_z(dsp, dest[0], dest[1], dest[2]);

    dsp->registers[DSP_REG_SR] &= BITMASK(16)-(1<<DSP_SR_V);
    dsp->registers[DSP_REG_SR] |= newsr & 0xfe;
}

static void emu_macr_m_x0_x0_b(dsp_core_t* dsp)
{
    uint32_t source[3], dest[3];
    uint16_t newsr;

    dsp_mul56(dsp->registers[DSP_REG_X0], dsp->registers[DSP_REG_X0], source, SIGN_MINUS);

    dest[0] = dsp->registers[DSP_REG_B2];
    dest[1] = dsp->registers[DSP_REG_B1];
    dest[2] = dsp->registers[DSP_REG_B0];
    newsr = dsp_add56(source, dest);

    newsr ^= dsp_rnd56(dsp, dest); /* V: of D + P + r */

    dsp->registers[DSP_REG_B2] = dest[0];
    dsp->registers[DSP_REG_B1] = dest[1];
    dsp->registers[DSP_REG_B0] = dest[2];

    emu_ccr_update_e_u_n_z(dsp, dest[0], dest[1], dest[2]);

    dsp->registers[DSP_REG_SR] &= BITMASK(16)-(1<<DSP_SR_V);
    dsp->registers[DSP_REG_SR] |= newsr & 0xfe;
}

static void emu_macr_p_y0_y0_a(dsp_core_t* dsp)
{
    uint32_t source[3], dest[3];
    uint16_t newsr;

    dsp_mul56(dsp->registers[DSP_REG_Y0], dsp->registers[DSP_REG_Y0], source, SIGN_PLUS);

    dest[0] = dsp->registers[DSP_REG_A2];
    dest[1] = dsp->registers[DSP_REG_A1];
    dest[2] = dsp->registers[DSP_REG_A0];
    newsr = dsp_add56(source, dest);

    newsr ^= dsp_rnd56(dsp, dest); /* V: of D + P + r */

    dsp->registers[DSP_REG_A2] = dest[0];
    dsp->registers[DSP_REG_A1] = dest[1];
    dsp->registers[DSP_REG_A0] = dest[2];

    emu_ccr_update_e_u_n_z(dsp, dest[0], dest[1], dest[2]);

    dsp->registers[DSP_REG_SR] &= BITMASK(16)-(1<<DSP_SR_V);
    dsp->registers[DSP_REG_SR] |= newsr & 0xfe;
}

static void emu_macr_m_y0_y0_a(dsp_core_t* dsp)
{
    uint32_t source[3], dest[3];
    uint16_t newsr;

    dsp_mul56(dsp->registers[DSP_REG_Y0], dsp->registers[DSP_REG_Y0], source, SIGN_MINUS);

    dest[0] = dsp->registers[DSP_REG_A2];
    dest[1] = dsp->registers[DSP_REG_A1];
    dest[2] = dsp->registers[DSP_REG_A0];
    newsr = dsp_add56(source, dest);

    newsr ^= dsp_rnd56(dsp, dest); /* V: of D + P + r */

    dsp->registers[DSP_REG_A2] = dest[0];
    dsp->registers[DSP_REG_A1] = dest[1];
    dsp->registers[DSP_REG_A0] = dest[2];

    emu_ccr_update_e_u_n_z(dsp, dest[0], dest[1], dest[2]);

    dsp->registers[DSP_REG_SR] &= BITMASK(16)-(1<<DSP_SR_V);
    dsp->registers[DSP_REG_SR] |= newsr & 0xfe;
}
static void emu_macr_p_y0_y0_b(dsp_core_t* dsp)
{
    uint32_t source[3], dest[3];
    uint16_t newsr;

    dsp_mul56(dsp->registers[DSP_REG_Y0], dsp->registers[DSP_REG_Y0], source, SIGN_PLUS);

    dest[0] = dsp->registers[DSP_REG_B2];
    dest[1] = dsp->registers[DSP_REG_B1];
    dest[2] = dsp->registers[DSP_REG_B0];
    newsr = dsp_add56(source, dest);

    newsr ^= dsp_rnd56(dsp, dest); /* V: of D + P + r */

    dsp->registers[DSP_REG_B2] = dest[0];
    dsp->registers[DSP_REG_B1] = dest[1];
    dsp->registers[DSP_REG_B0] = dest[2];

    emu_ccr_update_e_u_n_z(dsp, dest[0], dest[1], dest[2]);

    dsp->registers[DSP_REG_SR] &= BITMASK(16)-(1<<DSP_SR_V);
    dsp->registers[DSP_REG_SR] |= newsr & 0xfe;
}

static void emu_macr_m_y0_y0_b(dsp_core_t* dsp)
{
    uint32_t source[3], dest[3];
    uint16_t newsr;

    dsp_mul56(dsp->registers[DSP_REG_Y0], dsp->registers[DSP_REG_Y0], source, SIGN_MINUS);

    dest[0] = dsp->registers[DSP_REG_B2];
    dest[1] = dsp->registers[DSP_REG_B1];
    dest[2] = dsp->registers[DSP_REG_B0];
    newsr = dsp_add56(source, dest);

    newsr ^= dsp_rnd56(dsp, dest); /* V: of D + P + r */

    dsp->registers[DSP_REG_B2] = dest[0];
    dsp->registers[DSP_REG_B1] = dest[1];
    dsp->registers[DSP_REG_B0] = dest[2];

    emu_ccr_update_e_u_n_z(dsp, dest[0], dest[1], dest[2]);

    dsp->registers[DSP_REG_SR] &= BITMASK(16)-(1<<DSP_SR_V);
    dsp->registers[DSP_REG_SR] |= newsr & 0xfe;
}

static void emu_macr_p_x1_x0_a(dsp_core_t* dsp)
{
    uint32_t source[3], dest[3];
    uint16_t newsr;

    dsp_mul56(dsp->registers[DSP_REG_X1], dsp->registers[DSP_REG_X0], source, SIGN_PLUS);

    dest[0] = dsp->registers[DSP_REG_A2];
    dest[1] = dsp->registers[DSP_REG_A1];
    dest[2] = dsp->registers[DSP_REG_A0];
    newsr = dsp_add56(source, dest);

    newsr ^= dsp_rnd56(dsp, dest); /* V: of D + P + r */

    dsp->registers[DSP_REG_A2] = dest[0];
    dsp->registers[DSP_REG_A1] = dest[1];
    dsp->registers[DSP_REG_A0] = dest[2];

    emu_ccr_update_e_u_n_z(dsp, dest[0], dest[1], dest[2]);

    dsp->registers[DSP_REG_SR] &= BITMASK(16)-(1<<DSP_SR_V);
    dsp->registers[DSP_REG_SR] |= newsr & 0xfe;
}

static void emu_macr_m_x1_x0_a(dsp_core_t* dsp)
{
    uint32_t source[3], dest[3];
    uint16_t newsr;

    dsp_mul56(dsp->registers[DSP_REG_X1], dsp->registers[DSP_REG_X0], source, SIGN_MINUS);

    dest[0] = dsp->registers[DSP_REG_A2];
    dest[1] = dsp->registers[DSP_REG_A1];
    dest[2] = dsp->registers[DSP_REG_A0];
    newsr = dsp_add56(source, dest);

    newsr ^= dsp_rnd56(dsp, dest); /* V: of D + P + r */

    dsp->registers[DSP_REG_A2] = dest[0];
    dsp->registers[DSP_REG_A1] = dest[1];
    dsp->registers[DSP_REG_A0] = dest[2];

    emu_ccr_update_e_u_n_z(dsp, dest[0], dest[1], dest[2]);

    dsp->registers[DSP_REG_SR] &= BITMASK(16)-(1<<DSP_SR_V);
    dsp->registers[DSP_REG_SR] |= newsr & 0xfe;
}

static void emu_macr_p_x1_x0_b(dsp_core_t* dsp)
{
    uint32_t source[3], dest[3];
    uint16_t newsr;

    dsp_mul56(dsp->registers[DSP_REG_X1], dsp->registers[DSP_REG_X0], source, SIGN_PLUS);

    dest[0] = dsp->registers[DSP_REG_B2];
    dest[1] = dsp->registers[DSP_REG_B1];
    dest[2] = dsp->registers[DSP_REG_B0];
    newsr = dsp_add56(source, dest);

    newsr ^= dsp_rnd56(dsp, dest); /* V: of D + P + r */

    dsp->registers[DSP_REG_B2] = dest[0];
    dsp->registers[DSP_REG_B1] = dest[1];
    dsp->registers[DSP_REG_B0] = dest[2];

    emu_ccr_update_e_u_n_z(dsp, dest[0], dest[1], dest[2]);

    dsp->registers[DSP_REG_SR] &= BITMASK(16)-(1<<DSP_SR_V);
    dsp->registers[DSP_REG_SR] |= newsr & 0xfe;
}

static void emu_macr_m_x1_x0_b(dsp_core_t* dsp)
{
    uint32_t source[3], dest[3];
    uint16_t newsr;

    dsp_mul56(dsp->registers[DSP_REG_X1], dsp->registers[DSP_REG_X0], source, SIGN_MINUS);

    dest[0] = dsp->registers[DSP_REG_B2];
    dest[1] = dsp->registers[DSP_REG_B1];
    dest[2] = dsp->registers[DSP_REG_B0];
    newsr = dsp_add56(source, dest);

    newsr ^= dsp_rnd56(dsp, dest); /* V: of D + P + r */

    dsp->registers[DSP_REG_B2] = dest[0];
    dsp->registers[DSP_REG_B1] = dest[1];
    dsp->registers[DSP_REG_B0] = dest[2];

    emu_ccr_update_e_u_n_z(dsp, dest[0], dest[1], dest[2]);

    dsp->registers[DSP_REG_SR] &= BITMASK(16)-(1<<DSP_SR_V);
    dsp->registers[DSP_REG_SR] |= newsr & 0xfe;
}

static void emu_macr_p_y1_y0_a(dsp_core_t* dsp)
{
    uint32_t source[3], dest[3];
    uint16_t newsr;

    dsp_mul56(dsp->registers[DSP_REG_Y1], dsp->registers[DSP_REG_Y0], source, SIGN_PLUS);

    dest[0] = dsp->registers[DSP_REG_A2];
    dest[1] = dsp->registers[DSP_REG_A1];
    dest[2] = dsp->registers[DSP_REG_A0];
    newsr = dsp_add56(source, dest);

    newsr ^= dsp_rnd56(dsp, dest); /* V: of D + P + r */

    dsp->registers[DSP_REG_A2] = dest[0];
    dsp->registers[DSP_REG_A1] = dest[1];
    dsp->registers[DSP_REG_A0] = dest[2];

    emu_ccr_update_e_u_n_z(dsp, dest[0], dest[1], dest[2]);

    dsp->registers[DSP_REG_SR] &= BITMASK(16)-(1<<DSP_SR_V);
    dsp->registers[DSP_REG_SR] |= newsr & 0xfe;
}

static void emu_macr_m_y1_y0_a(dsp_core_t* dsp)
{
    uint32_t source[3], dest[3];
    uint16_t newsr;

    dsp_mul56(dsp->registers[DSP_REG_Y1], dsp->registers[DSP_REG_Y0], source, SIGN_MINUS);

    dest[0] = dsp->registers[DSP_REG_A2];
    dest[1] = dsp->registers[DSP_REG_A1];
    dest[2] = dsp->registers[DSP_REG_A0];
    newsr = dsp_add56(source, dest);

    newsr ^= dsp_rnd56(dsp, dest); /* V: of D + P + r */

    dsp->registers[DSP_REG_A2] = dest[0];
    dsp->registers[DSP_REG_A1] = dest[1];
    dsp->registers[DSP_REG_A0] = dest[2];

    emu_ccr_update_e_u_n_z(dsp, dest[0], dest[1], dest[2]);

    dsp->registers[DSP_REG_SR] &= BITMASK(16)-(1<<DSP_SR_V);
    dsp->registers[DSP_REG_SR] |= newsr & 0xfe;
}

static void emu_macr_p_y1_y0_b(dsp_core_t* dsp)
{
    uint32_t source[3], dest[3];
    uint16_t newsr;

    dsp_mul56(dsp->registers[DSP_REG_Y1], dsp->registers[DSP_REG_Y0], source, SIGN_PLUS);

    dest[0] = dsp->registers[DSP_REG_B2];
    dest[1] = dsp->registers[DSP_REG_B1];
    dest[2] = dsp->registers[DSP_REG_B0];
    newsr = dsp_add56(source, dest);

    newsr ^= dsp_rnd56(dsp, dest); /* V: of D + P + r */

    dsp->registers[DSP_REG_B2] = dest[0];
    dsp->registers[DSP_REG_B1] = dest[1];
    dsp->registers[DSP_REG_B0] = dest[2];

    emu_ccr_update_e_u_n_z(dsp, dest[0], dest[1], dest[2]);

    dsp->registers[DSP_REG_SR] &= BITMASK(16)-(1<<DSP_SR_V);
    dsp->registers[DSP_REG_SR] |= newsr & 0xfe;
}

static void emu_macr_m_y1_y0_b(dsp_core_t* dsp)
{
    uint32_t source[3], dest[3];
    uint16_t newsr;

    dsp_mul56(dsp->registers[DSP_REG_Y1], dsp->registers[DSP_REG_Y0], source, SIGN_MINUS);

    dest[0] = dsp->registers[DSP_REG_B2];
    dest[1] = dsp->registers[DSP_REG_B1];
    dest[2] = dsp->registers[DSP_REG_B0];
    newsr = dsp_add56(source, dest);

    newsr ^= dsp_rnd56(dsp, dest); /* V: of D + P + r */

    dsp->registers[DSP_REG_B2] = dest[0];
    dsp->registers[DSP_REG_B1] = dest[1];
    dsp->registers[DSP_REG_B0] = dest[2];

    emu_ccr_update_e_u_n_z(dsp, dest[0], dest[1], dest[2]);

    dsp->registers[DSP_REG_SR] &= BITMASK(16)-(1<<DSP_SR_V);
    dsp->registers[DSP_REG_SR] |= newsr & 0xfe;
}

static void emu_macr_p_x0_y1_a(dsp_core_t* dsp)
{
    uint32_t source[3], dest[3];
    uint16_t newsr;

    dsp_mul56(dsp->registers[DSP_REG_X0], dsp->registers[DSP_REG_Y1], source, SIGN_PLUS);

    dest[0] = dsp->registers[DSP_REG_A2];
    dest[1] = dsp->registers[DSP_REG_A1];
    dest[2] = dsp->registers[DSP_REG_A0];
    newsr = dsp_add56(source, dest);

    newsr ^= dsp_rnd56(dsp, dest); /* V: of D + P + r */

    dsp->registers[DSP_REG_A2] = dest[0];
    dsp->registers[DSP_REG_A1] = dest[1];
    dsp->registers[DSP_REG_A0] = dest[2];

    emu_ccr_update_e_u_n_z(dsp, dest[0], dest[1], dest[2]);

    dsp->registers[DSP_REG_SR] &= BITMASK(16)-(1<<DSP_SR_V);
    dsp->registers[DSP_REG_SR] |= newsr & 0xfe;
}

static void emu_macr_m_x0_y1_a(dsp_core_t* dsp)
{
    uint32_t source[3], dest[3];
    uint16_t newsr;

    dsp_mul56(dsp->registers[DSP_REG_X0], dsp->registers[DSP_REG_Y1], source, SIGN_MINUS);

    dest[0] = dsp->registers[DSP_REG_A2];
    dest[1] = dsp->registers[DSP_REG_A1];
    dest[2] = dsp->registers[DSP_REG_A0];
    newsr = dsp_add56(source, dest);

    newsr ^= dsp_rnd56(dsp, dest); /* V: of D + P + r */

    dsp->registers[DSP_REG_A2] = dest[0];
    dsp->registers[DSP_REG_A1] = dest[1];
    dsp->registers[DSP_REG_A0] = dest[2];

    emu_ccr_update_e_u_n_z(dsp, dest[0], dest[1], dest[2]);

    dsp->registers[DSP_REG_SR] &= BITMASK(16)-(1<<DSP_SR_V);
    dsp->registers[DSP_REG_SR] |= newsr & 0xfe;
}

static void emu_macr_p_x0_y1_b(dsp_core_t* dsp)
{
    uint32_t source[3], dest[3];
    uint16_t newsr;

    dsp_mul56(dsp->registers[DSP_REG_X0], dsp->registers[DSP_REG_Y1], source, SIGN_PLUS);

    dest[0] = dsp->registers[DSP_REG_B2];
    dest[1] = dsp->registers[DSP_REG_B1];
    dest[2] = dsp->registers[DSP_REG_B0];
    newsr = dsp_add56(source, dest);

    newsr ^= dsp_rnd56(dsp, dest); /* V: of D + P + r */

    dsp->registers[DSP_REG_B2] = dest[0];
    dsp->registers[DSP_REG_B1] = dest[1];
    dsp->registers[DSP_REG_B0] = dest[2];

    emu_ccr_update_e_u_n_z(dsp, dest[0], dest[1], dest[2]);

    dsp->registers[DSP_REG_SR] &= BITMASK(16)-(1<<DSP_SR_V);
    dsp->registers[DSP_REG_SR] |= newsr & 0xfe;
}

static void emu_macr_m_x0_y1_b(dsp_core_t* dsp)
{
    uint32_t source[3], dest[3];
    uint16_t newsr;

    dsp_mul56(dsp->registers[DSP_REG_X0], dsp->registers[DSP_REG_Y1], source, SIGN_MINUS);

    dest[0] = dsp->registers[DSP_REG_B2];
    dest[1] = dsp->registers[DSP_REG_B1];
    dest[2] = dsp->registers[DSP_REG_B0];
    newsr = dsp_add56(source, dest);

    newsr ^= dsp_rnd56(dsp, dest); /* V: of D + P + r */

    dsp->registers[DSP_REG_B2] = dest[0];
    dsp->registers[DSP_REG_B1] = dest[1];
    dsp->registers[DSP_REG_B0] = dest[2];

    emu_ccr_update_e_u_n_z(dsp, dest[0], dest[1], dest[2]);

    dsp->registers[DSP_REG_SR] &= BITMASK(16)-(1<<DSP_SR_V);
    dsp->registers[DSP_REG_SR] |= newsr & 0xfe;
}

static void emu_macr_p_y0_x0_a(dsp_core_t* dsp)
{
    uint32_t source[3], dest[3];
    uint16_t newsr;

    dsp_mul56(dsp->registers[DSP_REG_Y0], dsp->registers[DSP_REG_X0], source, SIGN_PLUS);

    dest[0] = dsp->registers[DSP_REG_A2];
    dest[1] = dsp->registers[DSP_REG_A1];
    dest[2] = dsp->registers[DSP_REG_A0];
    newsr = dsp_add56(source, dest);

    newsr ^= dsp_rnd56(dsp, dest); /* V: of D + P + r */

    dsp->registers[DSP_REG_A2] = dest[0];
    dsp->registers[DSP_REG_A1] = dest[1];
    dsp->registers[DSP_REG_A0] = dest[2];

    emu_ccr_update_e_u_n_z(dsp, dest[0], dest[1], dest[2]);

    dsp->registers[DSP_REG_SR] &= BITMASK(16)-(1<<DSP_SR_V);
    dsp->registers[DSP_REG_SR] |= newsr & 0xfe;
}

static void emu_macr_m_y0_x0_a(dsp_core_t* dsp)
{
    uint32_t source[3], dest[3];
    uint16_t newsr;

    dsp_mul56(dsp->registers[DSP_REG_Y0], dsp->registers[DSP_REG_X0], source, SIGN_MINUS);

    dest[0] = dsp->registers[DSP_REG_A2];
    dest[1] = dsp->registers[DSP_REG_A1];
    dest[2] = dsp->registers[DSP_REG_A0];
    newsr = dsp_add56(source, dest);

    newsr ^= dsp_rnd56(dsp, dest); /* V: of D + P + r */

    dsp->registers[DSP_REG_A2] = dest[0];
    dsp->registers[DSP_REG_A1] = dest[1];
    dsp->registers[DSP_REG_A0] = dest[2];

    emu_ccr_update_e_u_n_z(dsp, dest[0], dest[1], dest[2]);

    dsp->registers[DSP_REG_SR] &= BITMASK(16)-(1<<DSP_SR_V);
    dsp->registers[DSP_REG_SR] |= newsr & 0xfe;
}

static void emu_macr_p_y0_x0_b(dsp_core_t* dsp)
{
    uint32_t source[3], dest[3];
    uint16_t newsr;

    dsp_mul56(dsp->registers[DSP_REG_Y0], dsp->registers[DSP_REG_X0], source, SIGN_PLUS);

    dest[0] = dsp->registers[DSP_REG_B2];
    dest[1] = dsp->registers[DSP_REG_B1];
    dest[2] = dsp->registers[DSP_REG_B0];
    newsr = dsp_add56(source, dest);

    newsr ^= dsp_rnd56(dsp, dest); /* V: of D + P + r */

    dsp->registers[DSP_REG_B2] = dest[0];
    dsp->registers[DSP_REG_B1] = dest[1];
    dsp->registers[DSP_REG_B0] = dest[2];

    emu_ccr_update_e_u_n_z(dsp, dest[0], dest[1], dest[2]);

    dsp->registers[DSP_REG_SR] &= BITMASK(16)-(1<<DSP_SR_V);
    dsp->registers[DSP_REG_SR] |= newsr & 0xfe;
}

static void emu_macr_m_y0_x0_b(dsp_core_t* dsp)
{
    uint32_t source[3], dest[3];
    uint16_t newsr;

    dsp_mul56(dsp->registers[DSP_REG_Y0], dsp->registers[DSP_REG_X0], source, SIGN_MINUS);

    dest[0] = dsp->registers[DSP_REG_B2];
    dest[1] = dsp->registers[DSP_REG_B1];
    dest[2] = dsp->registers[DSP_REG_B0];
    newsr = dsp_add56(source, dest);

    newsr ^= dsp_rnd56(dsp, dest); /* V: of D + P + r */

    dsp->registers[DSP_REG_B2] = dest[0];
    dsp->registers[DSP_REG_B1] = dest[1];
    dsp->registers[DSP_REG_B0] = dest[2];

    emu_ccr_update_e_u_n_z(dsp, dest[0], dest[1], dest[2]);

    dsp->registers[DSP_REG_SR] &= BITMASK(16)-(1<<DSP_SR_V);
    dsp->registers[DSP_REG_SR] |= newsr & 0xfe;
}

static void emu_macr_p_x1_y0_a(dsp_core_t* dsp)
{
    uint32_t source[3], dest[3];
    uint16_t newsr;

    dsp_mul56(dsp->registers[DSP_REG_X1], dsp->registers[DSP_REG_Y0], source, SIGN_PLUS);

    dest[0] = dsp->registers[DSP_REG_A2];
    dest[1] = dsp->registers[DSP_REG_A1];
    dest[2] = dsp->registers[DSP_REG_A0];
    newsr = dsp_add56(source, dest);

    newsr ^= dsp_rnd56(dsp, dest); /* V: of D + P + r */

    dsp->registers[DSP_REG_A2] = dest[0];
    dsp->registers[DSP_REG_A1] = dest[1];
    dsp->registers[DSP_REG_A0] = dest[2];

    emu_ccr_update_e_u_n_z(dsp, dest[0], dest[1], dest[2]);

    dsp->registers[DSP_REG_SR] &= BITMASK(16)-(1<<DSP_SR_V);
    dsp->registers[DSP_REG_SR] |= newsr & 0xfe;
}

static void emu_macr_m_x1_y0_a(dsp_core_t* dsp)
{
    uint32_t source[3], dest[3];
    uint16_t newsr;

    dsp_mul56(dsp->registers[DSP_REG_X1], dsp->registers[DSP_REG_Y0], source, SIGN_MINUS);

    dest[0] = dsp->registers[DSP_REG_A2];
    dest[1] = dsp->registers[DSP_REG_A1];
    dest[2] = dsp->registers[DSP_REG_A0];
    newsr = dsp_add56(source, dest);

    newsr ^= dsp_rnd56(dsp, dest); /* V: of D + P + r */

    dsp->registers[DSP_REG_A2] = dest[0];
    dsp->registers[DSP_REG_A1] = dest[1];
    dsp->registers[DSP_REG_A0] = dest[2];

    emu_ccr_update_e_u_n_z(dsp, dest[0], dest[1], dest[2]);

    dsp->registers[DSP_REG_SR] &= BITMASK(16)-(1<<DSP_SR_V);
    dsp->registers[DSP_REG_SR] |= newsr & 0xfe;
}

static void emu_macr_p_x1_y0_b(dsp_core_t* dsp)
{
    uint32_t source[3], dest[3];
    uint16_t newsr;

    dsp_mul56(dsp->registers[DSP_REG_X1], dsp->registers[DSP_REG_Y0], source, SIGN_PLUS);

    dest[0] = dsp->registers[DSP_REG_B2];
    dest[1] = dsp->registers[DSP_REG_B1];
    dest[2] = dsp->registers[DSP_REG_B0];
    newsr = dsp_add56(source, dest);

    /* xbased: rounded after the add, as every other macr (it rounded a
     * dest not read yet: macr +x1,y0,b ran as a mac) */
    newsr ^= dsp_rnd56(dsp, dest); /* V: of D + P + r */

    dsp->registers[DSP_REG_B2] = dest[0];
    dsp->registers[DSP_REG_B1] = dest[1];
    dsp->registers[DSP_REG_B0] = dest[2];

    emu_ccr_update_e_u_n_z(dsp, dest[0], dest[1], dest[2]);

    dsp->registers[DSP_REG_SR] &= BITMASK(16)-(1<<DSP_SR_V);
    dsp->registers[DSP_REG_SR] |= newsr & 0xfe;
}

static void emu_macr_m_x1_y0_b(dsp_core_t* dsp)
{
    uint32_t source[3], dest[3];
    uint16_t newsr;

    dsp_mul56(dsp->registers[DSP_REG_X1], dsp->registers[DSP_REG_Y0], source, SIGN_MINUS);

    dest[0] = dsp->registers[DSP_REG_B2];
    dest[1] = dsp->registers[DSP_REG_B1];
    dest[2] = dsp->registers[DSP_REG_B0];
    newsr = dsp_add56(source, dest);

    newsr ^= dsp_rnd56(dsp, dest); /* V: of D + P + r */

    dsp->registers[DSP_REG_B2] = dest[0];
    dsp->registers[DSP_REG_B1] = dest[1];
    dsp->registers[DSP_REG_B0] = dest[2];

    emu_ccr_update_e_u_n_z(dsp, dest[0], dest[1], dest[2]);

    dsp->registers[DSP_REG_SR] &= BITMASK(16)-(1<<DSP_SR_V);
    dsp->registers[DSP_REG_SR] |= newsr & 0xfe;
}

static void emu_macr_p_y1_x1_a(dsp_core_t* dsp)
{
    uint32_t source[3], dest[3];
    uint16_t newsr;

    dsp_mul56(dsp->registers[DSP_REG_Y1], dsp->registers[DSP_REG_X1], source, SIGN_PLUS);

    dest[0] = dsp->registers[DSP_REG_A2];
    dest[1] = dsp->registers[DSP_REG_A1];
    dest[2] = dsp->registers[DSP_REG_A0];
    newsr = dsp_add56(source, dest);

    newsr ^= dsp_rnd56(dsp, dest); /* V: of D + P + r */

    dsp->registers[DSP_REG_A2] = dest[0];
    dsp->registers[DSP_REG_A1] = dest[1];
    dsp->registers[DSP_REG_A0] = dest[2];

    emu_ccr_update_e_u_n_z(dsp, dest[0], dest[1], dest[2]);

    dsp->registers[DSP_REG_SR] &= BITMASK(16)-(1<<DSP_SR_V);
    dsp->registers[DSP_REG_SR] |= newsr & 0xfe;
}

static void emu_macr_m_y1_x1_a(dsp_core_t* dsp)
{
    uint32_t source[3], dest[3];
    uint16_t newsr;

    dsp_mul56(dsp->registers[DSP_REG_Y1], dsp->registers[DSP_REG_X1], source, SIGN_MINUS);

    dest[0] = dsp->registers[DSP_REG_A2];
    dest[1] = dsp->registers[DSP_REG_A1];
    dest[2] = dsp->registers[DSP_REG_A0];
    newsr = dsp_add56(source, dest);

    newsr ^= dsp_rnd56(dsp, dest); /* V: of D + P + r */

    dsp->registers[DSP_REG_A2] = dest[0];
    dsp->registers[DSP_REG_A1] = dest[1];
    dsp->registers[DSP_REG_A0] = dest[2];

    emu_ccr_update_e_u_n_z(dsp, dest[0], dest[1], dest[2]);

    dsp->registers[DSP_REG_SR] &= BITMASK(16)-(1<<DSP_SR_V);
    dsp->registers[DSP_REG_SR] |= newsr & 0xfe;
}

static void emu_macr_p_y1_x1_b(dsp_core_t* dsp)
{
    uint32_t source[3], dest[3];
    uint16_t newsr;

    dsp_mul56(dsp->registers[DSP_REG_Y1], dsp->registers[DSP_REG_X1], source, SIGN_PLUS);

    dest[0] = dsp->registers[DSP_REG_B2];
    dest[1] = dsp->registers[DSP_REG_B1];
    dest[2] = dsp->registers[DSP_REG_B0];
    newsr = dsp_add56(source, dest);

    newsr ^= dsp_rnd56(dsp, dest); /* V: of D + P + r */

    dsp->registers[DSP_REG_B2] = dest[0];
    dsp->registers[DSP_REG_B1] = dest[1];
    dsp->registers[DSP_REG_B0] = dest[2];

    emu_ccr_update_e_u_n_z(dsp, dest[0], dest[1], dest[2]);

    dsp->registers[DSP_REG_SR] &= BITMASK(16)-(1<<DSP_SR_V);
    dsp->registers[DSP_REG_SR] |= newsr & 0xfe;
}

static void emu_macr_m_y1_x1_b(dsp_core_t* dsp)
{
    uint32_t source[3], dest[3];
    uint16_t newsr;

    dsp_mul56(dsp->registers[DSP_REG_Y1], dsp->registers[DSP_REG_X1], source, SIGN_MINUS);

    dest[0] = dsp->registers[DSP_REG_B2];
    dest[1] = dsp->registers[DSP_REG_B1];
    dest[2] = dsp->registers[DSP_REG_B0];
    newsr = dsp_add56(source, dest);

    newsr ^= dsp_rnd56(dsp, dest); /* V: of D + P + r */

    dsp->registers[DSP_REG_B2] = dest[0];
    dsp->registers[DSP_REG_B1] = dest[1];
    dsp->registers[DSP_REG_B0] = dest[2];

    emu_ccr_update_e_u_n_z(dsp, dest[0], dest[1], dest[2]);

    dsp->registers[DSP_REG_SR] &= BITMASK(16)-(1<<DSP_SR_V);
    dsp->registers[DSP_REG_SR] |= newsr & 0xfe;
}


static void emu_move(dsp_core_t* dsp)
{
    /*  move instruction inside alu opcodes
        taken care of by parallel move dispatcher */
}

static void emu_mpy_p_x0_x0_a(dsp_core_t* dsp)
{
    uint32_t source[3];

    dsp_mul56(dsp->registers[DSP_REG_X0], dsp->registers[DSP_REG_X0], source, SIGN_PLUS);

    dsp->registers[DSP_REG_A2] = source[0];
    dsp->registers[DSP_REG_A1] = source[1];
    dsp->registers[DSP_REG_A0] = source[2];

    emu_ccr_update_e_u_n_z(dsp, source[0], source[1], source[2]);
    dsp->registers[DSP_REG_SR] &= BITMASK(16)-(1<<DSP_SR_V);
}

static void emu_mpy_m_x0_x0_a(dsp_core_t* dsp)
{
    uint32_t source[3];

    dsp_mul56(dsp->registers[DSP_REG_X0], dsp->registers[DSP_REG_X0], source, SIGN_MINUS);

    dsp->registers[DSP_REG_A2] = source[0];
    dsp->registers[DSP_REG_A1] = source[1];
    dsp->registers[DSP_REG_A0] = source[2];

    emu_ccr_update_e_u_n_z(dsp, source[0], source[1], source[2]);
    dsp->registers[DSP_REG_SR] &= BITMASK(16)-(1<<DSP_SR_V);
}

static void emu_mpy_p_x0_x0_b(dsp_core_t* dsp)
{
    uint32_t source[3];

    dsp_mul56(dsp->registers[DSP_REG_X0], dsp->registers[DSP_REG_X0], source, SIGN_PLUS);

    dsp->registers[DSP_REG_B2] = source[0];
    dsp->registers[DSP_REG_B1] = source[1];
    dsp->registers[DSP_REG_B0] = source[2];

    emu_ccr_update_e_u_n_z(dsp, source[0], source[1], source[2]);
    dsp->registers[DSP_REG_SR] &= BITMASK(16)-(1<<DSP_SR_V);
}

static void emu_mpy_m_x0_x0_b(dsp_core_t* dsp)
{
    uint32_t source[3];


    dsp_mul56(dsp->registers[DSP_REG_X0], dsp->registers[DSP_REG_X0], source, SIGN_MINUS);

    dsp->registers[DSP_REG_B2] = source[0];
    dsp->registers[DSP_REG_B1] = source[1];
    dsp->registers[DSP_REG_B0] = source[2];

    emu_ccr_update_e_u_n_z(dsp, source[0], source[1], source[2]);
    dsp->registers[DSP_REG_SR] &= BITMASK(16)-(1<<DSP_SR_V);
}

static void emu_mpy_p_y0_y0_a(dsp_core_t* dsp)
{
    uint32_t source[3];

    dsp_mul56(dsp->registers[DSP_REG_Y0], dsp->registers[DSP_REG_Y0], source, SIGN_PLUS);

    dsp->registers[DSP_REG_A2] = source[0];
    dsp->registers[DSP_REG_A1] = source[1];
    dsp->registers[DSP_REG_A0] = source[2];

    emu_ccr_update_e_u_n_z(dsp, source[0], source[1], source[2]);
    dsp->registers[DSP_REG_SR] &= BITMASK(16)-(1<<DSP_SR_V);
}

static void emu_mpy_m_y0_y0_a(dsp_core_t* dsp)
{
    uint32_t source[3];

    dsp_mul56(dsp->registers[DSP_REG_Y0], dsp->registers[DSP_REG_Y0], source, SIGN_MINUS);

    dsp->registers[DSP_REG_A2] = source[0];
    dsp->registers[DSP_REG_A1] = source[1];
    dsp->registers[DSP_REG_A0] = source[2];

    emu_ccr_update_e_u_n_z(dsp, source[0], source[1], source[2]);
    dsp->registers[DSP_REG_SR] &= BITMASK(16)-(1<<DSP_SR_V);
}

static void emu_mpy_p_y0_y0_b(dsp_core_t* dsp)
{
    uint32_t source[3];

    dsp_mul56(dsp->registers[DSP_REG_Y0], dsp->registers[DSP_REG_Y0], source, SIGN_PLUS);

    dsp->registers[DSP_REG_B2] = source[0];
    dsp->registers[DSP_REG_B1] = source[1];
    dsp->registers[DSP_REG_B0] = source[2];

    emu_ccr_update_e_u_n_z(dsp, source[0], source[1], source[2]);
    dsp->registers[DSP_REG_SR] &= BITMASK(16)-(1<<DSP_SR_V);
}

static void emu_mpy_m_y0_y0_b(dsp_core_t* dsp)
{
    uint32_t source[3];

    dsp_mul56(dsp->registers[DSP_REG_Y0], dsp->registers[DSP_REG_Y0], source, SIGN_MINUS);

    dsp->registers[DSP_REG_B2] = source[0];
    dsp->registers[DSP_REG_B1] = source[1];
    dsp->registers[DSP_REG_B0] = source[2];

    emu_ccr_update_e_u_n_z(dsp, source[0], source[1], source[2]);
    dsp->registers[DSP_REG_SR] &= BITMASK(16)-(1<<DSP_SR_V);
}

static void emu_mpy_p_x1_x0_a(dsp_core_t* dsp)
{
    uint32_t source[3];

    dsp_mul56(dsp->registers[DSP_REG_X1], dsp->registers[DSP_REG_X0], source, SIGN_PLUS);

    dsp->registers[DSP_REG_A2] = source[0];
    dsp->registers[DSP_REG_A1] = source[1];
    dsp->registers[DSP_REG_A0] = source[2];

    emu_ccr_update_e_u_n_z(dsp, source[0], source[1], source[2]);
    dsp->registers[DSP_REG_SR] &= BITMASK(16)-(1<<DSP_SR_V);
}

static void emu_mpy_m_x1_x0_a(dsp_core_t* dsp)
{
    uint32_t source[3];

    dsp_mul56(dsp->registers[DSP_REG_X1], dsp->registers[DSP_REG_X0], source, SIGN_MINUS);

    dsp->registers[DSP_REG_A2] = source[0];
    dsp->registers[DSP_REG_A1] = source[1];
    dsp->registers[DSP_REG_A0] = source[2];

    emu_ccr_update_e_u_n_z(dsp, source[0], source[1], source[2]);
    dsp->registers[DSP_REG_SR] &= BITMASK(16)-(1<<DSP_SR_V);
}

static void emu_mpy_p_x1_x0_b(dsp_core_t* dsp)
{
    uint32_t source[3];

    dsp_mul56(dsp->registers[DSP_REG_X1], dsp->registers[DSP_REG_X0], source, SIGN_PLUS);

    dsp->registers[DSP_REG_B2] = source[0];
    dsp->registers[DSP_REG_B1] = source[1];
    dsp->registers[DSP_REG_B0] = source[2];

    emu_ccr_update_e_u_n_z(dsp, source[0], source[1], source[2]);
    dsp->registers[DSP_REG_SR] &= BITMASK(16)-(1<<DSP_SR_V);
}

static void emu_mpy_m_x1_x0_b(dsp_core_t* dsp)
{
    uint32_t source[3];

    dsp_mul56(dsp->registers[DSP_REG_X1], dsp->registers[DSP_REG_X0], source, SIGN_MINUS);

    dsp->registers[DSP_REG_B2] = source[0];
    dsp->registers[DSP_REG_B1] = source[1];
    dsp->registers[DSP_REG_B0] = source[2];

    emu_ccr_update_e_u_n_z(dsp, source[0], source[1], source[2]);
    dsp->registers[DSP_REG_SR] &= BITMASK(16)-(1<<DSP_SR_V);
}

static void emu_mpy_p_y1_y0_a(dsp_core_t* dsp)
{
    uint32_t source[3];

    dsp_mul56(dsp->registers[DSP_REG_Y1], dsp->registers[DSP_REG_Y0], source, SIGN_PLUS);

    dsp->registers[DSP_REG_A2] = source[0];
    dsp->registers[DSP_REG_A1] = source[1];
    dsp->registers[DSP_REG_A0] = source[2];

    emu_ccr_update_e_u_n_z(dsp, source[0], source[1], source[2]);
    dsp->registers[DSP_REG_SR] &= BITMASK(16)-(1<<DSP_SR_V);
}

static void emu_mpy_m_y1_y0_a(dsp_core_t* dsp)
{
    uint32_t source[3];

    dsp_mul56(dsp->registers[DSP_REG_Y1], dsp->registers[DSP_REG_Y0], source, SIGN_MINUS);

    dsp->registers[DSP_REG_A2] = source[0];
    dsp->registers[DSP_REG_A1] = source[1];
    dsp->registers[DSP_REG_A0] = source[2];

    emu_ccr_update_e_u_n_z(dsp, source[0], source[1], source[2]);
    dsp->registers[DSP_REG_SR] &= BITMASK(16)-(1<<DSP_SR_V);
}

static void emu_mpy_p_y1_y0_b(dsp_core_t* dsp)
{
    uint32_t source[3];

    dsp_mul56(dsp->registers[DSP_REG_Y1], dsp->registers[DSP_REG_Y0], source, SIGN_PLUS);

    dsp->registers[DSP_REG_B2] = source[0];
    dsp->registers[DSP_REG_B1] = source[1];
    dsp->registers[DSP_REG_B0] = source[2];

    emu_ccr_update_e_u_n_z(dsp, source[0], source[1], source[2]);
    dsp->registers[DSP_REG_SR] &= BITMASK(16)-(1<<DSP_SR_V);
}

static void emu_mpy_m_y1_y0_b(dsp_core_t* dsp)
{
    uint32_t source[3];

    dsp_mul56(dsp->registers[DSP_REG_Y1], dsp->registers[DSP_REG_Y0], source, SIGN_MINUS);

    dsp->registers[DSP_REG_B2] = source[0];
    dsp->registers[DSP_REG_B1] = source[1];
    dsp->registers[DSP_REG_B0] = source[2];

    emu_ccr_update_e_u_n_z(dsp, source[0], source[1], source[2]);
    dsp->registers[DSP_REG_SR] &= BITMASK(16)-(1<<DSP_SR_V);
}

static void emu_mpy_p_x0_y1_a(dsp_core_t* dsp)
{
    uint32_t source[3];

    dsp_mul56(dsp->registers[DSP_REG_X0], dsp->registers[DSP_REG_Y1], source, SIGN_PLUS);

    dsp->registers[DSP_REG_A2] = source[0];
    dsp->registers[DSP_REG_A1] = source[1];
    dsp->registers[DSP_REG_A0] = source[2];

    emu_ccr_update_e_u_n_z(dsp, source[0], source[1], source[2]);
    dsp->registers[DSP_REG_SR] &= BITMASK(16)-(1<<DSP_SR_V);
}

static void emu_mpy_m_x0_y1_a(dsp_core_t* dsp)
{
    uint32_t source[3];

    dsp_mul56(dsp->registers[DSP_REG_X0], dsp->registers[DSP_REG_Y1], source, SIGN_MINUS);

    dsp->registers[DSP_REG_A2] = source[0];
    dsp->registers[DSP_REG_A1] = source[1];
    dsp->registers[DSP_REG_A0] = source[2];

    emu_ccr_update_e_u_n_z(dsp, source[0], source[1], source[2]);
    dsp->registers[DSP_REG_SR] &= BITMASK(16)-(1<<DSP_SR_V);
}

static void emu_mpy_p_x0_y1_b(dsp_core_t* dsp)
{
    uint32_t source[3];

    dsp_mul56(dsp->registers[DSP_REG_X0], dsp->registers[DSP_REG_Y1], source, SIGN_PLUS);

    dsp->registers[DSP_REG_B2] = source[0];
    dsp->registers[DSP_REG_B1] = source[1];
    dsp->registers[DSP_REG_B0] = source[2];

    emu_ccr_update_e_u_n_z(dsp, source[0], source[1], source[2]);
    dsp->registers[DSP_REG_SR] &= BITMASK(16)-(1<<DSP_SR_V);
}

static void emu_mpy_m_x0_y1_b(dsp_core_t* dsp)
{
    uint32_t source[3];

    dsp_mul56(dsp->registers[DSP_REG_X0], dsp->registers[DSP_REG_Y1], source, SIGN_MINUS);

    dsp->registers[DSP_REG_B2] = source[0];
    dsp->registers[DSP_REG_B1] = source[1];
    dsp->registers[DSP_REG_B0] = source[2];

    emu_ccr_update_e_u_n_z(dsp, source[0], source[1], source[2]);
    dsp->registers[DSP_REG_SR] &= BITMASK(16)-(1<<DSP_SR_V);
}

static void emu_mpy_p_y0_x0_a(dsp_core_t* dsp)
{
    uint32_t source[3];

    dsp_mul56(dsp->registers[DSP_REG_Y0], dsp->registers[DSP_REG_X0], source, SIGN_PLUS);

    dsp->registers[DSP_REG_A2] = source[0];
    dsp->registers[DSP_REG_A1] = source[1];
    dsp->registers[DSP_REG_A0] = source[2];

    emu_ccr_update_e_u_n_z(dsp, source[0], source[1], source[2]);
    dsp->registers[DSP_REG_SR] &= BITMASK(16)-(1<<DSP_SR_V);
}

static void emu_mpy_m_y0_x0_a(dsp_core_t* dsp)
{
    uint32_t source[3];

    dsp_mul56(dsp->registers[DSP_REG_Y0], dsp->registers[DSP_REG_X0], source, SIGN_MINUS);

    dsp->registers[DSP_REG_A2] = source[0];
    dsp->registers[DSP_REG_A1] = source[1];
    dsp->registers[DSP_REG_A0] = source[2];

    emu_ccr_update_e_u_n_z(dsp, source[0], source[1], source[2]);
    dsp->registers[DSP_REG_SR] &= BITMASK(16)-(1<<DSP_SR_V);
}

static void emu_mpy_p_y0_x0_b(dsp_core_t* dsp)
{
    uint32_t source[3];

    dsp_mul56(dsp->registers[DSP_REG_Y0], dsp->registers[DSP_REG_X0], source, SIGN_PLUS);

    dsp->registers[DSP_REG_B2] = source[0];
    dsp->registers[DSP_REG_B1] = source[1];
    dsp->registers[DSP_REG_B0] = source[2];

    emu_ccr_update_e_u_n_z(dsp, source[0], source[1], source[2]);
    dsp->registers[DSP_REG_SR] &= BITMASK(16)-(1<<DSP_SR_V);
}

static void emu_mpy_m_y0_x0_b(dsp_core_t* dsp)
{
    uint32_t source[3];

    dsp_mul56(dsp->registers[DSP_REG_Y0], dsp->registers[DSP_REG_X0], source, SIGN_MINUS);

    dsp->registers[DSP_REG_B2] = source[0];
    dsp->registers[DSP_REG_B1] = source[1];
    dsp->registers[DSP_REG_B0] = source[2];

    emu_ccr_update_e_u_n_z(dsp, source[0], source[1], source[2]);
    dsp->registers[DSP_REG_SR] &= BITMASK(16)-(1<<DSP_SR_V);
}

static void emu_mpy_p_x1_y0_a(dsp_core_t* dsp)
{
    uint32_t source[3];

    dsp_mul56(dsp->registers[DSP_REG_X1], dsp->registers[DSP_REG_Y0], source, SIGN_PLUS);

    dsp->registers[DSP_REG_A2] = source[0];
    dsp->registers[DSP_REG_A1] = source[1];
    dsp->registers[DSP_REG_A0] = source[2];

    emu_ccr_update_e_u_n_z(dsp, source[0], source[1], source[2]);
    dsp->registers[DSP_REG_SR] &= BITMASK(16)-(1<<DSP_SR_V);
}

static void emu_mpy_m_x1_y0_a(dsp_core_t* dsp)
{
    uint32_t source[3];

    dsp_mul56(dsp->registers[DSP_REG_X1], dsp->registers[DSP_REG_Y0], source, SIGN_MINUS);

    dsp->registers[DSP_REG_A2] = source[0];
    dsp->registers[DSP_REG_A1] = source[1];
    dsp->registers[DSP_REG_A0] = source[2];

    emu_ccr_update_e_u_n_z(dsp, source[0], source[1], source[2]);
    dsp->registers[DSP_REG_SR] &= BITMASK(16)-(1<<DSP_SR_V);
}

static void emu_mpy_p_x1_y0_b(dsp_core_t* dsp)
{
    uint32_t source[3];

    dsp_mul56(dsp->registers[DSP_REG_X1], dsp->registers[DSP_REG_Y0], source, SIGN_PLUS);

    dsp->registers[DSP_REG_B2] = source[0];
    dsp->registers[DSP_REG_B1] = source[1];
    dsp->registers[DSP_REG_B0] = source[2];

    emu_ccr_update_e_u_n_z(dsp, source[0], source[1], source[2]);
    dsp->registers[DSP_REG_SR] &= BITMASK(16)-(1<<DSP_SR_V);
}

static void emu_mpy_m_x1_y0_b(dsp_core_t* dsp)
{
    uint32_t source[3];

    dsp_mul56(dsp->registers[DSP_REG_X1], dsp->registers[DSP_REG_Y0], source, SIGN_MINUS);

    dsp->registers[DSP_REG_B2] = source[0];
    dsp->registers[DSP_REG_B1] = source[1];
    dsp->registers[DSP_REG_B0] = source[2];

    emu_ccr_update_e_u_n_z(dsp, source[0], source[1], source[2]);
    dsp->registers[DSP_REG_SR] &= BITMASK(16)-(1<<DSP_SR_V);
}

static void emu_mpy_p_y1_x1_a(dsp_core_t* dsp)
{
    uint32_t source[3];

    dsp_mul56(dsp->registers[DSP_REG_Y1], dsp->registers[DSP_REG_X1], source, SIGN_PLUS);

    dsp->registers[DSP_REG_A2] = source[0];
    dsp->registers[DSP_REG_A1] = source[1];
    dsp->registers[DSP_REG_A0] = source[2];

    emu_ccr_update_e_u_n_z(dsp, source[0], source[1], source[2]);
    dsp->registers[DSP_REG_SR] &= BITMASK(16)-(1<<DSP_SR_V);
}

static void emu_mpy_m_y1_x1_a(dsp_core_t* dsp)
{
    uint32_t source[3];

    dsp_mul56(dsp->registers[DSP_REG_Y1], dsp->registers[DSP_REG_X1], source, SIGN_MINUS);

    dsp->registers[DSP_REG_A2] = source[0];
    dsp->registers[DSP_REG_A1] = source[1];
    dsp->registers[DSP_REG_A0] = source[2];

    emu_ccr_update_e_u_n_z(dsp, source[0], source[1], source[2]);
    dsp->registers[DSP_REG_SR] &= BITMASK(16)-(1<<DSP_SR_V);
}

static void emu_mpy_p_y1_x1_b(dsp_core_t* dsp)
{
    uint32_t source[3];

    dsp_mul56(dsp->registers[DSP_REG_Y1], dsp->registers[DSP_REG_X1], source, SIGN_PLUS);

    dsp->registers[DSP_REG_B2] = source[0];
    dsp->registers[DSP_REG_B1] = source[1];
    dsp->registers[DSP_REG_B0] = source[2];

    emu_ccr_update_e_u_n_z(dsp, source[0], source[1], source[2]);
    dsp->registers[DSP_REG_SR] &= BITMASK(16)-(1<<DSP_SR_V);
}

static void emu_mpy_m_y1_x1_b(dsp_core_t* dsp)
{
    uint32_t source[3];

    dsp_mul56(dsp->registers[DSP_REG_Y1], dsp->registers[DSP_REG_X1], source, SIGN_MINUS);

    dsp->registers[DSP_REG_B2] = source[0];
    dsp->registers[DSP_REG_B1] = source[1];
    dsp->registers[DSP_REG_B0] = source[2];

    emu_ccr_update_e_u_n_z(dsp, source[0], source[1], source[2]);
    dsp->registers[DSP_REG_SR] &= BITMASK(16)-(1<<DSP_SR_V);
}

static void emu_mpyr_p_x0_x0_a(dsp_core_t* dsp)
{
    uint32_t source[3];

    dsp_mul56(dsp->registers[DSP_REG_X0], dsp->registers[DSP_REG_X0], source, SIGN_PLUS);
    dsp_rnd56(dsp, source);

    dsp->registers[DSP_REG_A2] = source[0];
    dsp->registers[DSP_REG_A1] = source[1];
    dsp->registers[DSP_REG_A0] = source[2];

    emu_ccr_update_e_u_n_z(dsp, source[0], source[1], source[2]);
    dsp->registers[DSP_REG_SR] &= BITMASK(16)-(1<<DSP_SR_V);
}

static void emu_mpyr_m_x0_x0_a(dsp_core_t* dsp)
{
    uint32_t source[3];

    dsp_mul56(dsp->registers[DSP_REG_X0], dsp->registers[DSP_REG_X0], source, SIGN_MINUS);
    dsp_rnd56(dsp, source);

    dsp->registers[DSP_REG_A2] = source[0];
    dsp->registers[DSP_REG_A1] = source[1];
    dsp->registers[DSP_REG_A0] = source[2];

    emu_ccr_update_e_u_n_z(dsp, source[0], source[1], source[2]);
    dsp->registers[DSP_REG_SR] &= BITMASK(16)-(1<<DSP_SR_V);
}

static void emu_mpyr_p_x0_x0_b(dsp_core_t* dsp)
{
    uint32_t source[3];

    dsp_mul56(dsp->registers[DSP_REG_X0], dsp->registers[DSP_REG_X0], source, SIGN_PLUS);
    dsp_rnd56(dsp, source);

    dsp->registers[DSP_REG_B2] = source[0];
    dsp->registers[DSP_REG_B1] = source[1];
    dsp->registers[DSP_REG_B0] = source[2];

    emu_ccr_update_e_u_n_z(dsp, source[0], source[1], source[2]);
    dsp->registers[DSP_REG_SR] &= BITMASK(16)-(1<<DSP_SR_V);
}

static void emu_mpyr_m_x0_x0_b(dsp_core_t* dsp)
{
    uint32_t source[3];


    dsp_mul56(dsp->registers[DSP_REG_X0], dsp->registers[DSP_REG_X0], source, SIGN_MINUS);
    dsp_rnd56(dsp, source);

    dsp->registers[DSP_REG_B2] = source[0];
    dsp->registers[DSP_REG_B1] = source[1];
    dsp->registers[DSP_REG_B0] = source[2];

    emu_ccr_update_e_u_n_z(dsp, source[0], source[1], source[2]);
    dsp->registers[DSP_REG_SR] &= BITMASK(16)-(1<<DSP_SR_V);
}

static void emu_mpyr_p_y0_y0_a(dsp_core_t* dsp)
{
    uint32_t source[3];

    dsp_mul56(dsp->registers[DSP_REG_Y0], dsp->registers[DSP_REG_Y0], source, SIGN_PLUS);
    dsp_rnd56(dsp, source);

    dsp->registers[DSP_REG_A2] = source[0];
    dsp->registers[DSP_REG_A1] = source[1];
    dsp->registers[DSP_REG_A0] = source[2];

    emu_ccr_update_e_u_n_z(dsp, source[0], source[1], source[2]);
    dsp->registers[DSP_REG_SR] &= BITMASK(16)-(1<<DSP_SR_V);
}

static void emu_mpyr_m_y0_y0_a(dsp_core_t* dsp)
{
    uint32_t source[3];

    dsp_mul56(dsp->registers[DSP_REG_Y0], dsp->registers[DSP_REG_Y0], source, SIGN_MINUS);
    dsp_rnd56(dsp, source);

    dsp->registers[DSP_REG_A2] = source[0];
    dsp->registers[DSP_REG_A1] = source[1];
    dsp->registers[DSP_REG_A0] = source[2];

    emu_ccr_update_e_u_n_z(dsp, source[0], source[1], source[2]);
    dsp->registers[DSP_REG_SR] &= BITMASK(16)-(1<<DSP_SR_V);
}

static void emu_mpyr_p_y0_y0_b(dsp_core_t* dsp)
{
    uint32_t source[3];

    dsp_mul56(dsp->registers[DSP_REG_Y0], dsp->registers[DSP_REG_Y0], source, SIGN_PLUS);
    dsp_rnd56(dsp, source);

    dsp->registers[DSP_REG_B2] = source[0];
    dsp->registers[DSP_REG_B1] = source[1];
    dsp->registers[DSP_REG_B0] = source[2];

    emu_ccr_update_e_u_n_z(dsp, source[0], source[1], source[2]);
    dsp->registers[DSP_REG_SR] &= BITMASK(16)-(1<<DSP_SR_V);
}

static void emu_mpyr_m_y0_y0_b(dsp_core_t* dsp)
{
    uint32_t source[3];

    dsp_mul56(dsp->registers[DSP_REG_Y0], dsp->registers[DSP_REG_Y0], source, SIGN_MINUS);
    dsp_rnd56(dsp, source);

    dsp->registers[DSP_REG_B2] = source[0];
    dsp->registers[DSP_REG_B1] = source[1];
    dsp->registers[DSP_REG_B0] = source[2];

    emu_ccr_update_e_u_n_z(dsp, source[0], source[1], source[2]);
    dsp->registers[DSP_REG_SR] &= BITMASK(16)-(1<<DSP_SR_V);
}

static void emu_mpyr_p_x1_x0_a(dsp_core_t* dsp)
{
    uint32_t source[3];

    dsp_mul56(dsp->registers[DSP_REG_X1], dsp->registers[DSP_REG_X0], source, SIGN_PLUS);
    dsp_rnd56(dsp, source);

    dsp->registers[DSP_REG_A2] = source[0];
    dsp->registers[DSP_REG_A1] = source[1];
    dsp->registers[DSP_REG_A0] = source[2];

    emu_ccr_update_e_u_n_z(dsp, source[0], source[1], source[2]);
    dsp->registers[DSP_REG_SR] &= BITMASK(16)-(1<<DSP_SR_V);
}

static void emu_mpyr_m_x1_x0_a(dsp_core_t* dsp)
{
    uint32_t source[3];

    dsp_mul56(dsp->registers[DSP_REG_X1], dsp->registers[DSP_REG_X0], source, SIGN_MINUS);
    dsp_rnd56(dsp, source);

    dsp->registers[DSP_REG_A2] = source[0];
    dsp->registers[DSP_REG_A1] = source[1];
    dsp->registers[DSP_REG_A0] = source[2];

    emu_ccr_update_e_u_n_z(dsp, source[0], source[1], source[2]);
    dsp->registers[DSP_REG_SR] &= BITMASK(16)-(1<<DSP_SR_V);
}

static void emu_mpyr_p_x1_x0_b(dsp_core_t* dsp)
{
    uint32_t source[3];

    dsp_mul56(dsp->registers[DSP_REG_X1], dsp->registers[DSP_REG_X0], source, SIGN_PLUS);
    dsp_rnd56(dsp, source);

    dsp->registers[DSP_REG_B2] = source[0];
    dsp->registers[DSP_REG_B1] = source[1];
    dsp->registers[DSP_REG_B0] = source[2];

    emu_ccr_update_e_u_n_z(dsp, source[0], source[1], source[2]);
    dsp->registers[DSP_REG_SR] &= BITMASK(16)-(1<<DSP_SR_V);
}

static void emu_mpyr_m_x1_x0_b(dsp_core_t* dsp)
{
    uint32_t source[3];

    dsp_mul56(dsp->registers[DSP_REG_X1], dsp->registers[DSP_REG_X0], source, SIGN_MINUS);
    dsp_rnd56(dsp, source);

    dsp->registers[DSP_REG_B2] = source[0];
    dsp->registers[DSP_REG_B1] = source[1];
    dsp->registers[DSP_REG_B0] = source[2];

    emu_ccr_update_e_u_n_z(dsp, source[0], source[1], source[2]);
    dsp->registers[DSP_REG_SR] &= BITMASK(16)-(1<<DSP_SR_V);
}

static void emu_mpyr_p_y1_y0_a(dsp_core_t* dsp)
{
    uint32_t source[3];

    dsp_mul56(dsp->registers[DSP_REG_Y1], dsp->registers[DSP_REG_Y0], source, SIGN_PLUS);
    dsp_rnd56(dsp, source);

    dsp->registers[DSP_REG_A2] = source[0];
    dsp->registers[DSP_REG_A1] = source[1];
    dsp->registers[DSP_REG_A0] = source[2];

    emu_ccr_update_e_u_n_z(dsp, source[0], source[1], source[2]);
    dsp->registers[DSP_REG_SR] &= BITMASK(16)-(1<<DSP_SR_V);
}

static void emu_mpyr_m_y1_y0_a(dsp_core_t* dsp)
{
    uint32_t source[3];

    dsp_mul56(dsp->registers[DSP_REG_Y1], dsp->registers[DSP_REG_Y0], source, SIGN_MINUS);
    dsp_rnd56(dsp, source);

    dsp->registers[DSP_REG_A2] = source[0];
    dsp->registers[DSP_REG_A1] = source[1];
    dsp->registers[DSP_REG_A0] = source[2];

    emu_ccr_update_e_u_n_z(dsp, source[0], source[1], source[2]);
    dsp->registers[DSP_REG_SR] &= BITMASK(16)-(1<<DSP_SR_V);
}

static void emu_mpyr_p_y1_y0_b(dsp_core_t* dsp)
{
    uint32_t source[3];

    dsp_mul56(dsp->registers[DSP_REG_Y1], dsp->registers[DSP_REG_Y0], source, SIGN_PLUS);
    dsp_rnd56(dsp, source);

    dsp->registers[DSP_REG_B2] = source[0];
    dsp->registers[DSP_REG_B1] = source[1];
    dsp->registers[DSP_REG_B0] = source[2];

    emu_ccr_update_e_u_n_z(dsp, source[0], source[1], source[2]);
    dsp->registers[DSP_REG_SR] &= BITMASK(16)-(1<<DSP_SR_V);
}

static void emu_mpyr_m_y1_y0_b(dsp_core_t* dsp)
{
    uint32_t source[3];

    dsp_mul56(dsp->registers[DSP_REG_Y1], dsp->registers[DSP_REG_Y0], source, SIGN_MINUS);
    dsp_rnd56(dsp, source);

    dsp->registers[DSP_REG_B2] = source[0];
    dsp->registers[DSP_REG_B1] = source[1];
    dsp->registers[DSP_REG_B0] = source[2];

    emu_ccr_update_e_u_n_z(dsp, source[0], source[1], source[2]);
    dsp->registers[DSP_REG_SR] &= BITMASK(16)-(1<<DSP_SR_V);
}

static void emu_mpyr_p_x0_y1_a(dsp_core_t* dsp)
{
    uint32_t source[3];

    dsp_mul56(dsp->registers[DSP_REG_X0], dsp->registers[DSP_REG_Y1], source, SIGN_PLUS);
    dsp_rnd56(dsp, source);

    dsp->registers[DSP_REG_A2] = source[0];
    dsp->registers[DSP_REG_A1] = source[1];
    dsp->registers[DSP_REG_A0] = source[2];

    emu_ccr_update_e_u_n_z(dsp, source[0], source[1], source[2]);
    dsp->registers[DSP_REG_SR] &= BITMASK(16)-(1<<DSP_SR_V);
}

static void emu_mpyr_m_x0_y1_a(dsp_core_t* dsp)
{
    uint32_t source[3];

    dsp_mul56(dsp->registers[DSP_REG_X0], dsp->registers[DSP_REG_Y1], source, SIGN_MINUS);
    dsp_rnd56(dsp, source);

    dsp->registers[DSP_REG_A2] = source[0];
    dsp->registers[DSP_REG_A1] = source[1];
    dsp->registers[DSP_REG_A0] = source[2];

    emu_ccr_update_e_u_n_z(dsp, source[0], source[1], source[2]);
    dsp->registers[DSP_REG_SR] &= BITMASK(16)-(1<<DSP_SR_V);
}

static void emu_mpyr_p_x0_y1_b(dsp_core_t* dsp)
{
    uint32_t source[3];

    dsp_mul56(dsp->registers[DSP_REG_X0], dsp->registers[DSP_REG_Y1], source, SIGN_PLUS);
    dsp_rnd56(dsp, source);

    dsp->registers[DSP_REG_B2] = source[0];
    dsp->registers[DSP_REG_B1] = source[1];
    dsp->registers[DSP_REG_B0] = source[2];

    emu_ccr_update_e_u_n_z(dsp, source[0], source[1], source[2]);
    dsp->registers[DSP_REG_SR] &= BITMASK(16)-(1<<DSP_SR_V);
}

static void emu_mpyr_m_x0_y1_b(dsp_core_t* dsp)
{
    uint32_t source[3];

    dsp_mul56(dsp->registers[DSP_REG_X0], dsp->registers[DSP_REG_Y1], source, SIGN_MINUS);
    dsp_rnd56(dsp, source);

    dsp->registers[DSP_REG_B2] = source[0];
    dsp->registers[DSP_REG_B1] = source[1];
    dsp->registers[DSP_REG_B0] = source[2];

    emu_ccr_update_e_u_n_z(dsp, source[0], source[1], source[2]);
    dsp->registers[DSP_REG_SR] &= BITMASK(16)-(1<<DSP_SR_V);
}

static void emu_mpyr_p_y0_x0_a(dsp_core_t* dsp)
{
    uint32_t source[3];

    dsp_mul56(dsp->registers[DSP_REG_Y0], dsp->registers[DSP_REG_X0], source, SIGN_PLUS);
    dsp_rnd56(dsp, source);

    dsp->registers[DSP_REG_A2] = source[0];
    dsp->registers[DSP_REG_A1] = source[1];
    dsp->registers[DSP_REG_A0] = source[2];

    emu_ccr_update_e_u_n_z(dsp, source[0], source[1], source[2]);
    dsp->registers[DSP_REG_SR] &= BITMASK(16)-(1<<DSP_SR_V);
}

static void emu_mpyr_m_y0_x0_a(dsp_core_t* dsp)
{
    uint32_t source[3];

    dsp_mul56(dsp->registers[DSP_REG_Y0], dsp->registers[DSP_REG_X0], source, SIGN_MINUS);
    dsp_rnd56(dsp, source);

    dsp->registers[DSP_REG_A2] = source[0];
    dsp->registers[DSP_REG_A1] = source[1];
    dsp->registers[DSP_REG_A0] = source[2];

    emu_ccr_update_e_u_n_z(dsp, source[0], source[1], source[2]);
    dsp->registers[DSP_REG_SR] &= BITMASK(16)-(1<<DSP_SR_V);
}

static void emu_mpyr_p_y0_x0_b(dsp_core_t* dsp)
{
    uint32_t source[3];

    dsp_mul56(dsp->registers[DSP_REG_Y0], dsp->registers[DSP_REG_X0], source, SIGN_PLUS);
    dsp_rnd56(dsp, source);

    dsp->registers[DSP_REG_B2] = source[0];
    dsp->registers[DSP_REG_B1] = source[1];
    dsp->registers[DSP_REG_B0] = source[2];

    emu_ccr_update_e_u_n_z(dsp, source[0], source[1], source[2]);
    dsp->registers[DSP_REG_SR] &= BITMASK(16)-(1<<DSP_SR_V);
}

static void emu_mpyr_m_y0_x0_b(dsp_core_t* dsp)
{
    uint32_t source[3];

    dsp_mul56(dsp->registers[DSP_REG_Y0], dsp->registers[DSP_REG_X0], source, SIGN_MINUS);
    dsp_rnd56(dsp, source);

    dsp->registers[DSP_REG_B2] = source[0];
    dsp->registers[DSP_REG_B1] = source[1];
    dsp->registers[DSP_REG_B0] = source[2];

    emu_ccr_update_e_u_n_z(dsp, source[0], source[1], source[2]);
    dsp->registers[DSP_REG_SR] &= BITMASK(16)-(1<<DSP_SR_V);
}

static void emu_mpyr_p_x1_y0_a(dsp_core_t* dsp)
{
    uint32_t source[3];

    dsp_mul56(dsp->registers[DSP_REG_X1], dsp->registers[DSP_REG_Y0], source, SIGN_PLUS);
    dsp_rnd56(dsp, source);

    dsp->registers[DSP_REG_A2] = source[0];
    dsp->registers[DSP_REG_A1] = source[1];
    dsp->registers[DSP_REG_A0] = source[2];

    emu_ccr_update_e_u_n_z(dsp, source[0], source[1], source[2]);
    dsp->registers[DSP_REG_SR] &= BITMASK(16)-(1<<DSP_SR_V);
}

static void emu_mpyr_m_x1_y0_a(dsp_core_t* dsp)
{
    uint32_t source[3];

    dsp_mul56(dsp->registers[DSP_REG_X1], dsp->registers[DSP_REG_Y0], source, SIGN_MINUS);
    dsp_rnd56(dsp, source);

    dsp->registers[DSP_REG_A2] = source[0];
    dsp->registers[DSP_REG_A1] = source[1];
    dsp->registers[DSP_REG_A0] = source[2];

    emu_ccr_update_e_u_n_z(dsp, source[0], source[1], source[2]);
    dsp->registers[DSP_REG_SR] &= BITMASK(16)-(1<<DSP_SR_V);
}

static void emu_mpyr_p_x1_y0_b(dsp_core_t* dsp)
{
    uint32_t source[3];

    dsp_mul56(dsp->registers[DSP_REG_X1], dsp->registers[DSP_REG_Y0], source, SIGN_PLUS);
    dsp_rnd56(dsp, source);

    dsp->registers[DSP_REG_B2] = source[0];
    dsp->registers[DSP_REG_B1] = source[1];
    dsp->registers[DSP_REG_B0] = source[2];

    emu_ccr_update_e_u_n_z(dsp, source[0], source[1], source[2]);
    dsp->registers[DSP_REG_SR] &= BITMASK(16)-(1<<DSP_SR_V);
}

static void emu_mpyr_m_x1_y0_b(dsp_core_t* dsp)
{
    uint32_t source[3];

    dsp_mul56(dsp->registers[DSP_REG_X1], dsp->registers[DSP_REG_Y0], source, SIGN_MINUS);
    dsp_rnd56(dsp, source);

    dsp->registers[DSP_REG_B2] = source[0];
    dsp->registers[DSP_REG_B1] = source[1];
    dsp->registers[DSP_REG_B0] = source[2];

    emu_ccr_update_e_u_n_z(dsp, source[0], source[1], source[2]);
    dsp->registers[DSP_REG_SR] &= BITMASK(16)-(1<<DSP_SR_V);
}

static void emu_mpyr_p_y1_x1_a(dsp_core_t* dsp)
{
    uint32_t source[3];

    dsp_mul56(dsp->registers[DSP_REG_Y1], dsp->registers[DSP_REG_X1], source, SIGN_PLUS);
    dsp_rnd56(dsp, source);

    dsp->registers[DSP_REG_A2] = source[0];
    dsp->registers[DSP_REG_A1] = source[1];
    dsp->registers[DSP_REG_A0] = source[2];

    emu_ccr_update_e_u_n_z(dsp, source[0], source[1], source[2]);
    dsp->registers[DSP_REG_SR] &= BITMASK(16)-(1<<DSP_SR_V);
}

static void emu_mpyr_m_y1_x1_a(dsp_core_t* dsp)
{
    uint32_t source[3];

    dsp_mul56(dsp->registers[DSP_REG_Y1], dsp->registers[DSP_REG_X1], source, SIGN_MINUS);
    dsp_rnd56(dsp, source);

    dsp->registers[DSP_REG_A2] = source[0];
    dsp->registers[DSP_REG_A1] = source[1];
    dsp->registers[DSP_REG_A0] = source[2];

    emu_ccr_update_e_u_n_z(dsp, source[0], source[1], source[2]);
    dsp->registers[DSP_REG_SR] &= BITMASK(16)-(1<<DSP_SR_V);
}

static void emu_mpyr_p_y1_x1_b(dsp_core_t* dsp)
{
    uint32_t source[3];

    dsp_mul56(dsp->registers[DSP_REG_Y1], dsp->registers[DSP_REG_X1], source, SIGN_PLUS);
    dsp_rnd56(dsp, source);

    dsp->registers[DSP_REG_B2] = source[0];
    dsp->registers[DSP_REG_B1] = source[1];
    dsp->registers[DSP_REG_B0] = source[2];

    emu_ccr_update_e_u_n_z(dsp, source[0], source[1], source[2]);
    dsp->registers[DSP_REG_SR] &= BITMASK(16)-(1<<DSP_SR_V);
}

static void emu_mpyr_m_y1_x1_b(dsp_core_t* dsp)
{
    uint32_t source[3];

    dsp_mul56(dsp->registers[DSP_REG_Y1], dsp->registers[DSP_REG_X1], source, SIGN_MINUS);
    dsp_rnd56(dsp, source);

    dsp->registers[DSP_REG_B2] = source[0];
    dsp->registers[DSP_REG_B1] = source[1];
    dsp->registers[DSP_REG_B0] = source[2];

    emu_ccr_update_e_u_n_z(dsp, source[0], source[1], source[2]);
    dsp->registers[DSP_REG_SR] &= BITMASK(16)-(1<<DSP_SR_V);
}

static void emu_neg_a(dsp_core_t* dsp)
{
    uint32_t source[3], dest[3], overflowed;

    source[0] = dsp->registers[DSP_REG_A2];
    source[1] = dsp->registers[DSP_REG_A1];
    source[2] = dsp->registers[DSP_REG_A0];

    overflowed = ((source[2]==0) && (source[1]==0) && (source[0]==0x80));

    dest[0] = dest[1] = dest[2] = 0;

    dsp_sub56(source, dest);

    dsp->registers[DSP_REG_A2] = dest[0];
    dsp->registers[DSP_REG_A1] = dest[1];
    dsp->registers[DSP_REG_A0] = dest[2];

    dsp->registers[DSP_REG_SR] &= BITMASK(16)-(1<<DSP_SR_V);
    dsp->registers[DSP_REG_SR] |= (overflowed<<DSP_SR_L)|(overflowed<<DSP_SR_V);

    emu_ccr_update_e_u_n_z(dsp, dest[0], dest[1], dest[2]);
}

static void emu_neg_b(dsp_core_t* dsp)
{
    uint32_t source[3], dest[3], overflowed;

    source[0] = dsp->registers[DSP_REG_B2];
    source[1] = dsp->registers[DSP_REG_B1];
    source[2] = dsp->registers[DSP_REG_B0];

    overflowed = ((source[2]==0) && (source[1]==0) && (source[0]==0x80));

    dest[0] = dest[1] = dest[2] = 0;

    dsp_sub56(source, dest);

    dsp->registers[DSP_REG_B2] = dest[0];
    dsp->registers[DSP_REG_B1] = dest[1];
    dsp->registers[DSP_REG_B0] = dest[2];

    dsp->registers[DSP_REG_SR] &= BITMASK(16)-(1<<DSP_SR_V);
    dsp->registers[DSP_REG_SR] |= (overflowed<<DSP_SR_L)|(overflowed<<DSP_SR_V);

    emu_ccr_update_e_u_n_z(dsp, dest[0], dest[1], dest[2]);
}

static void emu_nop(dsp_core_t* dsp)
{
}

static void emu_not_a(dsp_core_t* dsp)
{
    dsp->registers[DSP_REG_A1] = ~dsp->registers[DSP_REG_A1];
    dsp->registers[DSP_REG_A1] &= BITMASK(24); /* FIXME: useless ? */

    dsp->registers[DSP_REG_SR] &= BITMASK(16)-((1<<DSP_SR_N)|(1<<DSP_SR_Z)|(1<<DSP_SR_V));
    dsp->registers[DSP_REG_SR] |= ((dsp->registers[DSP_REG_A1]>>23) & 1)<<DSP_SR_N;
    dsp->registers[DSP_REG_SR] |= (dsp->registers[DSP_REG_A1]==0)<<DSP_SR_Z;
}

static void emu_not_b(dsp_core_t* dsp)
{
    dsp->registers[DSP_REG_B1] = ~dsp->registers[DSP_REG_B1];
    dsp->registers[DSP_REG_B1] &= BITMASK(24); /* FIXME: useless ? */

    dsp->registers[DSP_REG_SR] &= BITMASK(16)-((1<<DSP_SR_N)|(1<<DSP_SR_Z)|(1<<DSP_SR_V));
    dsp->registers[DSP_REG_SR] |= ((dsp->registers[DSP_REG_B1]>>23) & 1)<<DSP_SR_N;
    dsp->registers[DSP_REG_SR] |= (dsp->registers[DSP_REG_B1]==0)<<DSP_SR_Z;
}

static void emu_or_x0_a(dsp_core_t* dsp)
{
    dsp->registers[DSP_REG_A1] |= dsp->registers[DSP_REG_X0];
    dsp->registers[DSP_REG_A1] &= BITMASK(24); /* FIXME: useless ? */

    dsp->registers[DSP_REG_SR] &= BITMASK(16)-((1<<DSP_SR_N)|(1<<DSP_SR_Z)|(1<<DSP_SR_V));
    dsp->registers[DSP_REG_SR] |= ((dsp->registers[DSP_REG_A1]>>23) & 1)<<DSP_SR_N;
    dsp->registers[DSP_REG_SR] |= (dsp->registers[DSP_REG_A1]==0)<<DSP_SR_Z;
}

static void emu_or_x0_b(dsp_core_t* dsp)
{
    dsp->registers[DSP_REG_B1] |= dsp->registers[DSP_REG_X0];
    dsp->registers[DSP_REG_B1] &= BITMASK(24); /* FIXME: useless ? */

    dsp->registers[DSP_REG_SR] &= BITMASK(16)-((1<<DSP_SR_N)|(1<<DSP_SR_Z)|(1<<DSP_SR_V));
    dsp->registers[DSP_REG_SR] |= ((dsp->registers[DSP_REG_B1]>>23) & 1)<<DSP_SR_N;
    dsp->registers[DSP_REG_SR] |= (dsp->registers[DSP_REG_B1]==0)<<DSP_SR_Z;
}

static void emu_or_y0_a(dsp_core_t* dsp)
{
    dsp->registers[DSP_REG_A1] |= dsp->registers[DSP_REG_Y0];
    dsp->registers[DSP_REG_A1] &= BITMASK(24); /* FIXME: useless ? */

    dsp->registers[DSP_REG_SR] &= BITMASK(16)-((1<<DSP_SR_N)|(1<<DSP_SR_Z)|(1<<DSP_SR_V));
    dsp->registers[DSP_REG_SR] |= ((dsp->registers[DSP_REG_A1]>>23) & 1)<<DSP_SR_N;
    dsp->registers[DSP_REG_SR] |= (dsp->registers[DSP_REG_A1]==0)<<DSP_SR_Z;
}

static void emu_or_y0_b(dsp_core_t* dsp)
{
    dsp->registers[DSP_REG_B1] |= dsp->registers[DSP_REG_Y0];
    dsp->registers[DSP_REG_B1] &= BITMASK(24); /* FIXME: useless ? */

    dsp->registers[DSP_REG_SR] &= BITMASK(16)-((1<<DSP_SR_N)|(1<<DSP_SR_Z)|(1<<DSP_SR_V));
    dsp->registers[DSP_REG_SR] |= ((dsp->registers[DSP_REG_B1]>>23) & 1)<<DSP_SR_N;
    dsp->registers[DSP_REG_SR] |= (dsp->registers[DSP_REG_B1]==0)<<DSP_SR_Z;
}

static void emu_or_x1_a(dsp_core_t* dsp)
{
    dsp->registers[DSP_REG_A1] |= dsp->registers[DSP_REG_X1];
    dsp->registers[DSP_REG_A1] &= BITMASK(24); /* FIXME: useless ? */

    dsp->registers[DSP_REG_SR] &= BITMASK(16)-((1<<DSP_SR_N)|(1<<DSP_SR_Z)|(1<<DSP_SR_V));
    dsp->registers[DSP_REG_SR] |= ((dsp->registers[DSP_REG_A1]>>23) & 1)<<DSP_SR_N;
    dsp->registers[DSP_REG_SR] |= (dsp->registers[DSP_REG_A1]==0)<<DSP_SR_Z;
}

static void emu_or_x1_b(dsp_core_t* dsp)
{
    dsp->registers[DSP_REG_B1] |= dsp->registers[DSP_REG_X1];
    dsp->registers[DSP_REG_B1] &= BITMASK(24); /* FIXME: useless ? */

    dsp->registers[DSP_REG_SR] &= BITMASK(16)-((1<<DSP_SR_N)|(1<<DSP_SR_Z)|(1<<DSP_SR_V));
    dsp->registers[DSP_REG_SR] |= ((dsp->registers[DSP_REG_B1]>>23) & 1)<<DSP_SR_N;
    dsp->registers[DSP_REG_SR] |= (dsp->registers[DSP_REG_B1]==0)<<DSP_SR_Z;
}

static void emu_or_y1_a(dsp_core_t* dsp)
{
    dsp->registers[DSP_REG_A1] |= dsp->registers[DSP_REG_Y1];
    dsp->registers[DSP_REG_A1] &= BITMASK(24); /* FIXME: useless ? */

    dsp->registers[DSP_REG_SR] &= BITMASK(16)-((1<<DSP_SR_N)|(1<<DSP_SR_Z)|(1<<DSP_SR_V));
    dsp->registers[DSP_REG_SR] |= ((dsp->registers[DSP_REG_A1]>>23) & 1)<<DSP_SR_N;
    dsp->registers[DSP_REG_SR] |= (dsp->registers[DSP_REG_A1]==0)<<DSP_SR_Z;
}

static void emu_or_y1_b(dsp_core_t* dsp)
{
    dsp->registers[DSP_REG_B1] |= dsp->registers[DSP_REG_Y1];
    dsp->registers[DSP_REG_B1] &= BITMASK(24); /* FIXME: useless ? */

    dsp->registers[DSP_REG_SR] &= BITMASK(16)-((1<<DSP_SR_N)|(1<<DSP_SR_Z)|(1<<DSP_SR_V));
    dsp->registers[DSP_REG_SR] |= ((dsp->registers[DSP_REG_B1]>>23) & 1)<<DSP_SR_N;
    dsp->registers[DSP_REG_SR] |= (dsp->registers[DSP_REG_B1]==0)<<DSP_SR_Z;
}

static void emu_rnd_a(dsp_core_t* dsp)
{
    uint32_t dest[3];

    dest[0] = dsp->registers[DSP_REG_A2];
    dest[1] = dsp->registers[DSP_REG_A1];
    dest[2] = dsp->registers[DSP_REG_A0];

    /* xbased: V (and L) of the rounding (A-6.100), it kept the old V */
    uint16_t newsr = dsp_rnd56(dsp, dest);

    dsp->registers[DSP_REG_A2] = dest[0];
    dsp->registers[DSP_REG_A1] = dest[1];
    dsp->registers[DSP_REG_A0] = dest[2];

    emu_ccr_update_e_u_n_z(dsp, dest[0], dest[1], dest[2]);
    dsp->registers[DSP_REG_SR] &= BITMASK(16)-(1<<DSP_SR_V);
    dsp->registers[DSP_REG_SR] |= newsr & ((1<<DSP_SR_V)|(1<<DSP_SR_L));
}

static void emu_rnd_b(dsp_core_t* dsp)
{
    uint32_t dest[3];

    dest[0] = dsp->registers[DSP_REG_B2];
    dest[1] = dsp->registers[DSP_REG_B1];
    dest[2] = dsp->registers[DSP_REG_B0];

    /* xbased: V (and L) of the rounding (A-6.100), it kept the old V */
    uint16_t newsr = dsp_rnd56(dsp, dest);

    dsp->registers[DSP_REG_B2] = dest[0];
    dsp->registers[DSP_REG_B1] = dest[1];
    dsp->registers[DSP_REG_B0] = dest[2];

    emu_ccr_update_e_u_n_z(dsp, dest[0], dest[1], dest[2]);
    dsp->registers[DSP_REG_SR] &= BITMASK(16)-(1<<DSP_SR_V);
    dsp->registers[DSP_REG_SR] |= newsr & ((1<<DSP_SR_V)|(1<<DSP_SR_L));
}

static void emu_rol_a(dsp_core_t* dsp)
{
    uint32_t newcarry;

    /* xbased: through C (A-6.101): bit 47 to C, the old C into bit 24 (it
     * rotated bit 47 into bit 24) */
    newcarry = (dsp->registers[DSP_REG_A1]>>23) & 1;

    dsp->registers[DSP_REG_A1] <<= 1;
    dsp->registers[DSP_REG_A1] |= (dsp->registers[DSP_REG_SR]>>DSP_SR_C) & 1;
    dsp->registers[DSP_REG_A1] &= BITMASK(24);

    dsp->registers[DSP_REG_SR] &= BITMASK(16)-((1<<DSP_SR_C)|(1<<DSP_SR_N)|(1<<DSP_SR_Z)|(1<<DSP_SR_V));
    dsp->registers[DSP_REG_SR] |= newcarry;
    dsp->registers[DSP_REG_SR] |= ((dsp->registers[DSP_REG_A1]>>23) & 1)<<DSP_SR_N;
    dsp->registers[DSP_REG_SR] |= (dsp->registers[DSP_REG_A1]==0)<<DSP_SR_Z;
}

static void emu_rol_b(dsp_core_t* dsp)
{
    uint32_t newcarry;

    /* xbased: through C (A-6.101): bit 47 to C, the old C into bit 24 (it
     * rotated bit 47 into bit 24) */
    newcarry = (dsp->registers[DSP_REG_B1]>>23) & 1;

    dsp->registers[DSP_REG_B1] <<= 1;
    dsp->registers[DSP_REG_B1] |= (dsp->registers[DSP_REG_SR]>>DSP_SR_C) & 1;
    dsp->registers[DSP_REG_B1] &= BITMASK(24);

    dsp->registers[DSP_REG_SR] &= BITMASK(16)-((1<<DSP_SR_C)|(1<<DSP_SR_N)|(1<<DSP_SR_Z)|(1<<DSP_SR_V));
    dsp->registers[DSP_REG_SR] |= newcarry;
    dsp->registers[DSP_REG_SR] |= ((dsp->registers[DSP_REG_B1]>>23) & 1)<<DSP_SR_N;
    dsp->registers[DSP_REG_SR] |= (dsp->registers[DSP_REG_B1]==0)<<DSP_SR_Z;
}

static void emu_ror_a(dsp_core_t* dsp)
{
    uint32_t newcarry;

    /* xbased: through C (A-6.102): bit 24 to C, the old C into bit 47, N
     * that bit (it rotated bit 24 into bit 47) */
    newcarry = dsp->registers[DSP_REG_A1] & 1;

    dsp->registers[DSP_REG_A1] >>= 1;
    dsp->registers[DSP_REG_A1] |= ((dsp->registers[DSP_REG_SR]>>DSP_SR_C) & 1)<<23;

    dsp->registers[DSP_REG_SR] &= BITMASK(16)-((1<<DSP_SR_C)|(1<<DSP_SR_N)|(1<<DSP_SR_Z)|(1<<DSP_SR_V));
    dsp->registers[DSP_REG_SR] |= newcarry;
    dsp->registers[DSP_REG_SR] |= ((dsp->registers[DSP_REG_A1]>>23) & 1)<<DSP_SR_N;
    dsp->registers[DSP_REG_SR] |= (dsp->registers[DSP_REG_A1]==0)<<DSP_SR_Z;
}

static void emu_ror_b(dsp_core_t* dsp)
{
    uint32_t newcarry;

    /* xbased: through C (A-6.102): bit 24 to C, the old C into bit 47, N
     * that bit (it rotated bit 24 into bit 47) */
    newcarry = dsp->registers[DSP_REG_B1] & 1;

    dsp->registers[DSP_REG_B1] >>= 1;
    dsp->registers[DSP_REG_B1] |= ((dsp->registers[DSP_REG_SR]>>DSP_SR_C) & 1)<<23;

    dsp->registers[DSP_REG_SR] &= BITMASK(16)-((1<<DSP_SR_C)|(1<<DSP_SR_N)|(1<<DSP_SR_Z)|(1<<DSP_SR_V));
    dsp->registers[DSP_REG_SR] |= newcarry;
    dsp->registers[DSP_REG_SR] |= ((dsp->registers[DSP_REG_B1]>>23) & 1)<<DSP_SR_N;
    dsp->registers[DSP_REG_SR] |= (dsp->registers[DSP_REG_B1]==0)<<DSP_SR_Z;
}

static void emu_sbc_x_a(dsp_core_t* dsp)
{
    uint32_t source[3], dest[3], curcarry;
    uint16_t newsr;

    curcarry = (dsp->registers[DSP_REG_SR]>>(DSP_SR_C)) & 1;

    dest[2] = dsp->registers[DSP_REG_A0];
    dest[1] = dsp->registers[DSP_REG_A1];
    dest[0] = dsp->registers[DSP_REG_A2];

    source[2] = dsp->registers[DSP_REG_X0];
    source[1] = dsp->registers[DSP_REG_X1];
    source[0] = source[1] & (1<<23) ? 0xff : 0x0;

    newsr = dsp_sub56(source, dest);

    if (curcarry) {
        source[0]=0; source[1]=0; source[2]=1;
        newsr |= dsp_sub56(source, dest);
    }

    dsp->registers[DSP_REG_A2] = dest[0];
    dsp->registers[DSP_REG_A1] = dest[1];
    dsp->registers[DSP_REG_A0] = dest[2];

    emu_ccr_update_e_u_n_z(dsp, dest[0], dest[1], dest[2]);

    dsp->registers[DSP_REG_SR] &= BITMASK(16)-((1<<DSP_SR_V)|(1<<DSP_SR_C));
    dsp->registers[DSP_REG_SR] |= newsr;
}

static void emu_sbc_x_b(dsp_core_t* dsp)
{
    uint32_t source[3], dest[3], curcarry;
    uint16_t newsr;

    curcarry = (dsp->registers[DSP_REG_SR]>>(DSP_SR_C)) & 1;

    dest[2] = dsp->registers[DSP_REG_B0];
    dest[1] = dsp->registers[DSP_REG_B1];
    dest[0] = dsp->registers[DSP_REG_B2];

    source[2] = dsp->registers[DSP_REG_X0];
    source[1] = dsp->registers[DSP_REG_X1];
    source[0] = source[1] & (1<<23) ? 0xff : 0x0;

    newsr = dsp_sub56(source, dest);

    if (curcarry) {
        source[0]=0; source[1]=0; source[2]=1;
        newsr |= dsp_sub56(source, dest);
    }

    dsp->registers[DSP_REG_B2] = dest[0];
    dsp->registers[DSP_REG_B1] = dest[1];
    dsp->registers[DSP_REG_B0] = dest[2];

    emu_ccr_update_e_u_n_z(dsp, dest[0], dest[1], dest[2]);

    dsp->registers[DSP_REG_SR] &= BITMASK(16)-((1<<DSP_SR_V)|(1<<DSP_SR_C));
    dsp->registers[DSP_REG_SR] |= newsr;
}

static void emu_sbc_y_a(dsp_core_t* dsp)
{
    uint32_t source[3], dest[3], curcarry;
    uint16_t newsr;

    curcarry = (dsp->registers[DSP_REG_SR]>>(DSP_SR_C)) & 1;

    dest[2] = dsp->registers[DSP_REG_A0];
    dest[1] = dsp->registers[DSP_REG_A1];
    dest[0] = dsp->registers[DSP_REG_A2];

    source[2] = dsp->registers[DSP_REG_Y0];
    source[1] = dsp->registers[DSP_REG_Y1];
    source[0] = source[1] & (1<<23) ? 0xff : 0x0;

    newsr = dsp_sub56(source, dest);

    if (curcarry) {
        source[0]=0; source[1]=0; source[2]=1;
        newsr |= dsp_sub56(source, dest);
    }

    dsp->registers[DSP_REG_A2] = dest[0];
    dsp->registers[DSP_REG_A1] = dest[1];
    dsp->registers[DSP_REG_A0] = dest[2];

    emu_ccr_update_e_u_n_z(dsp, dest[0], dest[1], dest[2]);

    dsp->registers[DSP_REG_SR] &= BITMASK(16)-((1<<DSP_SR_V)|(1<<DSP_SR_C));
    dsp->registers[DSP_REG_SR] |= newsr;
}

static void emu_sbc_y_b(dsp_core_t* dsp)
{
    uint32_t source[3], dest[3], curcarry;
    uint16_t newsr;

    curcarry = (dsp->registers[DSP_REG_SR]>>(DSP_SR_C)) & 1;

    dest[2] = dsp->registers[DSP_REG_B0];
    dest[1] = dsp->registers[DSP_REG_B1];
    dest[0] = dsp->registers[DSP_REG_B2];

    source[2] = dsp->registers[DSP_REG_Y0];
    source[1] = dsp->registers[DSP_REG_Y1];
    source[0] = source[1] & (1<<23) ? 0xff : 0x0;

    newsr = dsp_sub56(source, dest);

    if (curcarry) {
        source[0]=0; source[1]=0; source[2]=1;
        newsr |= dsp_sub56(source, dest);
    }

    dsp->registers[DSP_REG_B2] = dest[0];
    dsp->registers[DSP_REG_B1] = dest[1];
    dsp->registers[DSP_REG_B0] = dest[2];

    emu_ccr_update_e_u_n_z(dsp, dest[0], dest[1], dest[2]);

    dsp->registers[DSP_REG_SR] &= BITMASK(16)-((1<<DSP_SR_V)|(1<<DSP_SR_C));
    dsp->registers[DSP_REG_SR] |= newsr;
}

static void emu_sub_b_a(dsp_core_t* dsp)
{
    uint32_t source[3], dest[3];
    uint16_t newsr;

    dest[2] = dsp->registers[DSP_REG_A0];
    dest[1] = dsp->registers[DSP_REG_A1];
    dest[0] = dsp->registers[DSP_REG_A2];

    source[2] = dsp->registers[DSP_REG_B0];
    source[1] = dsp->registers[DSP_REG_B1];
    source[0] = dsp->registers[DSP_REG_B2];

    newsr = dsp_sub56(source, dest);

    dsp->registers[DSP_REG_A2] = dest[0];
    dsp->registers[DSP_REG_A1] = dest[1];
    dsp->registers[DSP_REG_A0] = dest[2];

    emu_ccr_update_e_u_n_z(dsp, dest[0], dest[1], dest[2]);

    dsp->registers[DSP_REG_SR] &= BITMASK(16)-((1<<DSP_SR_V)|(1<<DSP_SR_C));
    dsp->registers[DSP_REG_SR] |= newsr;
}

static void emu_sub_a_b(dsp_core_t* dsp)
{
    uint32_t source[3], dest[3];
    uint16_t newsr;

    dest[2] = dsp->registers[DSP_REG_B0];
    dest[1] = dsp->registers[DSP_REG_B1];
    dest[0] = dsp->registers[DSP_REG_B2];

    source[2] = dsp->registers[DSP_REG_A0];
    source[1] = dsp->registers[DSP_REG_A1];
    source[0] = dsp->registers[DSP_REG_A2];

    newsr = dsp_sub56(source, dest);

    dsp->registers[DSP_REG_B2] = dest[0];
    dsp->registers[DSP_REG_B1] = dest[1];
    dsp->registers[DSP_REG_B0] = dest[2];

    emu_ccr_update_e_u_n_z(dsp, dest[0], dest[1], dest[2]);

    dsp->registers[DSP_REG_SR] &= BITMASK(16)-((1<<DSP_SR_V)|(1<<DSP_SR_C));
    dsp->registers[DSP_REG_SR] |= newsr;
}

static void emu_sub_x_a(dsp_core_t* dsp)
{
    uint32_t source[3], dest[3];
    uint16_t newsr;

    dest[2] = dsp->registers[DSP_REG_A0];
    dest[1] = dsp->registers[DSP_REG_A1];
    dest[0] = dsp->registers[DSP_REG_A2];

    source[2] = dsp->registers[DSP_REG_X0];
    source[1] = dsp->registers[DSP_REG_X1];
    source[0] = source[1] & (1<<23) ? 0xff : 0x0;

    newsr = dsp_sub56(source, dest);

    dsp->registers[DSP_REG_A2] = dest[0];
    dsp->registers[DSP_REG_A1] = dest[1];
    dsp->registers[DSP_REG_A0] = dest[2];

    emu_ccr_update_e_u_n_z(dsp, dest[0], dest[1], dest[2]);

    dsp->registers[DSP_REG_SR] &= BITMASK(16)-((1<<DSP_SR_V)|(1<<DSP_SR_C));
    dsp->registers[DSP_REG_SR] |= newsr;
}

static void emu_sub_x_b(dsp_core_t* dsp)
{
    uint32_t source[3], dest[3];
    uint16_t newsr;

    dest[2] = dsp->registers[DSP_REG_B0];
    dest[1] = dsp->registers[DSP_REG_B1];
    dest[0] = dsp->registers[DSP_REG_B2];

    source[2] = dsp->registers[DSP_REG_X0];
    source[1] = dsp->registers[DSP_REG_X1];
    source[0] = source[1] & (1<<23) ? 0xff : 0x0;

    newsr = dsp_sub56(source, dest);

    dsp->registers[DSP_REG_B2] = dest[0];
    dsp->registers[DSP_REG_B1] = dest[1];
    dsp->registers[DSP_REG_B0] = dest[2];

    emu_ccr_update_e_u_n_z(dsp, dest[0], dest[1], dest[2]);

    dsp->registers[DSP_REG_SR] &= BITMASK(16)-((1<<DSP_SR_V)|(1<<DSP_SR_C));
    dsp->registers[DSP_REG_SR] |= newsr;
}

static void emu_sub_y_a(dsp_core_t* dsp)
{
    uint32_t source[3], dest[3];
    uint16_t newsr;

    dest[2] = dsp->registers[DSP_REG_A0];
    dest[1] = dsp->registers[DSP_REG_A1];
    dest[0] = dsp->registers[DSP_REG_A2];

    source[2] = dsp->registers[DSP_REG_Y0];
    source[1] = dsp->registers[DSP_REG_Y1];
    source[0] = source[1] & (1<<23) ? 0xff : 0x0;

    newsr = dsp_sub56(source, dest);

    dsp->registers[DSP_REG_A2] = dest[0];
    dsp->registers[DSP_REG_A1] = dest[1];
    dsp->registers[DSP_REG_A0] = dest[2];

    emu_ccr_update_e_u_n_z(dsp, dest[0], dest[1], dest[2]);

    dsp->registers[DSP_REG_SR] &= BITMASK(16)-((1<<DSP_SR_V)|(1<<DSP_SR_C));
    dsp->registers[DSP_REG_SR] |= newsr;
}

static void emu_sub_y_b(dsp_core_t* dsp)
{
    uint32_t source[3], dest[3];
    uint16_t newsr;

    dest[2] = dsp->registers[DSP_REG_B0];
    dest[1] = dsp->registers[DSP_REG_B1];
    dest[0] = dsp->registers[DSP_REG_B2];

    source[2] = dsp->registers[DSP_REG_Y0];
    source[1] = dsp->registers[DSP_REG_Y1];
    source[0] = source[1] & (1<<23) ? 0xff : 0x0;

    newsr = dsp_sub56(source, dest);

    dsp->registers[DSP_REG_B2] = dest[0];
    dsp->registers[DSP_REG_B1] = dest[1];
    dsp->registers[DSP_REG_B0] = dest[2];

    emu_ccr_update_e_u_n_z(dsp, dest[0], dest[1], dest[2]);

    dsp->registers[DSP_REG_SR] &= BITMASK(16)-((1<<DSP_SR_V)|(1<<DSP_SR_C));
    dsp->registers[DSP_REG_SR] |= newsr;
}

static void emu_sub_x0_a(dsp_core_t* dsp)
{
    uint32_t source[3], dest[3];
    uint16_t newsr;

    dest[2] = dsp->registers[DSP_REG_A0];
    dest[1] = dsp->registers[DSP_REG_A1];
    dest[0] = dsp->registers[DSP_REG_A2];

    source[2] = 0;
    source[1] = dsp->registers[DSP_REG_X0];
    source[0] = source[1] & (1<<23) ? 0xff : 0x0;

    newsr = dsp_sub56(source, dest);

    dsp->registers[DSP_REG_A2] = dest[0];
    dsp->registers[DSP_REG_A1] = dest[1];
    dsp->registers[DSP_REG_A0] = dest[2];

    emu_ccr_update_e_u_n_z(dsp, dest[0], dest[1], dest[2]);

    dsp->registers[DSP_REG_SR] &= BITMASK(16)-((1<<DSP_SR_V)|(1<<DSP_SR_C));
    dsp->registers[DSP_REG_SR] |= newsr;
}

static void emu_sub_x0_b(dsp_core_t* dsp)
{
    uint32_t source[3], dest[3];
    uint16_t newsr;

    dest[2] = dsp->registers[DSP_REG_B0];
    dest[1] = dsp->registers[DSP_REG_B1];
    dest[0] = dsp->registers[DSP_REG_B2];

    source[2] = 0;
    source[1] = dsp->registers[DSP_REG_X0];
    source[0] = source[1] & (1<<23) ? 0xff : 0x0;

    newsr = dsp_sub56(source, dest);

    dsp->registers[DSP_REG_B2] = dest[0];
    dsp->registers[DSP_REG_B1] = dest[1];
    dsp->registers[DSP_REG_B0] = dest[2];

    emu_ccr_update_e_u_n_z(dsp, dest[0], dest[1], dest[2]);

    dsp->registers[DSP_REG_SR] &= BITMASK(16)-((1<<DSP_SR_V)|(1<<DSP_SR_C));
    dsp->registers[DSP_REG_SR] |= newsr;
}

static void emu_sub_y0_a(dsp_core_t* dsp)
{
    uint32_t source[3], dest[3];
    uint16_t newsr;

    dest[2] = dsp->registers[DSP_REG_A0];
    dest[1] = dsp->registers[DSP_REG_A1];
    dest[0] = dsp->registers[DSP_REG_A2];

    source[2] = 0;
    source[1] = dsp->registers[DSP_REG_Y0];
    source[0] = source[1] & (1<<23) ? 0xff : 0x0;

    newsr = dsp_sub56(source, dest);

    dsp->registers[DSP_REG_A2] = dest[0];
    dsp->registers[DSP_REG_A1] = dest[1];
    dsp->registers[DSP_REG_A0] = dest[2];

    emu_ccr_update_e_u_n_z(dsp, dest[0], dest[1], dest[2]);

    dsp->registers[DSP_REG_SR] &= BITMASK(16)-((1<<DSP_SR_V)|(1<<DSP_SR_C));
    dsp->registers[DSP_REG_SR] |= newsr;
}

static void emu_sub_y0_b(dsp_core_t* dsp)
{
    uint32_t source[3], dest[3];
    uint16_t newsr;

    dest[2] = dsp->registers[DSP_REG_B0];
    dest[1] = dsp->registers[DSP_REG_B1];
    dest[0] = dsp->registers[DSP_REG_B2];

    source[2] = 0;
    source[1] = dsp->registers[DSP_REG_Y0];
    source[0] = source[1] & (1<<23) ? 0xff : 0x0;

    newsr = dsp_sub56(source, dest);

    dsp->registers[DSP_REG_B2] = dest[0];
    dsp->registers[DSP_REG_B1] = dest[1];
    dsp->registers[DSP_REG_B0] = dest[2];

    emu_ccr_update_e_u_n_z(dsp, dest[0], dest[1], dest[2]);

    dsp->registers[DSP_REG_SR] &= BITMASK(16)-((1<<DSP_SR_V)|(1<<DSP_SR_C));
    dsp->registers[DSP_REG_SR] |= newsr;
}

static void emu_sub_x1_a(dsp_core_t* dsp)
{
    uint32_t source[3], dest[3];
    uint16_t newsr;

    dest[2] = dsp->registers[DSP_REG_A0];
    dest[1] = dsp->registers[DSP_REG_A1];
    dest[0] = dsp->registers[DSP_REG_A2];

    source[2] = 0;
    source[1] = dsp->registers[DSP_REG_X1];
    source[0] = source[1] & (1<<23) ? 0xff : 0x0;

    newsr = dsp_sub56(source, dest);

    dsp->registers[DSP_REG_A2] = dest[0];
    dsp->registers[DSP_REG_A1] = dest[1];
    dsp->registers[DSP_REG_A0] = dest[2];

    emu_ccr_update_e_u_n_z(dsp, dest[0], dest[1], dest[2]);

    dsp->registers[DSP_REG_SR] &= BITMASK(16)-((1<<DSP_SR_V)|(1<<DSP_SR_C));
    dsp->registers[DSP_REG_SR] |= newsr;
}

static void emu_sub_x1_b(dsp_core_t* dsp)
{
    uint32_t source[3], dest[3];
    uint16_t newsr;

    dest[2] = dsp->registers[DSP_REG_B0];
    dest[1] = dsp->registers[DSP_REG_B1];
    dest[0] = dsp->registers[DSP_REG_B2];

    source[2] = 0;
    source[1] = dsp->registers[DSP_REG_X1];
    source[0] = source[1] & (1<<23) ? 0xff : 0x0;

    newsr = dsp_sub56(source, dest);

    dsp->registers[DSP_REG_B2] = dest[0];
    dsp->registers[DSP_REG_B1] = dest[1];
    dsp->registers[DSP_REG_B0] = dest[2];

    emu_ccr_update_e_u_n_z(dsp, dest[0], dest[1], dest[2]);

    dsp->registers[DSP_REG_SR] &= BITMASK(16)-((1<<DSP_SR_V)|(1<<DSP_SR_C));
    dsp->registers[DSP_REG_SR] |= newsr;
}

static void emu_sub_y1_a(dsp_core_t* dsp)
{
    uint32_t source[3], dest[3];
    uint16_t newsr;

    dest[2] = dsp->registers[DSP_REG_A0];
    dest[1] = dsp->registers[DSP_REG_A1];
    dest[0] = dsp->registers[DSP_REG_A2];

    source[2] = 0;
    source[1] = dsp->registers[DSP_REG_Y1];
    source[0] = source[1] & (1<<23) ? 0xff : 0x0;

    newsr = dsp_sub56(source, dest);

    dsp->registers[DSP_REG_A2] = dest[0];
    dsp->registers[DSP_REG_A1] = dest[1];
    dsp->registers[DSP_REG_A0] = dest[2];

    emu_ccr_update_e_u_n_z(dsp, dest[0], dest[1], dest[2]);

    dsp->registers[DSP_REG_SR] &= BITMASK(16)-((1<<DSP_SR_V)|(1<<DSP_SR_C));
    dsp->registers[DSP_REG_SR] |= newsr;
}

static void emu_sub_y1_b(dsp_core_t* dsp)
{
    uint32_t source[3], dest[3];
    uint16_t newsr;

    dest[2] = dsp->registers[DSP_REG_B0];
    dest[1] = dsp->registers[DSP_REG_B1];
    dest[0] = dsp->registers[DSP_REG_B2];

    source[2] = 0;
    source[1] = dsp->registers[DSP_REG_Y1];
    source[0] = source[1] & (1<<23) ? 0xff : 0x0;

    newsr = dsp_sub56(source, dest);

    dsp->registers[DSP_REG_B2] = dest[0];
    dsp->registers[DSP_REG_B1] = dest[1];
    dsp->registers[DSP_REG_B0] = dest[2];

    emu_ccr_update_e_u_n_z(dsp, dest[0], dest[1], dest[2]);

    dsp->registers[DSP_REG_SR] &= BITMASK(16)-((1<<DSP_SR_V)|(1<<DSP_SR_C));
    dsp->registers[DSP_REG_SR] |= newsr;
}

static void emu_subl_a(dsp_core_t* dsp)
{
    uint32_t source[3], dest[3];
    uint16_t newsr;

    dest[0] = dsp->registers[DSP_REG_A2];
    dest[1] = dsp->registers[DSP_REG_A1];
    dest[2] = dsp->registers[DSP_REG_A0];
    newsr = dsp_asl56(dest, 1) & ~(1<<DSP_SR_C); /* xbased: C the subtraction's */

    source[0] = dsp->registers[DSP_REG_B2];
    source[1] = dsp->registers[DSP_REG_B1];
    source[2] = dsp->registers[DSP_REG_B0];
    newsr |= dsp_sub56(source, dest);

    dsp->registers[DSP_REG_A2] = dest[0];
    dsp->registers[DSP_REG_A1] = dest[1];
    dsp->registers[DSP_REG_A0] = dest[2];

    emu_ccr_update_e_u_n_z(dsp, dest[0], dest[1], dest[2]);

    dsp->registers[DSP_REG_SR] &= BITMASK(16)-((1<<DSP_SR_V)|(1<<DSP_SR_C));
    dsp->registers[DSP_REG_SR] |= newsr;
}

static void emu_subl_b(dsp_core_t* dsp)
{
    uint32_t source[3], dest[3];
    uint16_t newsr;

    dest[0] = dsp->registers[DSP_REG_B2];
    dest[1] = dsp->registers[DSP_REG_B1];
    dest[2] = dsp->registers[DSP_REG_B0];
    newsr = dsp_asl56(dest, 1) & ~(1<<DSP_SR_C); /* xbased: C the subtraction's */

    source[0] = dsp->registers[DSP_REG_A2];
    source[1] = dsp->registers[DSP_REG_A1];
    source[2] = dsp->registers[DSP_REG_A0];
    newsr |= dsp_sub56(source, dest);

    dsp->registers[DSP_REG_B2] = dest[0];
    dsp->registers[DSP_REG_B1] = dest[1];
    dsp->registers[DSP_REG_B0] = dest[2];

    emu_ccr_update_e_u_n_z(dsp, dest[0], dest[1], dest[2]);

    dsp->registers[DSP_REG_SR] &= BITMASK(16)-((1<<DSP_SR_V)|(1<<DSP_SR_C));
    dsp->registers[DSP_REG_SR] |= newsr;
}

static void emu_subr_a(dsp_core_t* dsp)
{
    uint32_t source[3], dest[3];
    uint16_t newsr;

    dest[0] = dsp->registers[DSP_REG_A2];
    dest[1] = dsp->registers[DSP_REG_A1];
    dest[2] = dsp->registers[DSP_REG_A0];

    dsp_asr56(dest, 1); newsr = 0; /* xbased: C the add's / subtraction's */

    source[0] = dsp->registers[DSP_REG_B2];
    source[1] = dsp->registers[DSP_REG_B1];
    source[2] = dsp->registers[DSP_REG_B0];

    newsr |= dsp_sub56(source, dest);

    dsp->registers[DSP_REG_A2] = dest[0];
    dsp->registers[DSP_REG_A1] = dest[1];
    dsp->registers[DSP_REG_A0] = dest[2];

    emu_ccr_update_e_u_n_z(dsp, dest[0], dest[1], dest[2]);

    dsp->registers[DSP_REG_SR] &= BITMASK(16)-((1<<DSP_SR_V)|(1<<DSP_SR_C));
    dsp->registers[DSP_REG_SR] |= newsr;
}

static void emu_subr_b(dsp_core_t* dsp)
{
    uint32_t source[3], dest[3];
    uint16_t newsr;

    dest[0] = dsp->registers[DSP_REG_B2];
    dest[1] = dsp->registers[DSP_REG_B1];
    dest[2] = dsp->registers[DSP_REG_B0];

    dsp_asr56(dest, 1); newsr = 0; /* xbased: C the add's / subtraction's */

    source[0] = dsp->registers[DSP_REG_A2];
    source[1] = dsp->registers[DSP_REG_A1];
    source[2] = dsp->registers[DSP_REG_A0];

    newsr |= dsp_sub56(source, dest);

    dsp->registers[DSP_REG_B2] = dest[0];
    dsp->registers[DSP_REG_B1] = dest[1];
    dsp->registers[DSP_REG_B0] = dest[2];

    emu_ccr_update_e_u_n_z(dsp, dest[0], dest[1], dest[2]);

    dsp->registers[DSP_REG_SR] &= BITMASK(16)-((1<<DSP_SR_V)|(1<<DSP_SR_C));
    dsp->registers[DSP_REG_SR] |= newsr;
}

static void emu_tfr_b_a(dsp_core_t* dsp)
{
    dsp->registers[DSP_REG_A0] = dsp->registers[DSP_REG_B0];
    dsp->registers[DSP_REG_A1] = dsp->registers[DSP_REG_B1];
    dsp->registers[DSP_REG_A2] = dsp->registers[DSP_REG_B2];
}

static void emu_tfr_a_b(dsp_core_t* dsp)
{
    dsp->registers[DSP_REG_B0] = dsp->registers[DSP_REG_A0];
    dsp->registers[DSP_REG_B1] = dsp->registers[DSP_REG_A1];
    dsp->registers[DSP_REG_B2] = dsp->registers[DSP_REG_A2];
}

static void emu_tfr_x0_a(dsp_core_t* dsp)
{
    dsp->registers[DSP_REG_A0] = 0;
    dsp->registers[DSP_REG_A1] = dsp->registers[DSP_REG_X0];
    if (dsp->registers[DSP_REG_A1] & (1<<23))
        dsp->registers[DSP_REG_A2] = 0xff;
    else
        dsp->registers[DSP_REG_A2] = 0x0;
}

static void emu_tfr_x0_b(dsp_core_t* dsp)
{
    dsp->registers[DSP_REG_B0] = 0;
    dsp->registers[DSP_REG_B1] = dsp->registers[DSP_REG_X0];
    if (dsp->registers[DSP_REG_B1] & (1<<23))
        dsp->registers[DSP_REG_B2] = 0xff;
    else
        dsp->registers[DSP_REG_B2] = 0x0;
}

static void emu_tfr_y0_a(dsp_core_t* dsp)
{
    dsp->registers[DSP_REG_A0] = 0;
    dsp->registers[DSP_REG_A1] = dsp->registers[DSP_REG_Y0];
    if (dsp->registers[DSP_REG_A1] & (1<<23))
        dsp->registers[DSP_REG_A2] = 0xff;
    else
        dsp->registers[DSP_REG_A2] = 0x0;
}

static void emu_tfr_y0_b(dsp_core_t* dsp)
{
    dsp->registers[DSP_REG_B0] = 0;
    dsp->registers[DSP_REG_B1] = dsp->registers[DSP_REG_Y0];
    if (dsp->registers[DSP_REG_B1] & (1<<23))
        dsp->registers[DSP_REG_B2] = 0xff;
    else
        dsp->registers[DSP_REG_B2] = 0x0;
}

static void emu_tfr_x1_a(dsp_core_t* dsp)
{
    dsp->registers[DSP_REG_A0] = 0;
    dsp->registers[DSP_REG_A1] = dsp->registers[DSP_REG_X1];
    if (dsp->registers[DSP_REG_A1] & (1<<23))
        dsp->registers[DSP_REG_A2] = 0xff;
    else
        dsp->registers[DSP_REG_A2] = 0x0;
}

static void emu_tfr_x1_b(dsp_core_t* dsp)
{
    dsp->registers[DSP_REG_B0] = 0;
    dsp->registers[DSP_REG_B1] = dsp->registers[DSP_REG_X1];
    if (dsp->registers[DSP_REG_B1] & (1<<23))
        dsp->registers[DSP_REG_B2] = 0xff;
    else
        dsp->registers[DSP_REG_B2] = 0x0;
}

static void emu_tfr_y1_a(dsp_core_t* dsp)
{
    dsp->registers[DSP_REG_A0] = 0;
    dsp->registers[DSP_REG_A1] = dsp->registers[DSP_REG_Y1];
    if (dsp->registers[DSP_REG_A1] & (1<<23))
        dsp->registers[DSP_REG_A2] = 0xff;
    else
        dsp->registers[DSP_REG_A2] = 0x0;
}

static void emu_tfr_y1_b(dsp_core_t* dsp)
{
    dsp->registers[DSP_REG_B0] = 0;
    dsp->registers[DSP_REG_B1] = dsp->registers[DSP_REG_Y1];
    if (dsp->registers[DSP_REG_B1] & (1<<23))
        dsp->registers[DSP_REG_B2] = 0xff;
    else
        dsp->registers[DSP_REG_B2] = 0x0;
}

static void emu_tst_a(dsp_core_t* dsp)
{
    emu_ccr_update_e_u_n_z(dsp, dsp->registers[DSP_REG_A2],
                dsp->registers[DSP_REG_A1],
                dsp->registers[DSP_REG_A0]);

    dsp->registers[DSP_REG_SR] &= BITMASK(16)-(1<<DSP_SR_V);
}

static void emu_tst_b(dsp_core_t* dsp)
{
    emu_ccr_update_e_u_n_z(dsp, dsp->registers[DSP_REG_B2],
                dsp->registers[DSP_REG_B1],
                dsp->registers[DSP_REG_B0]);

    dsp->registers[DSP_REG_SR] &= BITMASK(16)-(1<<DSP_SR_V);
}

static void emu_max(dsp_core_t* dsp)
{
    /* xbased: the manual's MAX A,B: if B - A <= 0 then A -> B; C cleared
     * on the transfer, set otherwise (it transferred on A - B <= 0, a MIN,
     * with C the other way) */
    uint32_t source[3], dest[3];

    dest[2] = dsp->registers[DSP_REG_B0];
    dest[1] = dsp->registers[DSP_REG_B1];
    dest[0] = dsp->registers[DSP_REG_B2];

    source[2] = dsp->registers[DSP_REG_A0];
    source[1] = dsp->registers[DSP_REG_A1];
    source[0] = dsp->registers[DSP_REG_A2];

    dsp_sub56(source, dest);
    bool pass = ((dest[0] & (1<<7))
        || (dest[0] == 0 && dest[1] == 0 && dest[2] == 0));

    if (pass) {
        dsp->registers[DSP_REG_B0] = dsp->registers[DSP_REG_A0];
        dsp->registers[DSP_REG_B1] = dsp->registers[DSP_REG_A1];
        dsp->registers[DSP_REG_B2] = dsp->registers[DSP_REG_A2];
    }

    dsp->registers[DSP_REG_SR] &= BITMASK(16)-(1<<DSP_SR_C);
    dsp->registers[DSP_REG_SR] |= (!pass)<<DSP_SR_C;
}

/* xbased: MAXM A,B (A-6.64, it was missing): if |B| - |A| <= 0 then A -> B,
 * C cleared on the transfer, set otherwise */
static void emu_maxm(dsp_core_t* dsp)
{
    uint32_t source[3], dest[3];

    dest[2] = dsp->registers[DSP_REG_B0];
    dest[1] = dsp->registers[DSP_REG_B1];
    dest[0] = dsp->registers[DSP_REG_B2];
    dsp_abs56(dest);

    source[2] = dsp->registers[DSP_REG_A0];
    source[1] = dsp->registers[DSP_REG_A1];
    source[0] = dsp->registers[DSP_REG_A2];
    dsp_abs56(source);

    dsp_sub56(source, dest);
    bool pass = ((dest[0] & (1<<7))
        || (dest[0] == 0 && dest[1] == 0 && dest[2] == 0));

    if (pass) {
        dsp->registers[DSP_REG_B0] = dsp->registers[DSP_REG_A0];
        dsp->registers[DSP_REG_B1] = dsp->registers[DSP_REG_A1];
        dsp->registers[DSP_REG_B2] = dsp->registers[DSP_REG_A2];
    }

    dsp->registers[DSP_REG_SR] &= BITMASK(16)-(1<<DSP_SR_C);
    dsp->registers[DSP_REG_SR] |= (!pass)<<DSP_SR_C;
}

static const emu_func_t opcodes_alu[256] = {
    /* 0x00 - 0x3f */
    emu_move     , emu_tfr_b_a, emu_addr_b_a, emu_tst_a, emu_undefined, emu_cmp_b_a, emu_subr_a, emu_cmpm_b_a,
    emu_undefined, emu_tfr_a_b, emu_addr_a_b, emu_tst_b, emu_undefined, emu_cmp_a_b, emu_subr_b, emu_cmpm_a_b,
    emu_add_b_a, emu_rnd_a, emu_addl_b_a, emu_clr_a, emu_sub_b_a, emu_maxm, emu_subl_a, emu_not_a,
    emu_add_a_b, emu_rnd_b, emu_addl_a_b, emu_clr_b, emu_sub_a_b, emu_max, emu_subl_b, emu_not_b,
    emu_add_x_a, emu_adc_x_a, emu_asr_a, emu_lsr_a, emu_sub_x_a, emu_sbc_x_a, emu_abs_a, emu_ror_a,
    emu_add_x_b, emu_adc_x_b, emu_asr_b, emu_lsr_b, emu_sub_x_b, emu_sbc_x_b, emu_abs_b, emu_ror_b,
    emu_add_y_a, emu_adc_y_a, emu_asl_a, emu_lsl_a, emu_sub_y_a, emu_sbc_y_a, emu_neg_a, emu_rol_a,
    emu_add_y_b, emu_adc_y_b, emu_asl_b, emu_lsl_b, emu_sub_y_b, emu_sbc_y_b, emu_neg_b, emu_rol_b,

    /* 0x40 - 0x7f */
    emu_add_x0_a, emu_tfr_x0_a, emu_or_x0_a, emu_eor_x0_a, emu_sub_x0_a, emu_cmp_x0_a, emu_and_x0_a, emu_cmpm_x0_a,
    emu_add_x0_b, emu_tfr_x0_b, emu_or_x0_b, emu_eor_x0_b, emu_sub_x0_b, emu_cmp_x0_b, emu_and_x0_b, emu_cmpm_x0_b,
    emu_add_y0_a, emu_tfr_y0_a, emu_or_y0_a, emu_eor_y0_a, emu_sub_y0_a, emu_cmp_y0_a, emu_and_y0_a, emu_cmpm_y0_a,
    emu_add_y0_b, emu_tfr_y0_b, emu_or_y0_b, emu_eor_y0_b, emu_sub_y0_b, emu_cmp_y0_b, emu_and_y0_b, emu_cmpm_y0_b,
    emu_add_x1_a, emu_tfr_x1_a, emu_or_x1_a, emu_eor_x1_a, emu_sub_x1_a, emu_cmp_x1_a, emu_and_x1_a, emu_cmpm_x1_a,
    emu_add_x1_b, emu_tfr_x1_b, emu_or_x1_b, emu_eor_x1_b, emu_sub_x1_b, emu_cmp_x1_b, emu_and_x1_b, emu_cmpm_x1_b,
    emu_add_y1_a, emu_tfr_y1_a, emu_or_y1_a, emu_eor_y1_a, emu_sub_y1_a, emu_cmp_y1_a, emu_and_y1_a, emu_cmpm_y1_a,
    emu_add_y1_b, emu_tfr_y1_b, emu_or_y1_b, emu_eor_y1_b, emu_sub_y1_b, emu_cmp_y1_b, emu_and_y1_b, emu_cmpm_y1_b,

    /* 0x80 - 0xbf */
    emu_mpy_p_x0_x0_a, emu_mpyr_p_x0_x0_a, emu_mac_p_x0_x0_a, emu_macr_p_x0_x0_a, emu_mpy_m_x0_x0_a, emu_mpyr_m_x0_x0_a, emu_mac_m_x0_x0_a, emu_macr_m_x0_x0_a,
    emu_mpy_p_x0_x0_b, emu_mpyr_p_x0_x0_b, emu_mac_p_x0_x0_b, emu_macr_p_x0_x0_b, emu_mpy_m_x0_x0_b, emu_mpyr_m_x0_x0_b, emu_mac_m_x0_x0_b, emu_macr_m_x0_x0_b,
    emu_mpy_p_y0_y0_a, emu_mpyr_p_y0_y0_a, emu_mac_p_y0_y0_a, emu_macr_p_y0_y0_a, emu_mpy_m_y0_y0_a, emu_mpyr_m_y0_y0_a, emu_mac_m_y0_y0_a, emu_macr_m_y0_y0_a,
    emu_mpy_p_y0_y0_b, emu_mpyr_p_y0_y0_b, emu_mac_p_y0_y0_b, emu_macr_p_y0_y0_b, emu_mpy_m_y0_y0_b, emu_mpyr_m_y0_y0_b, emu_mac_m_y0_y0_b, emu_macr_m_y0_y0_b,
    emu_mpy_p_x1_x0_a, emu_mpyr_p_x1_x0_a, emu_mac_p_x1_x0_a, emu_macr_p_x1_x0_a, emu_mpy_m_x1_x0_a, emu_mpyr_m_x1_x0_a, emu_mac_m_x1_x0_a, emu_macr_m_x1_x0_a,
    emu_mpy_p_x1_x0_b, emu_mpyr_p_x1_x0_b, emu_mac_p_x1_x0_b, emu_macr_p_x1_x0_b, emu_mpy_m_x1_x0_b, emu_mpyr_m_x1_x0_b, emu_mac_m_x1_x0_b, emu_macr_m_x1_x0_b,
    emu_mpy_p_y1_y0_a, emu_mpyr_p_y1_y0_a, emu_mac_p_y1_y0_a, emu_macr_p_y1_y0_a, emu_mpy_m_y1_y0_a, emu_mpyr_m_y1_y0_a, emu_mac_m_y1_y0_a, emu_macr_m_y1_y0_a,
    emu_mpy_p_y1_y0_b, emu_mpyr_p_y1_y0_b, emu_mac_p_y1_y0_b, emu_macr_p_y1_y0_b, emu_mpy_m_y1_y0_b, emu_mpyr_m_y1_y0_b, emu_mac_m_y1_y0_b, emu_macr_m_y1_y0_b,

    /* 0xc0_m_ 0xff */
    emu_mpy_p_x0_y1_a, emu_mpyr_p_x0_y1_a, emu_mac_p_x0_y1_a, emu_macr_p_x0_y1_a, emu_mpy_m_x0_y1_a, emu_mpyr_m_x0_y1_a, emu_mac_m_x0_y1_a, emu_macr_m_x0_y1_a,
    emu_mpy_p_x0_y1_b, emu_mpyr_p_x0_y1_b, emu_mac_p_x0_y1_b, emu_macr_p_x0_y1_b, emu_mpy_m_x0_y1_b, emu_mpyr_m_x0_y1_b, emu_mac_m_x0_y1_b, emu_macr_m_x0_y1_b,
    emu_mpy_p_y0_x0_a, emu_mpyr_p_y0_x0_a, emu_mac_p_y0_x0_a, emu_macr_p_y0_x0_a, emu_mpy_m_y0_x0_a, emu_mpyr_m_y0_x0_a, emu_mac_m_y0_x0_a, emu_macr_m_y0_x0_a,
    emu_mpy_p_y0_x0_b, emu_mpyr_p_y0_x0_b, emu_mac_p_y0_x0_b, emu_macr_p_y0_x0_b, emu_mpy_m_y0_x0_b, emu_mpyr_m_y0_x0_b, emu_mac_m_y0_x0_b, emu_macr_m_y0_x0_b,
    emu_mpy_p_x1_y0_a, emu_mpyr_p_x1_y0_a, emu_mac_p_x1_y0_a, emu_macr_p_x1_y0_a, emu_mpy_m_x1_y0_a, emu_mpyr_m_x1_y0_a, emu_mac_m_x1_y0_a, emu_macr_m_x1_y0_a,
    emu_mpy_p_x1_y0_b, emu_mpyr_p_x1_y0_b, emu_mac_p_x1_y0_b, emu_macr_p_x1_y0_b, emu_mpy_m_x1_y0_b, emu_mpyr_m_x1_y0_b, emu_mac_m_x1_y0_b, emu_macr_m_x1_y0_b,
    emu_mpy_p_y1_x1_a, emu_mpyr_p_y1_x1_a, emu_mac_p_y1_x1_a, emu_macr_p_y1_x1_a, emu_mpy_m_y1_x1_a, emu_mpyr_m_y1_x1_a, emu_mac_m_y1_x1_a, emu_macr_m_y1_x1_a,
    emu_mpy_p_y1_x1_b, emu_mpyr_p_y1_x1_b, emu_mac_p_y1_x1_b, emu_macr_p_y1_x1_b, emu_mpy_m_y1_x1_b, emu_mpyr_m_y1_x1_b, emu_mac_m_y1_x1_b, emu_macr_m_y1_x1_b
};


/**********************************
 *  ALU instructions
 **********************************/

static void emu_pm_0(dsp_core_t* dsp);
static void emu_pm_1(dsp_core_t* dsp);
static void emu_pm_2(dsp_core_t* dsp);
static void emu_ifcc(dsp_core_t* dsp);
static void emu_pm_2_2(dsp_core_t* dsp);
static void emu_pm_3(dsp_core_t* dsp);
static void emu_pm_4(dsp_core_t* dsp);
static void emu_pm_4x(dsp_core_t* dsp);
static void emu_pm_5(dsp_core_t* dsp);
static void emu_pm_8(dsp_core_t* dsp);

/* xbased: the S bit (sticky, CCR bit 7), on each read of A or B to a data
 * bus (the manual's standard definition): bits 46 and 45 differ (no
 * scaling), 47 and 46 (scale down), 45 and 44 (scale up) */
static void emu_s_bit(dsp_core_t* dsp, int reg)
{
    const uint32_t scaling = (dsp->registers[DSP_REG_SR]>>DSP_SR_S0) & BITMASK(2);
    const uint32_t a1 = dsp->registers[DSP_REG_A1+reg];
    uint32_t b;
    switch (scaling) {
    case 0: b = (a1 >> 22) ^ (a1 >> 21); break;
    case 1: b = (a1 >> 23) ^ (a1 >> 22); break;
    case 2: b = (a1 >> 21) ^ (a1 >> 20); break;
    default: return;
    }
    dsp->registers[DSP_REG_SR] |= (b & 1) << 7;
}

/* the low word of A or B read as 48 bits with the scaling (the high one is
 * emu_pm_read_accu24's) */
static uint32_t emu_pm_read_accu_lo(dsp_core_t* dsp, int reg)
{
    const uint32_t scaling = (dsp->registers[DSP_REG_SR]>>DSP_SR_S0) & BITMASK(2);
    const uint32_t a0 = dsp->registers[DSP_REG_A0+reg], a1 = dsp->registers[DSP_REG_A1+reg];
    switch (scaling) {
    case 1: return ((a1 & 1) << 23) | (a0 >> 1);
    case 2: return (a0 << 1) & BITMASK(24);
    default: return a0;
    }
}

static int emu_pm_read_accu24(dsp_core_t* dsp, int numreg, uint32_t *dest)
{
    uint32_t scaling, value, reg;
    int got_limited = 0;

    /* Read an accumulator, stores it limited */

    scaling = (dsp->registers[DSP_REG_SR]>>DSP_SR_S0) & BITMASK(2);
    reg = numreg & 1;
    emu_s_bit(dsp, reg);

    value = (dsp->registers[DSP_REG_A2+reg]) << 24;
    value += dsp->registers[DSP_REG_A1+reg];

    switch(scaling) {
        case 0:
            /* No scaling */
            break;
        case 1:
            /* scaling down */
            value >>= 1;
            break;
        case 2:
            /* scaling up */
            value <<= 1;
            value |= (dsp->registers[DSP_REG_A0+reg]>>23) & 1;
            break;
        /* indeterminate */
        case 3:
            break;
    }

    /* limiting ? */
    value &= BITMASK(24);

    if (dsp->registers[DSP_REG_A2+reg] == 0) {
        if (value <= 0x007fffff) {
            /* No limiting */
            *dest=value;
            return 0;
        }
    }

    if (dsp->registers[DSP_REG_A2+reg] == 0xff) {
        if (value >= 0x00800000) {
            /* No limiting */
            *dest=value;
            return 0;
        }
    }

    if (dsp->registers[DSP_REG_A2+reg] & (1<<7)) {
        /* Limited to maximum negative value */
        *dest=0x00800000;
        dsp->registers[DSP_REG_SR] |= (1<<DSP_SR_L);
        got_limited=1;
    } else {
        /* Limited to maximal positive value */
        *dest=0x007fffff;
        dsp->registers[DSP_REG_SR] |= (1<<DSP_SR_L);
        got_limited=1;
    }

    return got_limited;
}

static void emu_pm_0(dsp_core_t* dsp)
{
    uint32_t memspace, numreg, addr, save_accu, save_xy0;
/*
    0000 100d 00mm mrrr S,x:ea  x0,D
    0000 100d 10mm mrrr S,y:ea  y0,D
*/
    memspace = (dsp->cur_inst>>15) & 1;
    numreg = (dsp->cur_inst>>16) & 1;
    emu_calc_ea(dsp, (dsp->cur_inst>>8) & BITMASK(6), &addr);

    /* Save A or B */
    emu_pm_read_accu24(dsp, numreg, &save_accu);

    /* Save X0 or Y0 */
    save_xy0 = dsp->registers[DSP_REG_X0+(memspace<<1)];

    /* Execute parallel instruction */
    opcodes_alu[dsp->cur_inst & BITMASK(8)](dsp);
    emu_alu_sm(dsp, dsp->cur_inst & BITMASK(8));

    /* Move [A|B] to [x|y]:ea */
    dsp56k_write_memory(dsp, memspace, addr, save_accu);

    /* Move [x|y]0 to [A|B] */
    dsp->registers[DSP_REG_A0+numreg] = 0;
    dsp->registers[DSP_REG_A1+numreg] = save_xy0;
    dsp->registers[DSP_REG_A2+numreg] = save_xy0 & (1<<23) ? 0xff : 0x0;
}

static void emu_pm_1(dsp_core_t* dsp)
{
    uint32_t memspace, numreg1, numreg2, value, xy_addr, retour, save_1, save_2;
/*
    0001 ffdf w0mm mrrr x:ea,D1     S2,D2
                        S1,x:ea     S2,D2
                        #xxxxxx,D1  S2,D2
    0001 deff w1mm mrrr S1,D1       y:ea,D2
                        S1,D1       S2,y:ea
                        S1,D1       #xxxxxx,D2
*/
    value = (dsp->cur_inst>>8) & BITMASK(6);
    retour = emu_calc_ea(dsp, value, &xy_addr);
    memspace = (dsp->cur_inst>>14) & 1;
    numreg1 = numreg2 = DSP_REG_NULL;

    if (memspace) {
        /* Y: */
        switch((dsp->cur_inst>>16) & BITMASK(2)) {
            case 0: numreg1 = DSP_REG_Y0;   break;
            case 1: numreg1 = DSP_REG_Y1;   break;
            case 2: numreg1 = DSP_REG_A;    break;
            case 3: numreg1 = DSP_REG_B;    break;
        }
    } else {
        /* X: */
        switch((dsp->cur_inst>>18) & BITMASK(2)) {
            case 0: numreg1 = DSP_REG_X0;   break;
            case 1: numreg1 = DSP_REG_X1;   break;
            case 2: numreg1 = DSP_REG_A;    break;
            case 3: numreg1 = DSP_REG_B;    break;
        }
    }

    if (dsp->cur_inst & (1<<15)) {
        /* Write D1 */
        if (retour)
            save_1 = xy_addr;
        else
            save_1 = dsp56k_read_memory(dsp, memspace, xy_addr);
    } else {
        /* Read S1 */
        if ((numreg1==DSP_REG_A) || (numreg1==DSP_REG_B))
            emu_pm_read_accu24(dsp, numreg1, &save_1);
        else
            save_1 = dsp->registers[numreg1];
    }

    /* S2 */
    if (memspace) {
        /* Y: */
        numreg2 = DSP_REG_A + ((dsp->cur_inst>>19) & 1);
    } else {
        /* X: */
        numreg2 = DSP_REG_A + ((dsp->cur_inst>>17) & 1);
    }
    emu_pm_read_accu24(dsp, numreg2, &save_2);


    /* Execute parallel instruction */
    opcodes_alu[dsp->cur_inst & BITMASK(8)](dsp);
    emu_alu_sm(dsp, dsp->cur_inst & BITMASK(8));


    /* Write parallel move values */
    if (dsp->cur_inst & (1<<15)) {
        /* Write D1 */
        if (numreg1 == DSP_REG_A) {
            dsp->registers[DSP_REG_A0] = 0x0;
            dsp->registers[DSP_REG_A1] = save_1;
            dsp->registers[DSP_REG_A2] = save_1 & (1<<23) ? 0xff : 0x0;
        }
        else if (numreg1 == DSP_REG_B) {
            dsp->registers[DSP_REG_B0] = 0x0;
            dsp->registers[DSP_REG_B1] = save_1;
            dsp->registers[DSP_REG_B2] = save_1 & (1<<23) ? 0xff : 0x0;
        }
        else {
    }       dsp->registers[numreg1] = save_1;
    } else {
        /* Read S1 */
        dsp56k_write_memory(dsp, memspace, xy_addr, save_1);
    }

    /* S2 -> D2 */
    if (memspace) {
        /* Y: */
        numreg2 = DSP_REG_X0 + ((dsp->cur_inst>>18) & 1);
    } else {
        /* X: */
        numreg2 = DSP_REG_Y0 + ((dsp->cur_inst>>16) & 1);
    }
    dsp->registers[numreg2] = save_2;
}

static void emu_pm_2(dsp_core_t* dsp)
{
    uint32_t dummy;
/*
    0010 0000 0000 0000 nop
    0010 0000 010m mrrr R update
    0010 00ee eeed dddd S,D
    001d dddd iiii iiii #xx,D
*/
    if ((dsp->cur_inst & 0xffff00) == 0x200000) {
        /* Execute parallel instruction */
        opcodes_alu[dsp->cur_inst & BITMASK(8)](dsp);
        emu_alu_sm(dsp, dsp->cur_inst & BITMASK(8));
        return;
    }

    if ((dsp->cur_inst & 0xffe000) == 0x204000) {
        emu_calc_ea(dsp, (dsp->cur_inst>>8) & BITMASK(5), &dummy);
        /* Execute parallel instruction */
        opcodes_alu[dsp->cur_inst & BITMASK(8)](dsp);
        emu_alu_sm(dsp, dsp->cur_inst & BITMASK(8));
        return;
    }

    /* xbased: IFcc 0x202Cxx, IFcc.U 0x203Cxx (it took them for a move from
     * a reserved register) */
    if ((dsp->cur_inst & 0xffe000) == 0x202000) {
        emu_ifcc(dsp);
        return;
    }

    if ((dsp->cur_inst & 0xfc0000) == 0x200000) {
        emu_pm_2_2(dsp);
        return;
    }

    emu_pm_3(dsp);
}

static void emu_pm_2_2(dsp_core_t* dsp)
{
/*
    0010 00ee eeed dddd S,D
*/
    uint32_t srcreg, dstreg, save_reg;

    srcreg = (dsp->cur_inst >> 13) & BITMASK(5);
    dstreg = (dsp->cur_inst >> 8) & BITMASK(5);

    if ((srcreg == DSP_REG_A) || (srcreg == DSP_REG_B))
        /* Accu to register: limited 24 bits */
        emu_pm_read_accu24(dsp, srcreg, &save_reg);
    else
        save_reg = dsp->registers[srcreg];

    /* Execute parallel instruction */
    opcodes_alu[dsp->cur_inst & BITMASK(8)](dsp);
    emu_alu_sm(dsp, dsp->cur_inst & BITMASK(8));

    /* Write reg */
    if (dstreg == DSP_REG_A) {
        dsp->registers[DSP_REG_A0] = 0x0;
        dsp->registers[DSP_REG_A1] = save_reg;
        dsp->registers[DSP_REG_A2] = save_reg & (1<<23) ? 0xff : 0x0;
    }
    else if (dstreg == DSP_REG_B) {
        dsp->registers[DSP_REG_B0] = 0x0;
        dsp->registers[DSP_REG_B1] = save_reg;
        dsp->registers[DSP_REG_B2] = save_reg & (1<<23) ? 0xff : 0x0;
    }
    else {
        dsp->registers[dstreg] = save_reg & BITMASK(registers_mask[dstreg]);
    }
}

static void emu_pm_3(dsp_core_t* dsp)
{
    uint32_t dstreg, srcvalue;
/*
    001d dddd iiii iiii #xx,R
*/

    /* Execute parallel instruction */
    opcodes_alu[dsp->cur_inst & BITMASK(8)](dsp);
    emu_alu_sm(dsp, dsp->cur_inst & BITMASK(8));

    /* Write reg */
    dstreg = (dsp->cur_inst >> 16) & BITMASK(5);
    srcvalue = (dsp->cur_inst >> 8) & BITMASK(8);

    switch(dstreg) {
        case DSP_REG_X0:
        case DSP_REG_X1:
        case DSP_REG_Y0:
        case DSP_REG_Y1:
        case DSP_REG_A:
        case DSP_REG_B:
            srcvalue <<= 16;
            break;
    }

    if (dstreg == DSP_REG_A) {
        dsp->registers[DSP_REG_A0] = 0x0;
        dsp->registers[DSP_REG_A1] = srcvalue;
        dsp->registers[DSP_REG_A2] = srcvalue & (1<<23) ? 0xff : 0x0;
    }
    else if (dstreg == DSP_REG_B) {
        dsp->registers[DSP_REG_B0] = 0x0;
        dsp->registers[DSP_REG_B1] = srcvalue;
        dsp->registers[DSP_REG_B2] = srcvalue & (1<<23) ? 0xff : 0x0;
    }
    else {
        dsp->registers[dstreg] = srcvalue & BITMASK(registers_mask[dstreg]);
    }
}

static void emu_pm_4(dsp_core_t* dsp)
{
/*
    0100 l0ll w0aa aaaa             l:aa,D
                        S,l:aa
    0100 l0ll w1mm mrrr             l:ea,D
                        S,l:ea
    01dd 0ddd w0aa aaaa             x:aa,D
                        S,x:aa
    01dd 0ddd w1mm mrrr             x:ea,D
                        S,x:ea
                        #xxxxxx,D
    01dd 1ddd w0aa aaaa             y:aa,D
                        S,y:aa
    01dd 1ddd w1mm mrrr             y:ea,D
                        S,y:ea
                        #xxxxxx,D
*/
    if ((dsp->cur_inst & 0xf40000)==0x400000) {
        emu_pm_4x(dsp);
        return;
    }

    emu_pm_5(dsp);
}

static void emu_pm_4x(dsp_core_t* dsp)
{
    uint32_t value, numreg, l_addr, save_lx, save_ly;
/*
    0100 l0ll w0aa aaaa         l:aa,D
                    S,l:aa
    0100 l0ll w1mm mrrr         l:ea,D
                    S,l:ea
*/
    value = (dsp->cur_inst>>8) & BITMASK(6);
    if (dsp->cur_inst & (1<<14)) {
        emu_calc_ea(dsp, value, &l_addr);
    } else {
        l_addr = value;
    }

    numreg = (dsp->cur_inst>>16) & BITMASK(2);
    numreg |= (dsp->cur_inst>>17) & (1<<2);

    if (dsp->cur_inst & (1<<15)) {
        /* Write D */
        save_lx = dsp56k_read_memory(dsp, DSP_SPACE_X,l_addr);
        save_ly = dsp56k_read_memory(dsp, DSP_SPACE_Y,l_addr);
    }
    else {
        /* Read S */
        switch(numreg) {
            case 0:
                /* A10 */
                emu_s_bit(dsp, 0);
                save_lx = dsp->registers[DSP_REG_A1];
                save_ly = dsp->registers[DSP_REG_A0];
                break;
            case 1:
                /* B10 */
                emu_s_bit(dsp, 1);
                save_lx = dsp->registers[DSP_REG_B1];
                save_ly = dsp->registers[DSP_REG_B0];
                break;
            case 2:
                /* X */
                save_lx = dsp->registers[DSP_REG_X1];
                save_ly = dsp->registers[DSP_REG_X0];
                break;
            case 3:
                /* Y */
                save_lx = dsp->registers[DSP_REG_Y1];
                save_ly = dsp->registers[DSP_REG_Y0];
                break;
            case 4:
                /* A */
                if (emu_pm_read_accu24(dsp, DSP_REG_A, &save_lx)) {
                    /* Was limited, set lower part */
                    save_ly = (save_lx & (1<<23) ? 0 : 0xffffff);
                } else {
                    /* Not limited (xbased: scaled as the high word) */
                    save_ly = emu_pm_read_accu_lo(dsp, 0);
                }
                break;
            case 5:
                /* B */
                if (emu_pm_read_accu24(dsp, DSP_REG_B, &save_lx)) {
                    /* Was limited, set lower part */
                    save_ly = (save_lx & (1<<23) ? 0 : 0xffffff);
                } else {
                    /* Not limited (xbased: scaled as the high word) */
                    save_ly = emu_pm_read_accu_lo(dsp, 1);
                }
                break;
            case 6:
                /* AB */
                emu_pm_read_accu24(dsp, DSP_REG_A, &save_lx);
                emu_pm_read_accu24(dsp, DSP_REG_B, &save_ly);
                break;
            case 7:
                /* BA */
                emu_pm_read_accu24(dsp, DSP_REG_B, &save_lx);
                emu_pm_read_accu24(dsp, DSP_REG_A, &save_ly);
                break;
        }
    }

    /* Execute parallel instruction */
    opcodes_alu[dsp->cur_inst & BITMASK(8)](dsp);
    emu_alu_sm(dsp, dsp->cur_inst & BITMASK(8));


    if (dsp->cur_inst & (1<<15)) {
        /* Write D */
        switch(numreg) {
            case 0: /* A10 */
                dsp->registers[DSP_REG_A1] = save_lx;
                dsp->registers[DSP_REG_A0] = save_ly;
                break;
            case 1: /* B10 */
                dsp->registers[DSP_REG_B1] = save_lx;
                dsp->registers[DSP_REG_B0] = save_ly;
                break;
            case 2: /* X */
                dsp->registers[DSP_REG_X1] = save_lx;
                dsp->registers[DSP_REG_X0] = save_ly;
                break;
            case 3: /* Y */
                dsp->registers[DSP_REG_Y1] = save_lx;
                dsp->registers[DSP_REG_Y0] = save_ly;
                break;
            case 4: /* A */
                dsp->registers[DSP_REG_A0] = save_ly;
                dsp->registers[DSP_REG_A1] = save_lx;
                dsp->registers[DSP_REG_A2] = save_lx & (1<<23) ? 0xff : 0;
                break;
            case 5: /* B */
                dsp->registers[DSP_REG_B0] = save_ly;
                dsp->registers[DSP_REG_B1] = save_lx;
                dsp->registers[DSP_REG_B2] = save_lx & (1<<23) ? 0xff : 0;
                break;
            case 6: /* AB */
                dsp->registers[DSP_REG_A0] = 0;
                dsp->registers[DSP_REG_A1] = save_lx;
                dsp->registers[DSP_REG_A2] = save_lx & (1<<23) ? 0xff : 0;
                dsp->registers[DSP_REG_B0] = 0;
                dsp->registers[DSP_REG_B1] = save_ly;
                dsp->registers[DSP_REG_B2] = save_ly & (1<<23) ? 0xff : 0;
                break;
            case 7: /* BA */
                dsp->registers[DSP_REG_B0] = 0;
                dsp->registers[DSP_REG_B1] = save_lx;
                dsp->registers[DSP_REG_B2] = save_lx & (1<<23) ? 0xff : 0;
                dsp->registers[DSP_REG_A0] = 0;
                dsp->registers[DSP_REG_A1] = save_ly;
                dsp->registers[DSP_REG_A2] = save_ly & (1<<23) ? 0xff : 0;
                break;
        }
    }
    else {
        /* Read S */
        dsp56k_write_memory(dsp, DSP_SPACE_X, l_addr, save_lx);
        dsp56k_write_memory(dsp, DSP_SPACE_Y, l_addr, save_ly);
    }
}

static void emu_pm_5(dsp_core_t* dsp)
{
    uint32_t memspace, numreg, value, xy_addr, retour;
/*
    01dd 0ddd w0aa aaaa             x:aa,D
                        S,x:aa
    01dd 0ddd w1mm mrrr             x:ea,D
                        S,x:ea
                        #xxxxxx,D
    01dd 1ddd w0aa aaaa             y:aa,D
                        S,y:aa
    01dd 1ddd w1mm mrrr             y:ea,D
                        S,y:ea
                        #xxxxxx,D
*/

    value = (dsp->cur_inst>>8) & BITMASK(6);

    if (dsp->cur_inst & (1<<14)) {
        retour = emu_calc_ea(dsp, value, &xy_addr);
    } else {
        xy_addr = value;
        retour = 0;
    }

    memspace = (dsp->cur_inst>>19) & 1;
    numreg = (dsp->cur_inst>>16) & BITMASK(3);
    numreg |= (dsp->cur_inst>>17) & (BITMASK(2)<<3);

    if (dsp->cur_inst & (1<<15)) {
        /* Write D */
        if (retour)
            value = xy_addr;
        else
            value = dsp56k_read_memory(dsp, memspace, xy_addr);
    }
    else {
        /* Read S */
        if ((numreg==DSP_REG_A) || (numreg==DSP_REG_B))
            emu_pm_read_accu24(dsp, numreg, &value);
        else
            value = dsp->registers[numreg];
    }


    /* Execute parallel instruction */
    opcodes_alu[dsp->cur_inst & BITMASK(8)](dsp);
    emu_alu_sm(dsp, dsp->cur_inst & BITMASK(8));

    if (dsp->cur_inst & (1<<15)) {
        /* Write D */
        if (numreg == DSP_REG_A) {
            dsp->registers[DSP_REG_A0] = 0x0;
            dsp->registers[DSP_REG_A1] = value;
            dsp->registers[DSP_REG_A2] = value & (1<<23) ? 0xff : 0x0;
        }
        else if (numreg == DSP_REG_B) {
            dsp->registers[DSP_REG_B0] = 0x0;
            dsp->registers[DSP_REG_B1] = value;
            dsp->registers[DSP_REG_B2] = value & (1<<23) ? 0xff : 0x0;
        }
        else {
            dsp->registers[numreg] = value & BITMASK(registers_mask[numreg]);
        }
    }
    else {
        /* Read S */
        dsp56k_write_memory(dsp, memspace, xy_addr, value);
    }
}

static void emu_pm_8(dsp_core_t* dsp)
{
    uint32_t ea1, ea2;
    uint32_t numreg1, numreg2;
    uint32_t save_reg1, save_reg2, x_addr, y_addr;
/*
    1wmm eeff WrrM MRRR             x:ea,D1     y:ea,D2
                        x:ea,D1     S2,y:ea
                        S1,x:ea     y:ea,D2
                        S1,x:ea     S2,y:ea
*/
    numreg1 = numreg2 = DSP_REG_NULL;

    ea1 = (dsp->cur_inst>>8) & BITMASK(5);
    if ((ea1>>3) == 0) {
        ea1 |= (1<<5);
    }
    ea2 = (dsp->cur_inst>>13) & BITMASK(2);
    ea2 |= (dsp->cur_inst>>17) & (BITMASK(2)<<3);
    if ((ea1 & (1<<2))==0) {
        ea2 |= 1<<2;
    }
    if ((ea2>>3) == 0) {
        ea2 |= (1<<5);
    }

    emu_calc_ea(dsp, ea1, &x_addr);
    emu_calc_ea(dsp, ea2, &y_addr);

    switch((dsp->cur_inst>>18) & BITMASK(2)) {
        case 0: numreg1=DSP_REG_X0; break;
        case 1: numreg1=DSP_REG_X1; break;
        case 2: numreg1=DSP_REG_A;  break;
        case 3: numreg1=DSP_REG_B;  break;
    }
    switch((dsp->cur_inst>>16) & BITMASK(2)) {
        case 0: numreg2=DSP_REG_Y0; break;
        case 1: numreg2=DSP_REG_Y1; break;
        case 2: numreg2=DSP_REG_A;  break;
        case 3: numreg2=DSP_REG_B;  break;
    }

    if (dsp->cur_inst & (1<<15)) {
        /* Write D1 */
        save_reg1 = dsp56k_read_memory(dsp, DSP_SPACE_X, x_addr);
    } else {
        /* Read S1 */
        if ((numreg1==DSP_REG_A) || (numreg1==DSP_REG_B))
            emu_pm_read_accu24(dsp, numreg1, &save_reg1);
        else
            save_reg1 = dsp->registers[numreg1];
    }

    if (dsp->cur_inst & (1<<22)) {
        /* Write D2 */
        save_reg2 = dsp56k_read_memory(dsp, DSP_SPACE_Y, y_addr);
    } else {
        /* Read S2 */
        if ((numreg2==DSP_REG_A) || (numreg2==DSP_REG_B))
            emu_pm_read_accu24(dsp, numreg2, &save_reg2);
        else
            save_reg2 = dsp->registers[numreg2];
    }


    /* Execute parallel instruction */
    opcodes_alu[dsp->cur_inst & BITMASK(8)](dsp);
    emu_alu_sm(dsp, dsp->cur_inst & BITMASK(8));

    /* Write first parallel move */
    if (dsp->cur_inst & (1<<15)) {
        /* Write D1 */
        if (numreg1 == DSP_REG_A) {
            dsp->registers[DSP_REG_A0] = 0x0;
            dsp->registers[DSP_REG_A1] = save_reg1;
            dsp->registers[DSP_REG_A2] = save_reg1 & (1<<23) ? 0xff : 0x0;
        }
        else if (numreg1 == DSP_REG_B) {
            dsp->registers[DSP_REG_B0] = 0x0;
            dsp->registers[DSP_REG_B1] = save_reg1;
            dsp->registers[DSP_REG_B2] = save_reg1 & (1<<23) ? 0xff : 0x0;
        }
        else {
            dsp->registers[numreg1] = save_reg1;
        }
    } else {
        /* Read S1 */
        dsp56k_write_memory(dsp, DSP_SPACE_X, x_addr, save_reg1);
    }

    /* Write second parallel move */
    if (dsp->cur_inst & (1<<22)) {
        /* Write D2 */
        if (numreg2 == DSP_REG_A) {
            dsp->registers[DSP_REG_A0] = 0x0;
            dsp->registers[DSP_REG_A1] = save_reg2;
            dsp->registers[DSP_REG_A2] = save_reg2 & (1<<23) ? 0xff : 0x0;
        }
        else if (numreg2 == DSP_REG_B) {
            dsp->registers[DSP_REG_B0] = 0x0;
            dsp->registers[DSP_REG_B1] = save_reg2;
            dsp->registers[DSP_REG_B2] = save_reg2 & (1<<23) ? 0xff : 0x0;
        }
        else {
            dsp->registers[numreg2] = save_reg2;
        }
    } else {
        /* Read S2 */
        dsp56k_write_memory(dsp, DSP_SPACE_Y, y_addr, save_reg2);
    }
}

static const emu_func_t opcodes_parmove[16] = {
    emu_pm_0, emu_pm_1, emu_pm_2, emu_pm_3, emu_pm_4, emu_pm_5, emu_pm_5, emu_pm_5,
    emu_pm_8, emu_pm_8, emu_pm_8, emu_pm_8, emu_pm_8, emu_pm_8, emu_pm_8, emu_pm_8
};


/**********************************
 *  Non-parallel moves instructions
 **********************************/

static void emu_add_x(dsp_core_t* dsp, uint32_t x, uint32_t d)
{
    uint32_t source[3], dest[3];
    if (d) {
        dest[0] = dsp->registers[DSP_REG_B2];
        dest[1] = dsp->registers[DSP_REG_B1];
        dest[2] = dsp->registers[DSP_REG_B0];
    } else {
        dest[0] = dsp->registers[DSP_REG_A2];
        dest[1] = dsp->registers[DSP_REG_A1];
        dest[2] = dsp->registers[DSP_REG_A0];
    }

    source[2] = 0;
    source[1] = x;
    source[0] = source[1] & (1<<23) ? 0xff : 0x0;

    uint16_t newsr = dsp_add56(source, dest);

    if (d) {
        dsp->registers[DSP_REG_B2] = dest[0];
        dsp->registers[DSP_REG_B1] = dest[1];
        dsp->registers[DSP_REG_B0] = dest[2];
    } else {
        dsp->registers[DSP_REG_A2] = dest[0];
        dsp->registers[DSP_REG_A1] = dest[1];
        dsp->registers[DSP_REG_A0] = dest[2];
    }

    emu_ccr_update_e_u_n_z(dsp, dest[0], dest[1], dest[2]);

    dsp->registers[DSP_REG_SR] &= BITMASK(16)-((1<<DSP_SR_V)|(1<<DSP_SR_C));
    dsp->registers[DSP_REG_SR] |= newsr;
    emu_sm_acc(dsp, d, 0); /* SM */
}

static void emu_add_imm(dsp_core_t* dsp)
{
    uint32_t xx = (dsp->cur_inst >> 8) & BITMASK(6);
    uint32_t d = (dsp->cur_inst >> 3) & 1;
    emu_add_x(dsp, xx, d);
}

static void emu_add_long(dsp_core_t* dsp)
{
    uint32_t xxxx = read_memory_p(dsp, dsp->pc+1);
    dsp->cur_inst_len++;
    uint32_t d = (dsp->cur_inst >> 3) & 1;
    emu_add_x(dsp, xxxx, d);
}

static void emu_and_x(dsp_core_t* dsp, uint32_t x, uint32_t d)
{
    int dstreg;
    if (d) {
        dstreg = DSP_REG_B1;
    } else {
        dstreg = DSP_REG_A1;
    }

    dsp->registers[dstreg] &= x;

    dsp->registers[DSP_REG_SR] &= BITMASK(16)-((1<<DSP_SR_N)|(1<<DSP_SR_Z)|(1<<DSP_SR_V));
    dsp->registers[DSP_REG_SR] |= ((dsp->registers[dstreg]>>23) & 1)<<DSP_SR_N;
    dsp->registers[DSP_REG_SR] |= (dsp->registers[dstreg]==0)<<DSP_SR_Z;
}

static void emu_and_imm(dsp_core_t* dsp)
{
    uint32_t xx = (dsp->cur_inst >> 8) & BITMASK(6);
    uint32_t d = (dsp->cur_inst >> 3) & 1;
    emu_and_x(dsp, xx, d);
}

static void emu_and_long(dsp_core_t* dsp)
{
    uint32_t xxxx = read_memory_p(dsp, dsp->pc+1);
    dsp->cur_inst_len++;
    uint32_t d = (dsp->cur_inst >> 3) & 1;
    emu_and_x(dsp, xxxx, d);
}

static void emu_andi(dsp_core_t* dsp)
{
    uint32_t regnum, value;

    value = (dsp->cur_inst >> 8) & BITMASK(8);
    regnum = dsp->cur_inst & BITMASK(2);
    switch(regnum) {
        case 0:
            /* mr */
            dsp->registers[DSP_REG_SR] &= (value<<8)|BITMASK(8);
            break;
        case 1:
            /* ccr */
            dsp->registers[DSP_REG_SR] &= (BITMASK(8)<<8)|value;
            break;
        case 2:
            /* omr */
            dsp->registers[DSP_REG_OMR] &= value;
            break;
    }
}

static void emu_asl_imm(dsp_core_t* dsp)
{
    uint32_t S = ((dsp->cur_inst >> 7) & 1);
    uint32_t D = dsp->cur_inst & 1;
    uint32_t ii = (dsp->cur_inst >> 1) & BITMASK(6);

    uint32_t dest[3];

    if (S) {
        dest[0] = dsp->registers[DSP_REG_B2];
        dest[1] = dsp->registers[DSP_REG_B1];
        dest[2] = dsp->registers[DSP_REG_B0];
    } else {
        dest[0] = dsp->registers[DSP_REG_A2];
        dest[1] = dsp->registers[DSP_REG_A1];
        dest[2] = dsp->registers[DSP_REG_A0];
    }

    uint16_t newsr = dsp_asl56(dest, ii);

    if (D) {
        dsp->registers[DSP_REG_B2] = dest[0];
        dsp->registers[DSP_REG_B1] = dest[1];
        dsp->registers[DSP_REG_B0] = dest[2];
    } else {
        dsp->registers[DSP_REG_A2] = dest[0];
        dsp->registers[DSP_REG_A1] = dest[1];
        dsp->registers[DSP_REG_A0] = dest[2];
    }

    dsp->registers[DSP_REG_SR] &= BITMASK(16)-((1<<DSP_SR_C)|(1<<DSP_SR_V));
    dsp->registers[DSP_REG_SR] |= newsr;

    emu_ccr_update_e_u_n_z(dsp, dest[0], dest[1], dest[2]);
    emu_sm_acc(dsp, D, 0); /* SM */
}

static void emu_asr_imm(dsp_core_t* dsp)
{
    uint32_t S = ((dsp->cur_inst >> 7) & 1);
    uint32_t D = dsp->cur_inst & 1;
    uint32_t ii = (dsp->cur_inst >> 1) & BITMASK(6);

    uint32_t dest[3];
    if (S) {
        dest[0] = dsp->registers[DSP_REG_B2];
        dest[1] = dsp->registers[DSP_REG_B1];
        dest[2] = dsp->registers[DSP_REG_B0];
    } else {
        dest[0] = dsp->registers[DSP_REG_A2];
        dest[1] = dsp->registers[DSP_REG_A1];
        dest[2] = dsp->registers[DSP_REG_A0];
    }

    uint16_t newsr = dsp_asr56(dest, ii);

    if (D) {
        dsp->registers[DSP_REG_B2] = dest[0];
        dsp->registers[DSP_REG_B1] = dest[1];
        dsp->registers[DSP_REG_B0] = dest[2];
    } else {
        dsp->registers[DSP_REG_A2] = dest[0];
        dsp->registers[DSP_REG_A1] = dest[1];
        dsp->registers[DSP_REG_A0] = dest[2];
    }

    dsp->registers[DSP_REG_SR] &= BITMASK(16)-((1<<DSP_SR_C)|(1<<DSP_SR_V));
    dsp->registers[DSP_REG_SR] |= newsr;

    emu_ccr_update_e_u_n_z(dsp, dest[0], dest[1], dest[2]);
    emu_sm_acc(dsp, D, 0); /* SM */
}

static void emu_bcc_long(dsp_core_t* dsp)
{
    uint32_t xxxx = read_memory_p(dsp, dsp->pc+1);
    dsp->cur_inst_len++;

    uint32_t cc_code = dsp->cur_inst & BITMASK(4);
    if (emu_calc_cc(dsp, cc_code)) {
        dsp->pc += xxxx;
        dsp->pc &= BITMASK(24);
        dsp->cur_inst_len = 0;
    }

    //TODO: cycles?
}

static void emu_bcc_imm(dsp_core_t* dsp)
{
    uint32_t xxx = (dsp->cur_inst & BITMASK(5))
                    + ((dsp->cur_inst & (BITMASK(4) << 6)) >> 1);

    uint32_t cc_code = (dsp->cur_inst >> 12) & BITMASK(4);

    if (emu_calc_cc(dsp, cc_code)) {
        dsp->pc += dsp_signextend(9, xxx);
        dsp->pc &= BITMASK(24);
        dsp->cur_inst_len = 0;
    }

    //TODO: cycles
}

static void emu_bchg_aa(dsp_core_t* dsp)
{
    uint32_t memspace, addr, value, newcarry, numbit;

    memspace = (dsp->cur_inst>>6) & 1;
    value = (dsp->cur_inst>>8) & BITMASK(6);
    numbit = dsp->cur_inst & BITMASK(5);

    addr = value;
    value = dsp56k_read_memory(dsp, memspace, addr);
    newcarry = (value>>numbit) & 1;
    if (newcarry) {
        value -= (1<<numbit);
    } else {
        value += (1<<numbit);
    }
    dsp56k_write_memory(dsp, memspace, addr, value);

    /* Set carry */
    dsp->registers[DSP_REG_SR] &= BITMASK(16)-(1<<DSP_SR_C);
    dsp->registers[DSP_REG_SR] |= newcarry<<DSP_SR_C;

    dsp->instr_cycle += 2;
}

static void emu_bchg_ea(dsp_core_t* dsp)
{
    uint32_t memspace, addr, value, newcarry, numbit;

    memspace = (dsp->cur_inst>>6) & 1;
    value = (dsp->cur_inst>>8) & BITMASK(6);
    numbit = dsp->cur_inst & BITMASK(5);

    emu_calc_ea(dsp, value, &addr);
    value = dsp56k_read_memory(dsp, memspace, addr);
    newcarry = (value>>numbit) & 1;
    if (newcarry) {
        value -= (1<<numbit);
    } else {
        value += (1<<numbit);
    }
    dsp56k_write_memory(dsp, memspace, addr, value);

    /* Set carry */
    dsp->registers[DSP_REG_SR] &= BITMASK(16)-(1<<DSP_SR_C);
    dsp->registers[DSP_REG_SR] |= newcarry<<DSP_SR_C;

    dsp->instr_cycle += 2;
}

static void emu_bchg_pp(dsp_core_t* dsp)
{
    uint32_t memspace, addr, value, newcarry, numbit;

    memspace = (dsp->cur_inst>>6) & 1;
    value = (dsp->cur_inst>>8) & BITMASK(6);
    numbit = dsp->cur_inst & BITMASK(5);

    addr = 0xffffc0 + value;
    value = dsp56k_read_memory(dsp, memspace, addr);
    newcarry = (value>>numbit) & 1;
    if (newcarry) {
        value -= (1<<numbit);
    } else {
        value += (1<<numbit);
    }
    dsp56k_write_memory(dsp, memspace, addr, value);

    /* Set carry */
    dsp->registers[DSP_REG_SR] &= BITMASK(16)-(1<<DSP_SR_C);
    dsp->registers[DSP_REG_SR] |= newcarry<<DSP_SR_C;

    dsp->instr_cycle += 2;
}

static void emu_bchg_reg(dsp_core_t* dsp)
{
    uint32_t value, numreg, newcarry, numbit;

    numreg = (dsp->cur_inst>>8) & BITMASK(6);
    numbit = dsp->cur_inst & BITMASK(5);

    if ((numreg==DSP_REG_A) || (numreg==DSP_REG_B)) {
        emu_pm_read_accu24(dsp, numreg, &value);
    } else {
        value = dsp->registers[numreg];
    }

    newcarry = (value>>numbit) & 1;
    if (newcarry) {
        value -= (1<<numbit);
    } else {
        value += (1<<numbit);
    }

    dsp_write_reg(dsp, numreg, value);

    /* Set carry (xbased: not for SR itself, A-6.11/12/19: C is then only
     * the bit changed when it's bit 0) */
    if (numreg != DSP_REG_SR) {
        dsp->registers[DSP_REG_SR] &= BITMASK(16)-(1<<DSP_SR_C);
        dsp->registers[DSP_REG_SR] |= newcarry<<DSP_SR_C;
    }

    dsp->instr_cycle += 2;
}

static void emu_bclr_aa(dsp_core_t* dsp)
{
    uint32_t memspace, addr, value, newcarry, numbit;

    memspace = (dsp->cur_inst>>6) & 1;
    addr = (dsp->cur_inst>>8) & BITMASK(6);
    numbit = dsp->cur_inst & BITMASK(5);

    value = dsp56k_read_memory(dsp, memspace, addr);
    newcarry = (value>>numbit) & 1;
    value &= 0xffffffff-(1<<numbit);
    dsp56k_write_memory(dsp, memspace, addr, value);

    /* Set carry */
    dsp->registers[DSP_REG_SR] &= BITMASK(16)-(1<<DSP_SR_C);
    dsp->registers[DSP_REG_SR] |= newcarry<<DSP_SR_C;

    dsp->instr_cycle += 2;
}

static void emu_bclr_ea(dsp_core_t* dsp)
{
    uint32_t memspace, addr, value, newcarry, numbit;

    memspace = (dsp->cur_inst>>6) & 1;
    value = (dsp->cur_inst>>8) & BITMASK(6);
    numbit = dsp->cur_inst & BITMASK(5);

    emu_calc_ea(dsp, value, &addr);
    value = dsp56k_read_memory(dsp, memspace, addr);
    newcarry = (value>>numbit) & 1;
    value &= 0xffffffff-(1<<numbit);
    dsp56k_write_memory(dsp, memspace, addr, value);

    /* Set carry */
    dsp->registers[DSP_REG_SR] &= BITMASK(16)-(1<<DSP_SR_C);
    dsp->registers[DSP_REG_SR] |= newcarry<<DSP_SR_C;

    dsp->instr_cycle += 2;
}

static void emu_bclr_pp(dsp_core_t* dsp)
{
    uint32_t memspace, addr, value, newcarry, numbit;

    memspace = (dsp->cur_inst>>6) & 1;
    value = (dsp->cur_inst>>8) & BITMASK(6);
    numbit = dsp->cur_inst & BITMASK(5);

    addr = 0xffffc0 + value;
    value = dsp56k_read_memory(dsp, memspace, addr);
    newcarry = (value>>numbit) & 1;
    value &= 0xffffffff-(1<<numbit);
    dsp56k_write_memory(dsp, memspace, addr, value);

    /* Set carry */
    dsp->registers[DSP_REG_SR] &= BITMASK(16)-(1<<DSP_SR_C);
    dsp->registers[DSP_REG_SR] |= newcarry<<DSP_SR_C;

    dsp->instr_cycle += 2;
}

static void emu_bclr_reg(dsp_core_t* dsp)
{
    uint32_t value, numreg, newcarry, numbit;

    numreg = (dsp->cur_inst>>8) & BITMASK(6);
    numbit = dsp->cur_inst & BITMASK(5);

    if ((numreg==DSP_REG_A) || (numreg==DSP_REG_B)) {
        emu_pm_read_accu24(dsp, numreg, &value);
    } else {
        value = dsp->registers[numreg];
    }

    newcarry = (value>>numbit) & 1;
    value &= 0xffffffff-(1<<numbit);

    dsp_write_reg(dsp, numreg, value);

    /* Set carry (xbased: not for SR itself, A-6.11/12/19: C is then only
     * the bit changed when it's bit 0) */
    if (numreg != DSP_REG_SR) {
        dsp->registers[DSP_REG_SR] &= BITMASK(16)-(1<<DSP_SR_C);
        dsp->registers[DSP_REG_SR] |= newcarry<<DSP_SR_C;
    }

    dsp->instr_cycle += 2;
}

static void emu_bra_long(dsp_core_t* dsp)
{
    uint32_t xxxx = read_memory_p(dsp, dsp->pc+1);
    dsp->cur_inst_len++;

    dsp->pc += xxxx;
    dsp->pc &= BITMASK(24);
    dsp->cur_inst_len = 0;
}

static void emu_bra_imm(dsp_core_t* dsp)
{
    uint32_t xxx = (dsp->cur_inst & BITMASK(5))
                    + ((dsp->cur_inst & (BITMASK(4) << 6)) >> 1);

    dsp->pc += dsp_signextend(9, xxx);
    dsp->pc &= BITMASK(24);
    dsp->cur_inst_len = 0;
}

static void emu_brclr_pp(dsp_core_t* dsp)
{
    uint32_t xxxx = read_memory_p(dsp, dsp->pc+1);
    dsp->cur_inst_len++;

    uint32_t memspace = (dsp->cur_inst>>6) & 1;
    uint32_t value = (dsp->cur_inst>>8) & BITMASK(6);
    uint32_t numbit = dsp->cur_inst & BITMASK(5);
    uint32_t addr = 0xffffc0 + value;
    value = dsp56k_read_memory(dsp, memspace, addr);

    dsp->instr_cycle += 4;

   if ((value & (1<<numbit))==0) {
        dsp->pc += xxxx;
        dsp->pc &= BITMASK(24);
        dsp->cur_inst_len = 0;
    }
}

static void emu_brclr_reg(dsp_core_t* dsp)
{
    uint32_t xxxx = read_memory_p(dsp, dsp->pc+1);
    dsp->cur_inst_len++;

    uint32_t numreg = (dsp->cur_inst>>8) & BITMASK(6);
    uint32_t numbit = dsp->cur_inst & BITMASK(5);

    uint32_t value;
    if ((numreg==DSP_REG_A) || (numreg==DSP_REG_B)) {
        emu_pm_read_accu24(dsp, numreg, &value);
    } else {
        value = dsp->registers[numreg];
    }

    dsp->instr_cycle += 4;

    if ((value & (1<<numbit)) == 0) {
        dsp->pc += xxxx;
        dsp->pc &= BITMASK(24);
        dsp->cur_inst_len=0;
    }
}

static void emu_brset_pp(dsp_core_t* dsp)
{
    uint32_t xxxx = read_memory_p(dsp, dsp->pc+1);
    dsp->cur_inst_len++;

    uint32_t memspace = (dsp->cur_inst>>6) & 1;
    uint32_t value = (dsp->cur_inst>>8) & BITMASK(6);
    uint32_t numbit = dsp->cur_inst & BITMASK(5);
    uint32_t addr = 0xffffc0 + value;
    value = dsp56k_read_memory(dsp, memspace, addr);

    dsp->instr_cycle += 4;

    if (value & (1<<numbit)) {
        dsp->pc += xxxx;
        dsp->pc &= BITMASK(24);
        dsp->cur_inst_len = 0;
    }
}

static void emu_brset_reg(dsp_core_t* dsp)
{
    uint32_t xxxx = read_memory_p(dsp, dsp->pc+1);
    dsp->cur_inst_len++;

    uint32_t numreg = (dsp->cur_inst>>8) & BITMASK(6);
    uint32_t numbit = dsp->cur_inst & BITMASK(5);

    uint32_t value;
    if ((numreg==DSP_REG_A) || (numreg==DSP_REG_B)) {
        emu_pm_read_accu24(dsp, numreg, &value);
    } else {
        value = dsp->registers[numreg];
    }

    dsp->instr_cycle += 4;

    if (value & (1<<numbit)) {
        dsp->pc += xxxx;
        dsp->pc &= BITMASK(24);
        dsp->cur_inst_len=0;
    }
}

static void emu_bset_aa(dsp_core_t* dsp)
{
    uint32_t memspace, addr, value, newcarry, numbit;

    memspace = (dsp->cur_inst>>6) & 1;
    value = (dsp->cur_inst>>8) & BITMASK(6);
    numbit = dsp->cur_inst & BITMASK(5);

    addr = value;
    value = dsp56k_read_memory(dsp, memspace, addr);
    newcarry = (value>>numbit) & 1;
    value |= (1<<numbit);
    dsp56k_write_memory(dsp, memspace, addr, value);

    /* Set carry */
    dsp->registers[DSP_REG_SR] &= BITMASK(16)-(1<<DSP_SR_C);
    dsp->registers[DSP_REG_SR] |= newcarry<<DSP_SR_C;

    dsp->instr_cycle += 2;
}

static void emu_bsr_long(dsp_core_t* dsp)
{
    uint32_t xxxx = read_memory_p(dsp, dsp->pc+1);
    dsp->cur_inst_len++;

    if (dsp->interrupt_state != DSP_INTERRUPT_LONG){
        dsp_stack_push(dsp, dsp->pc+dsp->cur_inst_len, dsp->registers[DSP_REG_SR], 0);
    } else {
        dsp->interrupt_state = DSP_INTERRUPT_DISABLED;
    }

    dsp->pc += xxxx;
    dsp->pc &= BITMASK(24);
    dsp->cur_inst_len = 0;

    dsp->instr_cycle += 4;
}

static void emu_bsr_imm(dsp_core_t* dsp)
{
    uint32_t xxx = (dsp->cur_inst & BITMASK(5))
                 + ((dsp->cur_inst & (BITMASK(4) << 6)) >> 1);

    if (dsp->interrupt_state != DSP_INTERRUPT_LONG){
        dsp_stack_push(dsp, dsp->pc+dsp->cur_inst_len, dsp->registers[DSP_REG_SR], 0);
    } else {
        dsp->interrupt_state = DSP_INTERRUPT_DISABLED;
    }

    dsp->pc += dsp_signextend(9, xxx);
    dsp->pc &= BITMASK(24);
    dsp->cur_inst_len = 0;

    dsp->instr_cycle += 2;
}

static void emu_bset_ea(dsp_core_t* dsp)
{
    uint32_t memspace, addr, value, newcarry, numbit;

    memspace = (dsp->cur_inst>>6) & 1;
    value = (dsp->cur_inst>>8) & BITMASK(6);
    numbit = dsp->cur_inst & BITMASK(5);

    emu_calc_ea(dsp, value, &addr);
    value = dsp56k_read_memory(dsp, memspace, addr);
    newcarry = (value>>numbit) & 1;
    value |= (1<<numbit);
    dsp56k_write_memory(dsp, memspace, addr, value);

    /* Set carry */
    dsp->registers[DSP_REG_SR] &= BITMASK(16)-(1<<DSP_SR_C);
    dsp->registers[DSP_REG_SR] |= newcarry<<DSP_SR_C;

    dsp->instr_cycle += 2;
}

static void emu_bset_pp(dsp_core_t* dsp)
{
    uint32_t memspace, addr, value, newcarry, numbit;

    memspace = (dsp->cur_inst>>6) & 1;
    value = (dsp->cur_inst>>8) & BITMASK(6);
    numbit = dsp->cur_inst & BITMASK(5);
    addr = 0xffffc0 + value;
    value = dsp56k_read_memory(dsp, memspace, addr);
    newcarry = (value>>numbit) & 1;
    value |= (1<<numbit);
    dsp56k_write_memory(dsp, memspace, addr, value);

    /* Set carry */
    dsp->registers[DSP_REG_SR] &= BITMASK(16)-(1<<DSP_SR_C);
    dsp->registers[DSP_REG_SR] |= newcarry<<DSP_SR_C;

    dsp->instr_cycle += 2;
}

static void emu_bset_reg(dsp_core_t* dsp)
{
    uint32_t value, numreg, newcarry, numbit;

    numreg = (dsp->cur_inst>>8) & BITMASK(6);
    numbit = dsp->cur_inst & BITMASK(5);

    if ((numreg==DSP_REG_A) || (numreg==DSP_REG_B)) {
        emu_pm_read_accu24(dsp, numreg, &value);
    } else {
        value = dsp->registers[numreg];
    }

    newcarry = (value>>numbit) & 1;
    value |= (1<<numbit);

    dsp_write_reg(dsp, numreg, value);

    /* Set carry (xbased: not for SR itself, A-6.11/12/19: C is then only
     * the bit changed when it's bit 0) */
    if (numreg != DSP_REG_SR) {
        dsp->registers[DSP_REG_SR] &= BITMASK(16)-(1<<DSP_SR_C);
        dsp->registers[DSP_REG_SR] |= newcarry<<DSP_SR_C;
    }

    dsp->instr_cycle += 2;
}

static void emu_btst_aa(dsp_core_t* dsp)
{
    uint32_t memspace, addr, value, newcarry, numbit;

    memspace = (dsp->cur_inst>>6) & 1;
    value = (dsp->cur_inst>>8) & BITMASK(6);
    numbit = dsp->cur_inst & BITMASK(5);

    addr = value;
    value = dsp56k_read_memory(dsp, memspace, addr);
    newcarry = (value>>numbit) & 1;

    /* Set carry */
    dsp->registers[DSP_REG_SR] &= BITMASK(16)-(1<<DSP_SR_C);
    dsp->registers[DSP_REG_SR] |= newcarry<<DSP_SR_C;

    dsp->instr_cycle += 2;
}

static void emu_btst_ea(dsp_core_t* dsp)
{
    uint32_t memspace, addr, value, newcarry, numbit;

    memspace = (dsp->cur_inst>>6) & 1;
    value = (dsp->cur_inst>>8) & BITMASK(6);
    numbit = dsp->cur_inst & BITMASK(5);

    emu_calc_ea(dsp, value, &addr);
    value = dsp56k_read_memory(dsp, memspace, addr);
    newcarry = (value>>numbit) & 1;

    /* Set carry */
    dsp->registers[DSP_REG_SR] &= BITMASK(16)-(1<<DSP_SR_C);
    dsp->registers[DSP_REG_SR] |= newcarry<<DSP_SR_C;

    dsp->instr_cycle += 2;
}

static void emu_btst_pp(dsp_core_t* dsp)
{
    uint32_t memspace, addr, value, newcarry, numbit;

    memspace = (dsp->cur_inst>>6) & 1;
    value = (dsp->cur_inst>>8) & BITMASK(6);
    numbit = dsp->cur_inst & BITMASK(5);

    addr = 0xffffc0 + value;
    value = dsp56k_read_memory(dsp, memspace, addr);
    newcarry = (value>>numbit) & 1;

    /* Set carry */
    dsp->registers[DSP_REG_SR] &= BITMASK(16)-(1<<DSP_SR_C);
    dsp->registers[DSP_REG_SR] |= newcarry<<DSP_SR_C;

    dsp->instr_cycle += 2;
}

static void emu_btst_reg(dsp_core_t* dsp)
{
    uint32_t value, numreg, newcarry, numbit;

    numreg = (dsp->cur_inst>>8) & BITMASK(6);
    numbit = dsp->cur_inst & BITMASK(5);

    if ((numreg==DSP_REG_A) || (numreg==DSP_REG_B)) {
        emu_pm_read_accu24(dsp, numreg, &value);
    } else {
        value = dsp->registers[numreg];
    }

    newcarry = (value>>numbit) & 1;

    /* Set carry */
    dsp->registers[DSP_REG_SR] &= BITMASK(16)-(1<<DSP_SR_C);
    dsp->registers[DSP_REG_SR] |= newcarry<<DSP_SR_C;

    dsp->instr_cycle += 2;
}

static void emu_cmp_imm(dsp_core_t* dsp)
{
    uint32_t xx = (dsp->cur_inst >> 8) & BITMASK(6);
    uint32_t d = (dsp->cur_inst >> 3) & 1;

    uint32_t source[3], dest[3];

    if (d) {
        dest[2] = dsp->registers[DSP_REG_B0];
        dest[1] = dsp->registers[DSP_REG_B1];
        dest[0] = dsp->registers[DSP_REG_B2];
    } else {
        dest[2] = dsp->registers[DSP_REG_A0];
        dest[1] = dsp->registers[DSP_REG_A1];
        dest[0] = dsp->registers[DSP_REG_A2];
    }

    source[2] = 0;
    source[1] = xx;
    source[0] = 0x0;

    uint16_t newsr = dsp_sub56(source, dest);
    newsr |= emu_sm_sat3(dsp, dest, 0); /* SM */

    emu_ccr_update_e_u_n_z(dsp, dest[0], dest[1], dest[2]);

    dsp->registers[DSP_REG_SR] &= BITMASK(16)-((1<<DSP_SR_V)|(1<<DSP_SR_C));
    dsp->registers[DSP_REG_SR] |= newsr;
}

static void emu_cmp_long(dsp_core_t* dsp)
{
    uint32_t xxxx = read_memory_p(dsp, dsp->pc+1);
    dsp->cur_inst_len++;

    uint32_t d = (dsp->cur_inst >> 3) & 1;

    uint32_t source[3], dest[3];
    if (d) {
        dest[2] = dsp->registers[DSP_REG_B0];
        dest[1] = dsp->registers[DSP_REG_B1];
        dest[0] = dsp->registers[DSP_REG_B2];
    } else {
        dest[2] = dsp->registers[DSP_REG_A0];
        dest[1] = dsp->registers[DSP_REG_A1];
        dest[0] = dsp->registers[DSP_REG_A2];
    }

    source[2] = 0;
    source[1] = xxxx;
    source[0] = source[1] & (1<<23) ? 0xff : 0x0; /* xbased: sign-extended, the manual's */

    uint16_t newsr = dsp_sub56(source, dest);
    newsr |= emu_sm_sat3(dsp, dest, 0); /* SM */

    emu_ccr_update_e_u_n_z(dsp, dest[0], dest[1], dest[2]);

    dsp->registers[DSP_REG_SR] &= BITMASK(16)-((1<<DSP_SR_V)|(1<<DSP_SR_C));
    dsp->registers[DSP_REG_SR] |= newsr;
}

static void emu_cmpu(dsp_core_t* dsp)
{
    /* xbased: the manual's CMPU: S2[47:0] - S1 unsigned, 48 bits (a word
     * left-aligned, zero-filled; an accumulator's 48 bits, its EXP left
     * out); Z on bits 47-0, N bit 47, V cleared, C the borrow (it did a
     * signed 56-bit subtraction of a limited word) */
    uint32_t ggg = (dsp->cur_inst >> 1) & BITMASK(3);
    uint32_t d = dsp->cur_inst & 1;
    uint64_t s1;

    switch (ggg) {
    case 0:
        s1 = ((uint64_t)dsp->registers[DSP_REG_A1+(d^1)] << 24) | dsp->registers[DSP_REG_A0+(d^1)];
        break;
    case 4: s1 = (uint64_t)dsp->registers[DSP_REG_X0] << 24; break;
    case 5: s1 = (uint64_t)dsp->registers[DSP_REG_Y0] << 24; break;
    case 6: s1 = (uint64_t)dsp->registers[DSP_REG_X1] << 24; break;
    case 7: s1 = (uint64_t)dsp->registers[DSP_REG_Y1] << 24; break;
    default: s1 = 0; break;
    }
    s1 &= (1ULL << 48) - 1;
    const uint64_t s2 = (((uint64_t)dsp->registers[DSP_REG_A1+d] << 24) | dsp->registers[DSP_REG_A0+d])
        & ((1ULL << 48) - 1);
    const uint64_t r = (s2 - s1) & ((1ULL << 48) - 1);

    dsp->registers[DSP_REG_SR] &= BITMASK(16)-(
        (1<<DSP_SR_V)|(1<<DSP_SR_C)|(1<<DSP_SR_Z)|(1<<DSP_SR_N));
    dsp->registers[DSP_REG_SR] |= (s2 < s1) << DSP_SR_C;
    dsp->registers[DSP_REG_SR] |= (r == 0) << DSP_SR_Z;
    dsp->registers[DSP_REG_SR] |= ((r >> 47) & 1) << DSP_SR_N;
}

static void emu_dec(dsp_core_t* dsp)
{
    uint32_t destreg, source[3], dest[3];
    uint16_t newsr;

    destreg = DSP_REG_A + (dsp->cur_inst & 1);
    if (destreg == DSP_REG_A) {
        dest[0] = dsp->registers[DSP_REG_A2];
        dest[1] = dsp->registers[DSP_REG_A1];
        dest[2] = dsp->registers[DSP_REG_A0];
    } else {
        dest[0] = dsp->registers[DSP_REG_B2];
        dest[1] = dsp->registers[DSP_REG_B1];
        dest[2] = dsp->registers[DSP_REG_B0];
    }

    source[2] = 1;
    source[1] = 0;
    source[0] = 0;

    newsr = dsp_sub56(source, dest);

    if (destreg == DSP_REG_A) {
        dsp->registers[DSP_REG_A2] = dest[0];
        dsp->registers[DSP_REG_A1] = dest[1];
        dsp->registers[DSP_REG_A0] = dest[2];
    } else {
        dsp->registers[DSP_REG_B2] = dest[0];
        dsp->registers[DSP_REG_B1] = dest[1];
        dsp->registers[DSP_REG_B0] = dest[2];
    }

    emu_ccr_update_e_u_n_z(dsp, dest[0], dest[1], dest[2]);

    dsp->registers[DSP_REG_SR] &= BITMASK(16)-((1<<DSP_SR_V)|(1<<DSP_SR_C));
    dsp->registers[DSP_REG_SR] |= newsr;
    emu_sm_acc(dsp, destreg - DSP_REG_A, 0); /* SM */
}

static void emu_div(dsp_core_t* dsp)
{
    uint32_t srcreg, destreg, source[3], dest[3];
    uint16_t newsr;

    srcreg = DSP_REG_NULL;
    switch((dsp->cur_inst>>4) & BITMASK(2)) {
    case 0: srcreg = DSP_REG_X0; break;
    case 1: srcreg = DSP_REG_Y0; break;
    case 2: srcreg = DSP_REG_X1; break;
    case 3: srcreg = DSP_REG_Y1; break;
    }
    source[2] = 0;
    source[1] = dsp->registers[srcreg];
    source[0] = source[1] & (1<<23) ? 0xff : 0x0;

    destreg = DSP_REG_A + ((dsp->cur_inst>>3) & 1);
    if (destreg == DSP_REG_A) {
        dest[0] = dsp->registers[DSP_REG_A2];
        dest[1] = dsp->registers[DSP_REG_A1];
        dest[2] = dsp->registers[DSP_REG_A0];
    } else {
        dest[0] = dsp->registers[DSP_REG_B2];
        dest[1] = dsp->registers[DSP_REG_B1];
        dest[2] = dsp->registers[DSP_REG_B0];
    }

    if (((dest[0]>>7) & 1) ^ ((source[1]>>23) & 1)) {
        /* D += S */
        newsr = dsp_asl56(dest, 1);
        dsp_add56(source, dest);
    } else {
        /* D -= S */
        newsr = dsp_asl56(dest, 1);
        dsp_sub56(source, dest);
    }

    dest[2] |= (dsp->registers[DSP_REG_SR]>>DSP_SR_C) & 1;

    if (destreg == DSP_REG_A) {
        dsp->registers[DSP_REG_A2] = dest[0];
        dsp->registers[DSP_REG_A1] = dest[1];
        dsp->registers[DSP_REG_A0] = dest[2];
    } else {
        dsp->registers[DSP_REG_B2] = dest[0];
        dsp->registers[DSP_REG_B1] = dest[1];
        dsp->registers[DSP_REG_B0] = dest[2];
    }

    dsp->registers[DSP_REG_SR] &= BITMASK(16)-((1<<DSP_SR_C)|(1<<DSP_SR_V));
    dsp->registers[DSP_REG_SR] |= (1-((dest[0]>>7) & 1))<<DSP_SR_C;
    dsp->registers[DSP_REG_SR] |= newsr & (1<<DSP_SR_L);
    dsp->registers[DSP_REG_SR] |= newsr & (1<<DSP_SR_V);
}

/*
    DO instruction parameter encoding

    xxxxxxxx 00xxxxxx 0xxxxxxx  aa
    xxxxxxxx 01xxxxxx 0xxxxxxx  ea
    xxxxxxxx YYxxxxxx 1xxxxxxx  imm
    xxxxxxxx 11xxxxxx 0xxxxxxx  reg
*/

/* xbased: a DO / DOR whose count is 0 isn't run with SC clear (the core has
 * no SC: clear): the manual's DO and DOR. The loop set up and ended at once
 * (LF, LA and LC back), pc past its last word. */
static void emu_do_skip_if_zero(dsp_core_t* dsp)
{
    uint32_t saved_pc, saved_sr, la;

    if (dsp->registers[DSP_REG_LC] != 0) {
        return;
    }
    la = dsp->registers[DSP_REG_LA];
    dsp_stack_pop(dsp, &saved_pc, &saved_sr);
    dsp->registers[DSP_REG_SR] &= ~(1u<<DSP_SR_LF);
    dsp->registers[DSP_REG_SR] |= saved_sr & (1<<DSP_SR_LF);
    dsp_stack_pop(dsp, &dsp->registers[DSP_REG_LA], &dsp->registers[DSP_REG_LC]);
    dsp->pc = la + 1;
    dsp->cur_inst_len = 0;
}

static void emu_do_aa(dsp_core_t* dsp)
{
    uint32_t memspace, addr;

    /* x:aa */
    /* y:aa */

    dsp_stack_push(dsp, dsp->registers[DSP_REG_LA], dsp->registers[DSP_REG_LC], 0);
    dsp->registers[DSP_REG_LA] = read_memory_p(dsp, dsp->pc+1) & BITMASK(16);
    dsp->cur_inst_len++;
    dsp_stack_push(dsp, dsp->pc+dsp->cur_inst_len, dsp->registers[DSP_REG_SR], 0);
    dsp->registers[DSP_REG_SR] |= (1<<DSP_SR_LF);

    memspace = (dsp->cur_inst>>6) & 1;
    addr = (dsp->cur_inst>>8) & BITMASK(6);
    dsp->registers[DSP_REG_LC] = dsp56k_read_memory(dsp, memspace, addr) & BITMASK(16);

    dsp->instr_cycle += 4;
    emu_do_skip_if_zero(dsp);
}

static void emu_do_imm(dsp_core_t* dsp)
{
    /* #xx */

    dsp_stack_push(dsp, dsp->registers[DSP_REG_LA], dsp->registers[DSP_REG_LC], 0);
    dsp->registers[DSP_REG_LA] = read_memory_p(dsp, dsp->pc+1) & BITMASK(16);
    dsp->cur_inst_len++;
    dsp_stack_push(dsp, dsp->pc+dsp->cur_inst_len, dsp->registers[DSP_REG_SR], 0);
    dsp->registers[DSP_REG_SR] |= (1<<DSP_SR_LF);

    dsp->registers[DSP_REG_LC] = ((dsp->cur_inst>>8) & BITMASK(8))
        + ((dsp->cur_inst & BITMASK(4))<<8);

    dsp->instr_cycle += 4;
    emu_do_skip_if_zero(dsp);
}

static void emu_do_ea(dsp_core_t* dsp)
{
    uint32_t memspace, ea_mode, addr;

    /* x:ea */
    /* y:ea */

    dsp_stack_push(dsp, dsp->registers[DSP_REG_LA], dsp->registers[DSP_REG_LC], 0);
    dsp->registers[DSP_REG_LA] = read_memory_p(dsp, dsp->pc+1) & BITMASK(16);
    dsp->cur_inst_len++;
    dsp_stack_push(dsp, dsp->pc+dsp->cur_inst_len, dsp->registers[DSP_REG_SR], 0);
    dsp->registers[DSP_REG_SR] |= (1<<DSP_SR_LF);

    memspace = (dsp->cur_inst>>6) & 1;
    ea_mode = (dsp->cur_inst>>8) & BITMASK(6);
    emu_calc_ea(dsp, ea_mode, &addr);
    dsp->registers[DSP_REG_LC] = dsp56k_read_memory(dsp, memspace, addr) & BITMASK(16);

    dsp->instr_cycle += 4;
    emu_do_skip_if_zero(dsp);
}

static void emu_do_reg(dsp_core_t* dsp)
{
    uint32_t numreg;

    /* S */

    dsp_stack_push(dsp, dsp->registers[DSP_REG_LA], dsp->registers[DSP_REG_LC], 0);
    dsp->registers[DSP_REG_LA] = read_memory_p(dsp, dsp->pc+1) & BITMASK(16);
    dsp->cur_inst_len++;

    numreg = (dsp->cur_inst>>8) & BITMASK(6);
    if ((numreg == DSP_REG_A) || (numreg == DSP_REG_B)) {
        emu_pm_read_accu24(dsp, numreg, &dsp->registers[DSP_REG_LC]);
    } else {
        dsp->registers[DSP_REG_LC] = dsp->registers[numreg];
    }
    dsp->registers[DSP_REG_LC] &= BITMASK(16);

    dsp_stack_push(dsp, dsp->pc+dsp->cur_inst_len, dsp->registers[DSP_REG_SR], 0);
    dsp->registers[DSP_REG_SR] |= (1<<DSP_SR_LF);

    dsp->instr_cycle += 4;
    emu_do_skip_if_zero(dsp);
}

static void emu_dor_imm(dsp_core_t* dsp)
{
    uint32_t xxxx = read_memory_p(dsp, dsp->pc+1);
    dsp->cur_inst_len++;

    dsp_stack_push(dsp, dsp->registers[DSP_REG_LA], dsp->registers[DSP_REG_LC], 0);
    dsp->registers[DSP_REG_LA] = (dsp->pc + xxxx) & BITMASK(16);

    dsp_stack_push(dsp, dsp->pc+dsp->cur_inst_len, dsp->registers[DSP_REG_SR], 0);
    dsp->registers[DSP_REG_SR] |= (1<<DSP_SR_LF);

    dsp->registers[DSP_REG_LC] = ((dsp->cur_inst>>8) & BITMASK(8))
        + ((dsp->cur_inst & BITMASK(4))<<8);

    dsp->instr_cycle += 4;
    emu_do_skip_if_zero(dsp);
}

static void emu_dor_reg(dsp_core_t* dsp)
{
    uint32_t xxxx = read_memory_p(dsp, dsp->pc+1);
    dsp->cur_inst_len++;

    dsp_stack_push(dsp, dsp->registers[DSP_REG_LA], dsp->registers[DSP_REG_LC], 0);
    dsp->registers[DSP_REG_LA] = (dsp->pc + xxxx) & BITMASK(16);

    dsp_stack_push(dsp, dsp->pc+dsp->cur_inst_len, dsp->registers[DSP_REG_SR], 0);
    dsp->registers[DSP_REG_SR] |= (1<<DSP_SR_LF);

    uint32_t numreg = (dsp->cur_inst>>8) & BITMASK(6);
    if ((numreg == DSP_REG_A) || (numreg == DSP_REG_B)) {
        emu_pm_read_accu24(dsp, numreg, &dsp->registers[DSP_REG_LC]);
    } else {
        dsp->registers[DSP_REG_LC] = dsp->registers[numreg];
    }
    dsp->registers[DSP_REG_LC] &= BITMASK(16);

    dsp->instr_cycle += 4;
    emu_do_skip_if_zero(dsp);
}

static void emu_enddo(dsp_core_t* dsp)
{
    uint32_t saved_pc, saved_sr;

    dsp_stack_pop(dsp, &saved_pc, &saved_sr);
    /* xbased: LF alone comes back (the manual's end of loop); &= 0x7f
     * also cleared the scaling, interrupt mask and trace bits */
    dsp->registers[DSP_REG_SR] &= ~(1u<<DSP_SR_LF);
    dsp->registers[DSP_REG_SR] |= saved_sr & (1<<DSP_SR_LF);
    dsp_stack_pop(dsp, &dsp->registers[DSP_REG_LA], &dsp->registers[DSP_REG_LC]);
}

static void emu_illegal(dsp_core_t* dsp)
{
    /* Raise interrupt p:0x003e */
    dsp56k_add_interrupt(dsp, DSP_INTER_ILLEGAL);
    if (dsp->exception_debugging) {
        assert(false);
    }
}

static void emu_inc(dsp_core_t* dsp)
{
    uint32_t destreg, source[3], dest[3];
    uint16_t newsr;

    destreg = DSP_REG_A + (dsp->cur_inst & 1);
    if (destreg == DSP_REG_A) {
        dest[0] = dsp->registers[DSP_REG_A2];
        dest[1] = dsp->registers[DSP_REG_A1];
        dest[2] = dsp->registers[DSP_REG_A0];
    } else {
        dest[0] = dsp->registers[DSP_REG_B2];
        dest[1] = dsp->registers[DSP_REG_B1];
        dest[2] = dsp->registers[DSP_REG_B0];
    }

    source[2] = 1;
    source[1] = 0;
    source[0] = 0;

    newsr = dsp_add56(source, dest);

    if (destreg == DSP_REG_A) {
        dsp->registers[DSP_REG_A2] = dest[0];
        dsp->registers[DSP_REG_A1] = dest[1];
        dsp->registers[DSP_REG_A0] = dest[2];
    } else {
        dsp->registers[DSP_REG_B2] = dest[0];
        dsp->registers[DSP_REG_B1] = dest[1];
        dsp->registers[DSP_REG_B0] = dest[2];
    }

    emu_ccr_update_e_u_n_z(dsp, dest[0], dest[1], dest[2]);

    dsp->registers[DSP_REG_SR] &= BITMASK(16)-((1<<DSP_SR_V)|(1<<DSP_SR_C));
    dsp->registers[DSP_REG_SR] |= newsr;
    emu_sm_acc(dsp, destreg - DSP_REG_A, 0); /* SM */
}

static void emu_jcc_imm(dsp_core_t* dsp)
{
    uint32_t cc_code, newpc;

    newpc = dsp->cur_inst & BITMASK(12);
    cc_code=(dsp->cur_inst>>12) & BITMASK(4);
    if (emu_calc_cc(dsp, cc_code)) {
        dsp->pc = newpc;
        dsp->cur_inst_len = 0;
    }

    dsp->instr_cycle += 2;
}

static void emu_jcc_ea(dsp_core_t* dsp)
{
    uint32_t newpc, cc_code;

    emu_calc_ea(dsp, (dsp->cur_inst >>8) & BITMASK(6), &newpc);
    cc_code=dsp->cur_inst & BITMASK(4);

    if (emu_calc_cc(dsp, cc_code)) {
        dsp->pc = newpc;
        dsp->cur_inst_len = 0;
    }

    dsp->instr_cycle += 2;
}

static void emu_jclr_aa(dsp_core_t* dsp)
{
    uint32_t memspace, addr, value, numbit, newaddr;

    memspace = (dsp->cur_inst>>6) & 1;
    addr = (dsp->cur_inst>>8) & BITMASK(6);
    numbit = dsp->cur_inst & BITMASK(5);
    value = dsp56k_read_memory(dsp, memspace, addr);
    newaddr = read_memory_p(dsp, dsp->pc+1);

    dsp->instr_cycle += 4;

    if ((value & (1<<numbit))==0) {
        dsp->pc = newaddr;
        dsp->cur_inst_len = 0;
        return;
    }
    ++dsp->cur_inst_len;
}

static void emu_jclr_ea(dsp_core_t* dsp)
{
    uint32_t memspace, addr, value, numbit, newaddr;

    memspace = (dsp->cur_inst>>6) & 1;
    value = (dsp->cur_inst>>8) & BITMASK(6);
    numbit = dsp->cur_inst & BITMASK(5);
    newaddr = read_memory_p(dsp, dsp->pc+1);

    emu_calc_ea(dsp, value, &addr);
    value = dsp56k_read_memory(dsp, memspace, addr);

    dsp->instr_cycle += 4;

    if ((value & (1<<numbit))==0) {
        dsp->pc = newaddr;
        dsp->cur_inst_len = 0;
        return;
    }
    ++dsp->cur_inst_len;
}

static void emu_jclr_pp(dsp_core_t* dsp)
{
    uint32_t memspace, addr, value, numbit, newaddr;

    memspace = (dsp->cur_inst>>6) & 1;
    value = (dsp->cur_inst>>8) & BITMASK(6);
    numbit = dsp->cur_inst & BITMASK(5);
    addr = 0xffffc0 + value;
    value = dsp56k_read_memory(dsp, memspace, addr);
    newaddr = read_memory_p(dsp, dsp->pc+1);

    dsp->instr_cycle += 4;

    if ((value & (1<<numbit))==0) {
        dsp->pc = newaddr;
        dsp->cur_inst_len = 0;
        return;
    }
    ++dsp->cur_inst_len;
}

static void emu_jclr_reg(dsp_core_t* dsp)
{
    uint32_t value, numreg, numbit, newaddr;

    numreg = (dsp->cur_inst>>8) & BITMASK(6);
    numbit = dsp->cur_inst & BITMASK(5);
    newaddr = read_memory_p(dsp, dsp->pc+1);

    if ((numreg==DSP_REG_A) || (numreg==DSP_REG_B)) {
        emu_pm_read_accu24(dsp, numreg, &value);
    } else {
        value = dsp->registers[numreg];
    }

    dsp->instr_cycle += 4;

    if ((value & (1<<numbit))==0) {
        dsp->pc = newaddr;
        dsp->cur_inst_len = 0;
        return;
    }
    ++dsp->cur_inst_len;
}

static void emu_jmp_ea(dsp_core_t* dsp)
{
    uint32_t newpc;

    emu_calc_ea(dsp, (dsp->cur_inst>>8) & BITMASK(6), &newpc);
    dsp->cur_inst_len = 0;
    dsp->pc = newpc;

    dsp->instr_cycle += 2;
}

static void emu_jmp_imm(dsp_core_t* dsp)
{
    uint32_t newpc;

    newpc = dsp->cur_inst & BITMASK(12);
    dsp->cur_inst_len = 0;
    dsp->pc = newpc;

    dsp->instr_cycle += 2;
}

static void emu_jscc_ea(dsp_core_t* dsp)
{
    uint32_t newpc, cc_code;

    emu_calc_ea(dsp, (dsp->cur_inst >>8) & BITMASK(6), &newpc);
    cc_code=dsp->cur_inst & BITMASK(4);

    if (emu_calc_cc(dsp, cc_code)) {
        dsp_stack_push(dsp, dsp->pc+dsp->cur_inst_len, dsp->registers[DSP_REG_SR], 0);
        dsp->pc = newpc;
        dsp->cur_inst_len = 0;
    }

    dsp->instr_cycle += 2;
}

static void emu_jscc_imm(dsp_core_t* dsp)
{
    uint32_t cc_code, newpc;

    newpc = dsp->cur_inst & BITMASK(12);
    cc_code=(dsp->cur_inst>>12) & BITMASK(4);
    if (emu_calc_cc(dsp, cc_code)) {
        dsp_stack_push(dsp, dsp->pc+dsp->cur_inst_len, dsp->registers[DSP_REG_SR], 0);
        dsp->pc = newpc;
        dsp->cur_inst_len = 0;
    }

    dsp->instr_cycle += 2;
}

static void emu_jsclr_aa(dsp_core_t* dsp)
{
    uint32_t memspace, addr, value, newpc, numbit, newaddr;

    memspace = (dsp->cur_inst>>6) & 1;
    addr = (dsp->cur_inst>>8) & BITMASK(6);
    numbit = dsp->cur_inst & BITMASK(5);
    value = dsp56k_read_memory(dsp, memspace, addr);
    newaddr = read_memory_p(dsp, dsp->pc+1);

    dsp->instr_cycle += 4;

    if ((value & (1<<numbit))==0) {
        dsp_stack_push(dsp, dsp->pc+2, dsp->registers[DSP_REG_SR], 0);
        newpc = newaddr;
        dsp->pc = newpc;
        dsp->cur_inst_len = 0;
        return;
    }
    ++dsp->cur_inst_len;
}

static void emu_jsclr_ea(dsp_core_t* dsp)
{
    uint32_t memspace, addr, value, newpc, numbit, newaddr;

    memspace = (dsp->cur_inst>>6) & 1;
    value = (dsp->cur_inst>>8) & BITMASK(6);
    numbit = dsp->cur_inst & BITMASK(5);
    emu_calc_ea(dsp, value, &addr);
    value = dsp56k_read_memory(dsp, memspace, addr);
    newaddr = read_memory_p(dsp, dsp->pc+1);

    dsp->instr_cycle += 4;

    if ((value & (1<<numbit))==0) {
        dsp_stack_push(dsp, dsp->pc+2, dsp->registers[DSP_REG_SR], 0);
        newpc = newaddr;
        dsp->pc = newpc;
        dsp->cur_inst_len = 0;
        return;
    }
    ++dsp->cur_inst_len;
}

static void emu_jsclr_pp(dsp_core_t* dsp)
{
    uint32_t memspace, addr, value, newpc, numbit, newaddr;

    memspace = (dsp->cur_inst>>6) & 1;
    value = (dsp->cur_inst>>8) & BITMASK(6);
    numbit = dsp->cur_inst & BITMASK(5);
    addr = 0xffffc0 + value;
    value = dsp56k_read_memory(dsp, memspace, addr);
    newaddr = read_memory_p(dsp, dsp->pc+1);

    dsp->instr_cycle += 4;

    if ((value & (1<<numbit))==0) {
        dsp_stack_push(dsp, dsp->pc+2, dsp->registers[DSP_REG_SR], 0);
        newpc = newaddr;
        dsp->pc = newpc;
        dsp->cur_inst_len = 0;
        return;
    }
    ++dsp->cur_inst_len;
}

static void emu_jsclr_reg(dsp_core_t* dsp)
{
    uint32_t value, numreg, newpc, numbit, newaddr;

    numreg = (dsp->cur_inst>>8) & BITMASK(6);
    numbit = dsp->cur_inst & BITMASK(5);
    newaddr = read_memory_p(dsp, dsp->pc+1);

    if ((numreg==DSP_REG_A) || (numreg==DSP_REG_B)) {
        emu_pm_read_accu24(dsp, numreg, &value);
    } else {
        value = dsp->registers[numreg];
    }

    dsp->instr_cycle += 4;

    if ((value & (1<<numbit))==0) {
        dsp_stack_push(dsp, dsp->pc+2, dsp->registers[DSP_REG_SR], 0);
        newpc = newaddr;
        dsp->pc = newpc;
        dsp->cur_inst_len = 0;
        return;
    }
    ++dsp->cur_inst_len;
}

static void emu_jset_aa(dsp_core_t* dsp)
{
    uint32_t memspace, addr, value, numbit, newpc, newaddr;

    memspace = (dsp->cur_inst>>6) & 1;
    addr = (dsp->cur_inst>>8) & BITMASK(6);
    numbit = dsp->cur_inst & BITMASK(5);
    value = dsp56k_read_memory(dsp, memspace, addr);
    newaddr = read_memory_p(dsp, dsp->pc+1);

    dsp->instr_cycle += 4;

    if (value & (1<<numbit)) {
        newpc = newaddr;
        dsp->pc = newpc;
        dsp->cur_inst_len=0;
        return;
    }
    ++dsp->cur_inst_len;
}

static void emu_jset_ea(dsp_core_t* dsp)
{
    uint32_t memspace, addr, value, numbit, newpc, newaddr;

    memspace = (dsp->cur_inst>>6) & 1;
    value = (dsp->cur_inst>>8) & BITMASK(6);
    numbit = dsp->cur_inst & BITMASK(5);
    emu_calc_ea(dsp, value, &addr);
    value = dsp56k_read_memory(dsp, memspace, addr);
    newaddr = read_memory_p(dsp, dsp->pc+1);

    dsp->instr_cycle += 4;

    if (value & (1<<numbit)) {
        newpc = newaddr;
        dsp->pc = newpc;
        dsp->cur_inst_len=0;
        return;
    }
    ++dsp->cur_inst_len;
}

static void emu_jset_pp(dsp_core_t* dsp)
{
    uint32_t memspace, addr, value, numbit, newpc, newaddr;

    memspace = (dsp->cur_inst>>6) & 1;
    value = (dsp->cur_inst>>8) & BITMASK(6);
    numbit = dsp->cur_inst & BITMASK(5);
    addr = 0xffffc0 + value;
    value = dsp56k_read_memory(dsp, memspace, addr);
    newaddr = read_memory_p(dsp, dsp->pc+1);

    dsp->instr_cycle += 4;

    if (value & (1<<numbit)) {
        newpc = newaddr;
        dsp->pc = newpc;
        dsp->cur_inst_len=0;
        return;
    }
    ++dsp->cur_inst_len;
}

static void emu_jset_reg(dsp_core_t* dsp)
{
    uint32_t value, numreg, numbit, newpc, newaddr;

    numreg = (dsp->cur_inst>>8) & BITMASK(6);
    numbit = dsp->cur_inst & BITMASK(5);
    newaddr = read_memory_p(dsp, dsp->pc+1);

    if ((numreg==DSP_REG_A) || (numreg==DSP_REG_B)) {
        emu_pm_read_accu24(dsp, numreg, &value);
    } else {
        value = dsp->registers[numreg];
    }

    dsp->instr_cycle += 4;

    if (value & (1<<numbit)) {
        newpc = newaddr;
        dsp->pc = newpc;
        dsp->cur_inst_len=0;
        return;
    }
    ++dsp->cur_inst_len;
}

static void emu_jsr_imm(dsp_core_t* dsp)
{
    uint32_t newpc;

    newpc = dsp->cur_inst & BITMASK(12);

    if (dsp->interrupt_state != DSP_INTERRUPT_LONG){
        dsp_stack_push(dsp, dsp->pc+dsp->cur_inst_len, dsp->registers[DSP_REG_SR], 0);
    }
    else {
        dsp->interrupt_state = DSP_INTERRUPT_DISABLED;
    }

    dsp->pc = newpc;
    dsp->cur_inst_len = 0;

    dsp->instr_cycle += 2;
}

static void emu_jsr_ea(dsp_core_t* dsp)
{
    uint32_t newpc;

    emu_calc_ea(dsp, (dsp->cur_inst>>8) & BITMASK(6),&newpc);

    if (dsp->interrupt_state != DSP_INTERRUPT_LONG){
        dsp_stack_push(dsp, dsp->pc+dsp->cur_inst_len, dsp->registers[DSP_REG_SR], 0);
    }
    else {
        dsp->interrupt_state = DSP_INTERRUPT_DISABLED;
    }

    dsp->pc = newpc;
    dsp->cur_inst_len = 0;

    dsp->instr_cycle += 2;
}

static void emu_jsset_aa(dsp_core_t* dsp)
{
    uint32_t memspace, addr, value, newpc, numbit, newaddr;

    memspace = (dsp->cur_inst>>6) & 1;
    addr = (dsp->cur_inst>>8) & BITMASK(6);
    numbit = dsp->cur_inst & BITMASK(5);
    value = dsp56k_read_memory(dsp, memspace, addr);
    newaddr = read_memory_p(dsp, dsp->pc+1);

    dsp->instr_cycle += 4;

    if (value & (1<<numbit)) {
        dsp_stack_push(dsp, dsp->pc+2, dsp->registers[DSP_REG_SR], 0);
        newpc = newaddr;
        dsp->pc = newpc;
        dsp->cur_inst_len = 0;
        return;
    }
    ++dsp->cur_inst_len;
}

static void emu_jsset_ea(dsp_core_t* dsp)
{
    uint32_t memspace, addr, value, newpc, numbit, newaddr;

    memspace = (dsp->cur_inst>>6) & 1;
    value = (dsp->cur_inst>>8) & BITMASK(6);
    numbit = dsp->cur_inst & BITMASK(5);
    emu_calc_ea(dsp, value, &addr);
    value = dsp56k_read_memory(dsp, memspace, addr);
    newaddr = read_memory_p(dsp, dsp->pc+1);

    dsp->instr_cycle += 4;

    if (value & (1<<numbit)) {
        dsp_stack_push(dsp, dsp->pc+2, dsp->registers[DSP_REG_SR], 0);
        newpc = newaddr;
        dsp->pc = newpc;
        dsp->cur_inst_len = 0;
        return;
    }
    ++dsp->cur_inst_len;
}

static void emu_jsset_pp(dsp_core_t* dsp)
{
    uint32_t memspace, addr, value, newpc, numbit, newaddr;

    memspace = (dsp->cur_inst>>6) & 1;
    value = (dsp->cur_inst>>8) & BITMASK(6);
    numbit = dsp->cur_inst & BITMASK(5);
    addr = 0xffffc0 + value;
    value = dsp56k_read_memory(dsp, memspace, addr);
    newaddr = read_memory_p(dsp, dsp->pc+1);

    dsp->instr_cycle += 4;

    if (value & (1<<numbit)) {
        dsp_stack_push(dsp, dsp->pc+2, dsp->registers[DSP_REG_SR], 0);
        newpc = newaddr;
        dsp->pc = newpc;
        dsp->cur_inst_len = 0;
        return;
    }
    ++dsp->cur_inst_len;
}

static void emu_jsset_reg(dsp_core_t* dsp)
{
    uint32_t value, numreg, newpc, numbit, newaddr;

    numreg = (dsp->cur_inst>>8) & BITMASK(6);
    numbit = dsp->cur_inst & BITMASK(5);
    newaddr = read_memory_p(dsp, dsp->pc+1);

    if ((numreg==DSP_REG_A) || (numreg==DSP_REG_B)) {
        emu_pm_read_accu24(dsp, numreg, &value);
    } else {
        value = dsp->registers[numreg];
    }

    dsp->instr_cycle += 4;

    if (value & (1<<numbit)) {
        dsp_stack_push(dsp, dsp->pc+2, dsp->registers[DSP_REG_SR], 0);
        newpc = newaddr;
        dsp->pc = newpc;
        dsp->cur_inst_len = 0;
        return;
    }
    ++dsp->cur_inst_len;
}

static void emu_lua(dsp_core_t* dsp)
{
    uint32_t value, srcreg, dstreg, srcsave, srcnew;

    // TODO: I don't think this is right

    srcreg = (dsp->cur_inst>>8) & BITMASK(3);

    srcsave = dsp->registers[DSP_REG_R0+srcreg];
    emu_calc_ea(dsp, (dsp->cur_inst>>8) & BITMASK(5), &value);
    srcnew = dsp->registers[DSP_REG_R0+srcreg];
    dsp->registers[DSP_REG_R0+srcreg] = srcsave;

    /* xbased: D any register (the manual's table A-31): R0-R7, N0-N7 as
     * they were, the data ALU's as a move writes them */
    dstreg = dsp->cur_inst & BITMASK(5);
    if (dstreg >= DSP_REG_R0) {
        dsp->registers[dstreg] = srcnew;
    } else {
        dsp_write_reg(dsp, dstreg, srcnew);
    }

    dsp->instr_cycle += 2;
}

static void emu_lua_rel(dsp_core_t* dsp)
{
    uint32_t aa = ((dsp->cur_inst >> 4) & BITMASK(4))
                + (((dsp->cur_inst >> 11) & BITMASK(3)) << 4);
    uint32_t addrreg = (dsp->cur_inst>>8) & BITMASK(3);
    uint32_t dstreg = dsp->cur_inst & BITMASK(3);

    uint32_t v = (dsp->registers[DSP_REG_R0+addrreg]
        + dsp_signextend(7, aa)) & BITMASK(24);

    if (dsp->cur_inst & (1<<3)) {
        dsp->registers[DSP_REG_N0+dstreg] = v;
    } else {
        dsp->registers[DSP_REG_R0+dstreg] = v;
    }

    dsp->instr_cycle += 2;
}

static void emu_movec_reg(dsp_core_t* dsp)
{
    uint32_t numreg1, numreg2, value, dummy;

    /* S1,D2 */
    /* S2,D1 */

    numreg2 = (dsp->cur_inst>>8) & BITMASK(6);
    numreg1 = dsp->cur_inst & BITMASK(6);

    if (dsp->cur_inst & (1<<15)) {
        /* Write D1 */

        if ((numreg2 == DSP_REG_A) || (numreg2 == DSP_REG_B)) {
            emu_pm_read_accu24(dsp, numreg2, &value);
        } else {
            value = dsp->registers[numreg2];
        }
        if (numreg1 == DSP_REG_SR) emu_sr_note(dsp, value);
        value &= BITMASK(registers_mask[numreg1]);
        dsp_write_reg(dsp, numreg1, value);
    } else {
        /* Read S1 */
        if (numreg1 == DSP_REG_SSH) {
            dsp_stack_pop(dsp, &value, &dummy);
        }
        else {
            value = dsp->registers[numreg1];
            if (numreg1 == DSP_REG_SR) value |= dsp->sr_ext;
        }

        if (numreg2 == DSP_REG_A) {
            dsp->registers[DSP_REG_A0] = 0;
            dsp->registers[DSP_REG_A1] = value & BITMASK(24);
            dsp->registers[DSP_REG_A2] = value & (1<<23) ? 0xff : 0x0;
        }
        else if (numreg2 == DSP_REG_B) {
            dsp->registers[DSP_REG_B0] = 0;
            dsp->registers[DSP_REG_B1] = value & BITMASK(24);
            dsp->registers[DSP_REG_B2] = value & (1<<23) ? 0xff : 0x0;
        }
        else {
            dsp->registers[numreg2] = value & BITMASK(registers_mask[numreg2]);
        }
    }
}

static void emu_movec_aa(dsp_core_t* dsp)
{
    uint32_t numreg, addr, memspace, value, dummy;

    /* x:aa,D1 */
    /* S1,x:aa */
    /* y:aa,D1 */
    /* S1,y:aa */

    numreg = dsp->cur_inst & BITMASK(6);
    addr = (dsp->cur_inst>>8) & BITMASK(6);
    memspace = (dsp->cur_inst>>6) & 1;

    if (dsp->cur_inst & (1<<15)) {
        /* Write D1 */
        value = dsp56k_read_memory(dsp, memspace, addr);
        if (numreg == DSP_REG_SR) emu_sr_note(dsp, value);
        value &= BITMASK(registers_mask[numreg]);
        dsp_write_reg(dsp, numreg, value);
    } else {
        /* Read S1 */
        if (numreg == DSP_REG_SSH) {
            dsp_stack_pop(dsp, &value, &dummy);
        }
        else {
            value = dsp->registers[numreg];
            if (numreg == DSP_REG_SR) value |= dsp->sr_ext;
        }
        dsp56k_write_memory(dsp, memspace, addr, value);
    }
}

static void emu_movec_imm(dsp_core_t* dsp)
{
    uint32_t numreg, value;

    /* #xx,D1 */
    numreg = dsp->cur_inst & BITMASK(6);
    value = (dsp->cur_inst>>8) & BITMASK(8);
    if (numreg == DSP_REG_SR) emu_sr_note(dsp, value);
    value &= BITMASK(registers_mask[numreg]);
    dsp_write_reg(dsp, numreg, value);
}

static void emu_movec_ea(dsp_core_t* dsp)
{
    uint32_t numreg, addr, memspace, ea_mode, value, dummy;
    int retour;

    /* x:ea,D1 */
    /* S1,x:ea */
    /* y:ea,D1 */
    /* S1,y:ea */
    /* #xxxx,D1 */

    numreg = dsp->cur_inst & BITMASK(6);
    ea_mode = (dsp->cur_inst>>8) & BITMASK(6);
    memspace = (dsp->cur_inst>>6) & 1;

    if (dsp->cur_inst & (1<<15)) {
        /* Write D1 */
        retour = emu_calc_ea(dsp, ea_mode, &addr);
        if (retour) {
            value = addr;
        } else {
            value = dsp56k_read_memory(dsp, memspace, addr);
        }
        if (numreg == DSP_REG_SR) emu_sr_note(dsp, value);
        value &= BITMASK(registers_mask[numreg]);
        dsp_write_reg(dsp, numreg, value);
    } else {
        /* Read S1 */
        emu_calc_ea(dsp, ea_mode, &addr);
        if (numreg == DSP_REG_SSH) {
            dsp_stack_pop(dsp, &value, &dummy);
        }
        else {
            value = dsp->registers[numreg];
            if (numreg == DSP_REG_SR) value |= dsp->sr_ext;
        }
        dsp56k_write_memory(dsp, memspace, addr, value);
    }
}

static void emu_movem_aa(dsp_core_t* dsp)
{
    uint32_t numreg, addr, value, dummy;

    numreg = dsp->cur_inst & BITMASK(6);
    addr = (dsp->cur_inst>>8) & BITMASK(6);

    if  (dsp->cur_inst & (1<<15)) {
        /* Write D */
        value = read_memory_p(dsp, addr);
        if (numreg == DSP_REG_SR) emu_sr_note(dsp, value);
        value &= BITMASK(registers_mask[numreg]);
        dsp_write_reg(dsp, numreg, value);
    } else {
        /* Read S */
        if (numreg == DSP_REG_SSH) {
            dsp_stack_pop(dsp, &value, &dummy);
        }
        else if ((numreg == DSP_REG_A) || (numreg == DSP_REG_B)) {
            emu_pm_read_accu24(dsp, numreg, &value);
        }
        else {
            value = dsp->registers[numreg];
            if (numreg == DSP_REG_SR) value |= dsp->sr_ext;
        }
        dsp56k_write_memory(dsp, DSP_SPACE_P, addr, value);
    }

    dsp->instr_cycle += 4;
}

static void emu_movem_ea(dsp_core_t* dsp)
{
    uint32_t numreg, addr, ea_mode, value, dummy;

    numreg = dsp->cur_inst & BITMASK(6);
    ea_mode = (dsp->cur_inst>>8) & BITMASK(6);
    emu_calc_ea(dsp, ea_mode, &addr);

    if  (dsp->cur_inst & (1<<15)) {
        /* Write D */
        value = read_memory_p(dsp, addr);
        if (numreg == DSP_REG_SR) emu_sr_note(dsp, value);
        value &= BITMASK(registers_mask[numreg]);
        dsp_write_reg(dsp, numreg, value);
    } else {
        /* Read S */
        if (numreg == DSP_REG_SSH) {
            dsp_stack_pop(dsp, &value, &dummy);
        }
        else if ((numreg == DSP_REG_A) || (numreg == DSP_REG_B)) {
            emu_pm_read_accu24(dsp, numreg, &value);
        }
        else {
            value = dsp->registers[numreg];
            if (numreg == DSP_REG_SR) value |= dsp->sr_ext;
        }
        dsp56k_write_memory(dsp, DSP_SPACE_P, addr, value);
    }

    dsp->instr_cycle += 4;
}

static void emu_movep_0(dsp_core_t* dsp)
{
    /* S,x:pp */
    /* x:pp,D */
    /* S,y:pp */
    /* y:pp,D */

    uint32_t addr, memspace, numreg, value, dummy;

    addr = 0xffffc0 + (dsp->cur_inst & BITMASK(6));
    memspace = (dsp->cur_inst>>16) & 1;
    numreg = (dsp->cur_inst>>8) & BITMASK(6);

    if  (dsp->cur_inst & (1<<15)) {
        /* Write pp */
        if ((numreg == DSP_REG_A) || (numreg == DSP_REG_B)) {
            emu_pm_read_accu24(dsp, numreg, &value);
        }
        else if (numreg == DSP_REG_SSH) {
            dsp_stack_pop(dsp, &value, &dummy);
        }
        else {
            value = dsp->registers[numreg];
            if (numreg == DSP_REG_SR) value |= dsp->sr_ext;
        }
        dsp56k_write_memory(dsp, memspace, addr, value);
    } else {
        /* Read pp */
        value = dsp56k_read_memory(dsp, memspace, addr);
        if (numreg == DSP_REG_SR) emu_sr_note(dsp, value);
        value &= BITMASK(registers_mask[numreg]);
        dsp_write_reg(dsp, numreg, value);
    }

    dsp->instr_cycle += 2;
}

static void emu_movep_1(dsp_core_t* dsp)
{
    /* p:ea,x:pp */
    /* x:pp,p:ea */
    /* p:ea,y:pp */
    /* y:pp,p:ea */

    uint32_t xyaddr, memspace, paddr;

    xyaddr = 0xffffc0 + (dsp->cur_inst & BITMASK(6));
    emu_calc_ea(dsp, (dsp->cur_inst>>8) & BITMASK(6), &paddr);
    memspace = (dsp->cur_inst>>16) & 1;

    if (dsp->cur_inst & (1<<15)) {
        /* Write pp */
        dsp56k_write_memory(dsp, memspace, xyaddr, read_memory_p(dsp, paddr));
    } else {
        /* Read pp */
        dsp56k_write_memory(dsp, DSP_SPACE_P, paddr, dsp56k_read_memory(dsp, memspace, xyaddr));
    }

    /* Movep is 4 cycles, but according to the motorola doc, */
    /* movep from p memory to x or y peripheral memory takes */
    /* 2 more cycles, so +4 cycles at total */
    dsp->instr_cycle += 4;
}

static void emu_movep_23(dsp_core_t* dsp)
{
    /* x:ea,x:pp */
    /* y:ea,x:pp */
    /* #xxxxxx,x:pp */
    /* x:pp,x:ea */
    /* x:pp,y:pp */
    /* x:ea,y:pp */
    /* y:ea,y:pp */
    /* #xxxxxx,y:pp */
    /* y:pp,y:ea */
    /* y:pp,x:ea */

    uint32_t addr, peraddr, easpace, perspace, ea_mode;
    int retour;

    peraddr = 0xffffc0 + (dsp->cur_inst & BITMASK(6));
    perspace = (dsp->cur_inst>>16) & 1;

    ea_mode = (dsp->cur_inst>>8) & BITMASK(6);
    easpace = (dsp->cur_inst>>6) & 1;
    retour = emu_calc_ea(dsp, ea_mode, &addr);

    if (dsp->cur_inst & (1<<15)) {
        /* Write pp */

        if (retour) {
            dsp56k_write_memory(dsp, perspace, peraddr, addr);
        } else {
            dsp56k_write_memory(dsp, perspace, peraddr, dsp56k_read_memory(dsp, easpace, addr));
        }
    } else {
        /* Read pp */
        dsp56k_write_memory(dsp, easpace, addr, dsp56k_read_memory(dsp, perspace, peraddr));
    }

    dsp->instr_cycle += 2;
}

static void emu_movep_x_qq(dsp_core_t* dsp)
{
    // 00000111W1MMMRRR0sqqqqqq

    uint32_t x_addr = 0xffff80 + (dsp->cur_inst & BITMASK(6));
    uint32_t ea_mode = (dsp->cur_inst>>8) & BITMASK(6);
    uint32_t ea_space = (dsp->cur_inst>>6) & 1;
    uint32_t ea_addr;
    int retour = emu_calc_ea(dsp, ea_mode, &ea_addr);

    if (dsp->cur_inst & (1<<15)) {
        /* Write qq */

        if (retour) {
            dsp56k_write_memory(dsp, DSP_SPACE_X, x_addr, ea_addr);
        } else {
            dsp56k_write_memory(dsp, DSP_SPACE_X, x_addr,
                dsp56k_read_memory(dsp, ea_space, ea_addr));
        }
    } else {
        /* Read qq */
        dsp56k_write_memory(dsp, ea_space, ea_addr,
            dsp56k_read_memory(dsp, DSP_SPACE_X, x_addr));
    }

    dsp->instr_cycle += 2;
}

static void emu_move_x_long(dsp_core_t* dsp)
{
    uint32_t xxxx = read_memory_p(dsp, dsp->pc+1);
    dsp->cur_inst_len++;

    int W = (dsp->cur_inst >> 6) & 1;
    uint32_t offreg = DSP_REG_R0 + ((dsp->cur_inst >> 8) & BITMASK(3));
    uint32_t numreg = dsp->cur_inst & BITMASK(6);
    uint32_t x_addr = (dsp->registers[offreg] + xxxx) & BITMASK(24);

    if (!W) {
        uint32_t value;
        if (numreg == DSP_REG_A || numreg == DSP_REG_B) {
            emu_pm_read_accu24(dsp, numreg, &value);
        } else {
            value = dsp->registers[numreg];
        }
        dsp56k_write_memory(dsp, DSP_SPACE_X, x_addr, value);
    } else {
        dsp_write_reg(dsp, numreg, dsp56k_read_memory(dsp, DSP_SPACE_X, x_addr));
    }

    // TODO: cycles
}

static void emu_move_xy_imm(dsp_core_t* dsp, int space)
{
    uint32_t xxx = (((dsp->cur_inst >> 11) & BITMASK(6)) << 1)
             + ((dsp->cur_inst >> 6) & 1);
    int W = (dsp->cur_inst >> 4) & 1;
    uint32_t offreg = DSP_REG_R0 + ((dsp->cur_inst >> 8) & BITMASK(3));
    uint32_t numreg = dsp->cur_inst & BITMASK(4);
    uint32_t addr = (dsp->registers[offreg] + dsp_signextend(7, xxx)) & BITMASK(24);

    if (!W) {
        uint32_t value;
        if (numreg == DSP_REG_A || numreg == DSP_REG_B) {
            emu_pm_read_accu24(dsp, numreg, &value);
        } else {
            value = dsp->registers[numreg];
        }
        dsp56k_write_memory(dsp, space, addr, value);
    } else {
        dsp_write_reg(dsp, numreg, dsp56k_read_memory(dsp, space, addr));
    }

    // TODO: cycles
}

/* xbased: EXTRACT / EXTRACTU #CO,S2,D (the DSP56300 family manual, A-6.39
 * and A-6.40): the field of S2 at offset CO[5:0], CO[17:12] bits wide,
 * right-aligned in D, the rest its sign (EXTRACT) or zeros (EXTRACTU); a
 * 56-bit operation; E U N Z the standard way, V and C cleared */
static void emu_extract_co(dsp_core_t* dsp, bool is_unsigned)
{
    uint32_t co = read_memory_p(dsp, dsp->pc+1);
    dsp->cur_inst_len++;
    uint32_t s = (dsp->cur_inst >> 4) & 1;
    uint32_t d = dsp->cur_inst & 1;
    uint32_t offset = co & BITMASK(6);
    uint32_t width = (co >> 12) & BITMASK(6);

    uint64_t v = (uint64_t)dsp->registers[DSP_REG_A0+s]
        | ((uint64_t)dsp->registers[DSP_REG_A1+s] << 24)
        | ((uint64_t)dsp->registers[DSP_REG_A2+s] << 48);
    uint64_t field = 0;
    if (width > 0 && offset < 56) {
        field = v >> offset;
        if (width < 64) {
            field &= (1ULL << width) - 1;
        }
        if (!is_unsigned && width < 64 && ((field >> (width - 1)) & 1)) {
            field |= ~0ULL << width;
        }
    }
    field &= (1ULL << 56) - 1;

    dsp->registers[DSP_REG_A0+d] = field & BITMASK(24);
    dsp->registers[DSP_REG_A1+d] = (field >> 24) & BITMASK(24);
    dsp->registers[DSP_REG_A2+d] = (field >> 48) & BITMASK(8);

    emu_ccr_update_e_u_n_z(dsp, dsp->registers[DSP_REG_A2+d],
                           dsp->registers[DSP_REG_A1+d],
                           dsp->registers[DSP_REG_A0+d]);
    dsp->registers[DSP_REG_SR] &= BITMASK(16)-((1<<DSP_SR_V)|(1<<DSP_SR_C));
}

static void emu_extract_imm(dsp_core_t* dsp)
{
    emu_extract_co(dsp, false);
}

static void emu_extractu_imm(dsp_core_t* dsp)
{
    emu_extract_co(dsp, true);
}

static void emu_move_x_imm(dsp_core_t* dsp)
{
    emu_move_xy_imm(dsp, DSP_SPACE_X);
}

static void emu_move_y_imm(dsp_core_t* dsp)
{
    emu_move_xy_imm(dsp, DSP_SPACE_Y);
}

static void emu_mpyi(dsp_core_t* dsp)
{
    uint32_t xxxx = read_memory_p(dsp, dsp->pc+1);
    dsp->cur_inst_len++;

    uint32_t k = (dsp->cur_inst >> 2) & 1;
    uint32_t d = (dsp->cur_inst >> 3) & 1;
    uint32_t qq = (dsp->cur_inst >> 4) & BITMASK(2);

    unsigned int srcreg = DSP_REG_NULL;
    switch (qq) {
    case 0: srcreg = DSP_REG_X0; break;
    case 1: srcreg = DSP_REG_Y0; break;
    case 2: srcreg = DSP_REG_X1; break;
    case 3: srcreg = DSP_REG_Y1; break;
    }

    uint32_t source[3];
    dsp_mul56(xxxx, dsp->registers[srcreg], source, k ? SIGN_MINUS : SIGN_PLUS);

    if (d) {
        dsp->registers[DSP_REG_B2] = source[0];
        dsp->registers[DSP_REG_B1] = source[1];
        dsp->registers[DSP_REG_B0] = source[2];
    } else {
        dsp->registers[DSP_REG_A2] = source[0];
        dsp->registers[DSP_REG_A1] = source[1];
        dsp->registers[DSP_REG_A0] = source[2];
    }

    emu_ccr_update_e_u_n_z(dsp, source[0], source[1], source[2]);
    dsp->registers[DSP_REG_SR] &= BITMASK(16)-(1<<DSP_SR_V);
    emu_sm_acc(dsp, d, 0); /* SM */
}

static void emu_norm(dsp_core_t* dsp)
{
    uint32_t cursr,cur_e, cur_euz, dest[3], numreg, rreg;
    uint16_t newsr;

    cursr = dsp->registers[DSP_REG_SR];
    cur_e = (cursr>>DSP_SR_E) & 1;  /* E */
    cur_euz = ~cur_e;           /* (not E) and U and (not Z) */
    cur_euz &= (cursr>>DSP_SR_U) & 1;
    cur_euz &= ~((cursr>>DSP_SR_Z) & 1);
    cur_euz &= 1;

    numreg = (dsp->cur_inst>>3) & 1;
    dest[0] = dsp->registers[DSP_REG_A2+numreg];
    dest[1] = dsp->registers[DSP_REG_A1+numreg];
    dest[2] = dsp->registers[DSP_REG_A0+numreg];
    rreg = DSP_REG_R0+((dsp->cur_inst>>8) & BITMASK(3));

    if (cur_euz) {
        newsr = dsp_asl56(dest, 1);
        --dsp->registers[rreg];
        dsp->registers[rreg] &= BITMASK(16);
    } else if (cur_e) {
        newsr = dsp_asr56(dest, 1);
        ++dsp->registers[rreg];
        dsp->registers[rreg] &= BITMASK(16);
    } else {
        newsr = 0;
    }

    dsp->registers[DSP_REG_A2+numreg] = dest[0];
    dsp->registers[DSP_REG_A1+numreg] = dest[1];
    dsp->registers[DSP_REG_A0+numreg] = dest[2];

    emu_ccr_update_e_u_n_z(dsp, dest[0], dest[1], dest[2]);

    /* xbased: V (and L) of the left shift, C unchanged (A-6.87; it put the
     * shift's bit out in C) */
    dsp->registers[DSP_REG_SR] &= BITMASK(16)-(1<<DSP_SR_V);
    dsp->registers[DSP_REG_SR] |= newsr & ((1<<DSP_SR_V)|(1<<DSP_SR_L));
}

static void emu_or_long(dsp_core_t* dsp)
{
    uint32_t xxxx = read_memory_p(dsp, dsp->pc+1);
    dsp->cur_inst_len++;

    int dstreg;
    if ((dsp->cur_inst >> 3) & 1) {
        dstreg = DSP_REG_B1;
    } else {
        dstreg = DSP_REG_A1;
    }

    dsp->registers[dstreg] |= xxxx;

    dsp->registers[DSP_REG_SR] &= BITMASK(16)-((1<<DSP_SR_N)|(1<<DSP_SR_Z)|(1<<DSP_SR_V));
    dsp->registers[DSP_REG_SR] |= ((dsp->registers[dstreg]>>23) & 1)<<DSP_SR_N;
    dsp->registers[DSP_REG_SR] |= (dsp->registers[dstreg]==0)<<DSP_SR_Z;

    // TODO: cycles?
}

static void emu_ori(dsp_core_t* dsp)
{
    uint32_t regnum, value;

    value = (dsp->cur_inst >> 8) & BITMASK(8);
    regnum = dsp->cur_inst & BITMASK(2);
    switch(regnum) {
        case 0:
            /* mr */
            dsp->registers[DSP_REG_SR] |= value<<8;
            break;
        case 1:
            /* ccr */
            dsp->registers[DSP_REG_SR] |= value;
            break;
        case 2:
            /* omr */
            dsp->registers[DSP_REG_OMR] |= value;
            break;
    }
}

/*
    REP instruction parameter encoding

    xxxxxxxx 00xxxxxx 0xxxxxxx  aa
    xxxxxxxx 01xxxxxx 0xxxxxxx  ea
    xxxxxxxx YYxxxxxx 1xxxxxxx  imm
    xxxxxxxx 11xxxxxx 0xxxxxxx  reg
*/

static void emu_rep_aa(dsp_core_t* dsp)
{
    /* x:aa */
    /* y:aa */
    dsp->registers[DSP_REG_LCSAVE] = dsp->registers[DSP_REG_LC];
    dsp->pc_on_rep = 1; /* Not decrement LC at first time */
    dsp->loop_rep = 1;  /* We are now running rep */

    dsp->registers[DSP_REG_LC]=dsp56k_read_memory(dsp, (dsp->cur_inst>>6) & 1,(dsp->cur_inst>>8) & BITMASK(6));

    dsp->instr_cycle += 2;
}

static void emu_rep_imm(dsp_core_t* dsp)
{
    /* #xxx */

    dsp->registers[DSP_REG_LCSAVE] = dsp->registers[DSP_REG_LC];
    dsp->pc_on_rep = 1; /* Not decrement LC at first time */
    dsp->loop_rep = 1;  /* We are now running rep */

    dsp->registers[DSP_REG_LC] = ((dsp->cur_inst>>8) & BITMASK(8))
        + ((dsp->cur_inst & BITMASK(4))<<8);

    dsp->instr_cycle += 2;
}

static void emu_rep_ea(dsp_core_t* dsp)
{
    uint32_t value;

    /* x:ea */
    /* y:ea */

    dsp->registers[DSP_REG_LCSAVE] = dsp->registers[DSP_REG_LC];
    dsp->pc_on_rep = 1; /* Not decrement LC at first time */
    dsp->loop_rep = 1;  /* We are now running rep */

    emu_calc_ea(dsp, (dsp->cur_inst>>8) & BITMASK(6),&value);
    dsp->registers[DSP_REG_LC]= dsp56k_read_memory(dsp, (dsp->cur_inst>>6) & 1, value);

    dsp->instr_cycle += 2;
}

static void emu_rep_reg(dsp_core_t* dsp)
{
    uint32_t numreg;

    /* R */

    dsp->registers[DSP_REG_LCSAVE] = dsp->registers[DSP_REG_LC];
    dsp->pc_on_rep = 1; /* Not decrement LC at first time */
    dsp->loop_rep = 1;  /* We are now running rep */

    numreg = (dsp->cur_inst>>8) & BITMASK(6);
    if ((numreg == DSP_REG_A) || (numreg == DSP_REG_B)) {
        emu_pm_read_accu24(dsp, numreg, &dsp->registers[DSP_REG_LC]);
    } else {
        dsp->registers[DSP_REG_LC] = dsp->registers[numreg];
    }
    dsp->registers[DSP_REG_LC] &= BITMASK(16);

    dsp->instr_cycle += 2;
}

static void emu_reset(dsp_core_t* dsp)
{
    /* Reset external peripherals */
    dsp->instr_cycle += 2;
}

static void emu_rti(dsp_core_t* dsp)
{
    uint32_t newpc = 0, newsr = 0;

    dsp_stack_pop(dsp, &newpc, &newsr);
    dsp->pc = newpc;
    dsp->registers[DSP_REG_SR] = newsr;
    dsp->cur_inst_len = 0;

    dsp->instr_cycle += 2;
}

static void emu_rts(dsp_core_t* dsp)
{
    uint32_t newpc = 0, newsr;

    dsp_stack_pop(dsp, &newpc, &newsr);
    dsp->pc = newpc;
    dsp->cur_inst_len = 0;

    dsp->instr_cycle += 2;
}

static void emu_stop(dsp_core_t* dsp)
{
    DPRINTF("Dsp: STOP instruction\n");
}

static void emu_sub_x(dsp_core_t* dsp, uint32_t x, uint32_t d)
{
    uint32_t source[3], dest[3];

    if (d) {
        dest[0] = dsp->registers[DSP_REG_B2];
        dest[1] = dsp->registers[DSP_REG_B1];
        dest[2] = dsp->registers[DSP_REG_B0];
    } else {
        dest[0] = dsp->registers[DSP_REG_A2];
        dest[1] = dsp->registers[DSP_REG_A1];
        dest[2] = dsp->registers[DSP_REG_A0];
    }

    source[2] = 0;
    source[1] = x;
    source[0] = source[1] & (1<<23) ? 0xff : 0x0;

    uint16_t newsr = dsp_sub56(source, dest);

    if (d) {
        dsp->registers[DSP_REG_B2] = dest[0];
        dsp->registers[DSP_REG_B1] = dest[1];
        dsp->registers[DSP_REG_B0] = dest[2];
    } else {
        dsp->registers[DSP_REG_A2] = dest[0];
        dsp->registers[DSP_REG_A1] = dest[1];
        dsp->registers[DSP_REG_A0] = dest[2];
    }

    emu_ccr_update_e_u_n_z(dsp, dest[0], dest[1], dest[2]);

    dsp->registers[DSP_REG_SR] &= BITMASK(16)-((1<<DSP_SR_V)|(1<<DSP_SR_C));
    dsp->registers[DSP_REG_SR] |= newsr;
    emu_sm_acc(dsp, d, 0); /* SM */
}

static void emu_sub_imm(dsp_core_t* dsp)
{
    uint32_t xx = (dsp->cur_inst >> 8) & BITMASK(6);
    uint32_t d = (dsp->cur_inst >> 3) & 1;
    emu_sub_x(dsp, xx, d);
}

static void emu_sub_long(dsp_core_t* dsp)
{
    uint32_t xxxx = read_memory_p(dsp, dsp->pc+1);
    dsp->cur_inst_len++;

    uint32_t d = (dsp->cur_inst >> 3) & 1;
    emu_sub_x(dsp, xxxx, d);
}

static void emu_tcc(dsp_core_t* dsp)
{
    uint32_t cc_code, regsrc1, regdest1;
    uint32_t regsrc2, regdest2;
    uint32_t val0, val1, val2;

    cc_code = (dsp->cur_inst>>12) & BITMASK(4);

    /* xbased: Tcc S2,D2 (00000010CCCC1ttt00000TTT) moves an address register
     * alone (it moved B to A, the JJJd 0000 of the S1,D1 table, and no R) */
    if ((dsp->cur_inst & 0x010800) == 0x000800) {
        if (emu_calc_cc(dsp, cc_code)) {
            dsp->registers[DSP_REG_R0+(dsp->cur_inst & BITMASK(3))] =
                dsp->registers[DSP_REG_R0+((dsp->cur_inst>>8) & BITMASK(3))];
        }
        return;
    }

    if (emu_calc_cc(dsp, cc_code)) {
        regsrc1 = registers_tcc[(dsp->cur_inst>>3) & BITMASK(4)][0];
        regdest1 = registers_tcc[(dsp->cur_inst>>3) & BITMASK(4)][1];

        /* Read S1 */
        if (regsrc1 == DSP_REG_A) {
            val0 = dsp->registers[DSP_REG_A0];
            val1 = dsp->registers[DSP_REG_A1];
            val2 = dsp->registers[DSP_REG_A2];
        }
        else if (regsrc1 == DSP_REG_B) {
            val0 = dsp->registers[DSP_REG_B0];
            val1 = dsp->registers[DSP_REG_B1];
            val2 = dsp->registers[DSP_REG_B2];
        }
        else {
            val0 = 0;
            val1 = dsp->registers[regsrc1];
            val2 = val1 & (1<<23) ? 0xff : 0x0;
        }

        /* Write D1 */
        if (regdest1 == DSP_REG_A) {
            dsp->registers[DSP_REG_A2] = val2;
            dsp->registers[DSP_REG_A1] = val1;
            dsp->registers[DSP_REG_A0] = val0;
        }
        else {
            dsp->registers[DSP_REG_B2] = val2;
            dsp->registers[DSP_REG_B1] = val1;
            dsp->registers[DSP_REG_B0] = val0;
        }

        /* S2,D2 transfer */
        if (dsp->cur_inst & (1<<16)) {
            regsrc2 = DSP_REG_R0+((dsp->cur_inst>>8) & BITMASK(3));
            regdest2 = DSP_REG_R0+(dsp->cur_inst & BITMASK(3));

            dsp->registers[regdest2] = dsp->registers[regsrc2];
        }
    }
}

static void emu_wait(dsp_core_t* dsp)
{
    DPRINTF("Dsp: WAIT instruction\n");
}

/**********************************
 * xbased: the instructions the decode table had no code for, written from
 * the DSP56300 Family Manual, appendix A (the A-6.n section of each one).
 * DO FOREVER, TRAP, VSL and DEBUG aren't here (no title runs them).
 **********************************/

/* Table A-15: SSS/sss (control or source register) and qqq */
static const int emu_sss_reg[8] = {
    DSP_REG_NULL, DSP_REG_NULL, DSP_REG_A1, DSP_REG_B1,
    DSP_REG_X0, DSP_REG_Y0, DSP_REG_X1, DSP_REG_Y1
};
static const int emu_qqq_reg[8] = {
    DSP_REG_NULL, DSP_REG_NULL, DSP_REG_A0, DSP_REG_B0,
    DSP_REG_X0, DSP_REG_Y0, DSP_REG_X1, DSP_REG_Y1
};
/* Table A-30: QQQQ, the S1,S2 pairs of the su/uu multiplies and DMAC */
static const int emu_qqqq_reg[16][2] = {
    {DSP_REG_X0, DSP_REG_X0}, {DSP_REG_Y0, DSP_REG_Y0}, {DSP_REG_X1, DSP_REG_X0}, {DSP_REG_Y1, DSP_REG_Y0},
    {DSP_REG_X0, DSP_REG_Y1}, {DSP_REG_Y0, DSP_REG_X0}, {DSP_REG_X1, DSP_REG_Y0}, {DSP_REG_Y1, DSP_REG_X1},
    {DSP_REG_X1, DSP_REG_X1}, {DSP_REG_Y1, DSP_REG_Y1}, {DSP_REG_X0, DSP_REG_X1}, {DSP_REG_Y0, DSP_REG_Y1},
    {DSP_REG_Y1, DSP_REG_X0}, {DSP_REG_X0, DSP_REG_Y0}, {DSP_REG_Y0, DSP_REG_X1}, {DSP_REG_X1, DSP_REG_Y1},
};

/* accumulator d (0 A, 1 B) as a sign-extended 56-bit value, and back */
static int64_t emu_acc_get(dsp_core_t* dsp, int d)
{
    const uint64_t v = (uint64_t)dsp->registers[DSP_REG_A0+d]
        | ((uint64_t)dsp->registers[DSP_REG_A1+d] << 24)
        | ((uint64_t)dsp->registers[DSP_REG_A2+d] << 48);
    return (int64_t)(v << 8) >> 8;
}

static void emu_acc_set(dsp_core_t* dsp, int d, int64_t v)
{
    dsp->registers[DSP_REG_A0+d] = (uint32_t)v & BITMASK(24);
    dsp->registers[DSP_REG_A1+d] = (uint32_t)(v >> 24) & BITMASK(24);
    dsp->registers[DSP_REG_A2+d] = (uint32_t)(v >> 48) & BITMASK(8);
}

static void emu_acc_eunz(dsp_core_t* dsp, int d)
{
    emu_ccr_update_e_u_n_z(dsp, dsp->registers[DSP_REG_A2+d],
                           dsp->registers[DSP_REG_A1+d],
                           dsp->registers[DSP_REG_A0+d]);
}

/* N and Z of a 24-bit result in bits 47-24, V cleared (the logical ops) */
static void emu_sr_nz24(dsp_core_t* dsp, uint32_t v)
{
    dsp->registers[DSP_REG_SR] &= BITMASK(16)-((1<<DSP_SR_N)|(1<<DSP_SR_Z)|(1<<DSP_SR_V));
    dsp->registers[DSP_REG_SR] |= ((v>>23) & 1)<<DSP_SR_N;
    dsp->registers[DSP_REG_SR] |= (v==0)<<DSP_SR_Z;
}

/* V (and L, sticky) of a 56-bit result held in 64 bits */
static void emu_sr_v56(dsp_core_t* dsp, int64_t r)
{
    const int over = r < -(1LL << 55) || r >= (1LL << 55);
    dsp->registers[DSP_REG_SR] &= BITMASK(16)-(1<<DSP_SR_V);
    dsp->registers[DSP_REG_SR] |= (over<<DSP_SR_V)|(over<<DSP_SR_L);
}

static int64_t emu_wrap56(int64_t r)
{
    return (int64_t)((uint64_t)r << 8) >> 8;
}

/* A-6.90 OR, A-6.38 EOR: #xx (6 bits, unsigned) and #xxxxxx with bits 47-24 */
static void emu_logic_x(dsp_core_t* dsp, uint32_t x, uint32_t d, int op)
{
    const int reg = d ? DSP_REG_B1 : DSP_REG_A1;
    if (op == 0) {
        dsp->registers[reg] |= x;
    } else {
        dsp->registers[reg] ^= x;
    }
    dsp->registers[reg] &= BITMASK(24);
    emu_sr_nz24(dsp, dsp->registers[reg]);
}

static void emu_or_imm(dsp_core_t* dsp)
{
    emu_logic_x(dsp, (dsp->cur_inst >> 8) & BITMASK(6), (dsp->cur_inst >> 3) & 1, 0);
}

static void emu_eor_imm(dsp_core_t* dsp)
{
    emu_logic_x(dsp, (dsp->cur_inst >> 8) & BITMASK(6), (dsp->cur_inst >> 3) & 1, 1);
}

static void emu_eor_long(dsp_core_t* dsp)
{
    uint32_t xxxx = read_memory_p(dsp, dsp->pc+1);
    dsp->cur_inst_len++;
    emu_logic_x(dsp, xxxx, (dsp->cur_inst >> 3) & 1, 1);
}

/* A-6.55 LSL, A-6.56 LSR (multi-bit): bits 47-24 shifted n, C the last bit
 * out (cleared for 0), N bit 47, Z bits 47-24, V cleared. The S,D form takes
 * the count from the register's 5 low bits (the #ii field's width) */
static void emu_lsx(dsp_core_t* dsp, uint32_t d, uint32_t n, int left)
{
    const int reg = d ? DSP_REG_B1 : DSP_REG_A1;
    uint32_t v = dsp->registers[reg] & BITMASK(24), c = 0;
    if (n > 0) {
        if (left) {
            c = n <= 24 ? (v >> (24 - n)) & 1 : 0;
            v = n < 24 ? (v << n) & BITMASK(24) : 0;
        } else {
            c = n <= 24 ? (v >> (n - 1)) & 1 : 0;
            v = n < 24 ? v >> n : 0;
        }
    }
    dsp->registers[reg] = v;
    emu_sr_nz24(dsp, v);
    dsp->registers[DSP_REG_SR] &= BITMASK(16)-(1<<DSP_SR_C);
    dsp->registers[DSP_REG_SR] |= c<<DSP_SR_C;
}

static void emu_lsr_imm(dsp_core_t* dsp)
{
    emu_lsx(dsp, dsp->cur_inst & 1, (dsp->cur_inst >> 1) & BITMASK(5), 0);
}

static void emu_lsl_reg(dsp_core_t* dsp)
{
    const uint32_t s = dsp->registers[emu_sss_reg[(dsp->cur_inst >> 1) & 7]];
    emu_lsx(dsp, dsp->cur_inst & 1, s & BITMASK(5), 1);
}

static void emu_lsr_reg(dsp_core_t* dsp)
{
    const uint32_t s = dsp->registers[emu_sss_reg[(dsp->cur_inst >> 1) & 7]];
    emu_lsx(dsp, dsp->cur_inst & 1, s & BITMASK(5), 0);
}

/* A-6.8 ASL S1,S2,D, A-6.9 ASR S1,S2,D: the count is S1's 6 low bits */
static void emu_asx_reg(dsp_core_t* dsp, int left)
{
    const uint32_t S = (dsp->cur_inst >> 4) & 1, D = dsp->cur_inst & 1;
    const uint32_t n = dsp->registers[emu_sss_reg[(dsp->cur_inst >> 1) & 7]] & BITMASK(6);
    uint32_t dest[3];
    dest[0] = dsp->registers[DSP_REG_A2+S];
    dest[1] = dsp->registers[DSP_REG_A1+S];
    dest[2] = dsp->registers[DSP_REG_A0+S];
    const uint16_t newsr = left ? dsp_asl56(dest, n) : dsp_asr56(dest, n);
    dsp->registers[DSP_REG_A2+D] = dest[0];
    dsp->registers[DSP_REG_A1+D] = dest[1];
    dsp->registers[DSP_REG_A0+D] = dest[2];
    dsp->registers[DSP_REG_SR] &= BITMASK(16)-((1<<DSP_SR_C)|(1<<DSP_SR_V));
    dsp->registers[DSP_REG_SR] |= newsr;
    emu_acc_eunz(dsp, D);
    emu_sm_acc(dsp, D, 0); /* SM */
}

static void emu_asl_reg(dsp_core_t* dsp)
{
    emu_asx_reg(dsp, 1);
}

static void emu_asr_reg(dsp_core_t* dsp)
{
    emu_asx_reg(dsp, 0);
}

/* A-6.23 CLB S,D: 9 minus the leading run of bit 55 over bits 55-0 into
 * D[47:24] (0 for a zero source), the LSP zeroed, the EXP sign-extended;
 * N, Z of the result, V cleared */
static void emu_clb(dsp_core_t* dsp)
{
    const int S = (dsp->cur_inst >> 1) & 1, D = dsp->cur_inst & 1;
    const uint64_t u = (uint64_t)emu_acc_get(dsp, S) & ((1ULL << 56) - 1);
    int32_t r = 0;
    if (u != 0) {
        const uint64_t top = (u >> 55) & 1;
        int cnt = 0;
        for (int i = 55; i >= 0 && ((u >> i) & 1) == top; i--) {
            cnt++;
        }
        r = 9 - cnt;
    }
    emu_acc_set(dsp, D, (int64_t)r << 24);
    emu_sr_nz24(dsp, (uint32_t)r & BITMASK(24));
}

/* A-6.88 NORMF S,D: S >= 0 is ASR S,D, S < 0 is ASL -S,D; V set if bit 55
 * changes during the shift, L E U N Z by the standard definition, C kept */
static void emu_normf(dsp_core_t* dsp)
{
    const int D = dsp->cur_inst & 1;
    const uint32_t s = dsp->registers[emu_sss_reg[(dsp->cur_inst >> 1) & 7]] & BITMASK(24);
    uint32_t dest[3];
    uint16_t newsr;
    dest[0] = dsp->registers[DSP_REG_A2+D];
    dest[1] = dsp->registers[DSP_REG_A1+D];
    dest[2] = dsp->registers[DSP_REG_A0+D];
    if (s & (1u << 23)) {
        uint32_t n = (uint32_t)(-(int32_t)(s | 0xff000000u));
        newsr = dsp_asl56(dest, n > 56 ? 56 : (int)n) & ((1<<DSP_SR_V)|(1<<DSP_SR_L));
    } else {
        dsp_asr56(dest, s > 56 ? 56 : (int)s);
        newsr = 0;
    }
    dsp->registers[DSP_REG_A2+D] = dest[0];
    dsp->registers[DSP_REG_A1+D] = dest[1];
    dsp->registers[DSP_REG_A0+D] = dest[2];
    dsp->registers[DSP_REG_SR] &= BITMASK(16)-(1<<DSP_SR_V);
    dsp->registers[DSP_REG_SR] |= newsr;
    emu_acc_eunz(dsp, D);
}

/* A-6.65 MERGE S,D: {S[11:0], D[35:24]} -> D[47:24]; N, Z, V cleared */
static void emu_merge(dsp_core_t* dsp)
{
    const int reg = (dsp->cur_inst & 1) ? DSP_REG_B1 : DSP_REG_A1;
    const uint32_t s = dsp->registers[emu_sss_reg[(dsp->cur_inst >> 1) & 7]];
    const uint32_t v = ((s & BITMASK(12)) << 12) | (dsp->registers[reg] & BITMASK(12));
    dsp->registers[reg] = v;
    emu_sr_nz24(dsp, v);
}

/* A-6.45 INSERT: S2[(width-1):0] -> D[(offset+width-1):offset], width =
 * S1[17:12], offset = S1[5:0]; E U N Z standard, V and C cleared */
static void emu_insert_do(dsp_core_t* dsp, uint32_t co)
{
    const int D = dsp->cur_inst & 1;
    const uint32_t offset = co & BITMASK(6), width = (co >> 12) & BITMASK(6);
    const uint64_t src = dsp->registers[emu_qqq_reg[(dsp->cur_inst >> 4) & 7]] & BITMASK(24);
    uint64_t v = (uint64_t)emu_acc_get(dsp, D) & ((1ULL << 56) - 1);
    if (offset < 56 && width > 0) {
        const uint64_t m = width >= 64 ? ~0ULL : (1ULL << width) - 1;
        const uint64_t dm = (m << offset) & ((1ULL << 56) - 1);
        v = (v & ~dm) | ((src << offset) & dm);
    }
    emu_acc_set(dsp, D, (int64_t)(v << 8) >> 8);
    emu_acc_eunz(dsp, D);
    dsp->registers[DSP_REG_SR] &= BITMASK(16)-((1<<DSP_SR_V)|(1<<DSP_SR_C));
}

static void emu_insert_reg(dsp_core_t* dsp)
{
    emu_insert_do(dsp, dsp->registers[emu_sss_reg[(dsp->cur_inst >> 1) & 7]]);
}

static void emu_insert_imm(dsp_core_t* dsp)
{
    const uint32_t co = read_memory_p(dsp, dsp->pc+1);
    dsp->cur_inst_len++;
    emu_insert_do(dsp, co);
}

/* A-6.39 EXTRACT S1,S2,D and A-6.40 EXTRACTU S1,S2,D (the #CO forms' code
 * with the control word from S1) */
static void emu_extract_reg_do(dsp_core_t* dsp, bool is_unsigned)
{
    const uint32_t co = dsp->registers[emu_sss_reg[(dsp->cur_inst >> 1) & 7]];
    const uint32_t s = (dsp->cur_inst >> 4) & 1, d = dsp->cur_inst & 1;
    const uint32_t offset = co & BITMASK(6), width = (co >> 12) & BITMASK(6);
    const uint64_t v = (uint64_t)emu_acc_get(dsp, s) & ((1ULL << 56) - 1);
    uint64_t field = 0;
    if (width > 0 && offset < 56) {
        field = v >> offset;
        if (width < 64) {
            field &= (1ULL << width) - 1;
        }
        if (!is_unsigned && width < 64 && ((field >> (width - 1)) & 1)) {
            field |= ~0ULL << width;
        }
    }
    emu_acc_set(dsp, d, (int64_t)(field << 8) >> 8);
    emu_acc_eunz(dsp, d);
    dsp->registers[DSP_REG_SR] &= BITMASK(16)-((1<<DSP_SR_V)|(1<<DSP_SR_C));
}

static void emu_extract_reg(dsp_core_t* dsp)
{
    emu_extract_reg_do(dsp, false);
}

static void emu_extractu_reg(dsp_core_t* dsp)
{
    emu_extract_reg_do(dsp, true);
}

/* the multiplies with an operand that isn't a QQQ pair: the product
 * (source, as dsp_mul56 leaves it) to D, kk = 0 mpy, 1 mpyr, 2 mac, 3 macr
 * (the opcode's two low bits). Same arithmetic as emu_mac/macr/mpy/mpyr */
static void emu_mul_to(dsp_core_t* dsp, uint32_t *source, int d, int kk)
{
    uint32_t dest[3];
    uint16_t newsr = 0;
    if (kk & 2) {
        dest[0] = dsp->registers[DSP_REG_A2+d];
        dest[1] = dsp->registers[DSP_REG_A1+d];
        dest[2] = dsp->registers[DSP_REG_A0+d];
        newsr = dsp_add56(source, dest);
    } else {
        dest[0] = source[0];
        dest[1] = source[1];
        dest[2] = source[2];
    }
    if (kk & 1) {
        newsr ^= dsp_rnd56(dsp, dest);
    }
    dsp->registers[DSP_REG_A2+d] = dest[0];
    dsp->registers[DSP_REG_A1+d] = dest[1];
    dsp->registers[DSP_REG_A0+d] = dest[2];
    emu_ccr_update_e_u_n_z(dsp, dest[0], dest[1], dest[2]);
    dsp->registers[DSP_REG_SR] &= BITMASK(16)-(1<<DSP_SR_V);
    if (kk & 2) {
        dsp->registers[DSP_REG_SR] |= newsr & ((1<<DSP_SR_V)|(1<<DSP_SR_L));
    }
    emu_sm_acc(dsp, d, kk & 1); /* SM */
}

/* A-6.58/61/80/83 MAC, MACR, MPY, MPYR S,#n,D: S times 2^-n (Table A-32:
 * n 1-23, the constant $800000 >> n), QQ from Table A-27 (Y1 X0 Y0 X1).
 * n = 0 (no constant in the table) is S times 1: S in bits 47-24 */
static void emu_mul_n(dsp_core_t* dsp)
{
    static const int qq_reg[4] = {DSP_REG_Y1, DSP_REG_X0, DSP_REG_Y0, DSP_REG_X1};
    const uint32_t n = (dsp->cur_inst >> 8) & BITMASK(5);
    const int d = (dsp->cur_inst >> 3) & 1, k = (dsp->cur_inst >> 2) & 1;
    const uint32_t s = dsp->registers[qq_reg[(dsp->cur_inst >> 4) & 3]] & BITMASK(24);
    uint32_t source[3];
    if (n == 0) {
        int64_t p = (int64_t)((int32_t)(s << 8) >> 8) * (1LL << 24);
        if (k) p = -p;
        source[0] = (uint32_t)(p >> 48) & BITMASK(8);
        source[1] = (uint32_t)(p >> 24) & BITMASK(24);
        source[2] = (uint32_t)p & BITMASK(24);
    } else {
        dsp_mul56(s, 0x800000u >> n, source, k ? SIGN_MINUS : SIGN_PLUS);
    }
    emu_mul_to(dsp, source, d, dsp->cur_inst & 3);
}

/* A-6.59 MACI, A-6.62 MACRI, A-6.84 MPYRI: #xxxxxx times S, qq from Table
 * A-28 (X0 Y0 X1 Y1); MPYI is emu_mpyi */
static void emu_mul_imm(dsp_core_t* dsp)
{
    static const int qq_reg[4] = {DSP_REG_X0, DSP_REG_Y0, DSP_REG_X1, DSP_REG_Y1};
    const uint32_t xxxx = read_memory_p(dsp, dsp->pc+1);
    dsp->cur_inst_len++;
    const int d = (dsp->cur_inst >> 3) & 1, k = (dsp->cur_inst >> 2) & 1;
    uint32_t source[3];
    dsp_mul56(xxxx, dsp->registers[qq_reg[(dsp->cur_inst >> 4) & 3]], source,
              k ? SIGN_MINUS : SIGN_PLUS);
    emu_mul_to(dsp, source, d, dsp->cur_inst & 3);
}

/* A-6.60 MAC su/uu, A-6.81 MPY su/uu, A-6.32 DMAC: a QQQQ pair (Table
 * A-30), S1 signed or not, S2 unsigned (su, uu) or signed (DMACss); the
 * product is fractional like the signed one (shifted left once). DMAC adds
 * to D shifted right 24 bits (arithmetic). L E U N Z V standard, S, C kept */
static void emu_mul_mixed(dsp_core_t* dsp, int s1_signed, int s2_signed, int acc, int dmac)
{
    const int d = (dsp->cur_inst >> 5) & 1, k = (dsp->cur_inst >> 4) & 1;
    const int *pr = emu_qqqq_reg[dsp->cur_inst & 15];
    const uint32_t a = dsp->registers[pr[0]] & BITMASK(24), b = dsp->registers[pr[1]] & BITMASK(24);
    const int64_t s1 = s1_signed ? (int64_t)((int32_t)(a << 8) >> 8) : (int64_t)a;
    const int64_t s2 = s2_signed ? (int64_t)((int32_t)(b << 8) >> 8) : (int64_t)b;
    int64_t p = (s1 * s2) * 2;
    if (k) p = -p;
    int64_t r = p;
    if (acc) {
        int64_t base = emu_acc_get(dsp, d);
        if (dmac) base >>= 24;
        r = base + p;
        emu_sr_v56(dsp, r);
    } else {
        dsp->registers[DSP_REG_SR] &= BITMASK(16)-(1<<DSP_SR_V);
    }
    emu_acc_set(dsp, d, emu_wrap56(r));
    emu_acc_eunz(dsp, d);
}

static void emu_mac_su(dsp_core_t* dsp)
{
    /* s (bit 6): 0 su, 1 uu (Table A-40) */
    emu_mul_mixed(dsp, !((dsp->cur_inst >> 6) & 1), 0, 1, 0);
}

static void emu_mpy_su(dsp_core_t* dsp)
{
    emu_mul_mixed(dsp, !((dsp->cur_inst >> 6) & 1), 0, 0, 0);
}

static void emu_dmac(dsp_core_t* dsp)
{
    /* ss (bits 8 and 6, Table A-39): 00 ss, 10 su, 11 uu, 01 reserved */
    const int ss = (((dsp->cur_inst >> 8) & 1) << 1) | ((dsp->cur_inst >> 6) & 1);
    if (ss == 1) {
        emu_undefined(dsp);
        return;
    }
    emu_mul_mixed(dsp, ss != 3, ss == 0, 1, 1);
    if (ss == 0) {
        emu_sm_acc(dsp, (dsp->cur_inst >> 5) & 1, 0); /* SM: DMACss only */
    }
}

/* A-6.73 MOVE Y:(Rn+xxxx) <-> R: emu_move_x_long in Y */
static void emu_move_y_long(dsp_core_t* dsp)
{
    uint32_t xxxx = read_memory_p(dsp, dsp->pc+1);
    dsp->cur_inst_len++;

    int W = (dsp->cur_inst >> 6) & 1;
    uint32_t offreg = DSP_REG_R0 + ((dsp->cur_inst >> 8) & BITMASK(3));
    uint32_t numreg = dsp->cur_inst & BITMASK(6);
    uint32_t y_addr = (dsp->registers[offreg] + xxxx) & BITMASK(24);

    if (!W) {
        uint32_t value;
        if (numreg == DSP_REG_A || numreg == DSP_REG_B) {
            emu_pm_read_accu24(dsp, numreg, &value);
        } else {
            value = dsp->registers[numreg];
        }
        dsp56k_write_memory(dsp, DSP_SPACE_Y, y_addr, value);
    } else {
        dsp_write_reg(dsp, numreg, dsp56k_read_memory(dsp, DSP_SPACE_Y, y_addr));
    }
}

/* A-6.79 MOVEP [X or Y]:ea <-> Y:qq: emu_movep_x_qq with Y:qq */
static void emu_movep_y_qq(dsp_core_t* dsp)
{
    uint32_t y_addr = 0xffff80 + (dsp->cur_inst & BITMASK(6));
    uint32_t ea_mode = (dsp->cur_inst>>8) & BITMASK(6);
    uint32_t ea_space = (dsp->cur_inst>>6) & 1;
    uint32_t ea_addr;
    int retour = emu_calc_ea(dsp, ea_mode, &ea_addr);

    if (dsp->cur_inst & (1<<15)) {
        if (retour) {
            dsp56k_write_memory(dsp, DSP_SPACE_Y, y_addr, ea_addr);
        } else {
            dsp56k_write_memory(dsp, DSP_SPACE_Y, y_addr,
                dsp56k_read_memory(dsp, ea_space, ea_addr));
        }
    } else {
        dsp56k_write_memory(dsp, ea_space, ea_addr,
            dsp56k_read_memory(dsp, DSP_SPACE_Y, y_addr));
    }
}

/* A-6.79 MOVEP [X or Y]:qq <-> P:ea */
static void emu_movep_qq_p(dsp_core_t* dsp)
{
    const uint32_t qaddr = 0xffff80 + (dsp->cur_inst & BITMASK(6));
    const uint32_t space = (dsp->cur_inst >> 6) & 1;
    uint32_t paddr;
    emu_calc_ea(dsp, (dsp->cur_inst>>8) & BITMASK(6), &paddr);
    if (dsp->cur_inst & (1<<15)) {
        dsp56k_write_memory(dsp, space, qaddr, read_memory_p(dsp, paddr));
    } else {
        dsp56k_write_memory(dsp, DSP_SPACE_P, paddr, dsp56k_read_memory(dsp, space, qaddr));
    }
}

/* A-6.79 MOVEP X:qq <-> R, Y:qq <-> R (qq = bit 6 and bits 4-0) */
static void emu_movep_qq_reg(dsp_core_t* dsp)
{
    const uint32_t space = (dsp->cur_inst >> 5) & 1 ? DSP_SPACE_Y : DSP_SPACE_X;
    const uint32_t addr = 0xffff80 + ((((dsp->cur_inst >> 6) & 1) << 5) | (dsp->cur_inst & BITMASK(5)));
    const uint32_t numreg = (dsp->cur_inst >> 8) & BITMASK(6);
    uint32_t value, dummy;
    if (dsp->cur_inst & (1<<15)) {
        if (numreg == DSP_REG_A || numreg == DSP_REG_B) {
            emu_pm_read_accu24(dsp, numreg, &value);
        } else if (numreg == DSP_REG_SSH) {
            dsp_stack_pop(dsp, &value, &dummy);
        } else {
            value = dsp->registers[numreg];
            if (numreg == DSP_REG_SR) value |= dsp->sr_ext;
        }
        dsp56k_write_memory(dsp, space, addr, value);
    } else {
        value = dsp56k_read_memory(dsp, space, addr);
        if (numreg == DSP_REG_SR) emu_sr_note(dsp, value);
        value &= BITMASK(registers_mask[numreg]);
        dsp_write_reg(dsp, numreg, value);
    }
}

/* A-6.10 Bcc Rn, A-6.13 BRA Rn, A-6.20 BSR Rn, A-6.17 BScc (Rn, xxx, xxxx):
 * PC + displacement; the subroutine forms push the next PC and SR. Rn is
 * 16 bits here (the registers' width), so its target wraps in 16 */
static void emu_branch_to(dsp_core_t* dsp, uint32_t target, int sub)
{
    if (sub) {
        if (dsp->interrupt_state != DSP_INTERRUPT_LONG) {
            dsp_stack_push(dsp, dsp->pc+dsp->cur_inst_len, dsp->registers[DSP_REG_SR], 0);
        } else {
            dsp->interrupt_state = DSP_INTERRUPT_DISABLED;
        }
    }
    dsp->pc = target;
    dsp->cur_inst_len = 0;
}

static uint32_t emu_rn_target(dsp_core_t* dsp)
{
    return (dsp->pc + dsp->registers[DSP_REG_R0 + ((dsp->cur_inst >> 8) & BITMASK(3))]) & BITMASK(16);
}

static void emu_bcc_rn(dsp_core_t* dsp)
{
    if (emu_calc_cc(dsp, dsp->cur_inst & BITMASK(4))) {
        emu_branch_to(dsp, emu_rn_target(dsp), 0);
    }
}

static void emu_bra_rn(dsp_core_t* dsp)
{
    emu_branch_to(dsp, emu_rn_target(dsp), 0);
}

static void emu_bsr_rn(dsp_core_t* dsp)
{
    emu_branch_to(dsp, emu_rn_target(dsp), 1);
}

static void emu_bscc_rn(dsp_core_t* dsp)
{
    if (emu_calc_cc(dsp, dsp->cur_inst & BITMASK(4))) {
        emu_branch_to(dsp, emu_rn_target(dsp), 1);
    }
}

static void emu_bscc_long(dsp_core_t* dsp)
{
    uint32_t xxxx = read_memory_p(dsp, dsp->pc+1);
    dsp->cur_inst_len++;
    if (emu_calc_cc(dsp, dsp->cur_inst & BITMASK(4))) {
        emu_branch_to(dsp, (dsp->pc + xxxx) & BITMASK(24), 1);
    }
}

static void emu_bscc_imm(dsp_core_t* dsp)
{
    uint32_t xxx = (dsp->cur_inst & BITMASK(5))
                 + ((dsp->cur_inst & (BITMASK(4) << 6)) >> 1);
    if (emu_calc_cc(dsp, (dsp->cur_inst >> 12) & BITMASK(4))) {
        emu_branch_to(dsp, (dsp->pc + dsp_signextend(9, xxx)) & BITMASK(24), 1);
    }
}

/* A-6.15 BRKcc: out of the current DO loop: LA + 1 -> PC, LF from SSL,
 * LA and LC from the stack (ENDDO's pops) */
static void emu_brkcc(dsp_core_t* dsp)
{
    if (emu_calc_cc(dsp, dsp->cur_inst & BITMASK(4))) {
        const uint32_t la = dsp->registers[DSP_REG_LA];
        emu_enddo(dsp);
        dsp->pc = (la + 1) & BITMASK(16);
        dsp->cur_inst_len = 0;
    }
}

/* the bit tests with a jump or branch: brclr/brset (A-6.14/16) and
 * bsclr/bsset (A-6.18/21) are PC relative, jclr/jset (A-6.47/51) and
 * jsclr/jsset (A-6.50/53) absolute; the operand from ea (A-19 modes,
 * updated either way), aa, pp, qq or a register */
enum { EMU_BIT_EA, EMU_BIT_AA, EMU_BIT_PP, EMU_BIT_QQ, EMU_BIT_REG };

static uint32_t emu_bit_operand(dsp_core_t* dsp, int form)
{
    const uint32_t f = (dsp->cur_inst >> 8) & BITMASK(6);
    const uint32_t space = (dsp->cur_inst >> 6) & 1;
    uint32_t addr, value;
    switch (form) {
    case EMU_BIT_EA:
        emu_calc_ea(dsp, f, &addr);
        return dsp56k_read_memory(dsp, space, addr);
    case EMU_BIT_AA:
        return dsp56k_read_memory(dsp, space, f);
    case EMU_BIT_PP:
        return dsp56k_read_memory(dsp, space, 0xffffc0 + f);
    case EMU_BIT_QQ:
        return dsp56k_read_memory(dsp, space, 0xffff80 + f);
    default:
        if (f == DSP_REG_A || f == DSP_REG_B) {
            emu_pm_read_accu24(dsp, f, &value);
        } else {
            value = dsp->registers[f];
        }
        return value;
    }
}

static void emu_bit_jump(dsp_core_t* dsp, int form, int want_set, int rel, int sub)
{
    const uint32_t xxxx = read_memory_p(dsp, dsp->pc+1);
    const uint32_t numbit = dsp->cur_inst & BITMASK(5);
    const uint32_t value = emu_bit_operand(dsp, form);
    dsp->cur_inst_len = 2;
    if (((value >> numbit) & 1) == (uint32_t)want_set) {
        const uint32_t target = rel ? (dsp->pc + xxxx) & BITMASK(24) : xxxx;
        if (sub) {
            dsp_stack_push(dsp, dsp->pc+2, dsp->registers[DSP_REG_SR], 0);
        }
        dsp->pc = target;
        dsp->cur_inst_len = 0;
    }
}

static void emu_brclr_ea(dsp_core_t* dsp) { emu_bit_jump(dsp, EMU_BIT_EA, 0, 1, 0); }
static void emu_brclr_aa(dsp_core_t* dsp) { emu_bit_jump(dsp, EMU_BIT_AA, 0, 1, 0); }
static void emu_brclr_qq(dsp_core_t* dsp) { emu_bit_jump(dsp, EMU_BIT_QQ, 0, 1, 0); }
static void emu_brset_ea(dsp_core_t* dsp) { emu_bit_jump(dsp, EMU_BIT_EA, 1, 1, 0); }
static void emu_brset_aa(dsp_core_t* dsp) { emu_bit_jump(dsp, EMU_BIT_AA, 1, 1, 0); }
static void emu_brset_qq(dsp_core_t* dsp) { emu_bit_jump(dsp, EMU_BIT_QQ, 1, 1, 0); }
static void emu_bsclr_ea(dsp_core_t* dsp) { emu_bit_jump(dsp, EMU_BIT_EA, 0, 1, 1); }
static void emu_bsclr_aa(dsp_core_t* dsp) { emu_bit_jump(dsp, EMU_BIT_AA, 0, 1, 1); }
static void emu_bsclr_pp(dsp_core_t* dsp) { emu_bit_jump(dsp, EMU_BIT_PP, 0, 1, 1); }
static void emu_bsclr_qq(dsp_core_t* dsp) { emu_bit_jump(dsp, EMU_BIT_QQ, 0, 1, 1); }
static void emu_bsclr_reg(dsp_core_t* dsp) { emu_bit_jump(dsp, EMU_BIT_REG, 0, 1, 1); }
static void emu_bsset_ea(dsp_core_t* dsp) { emu_bit_jump(dsp, EMU_BIT_EA, 1, 1, 1); }
static void emu_bsset_aa(dsp_core_t* dsp) { emu_bit_jump(dsp, EMU_BIT_AA, 1, 1, 1); }
static void emu_bsset_pp(dsp_core_t* dsp) { emu_bit_jump(dsp, EMU_BIT_PP, 1, 1, 1); }
static void emu_bsset_qq(dsp_core_t* dsp) { emu_bit_jump(dsp, EMU_BIT_QQ, 1, 1, 1); }
static void emu_bsset_reg(dsp_core_t* dsp) { emu_bit_jump(dsp, EMU_BIT_REG, 1, 1, 1); }
static void emu_jclr_qq(dsp_core_t* dsp) { emu_bit_jump(dsp, EMU_BIT_QQ, 0, 0, 0); }
static void emu_jset_qq(dsp_core_t* dsp) { emu_bit_jump(dsp, EMU_BIT_QQ, 1, 0, 0); }
static void emu_jsclr_qq(dsp_core_t* dsp) { emu_bit_jump(dsp, EMU_BIT_QQ, 0, 0, 1); }
static void emu_jsset_qq(dsp_core_t* dsp) { emu_bit_jump(dsp, EMU_BIT_QQ, 1, 0, 1); }

/* A-6.11/12/19/22 BCHG, BCLR, BSET, BTST on X:qq / Y:qq: C the bit before */
static void emu_bit_qq(dsp_core_t* dsp, int op /* 0 clr 1 set 2 chg 3 tst */)
{
    const uint32_t space = (dsp->cur_inst >> 6) & 1;
    const uint32_t addr = 0xffff80 + ((dsp->cur_inst >> 8) & BITMASK(6));
    const uint32_t numbit = dsp->cur_inst & BITMASK(5);
    uint32_t value = dsp56k_read_memory(dsp, space, addr);
    const uint32_t c = (value >> numbit) & 1;
    if (op != 3) {
        if (op == 0) value &= ~(1u << numbit);
        else if (op == 1) value |= 1u << numbit;
        else value ^= 1u << numbit;
        dsp56k_write_memory(dsp, space, addr, value);
    }
    dsp->registers[DSP_REG_SR] &= BITMASK(16)-(1<<DSP_SR_C);
    dsp->registers[DSP_REG_SR] |= c<<DSP_SR_C;
}

static void emu_bclr_qq(dsp_core_t* dsp) { emu_bit_qq(dsp, 0); }
static void emu_bset_qq(dsp_core_t* dsp) { emu_bit_qq(dsp, 1); }
static void emu_bchg_qq(dsp_core_t* dsp) { emu_bit_qq(dsp, 2); }
static void emu_btst_qq(dsp_core_t* dsp) { emu_bit_qq(dsp, 3); }

/* A-6.54 LRA Rn,D and LRA xxxx,D: PC + displacement into D (Table A-31) */
static void emu_lra_rn(dsp_core_t* dsp)
{
    const uint32_t r = dsp->registers[DSP_REG_R0 + ((dsp->cur_inst >> 8) & BITMASK(3))];
    dsp_write_reg(dsp, dsp->cur_inst & BITMASK(5), (dsp->pc + r) & BITMASK(24));
}

static void emu_lra_long(dsp_core_t* dsp)
{
    const uint32_t xxxx = read_memory_p(dsp, dsp->pc+1);
    dsp->cur_inst_len++;
    dsp_write_reg(dsp, dsp->cur_inst & BITMASK(5), (dsp->pc + xxxx) & BITMASK(24));
}

/* A-6.35 DOR [X or Y]:ea / :aa, label: emu_do_ea / emu_do_aa with LA PC relative */
static void emu_dor_mem(dsp_core_t* dsp, int aa)
{
    uint32_t memspace, addr;
    const uint32_t xxxx = read_memory_p(dsp, dsp->pc+1);

    dsp_stack_push(dsp, dsp->registers[DSP_REG_LA], dsp->registers[DSP_REG_LC], 0);
    dsp->registers[DSP_REG_LA] = (dsp->pc + xxxx) & BITMASK(16);
    dsp->cur_inst_len++;
    dsp_stack_push(dsp, dsp->pc+dsp->cur_inst_len, dsp->registers[DSP_REG_SR], 0);
    dsp->registers[DSP_REG_SR] |= (1<<DSP_SR_LF);

    memspace = (dsp->cur_inst>>6) & 1;
    if (aa) {
        addr = (dsp->cur_inst>>8) & BITMASK(6);
    } else {
        emu_calc_ea(dsp, (dsp->cur_inst>>8) & BITMASK(6), &addr);
    }
    dsp->registers[DSP_REG_LC] = dsp56k_read_memory(dsp, memspace, addr) & BITMASK(16);

    dsp->instr_cycle += 4;
    emu_do_skip_if_zero(dsp);
}

static void emu_dor_ea(dsp_core_t* dsp)
{
    emu_dor_mem(dsp, 0);
}

static void emu_dor_aa(dsp_core_t* dsp)
{
    emu_dor_mem(dsp, 1);
}

/* A-6.92-97 the program cache ops: no cache here, so nothing but their ea
 * update and extension word */
static void emu_pcache_ea(dsp_core_t* dsp)
{
    uint32_t addr;
    emu_calc_ea(dsp, (dsp->cur_inst>>8) & BITMASK(6), &addr);
}

static void emu_pcache_rel(dsp_core_t* dsp)
{
    dsp->cur_inst_len++;
}

/* A-6.41 IFcc, A-6.42 IFcc.U: the parallel ALU op if cc, with the CCR it
 * sets (.U) or the one before */
static void emu_ifcc(dsp_core_t* dsp)
{
    if (!emu_calc_cc(dsp, (dsp->cur_inst >> 8) & BITMASK(4))) {
        return;
    }
    const uint32_t ccr = dsp->registers[DSP_REG_SR] & BITMASK(8);
    opcodes_alu[dsp->cur_inst & BITMASK(8)](dsp);
    emu_alu_sm(dsp, dsp->cur_inst & BITMASK(8));
    if (!(dsp->cur_inst & (1 << 12))) {
        dsp->registers[DSP_REG_SR] = (dsp->registers[DSP_REG_SR] & ~BITMASK(8)) | ccr;
    }
}
