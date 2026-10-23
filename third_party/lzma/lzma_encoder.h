#ifndef LZMA_ENCODER_H
#define LZMA_ENCODER_H

#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <stdbool.h>
#include <WinUser.h>
#include <sysinfoapi.h>

#define ENC_NUM_BIT_MODEL_TOTAL_BITS 11
#define ENC_NUM_MOVE_BITS            5
#define ENC_PROB_INIT                ((1u << 11) / 2)
#define ENC_TOP_VALUE                (1u << 24)

#define ENC_kNumStates          12
#define ENC_kNumPosBitsMax      4
#define ENC_kNumPosStatesMax    (1 << ENC_kNumPosBitsMax)
#define ENC_kNumLenToPosStates  4
#define ENC_kNumAlignBits       4
#define ENC_kAlignTableSize     (1 << ENC_kNumAlignBits)
#define ENC_kStartPosModelIndex 4
#define ENC_kEndPosModelIndex   14
#define ENC_kNumFullDistances   (1 << (ENC_kEndPosModelIndex >> 1))
#define ENC_kMatchMinLen        2
#define ENC_kNumLenLowBits      3
#define ENC_kNumLenMidBits      3
#define ENC_kNumLenHighBits     8
#define ENC_kNumLenLowSymbols   (1 << ENC_kNumLenLowBits)
#define ENC_kNumLenMidSymbols   (1 << ENC_kNumLenMidBits)
#define ENC_kNumLenHighSymbols  (1 << ENC_kNumLenHighBits)
#define ENC_kNumPosSlotBits     6
#define ENC_kNumPosSlots        (1 << ENC_kNumPosSlotBits)

#define ENC_MATCH_MAX_LEN       273
#define ENC_HASH_BITS           21
#define ENC_HASH_SIZE           (1 << ENC_HASH_BITS)
#define ENC_WINDOW_BITS         25
#define ENC_WINDOW_SIZE         (1 << ENC_WINDOW_BITS)
#define ENC_WINDOW_MASK         (ENC_WINDOW_SIZE - 1)
#define ENC_CHAIN_LEN_SMALL      16384   /* < 4MB inputs */
#define ENC_CHAIN_LEN_MEDIUM     2048    /* 4MB - 32MB inputs */
#define ENC_CHAIN_LEN_LARGE      32      /* 32MB+ inputs */
#define ENC_CHAIN_THRESHOLD_MED  (4u * 1024u * 1024u)
#define ENC_CHAIN_THRESHOLD_LG   (32u * 1024u * 1024u)

static inline int enc_chain_len_for_size(size_t src_size) {
	if (src_size >= ENC_CHAIN_THRESHOLD_LG) return ENC_CHAIN_LEN_LARGE;
	if (src_size >= ENC_CHAIN_THRESHOLD_MED) return ENC_CHAIN_LEN_MEDIUM;
	return ENC_CHAIN_LEN_SMALL;
}

typedef uint16_t EncProb;

typedef struct {
	uint8_t* data;
	size_t    size;
	size_t    capacity;
	bool      overflow;
} EncOutBuf;

static bool enc_outbuf_init(EncOutBuf* b, size_t cap) {
	b->data = (uint8_t*)malloc(cap);
	b->size = 0;
	b->capacity = cap;
	b->overflow = false;
	return b->data != NULL;
}

static void enc_outbuf_free(EncOutBuf* b) { free(b->data); b->data = NULL; }

static void enc_outbuf_write(EncOutBuf* b, uint8_t byte) {
	if (b->size < b->capacity)
		b->data[b->size++] = byte;
	else
		b->overflow = true;
}

typedef struct {
	uint64_t low;
	uint32_t range;
	uint32_t cache_size;
	uint8_t  cache;
	EncOutBuf* out;
} EncRangeEncoder;

static void enc_range_init(EncRangeEncoder* re, EncOutBuf* out) {
	re->low = 0;
	re->range = 0xFFFFFFFFu;
	re->cache_size = 1;
	re->cache = 0;
	re->out = out;
}

static void enc_shift_low(EncRangeEncoder* re) {
	uint8_t carry = (uint8_t)(re->low >> 32);
	if (carry || (uint32_t)(re->low >> 24) != 0xFF) {
		uint8_t b = re->cache;
		re->cache = (uint8_t)(re->low >> 24);
		enc_outbuf_write(re->out, (uint8_t)(b + carry));
		uint8_t fill = (uint8_t)(0xFF + carry);
		for (uint32_t i = re->cache_size - 1; i > 0; i--)
			enc_outbuf_write(re->out, fill);
		re->cache_size = 1;
	}
	else {
		re->cache_size++;
	}
	re->low = (uint64_t)(re->low & 0x00FFFFFFu) << 8;
}

static void enc_encode_bit(EncRangeEncoder* re, EncProb* prob, int symbol) {
	uint32_t bound = (re->range >> ENC_NUM_BIT_MODEL_TOTAL_BITS) * (uint32_t)(*prob);
	if (symbol == 0) {
		re->range = bound;
		*prob = (EncProb)(*prob + (((1u << ENC_NUM_BIT_MODEL_TOTAL_BITS) - *prob) >> ENC_NUM_MOVE_BITS));
	}
	else {
		re->low += bound;
		re->range -= bound;
		*prob = (EncProb)(*prob - (*prob >> ENC_NUM_MOVE_BITS));
	}
	if (re->range < ENC_TOP_VALUE) {
		re->range <<= 8;
		enc_shift_low(re);
	}
}

static void enc_flush(EncRangeEncoder* re) {
	for (int i = 0; i < 5; i++) enc_shift_low(re);
}

static void enc_bittree(EncRangeEncoder* re, EncProb* probs, int num_bits, uint32_t symbol) {
	uint32_t m = 1;
	for (int i = num_bits - 1; i >= 0; i--) {
		int bit = (symbol >> i) & 1;
		enc_encode_bit(re, &probs[m], bit);
		m = (m << 1) | (uint32_t)bit;
	}
}

static void enc_bittree_reverse(EncRangeEncoder* re, EncProb* probs, int num_bits, uint32_t symbol) {
	uint32_t m = 1;
	for (int i = 0; i < num_bits; i++) {
		int bit = (int)(symbol & 1);
		enc_encode_bit(re, &probs[m], bit);
		m = (m << 1) | (uint32_t)bit;
		symbol >>= 1;
	}
}

static void enc_direct_bits(EncRangeEncoder* re, uint32_t value, int num_bits) {
	for (int i = num_bits - 1; i >= 0; i--) {
		re->range >>= 1;
		if (((value >> i) & 1))
			re->low += re->range;
		if (re->range < ENC_TOP_VALUE) {
			re->range <<= 8;
			enc_shift_low(re);
		}
	}
}

typedef struct {
	EncProb choice;
	EncProb choice2;
	EncProb low[ENC_kNumPosStatesMax][ENC_kNumLenLowSymbols];
	EncProb mid[ENC_kNumPosStatesMax][ENC_kNumLenMidSymbols];
	EncProb high[ENC_kNumLenHighSymbols];
} EncLenCoder;

typedef struct {
	EncProb is_match[ENC_kNumStates << ENC_kNumPosBitsMax];
	EncProb is_rep[ENC_kNumStates];
	EncProb is_rep_g0[ENC_kNumStates];
	EncProb is_rep_g1[ENC_kNumStates];
	EncProb is_rep_g2[ENC_kNumStates];
	EncProb is_rep0_long[ENC_kNumStates << ENC_kNumPosBitsMax];
	EncProb pos_slot[ENC_kNumLenToPosStates][ENC_kNumPosSlots];
	EncProb spec_pos[ENC_kNumFullDistances - ENC_kEndPosModelIndex + 1];
	EncProb align_probs[ENC_kAlignTableSize];
	EncLenCoder len_coder;
	EncLenCoder rep_len_coder;
	EncProb* literal;
} EncProbs;

static bool enc_probs_init(EncProbs* p, uint8_t lc, uint8_t lp) {
	memset(p, 0, sizeof(*p));

	size_t n = ((size_t)0x300) << (lc + lp);
	p->literal = (EncProb*)malloc(n * sizeof(EncProb));
	if (!p->literal) return false;

	for (size_t i = 0; i < (size_t)(ENC_kNumStates << ENC_kNumPosBitsMax); i++) {
		p->is_match[i] = ENC_PROB_INIT;
		p->is_rep0_long[i] = ENC_PROB_INIT;
	}
	for (int i = 0; i < ENC_kNumStates; i++) {
		p->is_rep[i] = ENC_PROB_INIT;
		p->is_rep_g0[i] = ENC_PROB_INIT;
		p->is_rep_g1[i] = ENC_PROB_INIT;
		p->is_rep_g2[i] = ENC_PROB_INIT;
	}
	for (int i = 0; i < ENC_kNumLenToPosStates; i++)
		for (int j = 0; j < ENC_kNumPosSlots; j++)
			p->pos_slot[i][j] = ENC_PROB_INIT;
	for (size_t i = 0; i < (size_t)(ENC_kNumFullDistances - ENC_kEndPosModelIndex + 1); i++)
		p->spec_pos[i] = ENC_PROB_INIT;
	for (int i = 0; i < ENC_kAlignTableSize; i++)
		p->align_probs[i] = ENC_PROB_INIT;

	EncLenCoder* lcs[2] = { &p->len_coder, &p->rep_len_coder };
	for (int k = 0; k < 2; k++) {
		lcs[k]->choice = ENC_PROB_INIT;
		lcs[k]->choice2 = ENC_PROB_INIT;
		for (int ps = 0; ps < ENC_kNumPosStatesMax; ps++) {
			for (int i = 0; i < ENC_kNumLenLowSymbols; i++) lcs[k]->low[ps][i] = ENC_PROB_INIT;
			for (int i = 0; i < ENC_kNumLenMidSymbols; i++) lcs[k]->mid[ps][i] = ENC_PROB_INIT;
		}
		for (int i = 0; i < ENC_kNumLenHighSymbols; i++) lcs[k]->high[i] = ENC_PROB_INIT;
	}
	for (size_t i = 0; i < n; i++) p->literal[i] = ENC_PROB_INIT;
	return true;
}

static void enc_probs_free(EncProbs* p) { free(p->literal); p->literal = NULL; }
static inline int enc_state_literal(int s) { return s < 4 ? 0 : s < 10 ? s - 3 : s - 6; }
static inline int enc_state_match(int s) { return s < 7 ? 7 : 10; }
static inline int enc_state_rep(int s) { return s < 7 ? 8 : 11; }
static inline int enc_state_short_rep(int s) { return s < 7 ? 9 : 11; }
static inline bool enc_is_after_literal(int s) { return s < 7; }


static void enc_len_encode(EncRangeEncoder* re, EncLenCoder* lc, int pos_state, uint32_t len) {
	len -= ENC_kMatchMinLen;
	if (len < ENC_kNumLenLowSymbols) {
		enc_encode_bit(re, &lc->choice, 0);
		enc_bittree(re, lc->low[pos_state], ENC_kNumLenLowBits, len);
		return;
	}
	enc_encode_bit(re, &lc->choice, 1);
	len -= ENC_kNumLenLowSymbols;
	if (len < ENC_kNumLenMidSymbols) {
		enc_encode_bit(re, &lc->choice2, 0);
		enc_bittree(re, lc->mid[pos_state], ENC_kNumLenMidBits, len);
		return;
	}
	enc_encode_bit(re, &lc->choice2, 1);
	len -= ENC_kNumLenMidSymbols;
	enc_bittree(re, lc->high, ENC_kNumLenHighBits, len);
}


static uint32_t enc_get_pos_slot(uint32_t dist) {
	if (dist < 2) return dist;
	uint32_t n = 0;
	uint32_t d = dist;
	if (d >= (1u << 16)) { d >>= 16; n += 16; }
	if (d >= (1u << 8)) { d >>= 8;  n += 8; }
	if (d >= (1u << 4)) { d >>= 4;  n += 4; }
	if (d >= (1u << 2)) { d >>= 2;  n += 2; }
	if (d >= (1u << 1)) { d >>= 1;  n += 1; }
	return (n << 1) | ((dist >> (n - 1)) & 1);
}

static void enc_encode_distance(EncRangeEncoder* re, EncProbs* p,
	uint32_t len_minus_min, uint32_t dist) {

	if (dist == 0xFFFFFFFFu) {
		uint32_t len_state = len_minus_min < ENC_kNumLenToPosStates
			? len_minus_min : (uint32_t)(ENC_kNumLenToPosStates - 1);
		enc_bittree(re, p->pos_slot[len_state], ENC_kNumPosSlotBits, 63);
		enc_direct_bits(re, 0x03FFFFFFu, 26);
		enc_bittree_reverse(re, p->align_probs, ENC_kNumAlignBits, 0xF);
		return;
	}

	uint32_t len_state = len_minus_min < ENC_kNumLenToPosStates
		? len_minus_min : (uint32_t)(ENC_kNumLenToPosStates - 1);
	uint32_t pos_slot = enc_get_pos_slot(dist);
	enc_bittree(re, p->pos_slot[len_state], ENC_kNumPosSlotBits, pos_slot);

	if (pos_slot < ENC_kStartPosModelIndex) return;

	uint32_t num_direct_bits = (pos_slot >> 1) - 1;
	uint32_t base = (2 | (pos_slot & 1)) << num_direct_bits;
	uint32_t residual = dist - base;

	if (pos_slot < ENC_kEndPosModelIndex) {
		enc_bittree_reverse(re, p->spec_pos + base - pos_slot,
			(int)num_direct_bits, residual);
	}

	else {
		enc_direct_bits(re, residual >> ENC_kNumAlignBits,
			(int)(num_direct_bits - ENC_kNumAlignBits));
		enc_bittree_reverse(re, p->align_probs, ENC_kNumAlignBits,
			residual & (ENC_kAlignTableSize - 1));
	}
}

static void enc_encode_literal(EncRangeEncoder* re, EncProbs* p,
	uint8_t lc, uint8_t lp,
	uint64_t out_pos, uint8_t prev_byte,
	uint8_t match_byte, int state, uint8_t sym) {
	uint32_t lit_state = ((uint32_t)(out_pos & ((1u << lp) - 1)) << lc) |
		(uint32_t)(prev_byte >> (8 - lc));
	EncProb* probs = p->literal + ((size_t)0x300 * lit_state);

	uint32_t symbol = 1;
	if (!enc_is_after_literal(state)) {
		while (symbol < 0x100) {
			uint32_t match_bit = ((uint32_t)match_byte >> 7) & 1;
			match_byte = (uint8_t)(match_byte << 1);
			uint32_t sym_bit = ((uint32_t)sym >> 7) & 1;
			sym = (uint8_t)(sym << 1);
			enc_encode_bit(re, &probs[((1 + match_bit) << 8) + symbol], (int)sym_bit);
			symbol = (symbol << 1) | sym_bit;
			if (match_bit != sym_bit) {
				while (symbol < 0x100) {
					sym_bit = ((uint32_t)sym >> 7) & 1;
					sym = (uint8_t)(sym << 1);
					enc_encode_bit(re, &probs[symbol], (int)sym_bit);
					symbol = (symbol << 1) | sym_bit;
				}
				return;
			}
		}
		return;
	}
	while (symbol < 0x100) {
		uint32_t sym_bit = ((uint32_t)sym >> 7) & 1;
		sym = (uint8_t)(sym << 1);
		enc_encode_bit(re, &probs[symbol], (int)sym_bit);
		symbol = (symbol << 1) | sym_bit;
	}
}

typedef struct {
	const uint8_t* src;
	size_t         src_size;
	size_t         pos;
	uint32_t* hash_table;
	uint32_t* chain;
} EncMatchFinder;

static bool enc_mf_init(EncMatchFinder* mf, const uint8_t* src, size_t size) {
	mf->src = src;
	mf->src_size = size;
	mf->pos = 0;
	mf->hash_table = (uint32_t*)malloc(ENC_HASH_SIZE * sizeof(uint32_t));
	mf->chain = (uint32_t*)malloc(ENC_WINDOW_SIZE * sizeof(uint32_t));
	if (!mf->hash_table || !mf->chain) return false;
	memset(mf->hash_table, 0xFF, ENC_HASH_SIZE * sizeof(uint32_t));
	memset(mf->chain, 0xFF, ENC_WINDOW_SIZE * sizeof(uint32_t));
	return true;
}

static void enc_mf_free(EncMatchFinder* mf) {
	free(mf->hash_table); mf->hash_table = NULL;
	free(mf->chain);      mf->chain = NULL;
}

static uint32_t enc_mf_hash(const uint8_t* p) {
	uint32_t v = (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16);
	v *= 0xC4A8B5A3u;
	v ^= v >> ENC_HASH_BITS;
	return v & (ENC_HASH_SIZE - 1);
}

static uint32_t enc_mf_find_peek(EncMatchFinder* mf, uint32_t* match_dist) {
	*match_dist = 0;
	size_t pos = mf->pos;
	if (pos + ENC_kMatchMinLen > mf->src_size) return 0;

	uint32_t hash = enc_mf_hash(mf->src + pos);
	size_t   limit = pos >= ENC_WINDOW_SIZE ? pos - ENC_WINDOW_SIZE : 0;
	size_t   max_len = mf->src_size - pos;
	if (max_len > ENC_MATCH_MAX_LEN) max_len = ENC_MATCH_MAX_LEN;

	uint32_t best_len = ENC_kMatchMinLen - 1;
	uint32_t best_dist = 0;
	uint32_t cur = mf->hash_table[hash];
	int      steps = enc_chain_len_for_size(mf->src_size);

	while (cur != 0xFFFFFFFFu && (size_t)cur >= limit && steps-- > 0) {
		const uint8_t* a = mf->src + pos;
		const uint8_t* b = mf->src + cur;
		size_t avail = mf->src_size - cur;
		size_t m = avail < max_len ? avail : max_len;
		uint32_t len = 0;
		while (len < m && a[len] == b[len]) len++;
		if (len > best_len) {
			best_len = len;
			best_dist = (uint32_t)(pos - cur);
			if (best_len >= (uint32_t)max_len) break;
		}
		cur = mf->chain[cur & ENC_WINDOW_MASK];
	}

	if (best_len >= ENC_kMatchMinLen) {
		*match_dist = best_dist;
		return best_len;
	}
	return 0;
}

static uint32_t enc_mf_find(EncMatchFinder* mf, uint32_t* match_dist) {
	*match_dist = 0;
	size_t pos = mf->pos;
	if (pos + ENC_kMatchMinLen > mf->src_size) return 0;

	uint32_t hash = enc_mf_hash(mf->src + pos);
	size_t   limit = pos >= ENC_WINDOW_SIZE ? pos - ENC_WINDOW_SIZE : 0;
	size_t   max_len = mf->src_size - pos;
	if (max_len > ENC_MATCH_MAX_LEN) max_len = ENC_MATCH_MAX_LEN;

	uint32_t best_len = ENC_kMatchMinLen - 1;
	uint32_t best_dist = 0;
	uint32_t cur = mf->hash_table[hash];
	int      steps = enc_chain_len_for_size(mf->src_size);
	while (cur != 0xFFFFFFFFu && (size_t)cur >= limit && steps-- > 0) {
		const uint8_t* a = mf->src + pos;
		const uint8_t* b = mf->src + cur;
		size_t avail = mf->src_size - cur;
		size_t m = avail < max_len ? avail : max_len;
		uint32_t len = 0;
		while (len < m && a[len] == b[len]) len++;
		if (len > best_len) {
			best_len = len;
			best_dist = (uint32_t)(pos - cur);
			if (best_len >= (uint32_t)max_len) break;
		}
		cur = mf->chain[cur & ENC_WINDOW_MASK];
	}

	mf->chain[pos & ENC_WINDOW_MASK] = mf->hash_table[hash];
	mf->hash_table[hash] = (uint32_t)pos;

	if (best_len >= ENC_kMatchMinLen) {
		*match_dist = best_dist;
		return best_len;
	}
	return 0;
}

static void enc_mf_advance(EncMatchFinder* mf) {
	size_t pos = mf->pos;
	if (pos + ENC_kMatchMinLen < mf->src_size) {
		uint32_t hash = enc_mf_hash(mf->src + pos);
		mf->chain[pos & ENC_WINDOW_MASK] = mf->hash_table[hash];
		mf->hash_table[hash] = (uint32_t)pos;
	}
	mf->pos++;
}

typedef struct {
	uint8_t  lc, lp, pb;
	uint32_t dict_size;
} LZMAEncProps;

typedef void (*LZMAProgressCallback)(size_t bytes_processed, size_t total_bytes, void* userData);

static void lzma_enc_props_default(LZMAEncProps* p) {
	p->lc = 3;
	p->lp = 0;
	p->pb = 0;
	p->dict_size = 1u << 27;
}

static void enc_write_header(EncOutBuf* out, const LZMAEncProps* props, uint64_t uncompressed_size) {
	uint8_t prop_byte = (uint8_t)((props->pb * 5 + props->lp) * 9 + props->lc);
	enc_outbuf_write(out, prop_byte);
	for (int i = 0; i < 4; i++)
		enc_outbuf_write(out, (uint8_t)(props->dict_size >> (i * 8)));
	for (int i = 0; i < 8; i++)
		enc_outbuf_write(out, (uint8_t)(uncompressed_size >> (i * 8)));
}

static uint8_t* lzma_compress(const uint8_t* input, size_t input_size,
	size_t* output_size,
	const LZMAEncProps* props_in,
	LZMAProgressCallback progress_cb, void* progress_userData) {
	*output_size = 0;

	LZMAEncProps props;
	if (props_in) props = *props_in;
	else          lzma_enc_props_default(&props);

	if ((unsigned)props.lc + props.lp > 4) return NULL;

	size_t out_cap = 13 + input_size + (input_size >> 3) + 65536;
	EncOutBuf out;
	if (!enc_outbuf_init(&out, out_cap)) return NULL;

	enc_write_header(&out, &props, (uint64_t)input_size);

	EncProbs probs;
	if (!enc_probs_init(&probs, props.lc, props.lp)) {
		enc_outbuf_free(&out);
		return NULL;
	}

	EncMatchFinder mf;
	if (!enc_mf_init(&mf, input, input_size)) {
		enc_probs_free(&probs);
		enc_outbuf_free(&out);
		return NULL;
	}

	EncRangeEncoder re;
	enc_range_init(&re, &out);

	uint32_t pb_mask = (1u << props.pb) - 1;
	int      state = 0;
	uint32_t rep0 = 0, rep1 = 0, rep2 = 0, rep3 = 0;

	DWORD last_report_tick = GetTickCount();
	const DWORD report_interval_ms = 50;

	while (mf.pos < input_size) {
		size_t   cur_pos = mf.pos;
		uint32_t pos_state = (uint32_t)(cur_pos & pb_mask);

		if (progress_cb) {
			DWORD now = GetTickCount();
			if (now - last_report_tick >= report_interval_ms) {
				progress_cb(cur_pos, input_size, progress_userData);
				last_report_tick = now;
			}
		}
		uint32_t match_dist = 0;
		uint32_t match_len = enc_mf_find(&mf, &match_dist);


		if (input_size < (32u * 1024u * 1024u)) {
			uint32_t rep_dists[4] = { rep0, rep1, rep2, rep3 };
			uint32_t best_rep_len = 0;
			uint32_t best_rep_dist_minus1 = 0;
			size_t   avail = input_size - cur_pos;
			size_t   cap = avail < ENC_MATCH_MAX_LEN ? avail : ENC_MATCH_MAX_LEN;

			for (int r = 0; r < 4; r++) {
				uint32_t rd = rep_dists[r] + 1;
				if ((size_t)rd > cur_pos) continue;
				uint32_t m = 0;
				while (m < cap && input[cur_pos + m] == input[cur_pos + m - rd]) m++;
				if (m >= ENC_kMatchMinLen && m > best_rep_len) {
					best_rep_len = m;
					best_rep_dist_minus1 = rep_dists[r];
				}
			}

			if (best_rep_len > 0 && best_rep_len + 1 >= match_len) {
				match_len = best_rep_len;
				match_dist = best_rep_dist_minus1 + 1;
			}
		}

		uint32_t match_dist_minus1 = match_dist > 0 ? match_dist - 1 : 0;

		bool is_rep = match_len >= ENC_kMatchMinLen &&
			(match_dist_minus1 == rep0 || match_dist_minus1 == rep1 ||
				match_dist_minus1 == rep2 || match_dist_minus1 == rep3);

		if (match_len >= ENC_kMatchMinLen && !is_rep && cur_pos + 1 < input_size
			&& input_size < (32u * 1024u * 1024u)) { 
			EncMatchFinder mf_peek = mf;
			mf_peek.pos = cur_pos + 1;
			uint32_t next_dist = 0;
			uint32_t next_len = enc_mf_find_peek(&mf_peek, &next_dist);
			if (next_len > match_len + 1) {
				match_len = 0;
			}
		}

		if (match_len < ENC_kMatchMinLen || (!is_rep && match_len == ENC_kMatchMinLen && match_dist > 128)) {
			enc_encode_bit(&re, &probs.is_match[(state << ENC_kNumPosBitsMax) + pos_state], 0);

			uint8_t prev_byte = cur_pos == 0 ? 0 : input[cur_pos - 1];
			uint8_t match_byte = 0;
			if (!enc_is_after_literal(state))
				match_byte = input[cur_pos - rep0 - 1];

			enc_encode_literal(&re, &probs, props.lc, props.lp,
				(uint64_t)cur_pos, prev_byte, match_byte, state, input[cur_pos]);
			state = enc_state_literal(state);
			mf.pos++;
			continue;
		}

		enc_encode_bit(&re, &probs.is_match[(state << ENC_kNumPosBitsMax) + pos_state], 1);

		if (is_rep) {
			enc_encode_bit(&re, &probs.is_rep[state], 1);

			if (match_dist_minus1 == rep0) {
				enc_encode_bit(&re, &probs.is_rep_g0[state], 0);
				if (match_len == 1) {
					enc_encode_bit(&re, &probs.is_rep0_long[(state << ENC_kNumPosBitsMax) + pos_state], 0);
					state = enc_state_short_rep(state);
					mf.pos++;
					continue;
				}
				enc_encode_bit(&re, &probs.is_rep0_long[(state << ENC_kNumPosBitsMax) + pos_state], 1);
			}
			else if (match_dist_minus1 == rep1) {
				enc_encode_bit(&re, &probs.is_rep_g0[state], 1);
				enc_encode_bit(&re, &probs.is_rep_g1[state], 0);
				uint32_t tmp = rep1; rep1 = rep0; rep0 = tmp;
			}
			else if (match_dist_minus1 == rep2) {
				enc_encode_bit(&re, &probs.is_rep_g0[state], 1);
				enc_encode_bit(&re, &probs.is_rep_g1[state], 1);
				enc_encode_bit(&re, &probs.is_rep_g2[state], 0);
				uint32_t tmp = rep2; rep2 = rep1; rep1 = rep0; rep0 = tmp;
			}
			else {
				enc_encode_bit(&re, &probs.is_rep_g0[state], 1);
				enc_encode_bit(&re, &probs.is_rep_g1[state], 1);
				enc_encode_bit(&re, &probs.is_rep_g2[state], 1);
				uint32_t tmp = rep3; rep3 = rep2; rep2 = rep1; rep1 = rep0; rep0 = tmp;
			}

			enc_len_encode(&re, &probs.rep_len_coder, (int)pos_state, match_len);
			state = enc_state_rep(state);
		}
		else {
			enc_encode_bit(&re, &probs.is_rep[state], 0);

			rep3 = rep2; rep2 = rep1; rep1 = rep0;
			rep0 = match_dist_minus1;

			enc_len_encode(&re, &probs.len_coder, (int)pos_state, match_len);
			enc_encode_distance(&re, &probs, match_len - ENC_kMatchMinLen, match_dist_minus1);
			state = enc_state_match(state);
		}

		mf.pos++;
		for (uint32_t i = 1; i < match_len; i++)
			enc_mf_advance(&mf);
	}

	uint32_t pos_state = (uint32_t)(mf.pos & pb_mask);
	enc_encode_bit(&re, &probs.is_match[(state << ENC_kNumPosBitsMax) + pos_state], 1);
	enc_encode_bit(&re, &probs.is_rep[state], 0);
	enc_len_encode(&re, &probs.len_coder, (int)pos_state, ENC_kMatchMinLen);
	enc_encode_distance(&re, &probs, 0, 0xFFFFFFFFu);

	enc_flush(&re);
	if (progress_cb) progress_cb(input_size, input_size, progress_userData);

	enc_mf_free(&mf);
	enc_probs_free(&probs);

	if (out.overflow) {
		enc_outbuf_free(&out);
		return NULL;
	}

	*output_size = out.size;
	uint8_t* result = (uint8_t*)realloc(out.data, out.size);
	if (!result) result = out.data;
	else         out.data = NULL;
	return result;
}

#endif