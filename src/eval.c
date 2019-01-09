/*
  Xiphos, a UCI chess engine
  Copyright (C) 2018, 2019 Milos Tatarevic

  Xiphos is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  Xiphos is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "bitboard.h"
#include "game.h"
#include "pawn_eval.h"
#include "phash.h"
#include "position.h"
#include "tables.h"

#define CHECK_SHIFT               1
#define SAFE_CHECK_BONUS          3
#define PUSHED_PASSERS_BONUS      10
#define BEHIND_PAWN_BONUS         8
#define K_SQ_ATTACK               2
#define K_CNT_LIMIT               8

#define PHASE_SHIFT               7
#define TOTAL_PHASE               (1 << PHASE_SHIFT)
#define TEMPO                     10

const int k_cnt_mul[K_CNT_LIMIT] = { 0, 3, 7, 12, 16, 18, 19, 20 };

int eval(position_t *pos)
{
  int side, score, score_mid, score_end, pcnt, sq, k_sq_f, k_sq_o,
      piece_o, open_file, k_score[N_SIDES], k_cnt[N_SIDES];
  uint64_t b, b0, b1, k_zone, occ, occ_f, occ_o, occ_o_np, occ_o_nk,
           occ_x, n_occ, p_occ, p_occ_f, p_occ_o, n_att, b_att, r_att,
           pushed_passers, p_pushed_safe, p_pushed[N_SIDES], p_att[N_SIDES],
           att_area[N_SIDES], att_area_nk[N_SIDES], checks[N_SIDES];
  phash_data_t phash_data;

  phash_data = pawn_eval(pos);
  score_mid = pos->score_mid + phash_data.score_mid;
  score_end = pos->score_end + phash_data.score_end;

  p_occ = pos->piece_occ[PAWN];
  occ = pos->occ[WHITE] | pos->occ[BLACK];
  pushed_passers = phash_data.pushed_passers;

  for (side = WHITE; side < N_SIDES; side ++)
  {
    p_occ_f = p_occ & pos->occ[side];
    p_att[side] = pawn_attacks(p_occ_f, side);
    p_pushed[side] = pushed_pawns(p_occ_f, ~occ, side);
  }

  for (side = WHITE; side < N_SIDES; side ++)
  {
    k_sq_f = pos->k_sq[side];
    k_sq_o = pos->k_sq[side ^ 1];
    k_zone = _b_king_zone[k_sq_o];

    occ_f = pos->occ[side];
    occ_o = pos->occ[side ^ 1];
    p_occ_f = p_occ & occ_f;
    p_occ_o = p_occ & occ_o;
    n_occ = ~(p_occ_f | p_att[side ^ 1] | _b(k_sq_f));
    occ_o_np = occ_o & ~p_occ_o;
    occ_o_nk = occ_o & ~_b(k_sq_o);
    occ_x = occ ^ pos->piece_occ[QUEEN];

    n_att = knight_attack(occ, k_sq_o);
    b_att = bishop_attack(occ_x, k_sq_o);
    r_att = rook_attack(occ_x, k_sq_o);

    checks[side] = 0;
    att_area_nk[side] = p_att[side];
    k_score[side] = k_cnt[side] = 0;

    #define _score_rook_open_files                                             \
      b = _b_file[_file(sq)];                                                  \
      if (!(b & p_occ_f))                                                      \
      {                                                                        \
        open_file = !(b & p_occ_o);                                            \
        score_mid += rook_file_bonus[PHASE_MID][open_file];                    \
        score_end += rook_file_bonus[PHASE_END][open_file];                    \
      }

    #define _score_threats(piece)                                              \
      b1 = b & occ_o_nk;                                                       \
      _loop(b1)                                                                \
      {                                                                        \
        piece_o = _to_white(pos->board[_bsf(b1)]);                             \
        score_mid += threats[PHASE_MID][piece][piece_o];                       \
        score_end += threats[PHASE_END][piece][piece_o];                       \
      }
    #define _nop(...)

    #define _score_piece(piece, method, att, _threats, _rook_bonus)            \
      b0 = pos->piece_occ[piece] & occ_f;                                      \
      _loop(b0)                                                                \
      {                                                                        \
        sq = _bsf(b0);                                                         \
        att_area_nk[side] |= b = method(occ_x, sq);                            \
        b &= n_occ;                                                            \
                                                                               \
        _threats(piece);                                                       \
                                                                               \
        /* mobility */                                                         \
        pcnt = _popcnt(b);                                                     \
        score_mid += mobility[PHASE_MID][piece][pcnt];                         \
        score_end += mobility[PHASE_END][piece][pcnt];                         \
                                                                               \
        /* king safety */                                                      \
        b &= k_zone | att;                                                     \
        if (b)                                                                 \
        {                                                                      \
          k_cnt[side] ++;                                                      \
          k_score[side] += _popcnt(b & k_zone);                                \
                                                                               \
          checks[side] |= b &= att;                                            \
          k_score[side] += _popcnt(b);                                         \
        }                                                                      \
                                                                               \
        _rook_bonus                                                            \
      }

    _score_piece(KNIGHT, knight_attack, n_att, _score_threats, );
    _score_piece(BISHOP, bishop_attack, b_att, _score_threats, );

    b_att |= r_att;
    _score_piece(QUEEN, queen_attack, b_att, _nop, );

    occ_x ^= pos->piece_occ[ROOK] & occ_f & ~(side == WHITE ? _B_RANK_1 : _B_RANK_8);
    _score_piece(ROOK, rook_attack, r_att, _score_threats, _score_rook_open_files);

    // include king attack
    att_area[side] = att_area_nk[side] | _b_piece_area[KING][k_sq_f];

    // passer protection/attacks
    score_end += _popcnt(att_area[side] & pushed_passers) * PUSHED_PASSERS_BONUS;

    // threat by king
    if(_b_piece_area[KING][k_sq_f] & occ_o & n_occ)
    {
      score_mid += threat_king[PHASE_MID];
      score_end += threat_king[PHASE_END];
    }

    // threats by protected pawns
    pcnt = _popcnt(pawn_attacks(att_area[side] & p_occ_f, side) & occ_o_np);
    score_mid += pcnt * threat_protected_pawn[PHASE_MID];
    score_end += pcnt * threat_protected_pawn[PHASE_END];

    // threats by protected pawns (after push)
    pcnt = _popcnt(pawn_attacks(att_area[side] & p_pushed[side], side) & occ_o_np);
    score_mid += pcnt * threat_protected_pawn_push[PHASE_MID];
    score_end += pcnt * threat_protected_pawn_push[PHASE_END];

    // N/B behind pawns
    b = (side == WHITE ? p_occ << 8 : p_occ >> 8) & occ_f &
        (pos->piece_occ[KNIGHT] | pos->piece_occ[BISHOP]);
    score_mid += _popcnt(b) * BEHIND_PAWN_BONUS;

    // bishop pair bonus
    if (_popcnt(pos->piece_occ[BISHOP] & occ_f) >= 2)
    {
      score_mid += bishop_pair[PHASE_MID];
      score_end += bishop_pair[PHASE_END];
    }

    score_mid = -score_mid;
    score_end = -score_end;
  }

  // use precalculated attacks (a separate loop is required)
  for (side = WHITE; side < N_SIDES; side ++)
  {
    p_pushed_safe = p_pushed[side] & (att_area[side] | ~att_area[side ^ 1]);

    // pawn mobility
    pcnt = _popcnt(p_pushed_safe);
    score_mid += pcnt * pawn_mobility[PHASE_MID];
    score_end += pcnt * pawn_mobility[PHASE_END];

    // pawn attacks on the king zone
    b = p_pushed_safe & _b_king_zone[pos->k_sq[side ^ 1]];
    k_score[side] += _popcnt(b);

    // bonus for safe checks
    if ((b = checks[side] & ~att_area[side ^ 1]))
    {
      k_cnt[side] ++;
      k_score[side] += _popcnt(b) * SAFE_CHECK_BONUS;
    }

    // attacked squares next to the king
    b = _b_piece_area[KING][pos->k_sq[side ^ 1]] &
         att_area[side] & ~att_area_nk[side ^ 1];
    k_score[side] += _popcnt(b) * K_SQ_ATTACK;

    // scale king safety
    score_mid += _sqr(k_score[side]) * k_cnt_mul[_min(k_cnt[side], K_CNT_LIMIT - 1)] / 8;

    score_mid = -score_mid;
    score_end = -score_end;
  }

  if (pos->side == BLACK)
  {
    score_mid = -score_mid;
    score_end = -score_end;
  }

  if (pos->phase >= TOTAL_PHASE)
    score = score_end;
  else
    score = ((score_mid * (TOTAL_PHASE - pos->phase)) +
             (score_end * pos->phase)) >> PHASE_SHIFT;

  return score + TEMPO;
}
