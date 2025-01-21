/*
  Stockfish, a UCI chess playing engine derived from Glaurung 2.1
  Copyright (C) 2004-2025 The Stockfish developers (see AUTHORS file)

  Stockfish is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  Stockfish is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "bitboard.h"

#include <algorithm>
#include <bitset>
#include <initializer_list>

#include <set>
#include <utility>
#ifndef USE_PEXT
    #include "magics.h"
#endif

namespace Stockfish {

uint8_t PopCnt16[1 << 16];
uint8_t SquareDistance[SQUARE_NB][SQUARE_NB];

Bitboard SquareBB[SQUARE_NB];
Bitboard LineBB[SQUARE_NB][SQUARE_NB];
Bitboard BetweenBB[SQUARE_NB][SQUARE_NB];
Bitboard PseudoAttacks[PIECE_TYPE_NB][SQUARE_NB];
Bitboard PawnAttacks[COLOR_NB][SQUARE_NB];
Bitboard PawnAttacksTo[COLOR_NB][SQUARE_NB];

Magic RookMagics[SQUARE_NB];
Magic CannonMagics[SQUARE_NB];
Magic BishopMagics[SQUARE_NB];
Magic KnightMagics[SQUARE_NB];
Magic KnightToMagics[SQUARE_NB];

namespace {

Bitboard RookTable[0x108000];    // 存储车的攻击
Bitboard CannonTable[0x108000];  // 存储炮的攻击
Bitboard BishopTable[0x228];     // 存储相象的攻击
Bitboard KnightTable[0x380];     // 存储马的攻击
Bitboard KnightToTable[0x3E0];   // To store by knight attacks（ToDo: 未知）

// 马的所有可能移动方向（8个日字形方向）
const std::set<Direction> KnightDirections{2 * SOUTH + WEST, 2 * SOUTH + EAST, SOUTH + 2 * WEST,
                                           SOUTH + 2 * EAST, NORTH + 2 * WEST, NORTH + 2 * EAST,
                                           2 * NORTH + WEST, 2 * NORTH + EAST};
// 象/相的所有可能移动方向（4个田字形对角线方向）
const std::set<Direction> BishopDirections{2 * NORTH_EAST, 2 * SOUTH_EAST, 2 * SOUTH_WEST,
                                           2 * NORTH_WEST};


template<PieceType pt>
void init_magics(Bitboard table[], Magic magics[] IF_NOT_PEXT(, const Bitboard magicsInit[]));

template<PieceType pt>
Bitboard lame_leaper_path(Direction d, Square s);

// Returns the bitboard of target square for the given step
// from the given square. If the step is off the board, returns empty bitboard.
Bitboard safe_destination(Square s, int step) {
    Square to = Square(s + step);
    return is_ok(to) && distance(s, to) <= 2 ? square_bb(to) : Bitboard(0);
}

}

// Returns an ASCII representation of a bitboard suitable
// to be printed to standard output. Useful for debugging.
std::string Bitboards::pretty(Bitboard b) {

    std::string s = "+---+---+---+---+---+---+---+---+---+\n";

    for (Rank r = RANK_9; r >= RANK_0; --r)
    {
        for (File f = FILE_A; f <= FILE_I; ++f)
            s += b & make_square(f, r) ? "| X " : "|   ";

        s += "| " + std::to_string(r) + "\n+---+---+---+---+---+---+---+---+---+\n";
    }
    s += "  a   b   c   d   e   f   g   h   i\n";

    return s;
}


// Initializes various bitboard tables. It is called at
// startup and relies on global objects to be already zero-initialized.
void Bitboards::init() {

    for (unsigned i = 0; i < (1 << 16); ++i)
        PopCnt16[i] = uint8_t(std::bitset<16>(i).count());

    for (Square s = SQ_A0; s <= SQ_I9; ++s)
        SquareBB[s] = (Bitboard(1ULL) << std::uint8_t(s));

    for (Square s1 = SQ_A0; s1 <= SQ_I9; ++s1)
        for (Square s2 = SQ_A0; s2 <= SQ_I9; ++s2)
            SquareDistance[s1][s2] = std::max(distance<File>(s1, s2), distance<Rank>(s1, s2));

    init_magics<ROOK>(RookTable, RookMagics IF_NOT_PEXT(, RookMagicsInit));
    init_magics<CANNON>(CannonTable, CannonMagics IF_NOT_PEXT(, RookMagicsInit));
    init_magics<BISHOP>(BishopTable, BishopMagics IF_NOT_PEXT(, BishopMagicsInit));
    init_magics<KNIGHT>(KnightTable, KnightMagics IF_NOT_PEXT(, KnightMagicsInit));
    init_magics<KNIGHT_TO>(KnightToTable, KnightToMagics IF_NOT_PEXT(, KnightToMagicsInit));

    for (Square s1 = SQ_A0; s1 <= SQ_I9; ++s1)
    {
        PawnAttacks[WHITE][s1] = pawn_attacks_bb<WHITE>(s1);
        PawnAttacks[BLACK][s1] = pawn_attacks_bb<BLACK>(s1);

        PawnAttacksTo[WHITE][s1] = pawn_attacks_to_bb<WHITE>(s1);
        PawnAttacksTo[BLACK][s1] = pawn_attacks_to_bb<BLACK>(s1);

        PseudoAttacks[ROOK][s1]   = attacks_bb<ROOK>(s1, 0);
        PseudoAttacks[BISHOP][s1] = attacks_bb<BISHOP>(s1, 0);
        PseudoAttacks[KNIGHT][s1] = attacks_bb<KNIGHT>(s1, 0);

        // Only generate pseudo attacks in the palace squares for king and advisor
        if (Palace & s1)
        {
            for (int step : {NORTH, SOUTH, WEST, EAST})
                PseudoAttacks[KING][s1] |= safe_destination(s1, step);
            PseudoAttacks[KING][s1] &= Palace;

            for (int step : {NORTH_WEST, NORTH_EAST, SOUTH_WEST, SOUTH_EAST})
                PseudoAttacks[ADVISOR][s1] |= safe_destination(s1, step);
            PseudoAttacks[ADVISOR][s1] &= Palace;
        }

        for (Square s2 = SQ_A0; s2 <= SQ_I9; ++s2)
        {
            if (PseudoAttacks[ROOK][s1] & s2)
            {
                LineBB[s1][s2] = (attacks_bb(ROOK, s1, 0) & attacks_bb(ROOK, s2, 0)) | s1 | s2;
                BetweenBB[s1][s2] =
                  (attacks_bb(ROOK, s1, square_bb(s2)) & attacks_bb(ROOK, s2, square_bb(s1)));
            }

            if (PseudoAttacks[KNIGHT][s1] & s2)
                BetweenBB[s1][s2] |= lame_leaper_path<KNIGHT_TO>(Direction(s2 - s1), s1);

            BetweenBB[s1][s2] |= s2;
        }
    }
}

namespace {

// 生成滑动棋子（车、炮）的攻击范围模板函数
// pt: 棋子类型（ROOK或CANNON），sq: 起始位置，occupied: 障碍物位棋盘
template<PieceType pt>
Bitboard sliding_attack(Square sq, Bitboard occupied) {
    assert(pt == ROOK || pt == CANNON); // 仅处理车和炮
    Bitboard attack = 0;

    // 遍历四个基本方向：北、南、东、西（中国象棋车炮的直线移动）
    for (auto const& d : {NORTH, SOUTH, EAST, WEST}) {
        bool hurdle = false; // 炮专用标志：是否已跨越障碍
        
        // 沿方向逐步移动：s从起始点开始，每次增加方向向量
        for (Square s = sq + d; 
             is_ok(s) && distance(s - d, s) == 1; // 检查坐标合法性及步长有效性
             s += d) 
        {
            // 车始终添加路径，炮仅在跨越障碍后添加攻击位置
            if (pt == ROOK || hurdle)
                attack |= s;

            // 遇到障碍物时的处理逻辑
            if (occupied & s) {
                if (pt == CANNON && !hurdle) // 炮第一次遇到障碍物时标记为跨越
                    hurdle = true;
                else                         // 车直接停止，炮跨越后再次遇到障碍物时停止
                    break;
            }
        }
    }
    return attack;
}

/* 生成特定方向的蹩腿位置（用于马/象）*/
// pt: 棋子类型（BISHOP/KNIGHT/KNIGHT_TO）
// d: 移动方向，s: 起始位置，返回蹩腿位置位棋盘
template<PieceType pt>
Bitboard lame_leaper_path(Direction d, Square s) {
    Bitboard b  = 0;
    Square   to = s + d;
    
    // 有效性检查：目标位置合法且移动距离不超过3（适应中国象棋规则）
    if (!is_ok(to) || distance(s, to) >= 4)
        return b;

    // KNIGHT_TO类型特殊处理：反向计算攻击来源（用于攻击目标查询）
    if (pt == KNIGHT_TO) {
        std::swap(s, to); // 交换起点终点
        d = -d;           // 方向取反
    }

    // 分解方向向量为纵向（dr）和横向（df）分量
    Direction dr = d > 0 ? NORTH : SOUTH; // 纵向分量（南北）
    Direction df = (std::abs(d % NORTH) < NORTH/2 ? d%NORTH : -(d%NORTH)) < 0 
                   ? WEST : EAST; // 横向分量（东西）

    // 计算坐标差判断移动类型（优先处理横向或纵向蹩腿）
    int diff = std::abs(file_of(to) - file_of(s)) - std::abs(rank_of(to) - rank_of(s));
    
    // 根据坐标差决定蹩腿位置：
    if (diff > 0)       // 横向差更大，蹩腿位置为横向
        s += df;
    else if (diff < 0)  // 纵向差更大，蹩腿位置为纵向
        s += dr;
    else                // 相等时取对角线方向（象）
        s += df + dr;

    b |= s; // 记录蹩腿位置
    return b;
}

/* 生成棋子所有可能移动路径的蹩腿位置 */
// pt: 棋子类型（BISHOP/KNIGHT）
template<PieceType pt>
Bitboard lame_leaper_path(Square s) {
    Bitboard b = 0;
    // 遍历棋子所有可能方向：象用田字方向，马用日字方向
    for (const auto& d : pt == BISHOP ? BishopDirections : KnightDirections)
        b |= lame_leaper_path<pt>(d, s); // 收集所有方向的蹩腿位置
    
    // 象的特殊处理：限制在己方半场（通过HalfBB数组过滤）
    if (pt == BISHOP)
        b &= HalfBB[rank_of(s) > RANK_4]; // RANK_4为楚河汉界分界线
    return b;
}

/* 生成考虑蹩腿规则的实际攻击范围 */
// pt: 棋子类型（BISHOP/KNIGHT）
template<PieceType pt>
Bitboard lame_leaper_attack(Square s, Bitboard occupied) {
    Bitboard b = 0;
    // 遍历所有可能移动方向
    for (const auto& d : pt == BISHOP ? BishopDirections : KnightDirections) {
        Square to = s + d;
        // 有效性检查：目标合法、距离合理、蹩腿位置无阻挡
        if (is_ok(to) && distance(s, to) < 4 && 
            !(lame_leaper_path<pt>(d, s) & occupied)) 
        {
            b |= to; // 添加可达位置
        }
    }
    // 象的特殊处理：限制在己方半场
    if (pt == BISHOP)
        b &= HalfBB[rank_of(s) > RANK_4];
    return b;
}


// Computes all rook and bishop attacks at startup. Magic
// bitboards are used to look up attacks of sliding pieces. As a reference see
// https://www.chessprogramming.org/Magic_Bitboards. In particular, here we use
// the so called "fancy" approach.
template<PieceType pt>
void init_magics(Bitboard table[], Magic magics[] IF_NOT_PEXT(, const Bitboard magicsInit[])) {

    Bitboard edges, b;
    uint64_t size = 0;

    for (Square s = SQ_A0; s <= SQ_I9; ++s)
    {
        // Board edges are not considered in the relevant occupancies
        edges = ((Rank0BB | Rank9BB) & ~rank_bb(s)) | ((FileABB | FileIBB) & ~file_bb(s));

        // Given a square 's', the mask is the bitboard of sliding attacks from
        // 's' computed on an empty board. The index must be big enough to contain
        // all the attacks for each possible subset of the mask and so is 2 power
        // the number of 1s of the mask.
        Magic& m = magics[s];
        m.mask   = pt == ROOK   ? sliding_attack<pt>(s, 0)
                 : pt == CANNON ? RookMagics[s].mask
                                : lame_leaper_path<pt>(s);
        if (pt != KNIGHT_TO)
            m.mask &= ~edges;

#ifdef USE_PEXT
        m.shift = popcount(uint64_t(m.mask));
#else
        m.magic = magicsInit[s];
        m.shift = 128 - popcount(m.mask);
#endif

        // Set the offset for the attacks table of the square. We have individual
        // table sizes for each square with "Fancy Magic Bitboards".
        m.attacks = s == SQ_A0 ? table : magics[s - 1].attacks + size;

        // Use Carry-Rippler trick to enumerate all subsets of masks[s] and
        // store the corresponding attack bitboard in m.attacks.
        b = size = 0;
        do
        {
            m.attacks[m.index(b)] =
              pt == ROOK || pt == CANNON ? sliding_attack<pt>(s, b) : lame_leaper_attack<pt>(s, b);

            size++;
            b = (b - m.mask) & m.mask;
        } while (b);
    }
}
}

}  // namespace Stockfish
