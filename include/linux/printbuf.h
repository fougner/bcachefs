/* SPDX-License-Identifier: LGPL-2.1+ */
/* Copyright (C) 2022 Kent Overstreet */

#ifndef _LINUX_PRINTBUF_H
#define _LINUX_PRINTBUF_H

/*
 * Printbufs: Simple strings for printing to, with optional heap allocation
 *
 * This code has provisions for use in userspace, to aid in making other code
 * portable between kernelspace and userspace.
 *
 * Basic example:
 *   struct printbuf buf = PRINTBUF;
 *
 *   prt_printf(&buf, "foo=");
 *   foo_to_text(&buf, foo);
 *   printk("%s", buf.buf);
 *   printbuf_exit(&buf);
 *
 * Or
 *   struct printbuf buf = PRINTBUF_EXTERN(char_buf, char_buf_size)
 *
 * We can now write pretty printers instead of writing code that dumps
 * everything to the kernel log buffer, and then those pretty-printers can be
 * used by other code that outputs to kernel log, sysfs, debugfs, etc.
 *
 * Memory allocation: Outputing to a printbuf may allocate memory. This
 * allocation is done with GFP_KERNEL, by default: use the newer
 * memalloc_*_(save|restore) functions as needed.
 *
 * Since no equivalent yet exists for GFP_ATOMIC/GFP_NOWAIT, memory allocations
 * will be done with GFP_NOWAIT if printbuf->atomic is nonzero.
 *
 * It's allowed to grab the output buffer and free it later with kfree() instead
 * of using printbuf_exit(), if the user just needs a heap allocated string at
 * the end.
 *
 * Memory allocation failures: We don't return errors directly, because on
 * memory allocation failure we usually don't want to bail out and unwind - we
 * want to print what we've got, on a best-effort basis. But code that does want
 * to return -ENOMEM may check printbuf.allocation_failure.
 *
 * Indenting, tabstops:
 *
 * To aid is writing multi-line pretty printers spread across multiple
 * functions, printbufs track the current indent level.
 *
 * printbuf_indent_push() and printbuf_indent_pop() increase and decrease the current indent
 * level, respectively.
 *
 * To use tabstops, set printbuf->tabstops[]; they are in units of spaces, from
 * start of line. Once set, prt_tab() will output spaces up to the next tabstop.
 * prt_tab_rjust() will also advance the current line of text up to the next
 * tabstop, but it does so by shifting text since the previous tabstop up to the
 * next tabstop - right justifying it.
 *
 * Make sure you use prt_newline() instead of \n in the format string for indent
 * level and tabstops to work corretly.
 *
 * Output units: printbuf->units exists to tell pretty-printers how to output
 * numbers: a raw value (e.g. directly from a superblock field), as bytes, or as
 * human readable bytes. prt_units() obeys it.
 */

#include <linux/kernel.h>
#include <linux/string.h>

enum printbuf_si {
	PRINTBUF_UNITS_2,	/* use binary powers of 2^10 */
	PRINTBUF_UNITS_10,	/* use powers of 10^3 (standard SI) */
};

#define PRINTBUF_INLINE_TABSTOPS	4

struct printbuf {
	char			*buf;
	unsigned		size;
	unsigned		pos;
	unsigned		last_newline;
	unsigned		last_field;
	unsigned		indent;
	/*
	 * If nonzero, allocations will be done with GFP_ATOMIC:
	 */
	u8			atomic;
	bool			allocation_failure:1;
	bool			heap_allocated:1;
	enum printbuf_si	si_units:1;
	bool			human_readable_units:1;
	bool			has_indent_or_tabstops:1;
	bool			suppress_indent_tabstop_handling:1;
	u8			nr_tabstops;

	/*
	 * Do not modify directly: use printbuf_tabstop_add(),
	 * printbuf_tabstop_get()
	 */
	u8			cur_tabstop;
	u8			_tabstops[PRINTBUF_INLINE_TABSTOPS];
};

int printbuf_make_room(struct printbuf *, unsigned);
const char *printbuf_str(const struct printbuf *);
void printbuf_exit(struct printbuf *);

void printbuf_tabstops_reset(struct printbuf *);
void printbuf_tabstop_pop(struct printbuf *);
int printbuf_tabstop_push(struct printbuf *, unsigned);

void printbuf_indent_add(struct printbuf *, unsigned);
void printbuf_indent_sub(struct printbuf *, unsigned);

void prt_newline(struct printbuf *);
void prt_tab(struct printbuf *);
void prt_tab_rjust(struct printbuf *);

void prt_bytes_indented(struct printbuf *, const char *, unsigned);
void prt_human_readable_u64(struct printbuf *, u64);
void prt_human_readable_s64(struct printbuf *, s64);
void prt_units_u64(struct printbuf *, u64);
void prt_units_s64(struct printbuf *, s64);

/* Initializer for a heap allocated printbuf: */
#define PRINTBUF ((struct printbuf) { .heap_allocated = true })

/* Initializer a printbuf that points to an external buffer: */
#define PRINTBUF_EXTERN(_buf, _size)			\
((struct printbuf) {					\
	.buf	= _buf,					\
	.size	= _size,				\
})

/*
 * Returns size remaining of output buffer:
 */
static inline unsigned printbuf_remaining_size(struct printbuf *out)
{
	return out->pos < out->size ? out->size - out->pos : 0;
}

/*
 * Returns number of characters we can print to the output buffer - i.e.
 * excluding the terminating nul:
 */
static inline unsigned printbuf_remaining(struct printbuf *out)
{
	return out->pos < out->size ? out->size - out->pos - 1 : 0;
}

static inline unsigned printbuf_written(struct printbuf *out)
{
	return out->size ? min(out->pos, out->size - 1) : 0;
}

/*
 * Returns true if output was truncated:
 */
static inline bool printbuf_overflowed(struct printbuf *out)
{
	return out->pos >= out->size;
}

static inline void printbuf_nul_terminate(struct printbuf *out)
{
	printbuf_make_room(out, 1);

	if (out->pos < out->size)
		out->buf[out->pos] = 0;
	else if (out->size)
		out->buf[out->size - 1] = 0;
}

/* Doesn't call printbuf_make_room(), doesn't nul terminate: */
static inline void __prt_char_reserved(struct printbuf *out, char c)
{
	if (printbuf_remaining(out))
		out->buf[out->pos] = c;
	out->pos++;
}

/* Doesn't nul terminate: */
static inline void __prt_char(struct printbuf *out, char c)
{
	printbuf_make_room(out, 1);
	__prt_char_reserved(out, c);
}

static inline void prt_char(struct printbuf *out, char c)
{
	__prt_char(out, c);
	printbuf_nul_terminate(out);
}

static inline void __prt_chars_reserved(struct printbuf *out, char c, unsigned n)
{
	unsigned i, can_print = min(n, printbuf_remaining(out));

	for (i = 0; i < can_print; i++)
		out->buf[out->pos++] = c;
	out->pos += n - can_print;
}

static inline void prt_chars(struct printbuf *out, char c, unsigned n)
{
	printbuf_make_room(out, n);
	__prt_chars_reserved(out, c, n);
	printbuf_nul_terminate(out);
}

static inline void prt_bytes(struct printbuf *out, const void *b, unsigned n)
{
	unsigned i, can_print;

	printbuf_make_room(out, n);

	can_print = min(n, printbuf_remaining(out));

	for (i = 0; i < can_print; i++)
		out->buf[out->pos++] = ((char *) b)[i];
	out->pos += n - can_print;

	printbuf_nul_terminate(out);
}

static inline void prt_str(struct printbuf *out, const char *str)
{
	prt_bytes(out, str, strlen(str));
}

static inline void prt_str_indented(struct printbuf *out, const char *str)
{
	prt_bytes_indented(out, str, strlen(str));
}

static inline void prt_hex_byte(struct printbuf *out, u8 byte)
{
	printbuf_make_room(out, 2);
	__prt_char_reserved(out, hex_asc_hi(byte));
	__prt_char_reserved(out, hex_asc_lo(byte));
	printbuf_nul_terminate(out);
}

static inline void prt_hex_byte_upper(struct printbuf *out, u8 byte)
{
	printbuf_make_room(out, 2);
	__prt_char_reserved(out, hex_asc_upper_hi(byte));
	__prt_char_reserved(out, hex_asc_upper_lo(byte));
	printbuf_nul_terminate(out);
}

/**
 * printbuf_reset - re-use a printbuf without freeing and re-initializing it:
 */
static inline void printbuf_reset(struct printbuf *buf)
{
	buf->pos		= 0;
	buf->allocation_failure	= 0;
	buf->indent		= 0;
	buf->nr_tabstops	= 0;
	buf->cur_tabstop	= 0;
}

/**
 * printbuf_atomic_inc - mark as entering an atomic section
 */
static inline void printbuf_atomic_inc(struct printbuf *buf)
{
	buf->atomic++;
}

/**
 * printbuf_atomic_inc - mark as leaving an atomic section
 */
static inline void printbuf_atomic_dec(struct printbuf *buf)
{
	buf->atomic--;
}

/*
 * This is used for the %pf(%p) sprintf format extension, where we pass a pretty
 * printer and arguments to the pretty-printer to sprintf
 *
 * Instead of passing a pretty-printer function to sprintf directly, we pass it
 * a pointer to a struct call_pp, so that sprintf can check that the magic
 * number is present, which in turn ensures that the CALL_PP() macro has been
 * used in order to typecheck the arguments to the pretty printer function
 *
 * Example usage:
 *   sprintf("%pf(%p)", CALL_PP(prt_bdev, bdev));
 */
struct call_pp {
	unsigned long	magic;
	void		*fn;
};

#define PP_TYPECHECK(fn, ...)					\
	({ while (0) fn((struct printbuf *) NULL, ##__VA_ARGS__); })

#define CALL_PP_MAGIC		(unsigned long) 0xce0b92d22f6b6be4

#define CALL_PP(fn, ...)					\
	(PP_TYPECHECK(fn, ##__VA_ARGS__),			\
	 &((struct call_pp) { CALL_PP_MAGIC, fn })), ##__VA_ARGS__

#endif /* _LINUX_PRINTBUF_H */
