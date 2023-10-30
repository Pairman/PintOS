/**
 * fixed_point.h is a 32-bit fixed_point numeric library.
 *
 * The datatype fixed_t is a typedef of int
 * which has FP_SHIFT_BITS fractional bits.
*/
#ifndef __FIXED_POINT_H
#define __FIXED_POINT_H

/* Max and min comparison. */
#define MAX(a, b) (((a) > (b)) ? (a) : (b))
#define MIN(a, b) (((a) < (b)) ? (a) : (b))

/* Datatype for fixed point number. */
typedef int fixed_t;

/* Number of LSB fractional bits. */
#define FP_SHIFT_BITS 16

/* Convert from int to fixed_t, no overflow protection guaranteed. */
#define FP_FIX(a) ((a) << FP_SHIFT_BITS)

/* Extract the integer part of fixed_t. */
#define FP_INT(a) ((a) >> FP_SHIFT_BITS)

/* Round fixed_t to int. */
#define FP_RND(a) ((int)(((a) >= 0) ? \
		  (((a) + (1 << (FP_SHIFT_BITS - 1))) >> FP_SHIFT_BITS) : \
		  (((a) - (1 << (FP_SHIFT_BITS - 1))) >> FP_SHIFT_BITS)))

/* Add two fixed_t. */
#define FP_ADD(a, b) ((a) + (b))

/* Add a fixed_t and an int. */
#define FP_ADDI(a, n) ((a) + ((n) << FP_SHIFT_BITS))
#define FP_IADD(a, n) ((n) + ((a) << FP_SHIFT_BITS))

/* Add two int. */
#define FP_IADDI(m, n) (((m) << FP_SHIFT_BITS) + ((n) << FP_SHIFT_BITS))

/* Substract two fixed_t. */
#define FP_SUB(a, b) ((a) - (b))

/* Substract an int from a fixed_t. */
#define FP_SUBI(a, n) ((a) - ((n) << FP_SHIFT_BITS))

/* Substract a fixed_t from an int. */
#define FP_ISUB(n, a) (((n) << FP_SHIFT_BITS) - (a))

/* Substract two int. */
#define FP_ISUBI(m, n) (((m) << FP_SHIFT_BITS) - ((n) << FP_SHIFT_BITS))

/* Multiply two fixed_t. */
#define FP_MUL(a, b) ((fixed_t)(((int64_t)(a)) * (b) >> FP_SHIFT_BITS))

/* Multiply a fixed_t by an int. */
#define FP_MULI(a, n) ((a) * (n))
#define FP_IMUL(n, a) ((n) * (a))

/* Multiply two int. */
#define FP_IMULI(m, n) (((m) << FP_SHIFT_BITS) * (n))

/* Divide two fixed_t. */
#define FP_DIV(a, b) ((fixed_t)((((int64_t)(a)) << FP_SHIFT_BITS) / (b)))

/* Divide a fixed_t by an int. */
#define FP_DIVI(a, n) ((a) / (n))

/* Divide an int by a fixed_t. */
#define FP_IDIV(n, a) ((fixed_t)((((int64_t)(n)) << (2 * FP_SHIFT_BITS)) / (a)))

/* Divide two int. */
#define FP_IDIVI(m, n) ((fixed_t)((((int64_t)(m)) << (2 * FP_SHIFT_BITS)) / \
			          ((n) << FP_SHIFT_BITS)))

#endif /* __FIXED_POINT_H */