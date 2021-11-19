#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <assert.h>
#include "mmpriv.h"
#include "kalloc.h"
#include "krmq.h"
#include <x86intrin.h>
//#include "simd_chain.h"
//#include "parallel_chaining_32_bit.h"
#include "parallel_chaining_v2_22.h"

extern uint64_t dp_time, rmq_time, rmq_t1, rmq_t2, rmq_t3, rmq_t4;


uint64_t *mg_chain_backtrack(void *km, int64_t n, const int32_t *f, const int64_t *p, int32_t *v, int32_t *t, int32_t min_cnt, int32_t min_sc, int32_t *n_u_, int32_t *n_v_)
{
	mm128_t *z;
	uint64_t *u;
	int64_t i, k, n_z, n_v;
	int32_t n_u;

	*n_u_ = *n_v_ = 0;
	for (i = 0, n_z = 0; i < n; ++i) // precompute n_z
		if (f[i] >= min_sc) ++n_z;
	if (n_z == 0) return 0;
	KMALLOC(km, z, n_z);
	for (i = 0, k = 0; i < n; ++i) // populate z[]
		if (f[i] >= min_sc) z[k].x = f[i], z[k++].y = i;
	radix_sort_128x(z, z + n_z);

	memset(t, 0, n * 4);
	for (k = n_z - 1, n_v = n_u = 0; k >= 0; --k) { // precompute n_u
		int64_t n_v0 = n_v;
		int32_t sc;
		for (i = z[k].y; i >= 0 && t[i] == 0; i = p[i])
			++n_v, t[i] = 1;
		sc = i < 0? z[k].x : (int32_t)z[k].x - f[i];
		if (sc >= min_sc && n_v > n_v0 && n_v - n_v0 >= min_cnt)
			++n_u;
		else n_v = n_v0;
	}
	KMALLOC(km, u, n_u);
	memset(t, 0, n * 4);
	for (k = n_z - 1, n_v = n_u = 0; k >= 0; --k) { // populate u[]
		int64_t n_v0 = n_v;
		int32_t sc;
		for (i = z[k].y; i >= 0 && t[i] == 0; i = p[i])
			v[n_v++] = i, t[i] = 1;
		sc = i < 0? z[k].x : (int32_t)z[k].x - f[i];
		if (sc >= min_sc && n_v > n_v0 && n_v - n_v0 >= min_cnt)
			u[n_u++] = (uint64_t)sc << 32 | (n_v - n_v0);
		else n_v = n_v0;
	}
	kfree(km, z);
	assert(n_v < INT32_MAX);
	*n_u_ = n_u, *n_v_ = n_v;
	return u;
}

static mm128_t *compact_a(void *km, int32_t n_u, uint64_t *u, int32_t n_v, int32_t *v, mm128_t *a)
{
	mm128_t *b, *w;
	uint64_t *u2;
	int64_t i, j, k;

	// write the result to b[]
	KMALLOC(km, b, n_v);
	for (i = 0, k = 0; i < n_u; ++i) {
		int32_t k0 = k, ni = (int32_t)u[i];
		for (j = 0; j < ni; ++j)
			b[k++] = a[v[k0 + (ni - j - 1)]];
	}
	kfree(km, v);

	// sort u[] and a[] by the target position, such that adjacent chains may be joined
	KMALLOC(km, w, n_u);
	for (i = k = 0; i < n_u; ++i) {
		w[i].x = b[k].x, w[i].y = (uint64_t)k<<32|i;
		k += (int32_t)u[i];
	}
	radix_sort_128x(w, w + n_u);
	KMALLOC(km, u2, n_u);
	for (i = k = 0; i < n_u; ++i) {
		int32_t j = (int32_t)w[i].y, n = (int32_t)u[j];
		u2[i] = u[j];
		memcpy(&a[k], &b[w[i].y>>32], n * sizeof(mm128_t));
		k += n;
	}
	memcpy(u, u2, n_u * 8);
	memcpy(b, a, k * sizeof(mm128_t)); // write _a_ to _b_ and deallocate _a_ because _a_ is oversized, sometimes a lot
	kfree(km, a); kfree(km, w); kfree(km, u2);
	return b;
}

static inline int32_t comput_sc(const mm128_t *ai, const mm128_t *aj, int32_t max_dist_x, int32_t max_dist_y, int32_t bw, float chn_pen_gap, float chn_pen_skip, int is_cdna, int n_seg)
{

	uint64_t ai_x, ai_y, aj_x, aj_y;
	ai_x = ai->x; ai_y = ai->y; aj_x = aj->x; aj_y = aj->y;

#ifdef CHAIN_DEBUG
	int32_t sc_vect = obj.comput_sc_vectorized_avx2_caller(ai_x, ai_y, aj_x, aj_y, aj->y>>32&0xff);
#endif

	//if (sc_vect == 0) return INT32_MIN;
	//else 
	//return sc_vect;

	//fprintf(stderr, "%lld %lld %lld %lld \n", ai_x, ai_y, aj_x, aj_y); 
	//fprintf(stderr, "%lld %lld %lld %f %f %d %d\n", max_dist_x, max_dist_y, bw, chn_pen_gap, chn_pen_skip, is_cdna, n_seg);
	int32_t dq = (int32_t)ai_y - (int32_t)aj_y, dr, dd, dg, q_span, sc;
	int32_t sidi = (ai_y & MM_SEED_SEG_MASK) >> MM_SEED_SEG_SHIFT;
	int32_t sidj = (aj_y & MM_SEED_SEG_MASK) >> MM_SEED_SEG_SHIFT;
	if (dq <= 0 || dq > max_dist_x) { 

#ifdef CHAIN_DEBUG
		if(INT32_MIN != sc_vect){
		//fprintf(stderr, "score mismatch %d -- %d", sc , sc_vect);
			fprintf(stderr, "int-min exit: %llu, %llu, %llu, %llu : %d -- %d\n", ai_x, ai_y, aj_x, aj_y, sc, sc_vect);
		}
#endif
	  return INT32_MIN; 
	}
	dr = (int32_t)(ai_x - aj_x);
	if (sidi == sidj && (dr == 0 || dq > max_dist_y)) { 

#ifdef CHAIN_DEBUG
		if(INT32_MIN != sc_vect){
		//fprintf(stderr, "score mismatch %d -- %d", sc , sc_vect);
			fprintf(stderr, "int-min exit: %llu, %llu, %llu, %llu : %d -- %d\n", ai_x, ai_y, aj_x, aj_y, sc, sc_vect);
		}
#endif
		return INT32_MIN;
	}
	dd = dr > dq? dr - dq : dq - dr;
	if (sidi == sidj && dd > bw) {

#ifdef CHAIN_DEBUG
		if(INT32_MIN != sc_vect){
		//fprintf(stderr, "score mismatch %d -- %d", sc , sc_vect);
			fprintf(stderr, "int-min exit: %llu, %llu, %llu, %llu : %d -- %d\n", ai_x, ai_y, aj_x, aj_y, sc, sc_vect);
		}
#endif
		return INT32_MIN; 
	}
	if (n_seg > 1 && !is_cdna && sidi == sidj && dr > max_dist_y) { 

#ifdef CHAIN_DEBUG
		if(INT32_MIN != sc_vect){
		//fprintf(stderr, "score mismatch %d -- %d", sc , sc_vect);
			fprintf(stderr, "int-min exit: %llu, %llu, %llu, %llu : %d -- %d\n", ai_x, ai_y, aj_x, aj_y, sc, sc_vect);
		}
#endif
		return INT32_MIN;
	}
	dg = dr < dq? dr : dq;
	q_span = aj->y>>32&0xff;
	sc = q_span < dg? q_span : dg;
	if (dd || dg > q_span) {
		float lin_pen, log_pen;
		lin_pen = chn_pen_gap * (float)dd + chn_pen_skip * (float)dg;
		log_pen = dd >= 1? mg_log2(dd + 1) : 0.0f; // mg_log2() only works for dd>=2
		if (is_cdna || sidi != sidj) {
			if (sidi != sidj && dr == 0) ++sc; // possibly due to overlapping paired ends; give a minor bonus
			else if (dr > dq || sidi != sidj) sc -= (int)(lin_pen < log_pen? lin_pen : log_pen); // deletion or jump between paired ends
			else sc -= (int)(lin_pen + .5f * log_pen);
		} else sc -= (int)(lin_pen + .5f * log_pen);
	}
#ifdef CHAIN_DEBUG

	if(sc != sc_vect ){
		//fprintf(stderr, "score mismatch %d -- %d", sc , sc_vect);
		fprintf(stderr, "outer: %llu, %llu, %llu, %llu : %d -- %d\n", ai_x, ai_y, aj_x, aj_y, sc, sc_vect);
	}
#endif
	return sc;
}

/* Input:
 *   a[].x: tid<<33 | rev<<32 | tpos
 *   a[].y: flags<<40 | q_span<<32 | q_pos
 * Output:
 *   n_u: #chains
 *   u[]: score<<32 | #anchors (sum of lower 32 bits of u[] is the returned length of a[])
 * input a[] is deallocated on return
 */
mm128_t *mg_lchain_dp(int max_dist_x, int max_dist_y, int bw, int max_skip, int max_iter, int min_cnt, int min_sc, float chn_pen_gap, float chn_pen_skip,
					  int is_cdna, int n_seg, int64_t n, mm128_t *a, int *n_u_, uint64_t **_u, void *km)
{ // TODO: make sure this works when n has more than 32 bits
	///fprintf(stderr, "chaining called\n");



#ifdef MANUAL_PROFILING
	uint64_t align_start = __rdtsc();
#endif

	int32_t *f, *t, *v, *v_1, *p_1, n_u, n_v, mmax_f = 0;
	int64_t *p, i, j, max_ii, st = 0, n_iter = 0;
	uint64_t *u;
	uint32_t* f_1;
	if (_u) *_u = 0, *n_u_ = 0;
	if (n == 0 || a == 0) {
		kfree(km, a);
		return 0;
	}
	if (max_dist_x < bw) max_dist_x = bw;
	if (max_dist_y < bw && !is_cdna) max_dist_y = bw;
	KMALLOC(km, p, n);
	KMALLOC(km, p_1, n);
	KMALLOC(km, f, n);
	KMALLOC(km, f_1, n);
	KMALLOC(km, v, n);
	KMALLOC(km, v_1, n);
	KCALLOC(km, t, n);

#ifdef PARALLEL_CHAINING

// Parallel chaining data-structures
	anchor_t* anchors = (anchor_t*)malloc(n* sizeof(anchor_t));
	for (i = 0; i < n; ++i) {
		uint64_t ri = a[i].x;
		int32_t qi = (int32_t)a[i].y, q_span = a[i].y>>32&0xff; // NB: only 8 bits of span is used!!!
		anchors[i].r = ri;
		anchors[i].q = qi;
		anchors[i].l = q_span;
	}
	num_bits_t *anchor_r, *anchor_q, *anchor_l;
	create_SoA_Anchors_32_bit(anchors, n, anchor_r, anchor_q, anchor_l);
	//dp_chain obj(max_dist_x, max_dist_y, bw, max_skip, max_iter, 0, is_cdna, n_seg);
	dp_chain obj(max_dist_x, max_dist_y, bw, max_skip, max_iter, min_cnt, min_sc, chn_pen_gap, chn_pen_skip, is_cdna, n_seg);

	obj.mm_dp_vectorized(n, &anchors[0], anchor_r, anchor_q, anchor_l, f_1, p_1, v_1, max_dist_x, max_dist_y, NULL, NULL);

	// -16 is due to extra padding at the start of arrays
	anchor_r -= 16; anchor_q -= 16; anchor_l -= 16;
	free(anchor_r); 
	free(anchor_q); 
	free(anchor_l);
	free(anchors);
	for(int i = 0; i < n; i++){
	//		if(f[i] != f_1[i] || p[i] != p_1[i] || v[i] !=v_1[i])
	//		{
//				fprintf(stderr, "i:%d %d %d %d %d %d %d\n",i, f[i], f_1[i], p[i], p_1[i], v[i], v_1[i] );
	//		}
#if 1
			f[i] = f_1[i];
			p[i] = p_1[i];
			v[i] = v_1[i];
#endif
	}

//
#else

	// fill the score and backtrack arrays
	for (i = 0, max_ii = -1; i < n; ++i) {
		int64_t max_j = -1, end_j;
		int32_t max_f = a[i].y>>32&0xff, n_skip = 0;
		while (st < i && (a[i].x>>32 != a[st].x>>32 || a[i].x > a[st].x + max_dist_x)) ++st;
		if (i - st > max_iter) st = i - max_iter;
		int my_cnt = 0;
		for (j = i - 1; j >= st; --j) {
			int32_t sc;
			sc = comput_sc(&a[i], &a[j], max_dist_x, max_dist_y, bw, chn_pen_gap, chn_pen_skip, is_cdna, n_seg);
			++n_iter;
	//		if(i == 177){ 
				//fprintf(stderr, "args: %d %d %d %d %d\n", a[i].x, a[i].y, a[j].x, a[j].y, a[j].y>>32&0xff);
				//fprintf(stderr, "j_th %d score: %d\n", ++my_cnt, sc);
	//		}
			if (sc == INT32_MIN) continue;
			sc += f[j];
			if (sc > max_f) {
				max_f = sc, max_j = j;
				if (n_skip > 0) --n_skip;
			} else if (t[j] == (int32_t)i) {
				if (++n_skip > max_skip)
					break;
			}
			if (p[j] >= 0) t[p[j]] = i;
		}
		end_j = j;
		int debug_iter = 2057329;
		//if (i == debug_iter) fprintf(stderr, "mm2 -- endj: %d max_ii: %d max_f: %d \n", end_j, max_ii, max_f);	

#if 1	
		if (max_ii < 0 || a[i].x - a[max_ii].x > (int64_t)max_dist_x) {
			int32_t max = INT32_MIN;
			max_ii = -1;
			for (j = i - 1; j >= st; --j) {
				if (max < (int32_t)f[j]) max = f[j], max_ii = j;
			}
		}
#endif			
#if 1	
		if (max_ii >= 0 && max_ii < end_j) {
			int32_t tmp;
			tmp = comput_sc(&a[i], &a[max_ii], max_dist_x, max_dist_y, bw, chn_pen_gap, chn_pen_skip, is_cdna, n_seg);
		//	if (i == debug_iter) fprintf(stderr, "mm2: endj: %d max_ii: %d max_f: %d tmp_score: %d \n", end_j, max_ii, max_f, tmp);	
		

			if (tmp != INT32_MIN && max_f < tmp + f[max_ii])			{
		//	if (i == debug_iter) fprintf(stderr, "mm2: endj: %d max_ii: %d max_f: %d tmp_score: %d \n", end_j, max_ii, max_f, tmp);	
				max_f = tmp + f[max_ii], max_j = max_ii;
		//	if (i == debug_iter) fprintf(stderr, "mm2: endj: %d max_ii: %d max_f: %d tmp_score: %d sum : %d \n", end_j, max_ii, max_f, tmp, tmp + f[max_ii]);	

			}
		}
#endif			
		f[i] = max_f, p[i] = max_j;
		v[i] = max_j >= 0 && v[max_j] > max_f? v[max_j] : max_f; // v[] keeps the peak score up to i; f[] is the score ending at i, not always the peak

#if 1
		if (max_ii < 0 || (a[i].x - a[max_ii].x <= (int64_t)max_dist_x && f[max_ii] < f[i]))
			max_ii = i;
		if (mmax_f < max_f) mmax_f = max_f;
#endif
	}

#endif

#ifdef CHAIN_DEBUG

		for(int i = 0; i < n; i++){
			if(f[i] != f_1[i] || p[i] != p_1[i] || v[i] !=v_1[i])
			{
				fprintf(stderr, "i:%d %d %d %d %d %d %d\n",i, f[i], f_1[i], p[i], p_1[i], v[i], v_1[i] );
			}
#if 0
			f[i] = f_1[i];
			p[i] = p_1[i];
			v[i] = v_1[i];
#endif
		}

#endif
	u = mg_chain_backtrack(km, n, f, p, v, t, min_cnt, min_sc, &n_u, &n_v);
	*n_u_ = n_u, *_u = u; // NB: note that u[] may not be sorted by score here
	kfree(km, p); kfree(km, p_1); kfree(km, f); kfree(km, f_1); kfree(km, t); kfree(km, v_1);
	if (n_u == 0) {
		kfree(km, a); kfree(km, v);
		return 0;
	}


#ifdef MANUAL_PROFILING
	dp_time += __rdtsc() - align_start;
#endif
	return compact_a(km, n_u, u, n_v, v, a);
}

typedef struct lc_elem_s {
	int32_t y;
	int64_t i;
	double pri;
	KRMQ_HEAD(struct lc_elem_s) head;
} lc_elem_t;

#define lc_elem_cmp(a, b) ((a)->y < (b)->y? -1 : (a)->y > (b)->y? 1 : ((a)->i > (b)->i) - ((a)->i < (b)->i))
#define lc_elem_lt2(a, b) ((a)->pri < (b)->pri)
KRMQ_INIT(lc_elem, lc_elem_t, head, lc_elem_cmp, lc_elem_lt2)

KALLOC_POOL_INIT(rmq, lc_elem_t)

static inline int32_t comput_sc_simple(const mm128_t *ai, const mm128_t *aj, float chn_pen_gap, float chn_pen_skip, int32_t *exact, int32_t *width)
{
	int32_t dq = (int32_t)ai->y - (int32_t)aj->y, dr, dd, dg, q_span, sc;
	dr = (int32_t)(ai->x - aj->x);
	*width = dd = dr > dq? dr - dq : dq - dr;
	dg = dr < dq? dr : dq;
	q_span = aj->y>>32&0xff;
	sc = q_span < dg? q_span : dg;
	if (exact) *exact = (dd == 0 && dg <= q_span);
	if (dd || dq > q_span) {
		float lin_pen, log_pen;
		lin_pen = chn_pen_gap * (float)dd + chn_pen_skip * (float)dg;
		log_pen = dd >= 1? mg_log2(dd + 1) : 0.0f; // mg_log2() only works for dd>=2
		sc -= (int)(lin_pen + .5f * log_pen);
	}
	return sc;
}

mm128_t *mg_lchain_rmq(int max_dist, int max_dist_inner, int bw, int max_chn_skip, int cap_rmq_size, int min_cnt, int min_sc, float chn_pen_gap, float chn_pen_skip,
					   int64_t n, mm128_t *a, int *n_u_, uint64_t **_u, void *km)
{
#ifdef MANUAL_PROFILING
	uint64_t start = __rdtsc();
#endif
	uint64_t tim;
	//fprintf(stderr, "rmq call \n");
	int32_t *f,*t, *v, n_u, n_v, mmax_f = 0, max_rmq_size = 0;
	int64_t *p, i, i0, st = 0, st_inner = 0, n_iter = 0;
	uint64_t *u;
	lc_elem_t *root = 0, *root_inner = 0;
	void *mem_mp = 0;
	kmp_rmq_t *mp;

	if (_u) *_u = 0, *n_u_ = 0;
	if (n == 0 || a == 0) {
		kfree(km, a);
		return 0;
	}
	if (max_dist < bw) max_dist = bw;
	if (max_dist_inner <= 0 || max_dist_inner >= max_dist) max_dist_inner = 0;
	KMALLOC(km, p, n);
	KMALLOC(km, f, n);
	KCALLOC(km, t, n);
	KMALLOC(km, v, n);
	mem_mp = km_init2(km, 0x10000);
	mp = kmp_init_rmq(mem_mp);

	// fill the score and backtrack arrays
	for (i = i0 = 0; i < n; ++i) {
		int64_t max_j = -1;
		int32_t q_span = a[i].y>>32&0xff, max_f = q_span;
		lc_elem_t s, *q, *r, lo, hi;
		// add in-range anchors
#ifdef MANUAL_PROFILING_RMQ
	tim = __rdtsc();
#endif
		if (i0 < i && a[i0].x != a[i].x) {
			int64_t j;
			for (j = i0; j < i; ++j) {
				q = kmp_alloc_rmq(mp);
				q->y = (int32_t)a[j].y, q->i = j, q->pri = -(f[j] + 0.5 * chn_pen_gap * ((int32_t)a[j].x + (int32_t)a[j].y));
				krmq_insert(lc_elem, &root, q, 0);
				if (max_dist_inner > 0) {
					r = kmp_alloc_rmq(mp);
					*r = *q;
					krmq_insert(lc_elem, &root_inner, r, 0);
				}
			}
			i0 = i;
		}
#ifdef MANUAL_PROFILING_RMQ
	rmq_t1 += __rdtsc() - tim;
#endif
		// get rid of active chains out of range
#ifdef MANUAL_PROFILING_RMQ
	tim = __rdtsc();
#endif
		while (st < i && (a[i].x>>32 != a[st].x>>32 || a[i].x > a[st].x + max_dist || krmq_size(head, root) > cap_rmq_size)) {
			s.y = (int32_t)a[st].y, s.i = st;
			if ((q = krmq_find(lc_elem, root, &s, 0)) != 0) {
				q = krmq_erase(lc_elem, &root, q, 0);
				kmp_free_rmq(mp, q);
			}
			++st;
		}
#ifdef MANUAL_PROFILING_RMQ
	rmq_t2 += __rdtsc() - tim;
#endif
#ifdef MANUAL_PROFILING_RMQ
	tim = __rdtsc();
#endif
		if (max_dist_inner > 0)  { // similar to the block above, but applied to the inner tree
			while (st_inner < i && (a[i].x>>32 != a[st_inner].x>>32 || a[i].x > a[st_inner].x + max_dist_inner || krmq_size(head, root_inner) > cap_rmq_size)) {
				s.y = (int32_t)a[st_inner].y, s.i = st_inner;
				if ((q = krmq_find(lc_elem, root_inner, &s, 0)) != 0) {
					q = krmq_erase(lc_elem, &root_inner, q, 0);
					kmp_free_rmq(mp, q);
				}
				++st_inner;
			}
		}
#ifdef MANUAL_PROFILING_RMQ
	rmq_t3 += __rdtsc() - tim;
#endif
		// RMQ
		lo.i = INT32_MAX, lo.y = (int32_t)a[i].y - max_dist;
		hi.i = 0, hi.y = (int32_t)a[i].y;
		if ((q = krmq_rmq(lc_elem, root, &lo, &hi)) != 0) {
			int32_t sc, exact, width, n_skip = 0;
			int64_t j = q->i;
			assert(q->y >= lo.y && q->y <= hi.y);
			sc = f[j] + comput_sc_simple(&a[i], &a[j], chn_pen_gap, chn_pen_skip, &exact, &width);
			if (width <= bw && sc > max_f) max_f = sc, max_j = j;
			if (!exact && root_inner && (int32_t)a[i].y > 0) {
				lc_elem_t *lo, *hi;
				s.y = (int32_t)a[i].y - 1, s.i = n;
				krmq_interval(lc_elem, root_inner, &s, &lo, &hi);
				if (lo) {
					const lc_elem_t *q;
					int32_t width, n_rmq_iter = 0;
					krmq_itr_t(lc_elem) itr;
					krmq_itr_find(lc_elem, root_inner, lo, &itr);
					while ((q = krmq_at(&itr)) != 0) {
#ifdef MANUAL_PROFILING_RMQ
	tim = __rdtsc();
#endif
						if (q->y < (int32_t)a[i].y - max_dist_inner) break;
						++n_rmq_iter;
						j = q->i;
						sc = f[j] + comput_sc_simple(&a[i], &a[j], chn_pen_gap, chn_pen_skip, 0, &width);
						if (width <= bw) {
							if (sc > max_f) {
								max_f = sc, max_j = j;
								if (n_skip > 0) --n_skip;
							} else if (t[j] == (int32_t)i) {
								if (++n_skip > max_chn_skip)
									break;
							}
							if (p[j] >= 0) t[p[j]] = i;
						}
						if (!krmq_itr_prev(lc_elem, &itr)) break;
#ifdef MANUAL_PROFILING_RMQ
	rmq_t4 += __rdtsc() - tim;
#endif
					}
					n_iter += n_rmq_iter;
				}
			}
		}

		// set max
		assert(max_j < 0 || (a[max_j].x < a[i].x && (int32_t)a[max_j].y < (int32_t)a[i].y));
		f[i] = max_f, p[i] = max_j;
		v[i] = max_j >= 0 && v[max_j] > max_f? v[max_j] : max_f; // v[] keeps the peak score up to i; f[] is the score ending at i, not always the peak
		if (mmax_f < max_f) mmax_f = max_f;
		if (max_rmq_size < krmq_size(head, root)) max_rmq_size = krmq_size(head, root);
	}
	km_destroy(mem_mp);

	u = mg_chain_backtrack(km, n, f, p, v, t, min_cnt, min_sc, &n_u, &n_v);
	*n_u_ = n_u, *_u = u; // NB: note that u[] may not be sorted by score here
	kfree(km, p); kfree(km, f); kfree(km, t);
	if (n_u == 0) {
		kfree(km, a); kfree(km, v);
		return 0;
	}
#ifdef MANUAL_PROFILING
	rmq_time += __rdtsc() -  start;
#endif
	return compact_a(km, n_u, u, n_v, v, a);
}
