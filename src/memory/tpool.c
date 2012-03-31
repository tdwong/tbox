/*!The Treasure Box Library
 * 
 * TBox is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 * 
 * TBox is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 * 
 * You should have received a copy of the GNU Lesser General Public License
 * along with TBox; 
 * If not, see <a href="http://www.gnu.org/licenses/"> http://www.gnu.org/licenses/</a>
 * 
 * Copyright (C) 2009 - 2012, ruki All rights reserved.
 *
 * \author		ruki
 * \file		tpool.c
 *
 */

/* ///////////////////////////////////////////////////////////////////////
 * includes
 */
#include "tpool.h"
#include "../libc/libc.h"
#include "../math/math.h"
#include "../utils/utils.h"

/* ///////////////////////////////////////////////////////////////////////
 * macros
 */

// the magic number
#define TB_TPOOL_MAGIC 							(0xdead)

// the align maxn
#define TB_TPOOL_ALIGN_MAXN 					(64)

// the block maxn in the chunk
//#define TB_TPOOL_BLOCK_MAXN 					(sizeof(tb_size_t) << 3)
#if TB_CPU_BITBYTE == 8
# 	define TB_TPOOL_BLOCK_MAXN 					(64)
#elif TB_CPU_BITBYTE == 4
# 	define TB_TPOOL_BLOCK_MAXN 					(32)
#endif

/* ///////////////////////////////////////////////////////////////////////
 * types
 */

// the tpool info type
#ifdef TB_DEBUG
typedef struct __tb_tpool_info_t
{
	// the used size
	tb_size_t 			used;

	// the peak size
	tb_size_t 			peak;

	// the need size
	tb_size_t 			need;

	// the real size
	tb_size_t 			real;

	// the fail count
	tb_size_t 			fail;

	// the pred count
	tb_size_t 			pred;

	// the aloc count
	tb_size_t 			aloc;

}tb_tpool_info_t;
#endif

/* the tiny pool type
 *
 * pool: |---------|-----------------|-----------------------------------------------|
 *           head          used                            data                         
 *                 |--------|--------|
 *                    head     body
 *
 * used:
 * head: |---------------------------|-----------------------------|--- ... ---------|
 *             chunk0(32|64 bits)              chunk1                  chunki
 *       |---------------------------|
 *             sizeof(tb_size_t)
 *
 *       |-------|----|--------|-----|
 *                block0,1.. blocki   <= for big endian
 *
 *       |-----------|-----|------|--|
 *                blocki.. block1,0   <= for little endian <= use it now
 *
 * 
 * body: |---------------------------|-----------------------------|--- ... ---------|
 *             chunk0(32|64 bits)              chunk1                  chunki
 *       |---------------------------|
 *             sizeof(tb_size_t)
 *
 *       |------||||||||||||---||||--|
 *                block0,1.. blocki   <= for big endian
 *
 *       |------||||||||||||---||||--|
 *                blocki.. block1,0   <= for little endian <= use it now
 *
 *
 * data: |----------------------------------------------------|----|--- ... ---------|
 *                         chunk0(32|64 blocks)                         chunki
 *       |--------------|                       |-------------|
 *            block0                                blocki          
 *       |-----|-----|------|--------- ... -----|------|------|----|--- ... ---------|
 *        step0 step1 step2                       stepi   ...
 *
 * note:
 * 1. align bytes <= 64
 * 2. alloc bytes <= (32|64) * 16 == 512|1024 for one chunk
 * 3. step bytes == max(align, 16)
 */
typedef struct __tb_tpool_t
{
	// the magic 
	tb_size_t 			magic 	: 16;

	// the align
	tb_size_t 			align 	: 7;

	// the step
	tb_size_t 			step 	: 7;

	// the full
	tb_size_t 			full 	: 1;

	// the head
	tb_size_t* 			head;

	// the body
	tb_size_t* 			body;

	// the maxn
	tb_size_t 			maxn;

	// the pred
	tb_size_t 			pred;

	// the data
	tb_byte_t* 			data;

	// the info
#ifdef TB_DEBUG
	tb_tpool_info_t 	info;
#endif

}tb_tpool_t;

/* ///////////////////////////////////////////////////////////////////////
 * implemention
 */
static tb_pointer_t tb_tpool_malloc_pred(tb_tpool_t* tpool, tb_size_t size)
{
	return TB_NULL;
}
static tb_pointer_t tb_tpool_malloc_find(tb_tpool_t* tpool, tb_size_t size)
{
	// init
	tb_size_t* 	head = tpool->head;
	tb_size_t* 	body = tpool->body;
	tb_size_t 	maxn = tpool->maxn;
	tb_byte_t* 	data = TB_NULL;

	// find the free chunk
	tb_size_t* 	p = body;
	tb_size_t* 	e = body + maxn;
//	while (p < e && *p == 0xffffffff) p++;
//	while (p < e && *p == 0xffffffffffffffffL) p++;
	while (p < e && !(*p + 1)) p++;
	tb_check_return_val(p < e, TB_NULL);
	head += (p - body);
	body = p;

	// the free block bit in the chunk
	// e.g. 3 blocks => bits: 111
	tb_size_t 	bitn = tb_align(size, tpool->step) / tpool->step;
	tb_size_t 	bits = ((tb_size_t)1 << bitn) - 1;
	tb_assert_and_check_return_val(bitn && bitn <= TB_TPOOL_BLOCK_MAXN, TB_NULL);

	// find the free bit index for the enough space in the little-endian sort
#if 0
	tb_size_t 	blki = 0;
	tb_size_t 	blkn = TB_TPOOL_BLOCK_MAXN;
	tb_size_t 	blks = ~*body;
	while (((((blks >> (TB_TPOOL_BLOCK_MAXN - blkn)) & bits) != bits) && blkn--)) ;
	blki = TB_TPOOL_BLOCK_MAXN - blkn;
#else
	tb_size_t 	blki = 0;
	tb_size_t 	blkn = TB_TPOOL_BLOCK_MAXN;
	tb_size_t 	blks = ~*body;
	while (blki < TB_TPOOL_BLOCK_MAXN)
	{
		if (((blks >> (blki + 0)) & bits) == bits) { blki += 0; break; }
		if (((blks >> (blki + 1)) & bits) == bits) { blki += 1; break; }
		if (((blks >> (blki + 2)) & bits) == bits) { blki += 2; break; }
		if (((blks >> (blki + 3)) & bits) == bits) { blki += 3; break; }
		if (((blks >> (blki + 4)) & bits) == bits) { blki += 4; break; }
		if (((blks >> (blki + 5)) & bits) == bits) { blki += 5; break; }
		if (((blks >> (blki + 6)) & bits) == bits) { blki += 6; break; }
		if (((blks >> (blki + 7)) & bits) == bits) { blki += 7; break; }
		blki += 8;
	}
#endif

	// no space?
	tb_check_return_val(blkn, TB_NULL);

	// alloc it
	data = tpool->data + ((body - tpool->body) * TB_TPOOL_BLOCK_MAXN + blki) * tpool->step;
	*body |= bits << blki;
	*head |= (tb_size_t)1 << blki;

	// ok
	return data;
}
/* ///////////////////////////////////////////////////////////////////////
 * interfaces
 */
tb_handle_t tb_tpool_init(tb_byte_t* data, tb_size_t size, tb_size_t align)
{
	// check
	tb_assert_and_check_return_val(data && size, TB_NULL);
	tb_assert_static(TB_TPOOL_BLOCK_MAXN == sizeof(tb_size_t) << 3);

	// align
	align = align? tb_align_pow2(align) : TB_CPU_BITBYTE;
	align = tb_max(align, TB_CPU_BITBYTE);
	tb_assert_and_check_return_val(align <= TB_TPOOL_ALIGN_MAXN, TB_NULL);

	// align data
	tb_size_t byte = (tb_size_t)tb_align((tb_size_t)data, align) - (tb_size_t)data;
	tb_assert_and_check_return_val(size >= byte, TB_NULL);
	size -= byte;
	data += byte;
	tb_assert_and_check_return_val(size, TB_NULL);

	// init data
	tb_memset(data, 0, size);

	// init tpool
	tb_tpool_t* tpool = data;

	// init magic
	tpool->magic = TB_TPOOL_MAGIC;

	// init align
	tpool->align = align;

	// init step
	tpool->step = tb_max(tpool->align, 16);

	// init full
	tpool->full = 0;

	// init head
	tpool->head = (tb_size_t*)tb_align((tb_size_t)&tpool[1], tpool->align);
	tb_assert_and_check_return_val(data + size > (tb_byte_t*)tpool->head, TB_NULL);
	tb_assert_and_check_return_val(!(((tb_size_t)tpool->head) & (TB_CPU_BITBYTE - 1)), TB_NULL);

	/* init maxn
	 *
	 * head + body + data < left
	 * sizeof(tb_size_t) * maxn * 2 + maxn * sizeof(tb_size_t) * 8 * step < left
	 * sizeof(tb_size_t) * maxn * 2 * (1 + 4 * step) < left
	 * maxn < left / ((1 + 4 * step) * 2 * sizeof(tb_size_t))
	 */
	tpool->maxn = (data + size - (tb_byte_t*)tpool->head) / ((1 + (tpool->step << 2)) * (sizeof(tb_size_t) << 1));
	tb_assert_and_check_return_val(tpool->maxn, TB_NULL);

	// init body
	tpool->body = tpool->head + tpool->maxn;
	tb_assert_and_check_return_val(data + size > (tb_byte_t*)tpool->body, TB_NULL);
	tb_assert_and_check_return_val(!(((tb_size_t)tpool->body) & (TB_CPU_BITBYTE - 1)), TB_NULL);

	// init data
	tpool->data = (tb_byte_t*)tb_align((tb_size_t)(tpool->body + tpool->maxn), tpool->align);
	tb_assert_and_check_return_val(data + size > tpool->data, TB_NULL);
	tb_assert_and_check_return_val(tpool->maxn * tpool->step * TB_TPOOL_BLOCK_MAXN <= (data + size - tpool->data), TB_NULL);

	// init pred => the first chunk index at used
	tpool->pred = 0;

	// init info
#ifdef TB_DEBUG
	tpool->info.used = 0;
	tpool->info.peak = 0;
	tpool->info.need = 0;
	tpool->info.real = 0;
	tpool->info.fail = 0;
	tpool->info.pred = 0;
	tpool->info.aloc = 0;
#endif

	// ok
	return ((tb_handle_t)tpool);
}
tb_void_t tb_tpool_exit(tb_handle_t handle)
{
	// check 
	tb_tpool_t* tpool = (tb_tpool_t*)handle;
	tb_assert_and_check_return(tpool && tpool->magic == TB_TPOOL_MAGIC);

	// clear body
	tb_tpool_clear(handle);

	// clear head
	tb_memset(tpool, 0, sizeof(tb_tpool_t));
}
tb_void_t tb_tpool_clear(tb_handle_t handle)
{
	// check 
	tb_tpool_t* tpool = (tb_tpool_t*)handle;
	tb_assert_and_check_return(tpool && tpool->magic == TB_TPOOL_MAGIC);

	// clear data
	if (tpool->data) tb_memset(tpool->data, 0, tpool->maxn * tpool->step * TB_TPOOL_BLOCK_MAXN);
	
	// clear head
	if (tpool->head) tb_memset(tpool->head, 0, tpool->maxn * sizeof(tb_size_t));

	// clear body
	if (tpool->body) tb_memset(tpool->body, 0, tpool->maxn * sizeof(tb_size_t));

	// reinit pred
	tpool->pred = 0;
	
	// reinit full
	tpool->full = 0;
	
	// reinit info
#ifdef TB_DEBUG
	tpool->info.used = 0;
	tpool->info.peak = 0;
	tpool->info.need = 0;
	tpool->info.real = 0;
	tpool->info.fail = 0;
	tpool->info.pred = 0;
	tpool->info.aloc = 0;
#endif
}

tb_pointer_t tb_tpool_malloc(tb_handle_t handle, tb_size_t size)
{
	// check
	tb_tpool_t* tpool = (tb_tpool_t*)handle;
	tb_assert_and_check_return_val(tpool && tpool->magic == TB_TPOOL_MAGIC, TB_NULL);

	// no size?
	tb_check_return_val(size, TB_NULL);

	// too large?
	tb_check_return_val(size <= tpool->step * TB_TPOOL_BLOCK_MAXN, TB_NULL);

	// full?
	tb_check_return_val(!tpool->full, TB_NULL);

	// predict it?
	tb_pointer_t data = TB_NULL;
//	tb_pointer_t data = tb_tpool_malloc_pred(tpool, size);

	// find the free block
	if (!data) data = tb_tpool_malloc_find(tpool, size);

	// update info
#ifdef TB_DEBUG
	if (data)
	{
		// update the used size
		tpool->info.used += tb_align(size, tpool->step);

		// update the need size
		tpool->info.need += size;

		// update the real size		
		tpool->info.real += tb_align(size, tpool->step);

		// update the peak size
		if (tpool->info.used > tpool->info.peak) tpool->info.peak = tpool->info.used;
		
	}
	// fail++
	else tpool->info.fail++;
	
	// aloc++
	tpool->info.aloc++;
#endif

	// full?
	if (!data) tpool->full = 1;

	// ok?
	return data;
}

tb_pointer_t tb_tpool_malloc0(tb_handle_t handle, tb_size_t size)
{
	return TB_NULL;
}

tb_pointer_t tb_tpool_nalloc(tb_handle_t handle, tb_size_t item, tb_size_t size)
{
	return TB_NULL;
}

tb_pointer_t tb_tpool_nalloc0(tb_handle_t handle, tb_size_t item, tb_size_t size)
{
	return TB_NULL;
}

tb_pointer_t tb_tpool_ralloc(tb_handle_t handle, tb_pointer_t data, tb_size_t size)
{
	return TB_NULL;
}

tb_bool_t tb_tpool_free(tb_handle_t handle, tb_pointer_t data)
{
	return TB_TRUE;
}


#ifdef TB_DEBUG
tb_void_t tb_tpool_dump(tb_handle_t handle)
{
	tb_tpool_t* tpool = (tb_tpool_t*)handle;
	tb_assert_and_check_return(tpool);

	tb_print("======================================================================");
	tb_print("tpool: magic: %#lx",	tpool->magic);
	tb_print("tpool: align: %lu", 	tpool->align);
	tb_print("tpool: step: %lu", 	tpool->step);
	tb_print("tpool: data: %p", 	tpool->data);
	tb_print("tpool: size: %lu", 	tpool->maxn * tpool->step * TB_TPOOL_BLOCK_MAXN);
	tb_print("tpool: full: %lu", 	tpool->full);
	tb_print("tpool: used: %lu", 	tpool->info.used);
	tb_print("tpool: peak: %lu", 	tpool->info.peak);
	tb_print("tpool: wast: %lu%%", 	tpool->info.real? (tpool->info.real - tpool->info.need) * 100 / tpool->info.real : 0);
	tb_print("tpool: fail: %lu", 	tpool->info.fail);
	tb_print("tpool: pred: %lu%%", 	tpool->info.aloc? ((tpool->info.pred * 100) / tpool->info.aloc) : 0);

	tb_size_t 	i = 0;
	tb_size_t 	m = tpool->maxn;
	for (i = 0; i < m; i++)
	{
#if TB_CPU_BITSIZE == 64
		if (tpool->body[i]) tb_print("\ttpool: [%lu]: head: %064b, body: %064b", i, tpool->head[i], tpool->body[i]);
#elif TB_CPU_BITSIZE == 32
		if (tpool->body[i]) tb_print("\ttpool: [%lu]: head: %032b, body: %032b", i, tpool->head[i], tpool->body[i]);
#endif
	}
}
#endif