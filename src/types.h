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

#ifndef TYPES_H_INCLUDED
    #define TYPES_H_INCLUDED

// 当使用提供的 Makefile 编译时（例如在 Linux 和 OSX 上），配置会自动完成。要开始使用，请输入 `make help`。
//
// 当不使用 Makefile 时（例如使用 Microsoft Visual Studio），需要手动设置一些开关：
//
// -DNDEBUG      | 禁用调试模式。发布版本请始终使用此选项。
//
// -DNO_PREFETCH | 禁用 prefetch 汇编指令的使用。在某些非常旧的机器上运行可能需要此选项。
//
// -DUSE_POPCNT  | 添加对 popcnt 汇编指令的运行时支持。仅在 64 位模式下有效，并且需要硬件支持 popcnt。
//
// -DUSE_PEXT    | 添加对 pext 汇编指令的运行时支持。仅在 64 位模式下有效，并且需要硬件支持 pext。

    #include <cassert>
    #include <cstdint>

    #if defined(_MSC_VER)
        // 禁用 MSVC 编译器的一些烦人和嘈杂的警告
        #pragma warning(disable: 4127)  // 条件表达式是常量
        #pragma warning(disable: 4146)  // 一元减运算符应用于无符号类型
        #pragma warning(disable: 4800)  // 将值强制转换为布尔值 'true' 或 'false'
    #endif

// 预定义宏的混乱：
//
// __GNUC__                编译器是 GCC、Clang 或 ICX
// __clang__               编译器是 Clang 或 ICX
// __INTEL_LLVM_COMPILER   编译器是 ICX
// _MSC_VER                编译器是 MSVC
// _WIN32                  在 Windows 上构建（任何版本）
// _WIN64                  在 64 位 Windows 上构建

    #if defined(__GNUC__) && (__GNUC__ < 9 || (__GNUC__ == 9 && __GNUC_MINOR__ <= 2)) \
      && defined(_WIN32) && !defined(__clang__)
        #define ALIGNAS_ON_STACK_VARIABLES_BROKEN
    #endif

    #define ASSERT_ALIGNED(ptr, alignment) assert(reinterpret_cast<uintptr_t>(ptr) % alignment == 0)

    #if defined(_MSC_VER) && !defined(__clang__)
        #include <__msvc_int128.hpp>  // Microsoft 头文件，用于 std::_Unsigned128
using __uint128_t = std::_Unsigned128;
    #endif

    #if defined(_WIN64) && defined(_MSC_VER)  // 未使用 Makefile
        #include <intrin.h>                   // Microsoft 头文件，用于 _BitScanForward64()
        #define IS_64BIT
    #endif

    #if defined(USE_POPCNT) && defined(_MSC_VER)
        #include <nmmintrin.h>  // Microsoft 头文件，用于 _mm_popcnt_u64()
    #endif

    #if !defined(NO_PREFETCH) && defined(_MSC_VER)
        #include <xmmintrin.h>  // Microsoft 头文件，用于 _mm_prefetch()
    #endif

    #if defined(USE_PEXT)
        #include <immintrin.h>  // 头文件，用于 _pext_u64() 内联函数
        #if defined(_MSC_VER) && !defined(__clang__)
            #define pext(b, m, s) \
                ((_pext_u64(b._Word[1], m._Word[1]) << s) | _pext_u64(b._Word[0], m._Word[0]))
        #else
            #define pext(b, m, s) ((_pext_u64(b >> 64, m >> 64) << s) | _pext_u64(b, m))
        #endif
    #else
        #define pext(b, m, s) 0
    #endif

namespace Stockfish {

    #ifdef USE_POPCNT
constexpr bool HasPopCnt = true;
    #else
constexpr bool HasPopCnt = false;
    #endif

    #ifdef USE_PEXT
constexpr bool HasPext = true;
    #else
constexpr bool HasPext = false;
    #endif

    #ifdef IS_64BIT
constexpr bool Is64Bit = true;
    #else
constexpr bool Is64Bit = false;
    #endif

using Key      = uint64_t;
using Bitboard = __uint128_t;

constexpr int MAX_MOVES = 128;
constexpr int MAX_PLY   = 246; // 最大层数

// 红黑方
enum Color {
    WHITE, // 红方，应为RED，这里使用国际象棋的先手叫法
    BLACK, // 黑方
    COLOR_NB = 2
};

// 用于表示某个评估值是通过何种搜索得出的
enum Bound {
    // 表示未进行搜索，通常用于仅将局面的静态评估值或最佳着法存入置换表时
    BOUND_NONE,
 
    // 表示fail-low时的状态
    // 这是该局面的分数的最大值，表示该节点的真实评估值小于等于ttValue
    // 在非 PV 节点中，或者在没有最佳着法（例如，通过粗略估算进行剪枝时）的情况下，会处于这种状态
    BOUND_UPPER,
 
    // 表示fail-high时的状态
    // 这是该局面的分数的最低保证值，表示该节点的真实评估值大于等于ttValue
    // 在beta剪枝时，将剪枝时的值存入置换表时会使用此状态
    // 因为进行了beta剪枝，这说明在剩下的其他着法中，应该还有比这个评估值更高的着法
    BOUND_LOWER,
 
    // 表示既没有fail-low也没有fail-high，可以被认为是准确的分数。
    // 如果是PV节点并且bestMove作为具体的着法存在，则为此状态。
    // 注意：BOUND_EXACT & BOUND_UPPER == 1, BOUND_EXACT & BOUND_LOWER == 1
    BOUND_EXACT = BOUND_UPPER | BOUND_LOWER
};

// Value is used as an alias for int, this is done to differentiate between a search
// value and any other integer value. The values used in search are always supposed
// to be in the range (-VALUE_NONE, VALUE_NONE] and should not exceed this range.
using Value = int;

constexpr Value VALUE_ZERO     = 0;
constexpr Value VALUE_DRAW     = 0;
constexpr Value VALUE_NONE     = 32002;
constexpr Value VALUE_INFINITE = 32001;

constexpr Value VALUE_MATE             = 32000; // 绝杀分值
constexpr Value VALUE_MATE_IN_MAX_PLY  = VALUE_MATE - MAX_PLY;
constexpr Value VALUE_MATED_IN_MAX_PLY = -VALUE_MATE_IN_MAX_PLY;

constexpr bool is_valid(Value value) { return value != VALUE_NONE; }

constexpr bool is_win(Value value) {
    assert(is_valid(value));
    return value >= VALUE_MATE_IN_MAX_PLY;
}

constexpr bool is_loss(Value value) {
    assert(is_valid(value));
    return value <= VALUE_MATED_IN_MAX_PLY;
}

constexpr bool is_decisive(Value value) { return is_win(value) || is_loss(value); }

// 每种棋子的子力价值
constexpr Value RookValue    = 1305; // 车的价值
constexpr Value AdvisorValue = 219;  // 士的价值
constexpr Value CannonValue  = 773;  // 炮的价值
constexpr Value PawnValue    = 144;  // 兵卒的价值
constexpr Value KnightValue  = 720;  // 马的价值
constexpr Value BishopValue  = 187;  // 相象的价值

// clang-format off
// 棋子类型
// 经过设计，奇数均为大子，便于判断
// 例：ROOK(1), CANNON(3), KNIGHT(5), KING(7)
// 使用pt & 1可判断是否为大子
enum PieceType {
    NO_PIECE_TYPE, ROOK, ADVISOR, CANNON, PAWN, KNIGHT, BISHOP, KING, KNIGHT_TO,
    ALL_PIECES = 0,
    PIECE_TYPE_NB = 8
};

// 棋子
enum Piece {
    NO_PIECE,
    W_ROOK           , W_ADVISOR, W_CANNON, W_PAWN, W_KNIGHT, W_BISHOP, W_KING,
    B_ROOK = ROOK + 8, B_ADVISOR, B_CANNON, B_PAWN, B_KNIGHT, B_BISHOP, B_KING,
    PIECE_NB
};

// 子力价值数组
constexpr Value PieceValue[PIECE_NB] = {
  VALUE_ZERO, RookValue,   AdvisorValue, CannonValue, PawnValue,  KnightValue, BishopValue,  VALUE_ZERO,
  VALUE_ZERO, RookValue,   AdvisorValue, CannonValue, PawnValue,  KnightValue, BishopValue,  VALUE_ZERO};
// clang-format on

using Depth = int;

enum : int {
    // The following DEPTH_ constants are used for transposition table entries
    // and quiescence search move generation stages. In regular search, the
    // depth stored in the transposition table is literal: the search depth
    // (effort) used to make the corresponding transposition table value. In
    // quiescence search, however, the transposition table entries only store
    // the current quiescence move generation stage (which should thus compare
    // lower than any regular search depth).
    DEPTH_QS = 0,
    // For transposition table entries where no searching at all was done
    // (whether regular or qsearch) we use DEPTH_UNSEARCHED, which should thus
    // compare lower than any quiescence or regular depth. DEPTH_ENTRY_OFFSET
    // is used only for the transposition table entry occupancy check (see tt.cpp),
    // and should thus be lower than DEPTH_UNSEARCHED.
    DEPTH_UNSEARCHED   = -2,
    DEPTH_ENTRY_OFFSET = -3
};

// clang-format off
// 位置
enum Square : int {
    SQ_A0, SQ_B0, SQ_C0, SQ_D0, SQ_E0, SQ_F0, SQ_G0, SQ_H0, SQ_I0,
    SQ_A1, SQ_B1, SQ_C1, SQ_D1, SQ_E1, SQ_F1, SQ_G1, SQ_H1, SQ_I1,
    SQ_A2, SQ_B2, SQ_C2, SQ_D2, SQ_E2, SQ_F2, SQ_G2, SQ_H2, SQ_I2,
    SQ_A3, SQ_B3, SQ_C3, SQ_D3, SQ_E3, SQ_F3, SQ_G3, SQ_H3, SQ_I3,
    SQ_A4, SQ_B4, SQ_C4, SQ_D4, SQ_E4, SQ_F4, SQ_G4, SQ_H4, SQ_I4,
    SQ_A5, SQ_B5, SQ_C5, SQ_D5, SQ_E5, SQ_F5, SQ_G5, SQ_H5, SQ_I5,
    SQ_A6, SQ_B6, SQ_C6, SQ_D6, SQ_E6, SQ_F6, SQ_G6, SQ_H6, SQ_I6,
    SQ_A7, SQ_B7, SQ_C7, SQ_D7, SQ_E7, SQ_F7, SQ_G7, SQ_H7, SQ_I7,
    SQ_A8, SQ_B8, SQ_C8, SQ_D8, SQ_E8, SQ_F8, SQ_G8, SQ_H8, SQ_I8,
    SQ_A9, SQ_B9, SQ_C9, SQ_D9, SQ_E9, SQ_F9, SQ_G9, SQ_H9, SQ_I9,
    SQ_NONE,

    SQUARE_ZERO = 0,
    SQUARE_NB   = 90
};
// clang-format on

// 方向
enum Direction : int {
    NORTH = 9,
    EAST  = 1,
    SOUTH = -NORTH,
    WEST  = -EAST,

    NORTH_EAST = NORTH + EAST,
    SOUTH_EAST = SOUTH + EAST,
    SOUTH_WEST = SOUTH + WEST,
    NORTH_WEST = NORTH + WEST
};

// 规定起始局面，红方在下黑方在上时，红方左侧车的位置为A0

// 列
enum File : int {
    FILE_A,
    FILE_B,
    FILE_C,
    FILE_D,
    FILE_E,
    FILE_F,
    FILE_G,
    FILE_H,
    FILE_I,
    FILE_NB
};

// 行
enum Rank : int {
    RANK_0,
    RANK_1,
    RANK_2,
    RANK_3,
    RANK_4,
    RANK_5,
    RANK_6,
    RANK_7,
    RANK_8,
    RANK_9,
    RANK_NB
};

// For fast repetition checks
struct BloomFilter {
    constexpr static uint64_t FILTER_SIZE = 1 << 14;
    uint8_t                   operator[](Key key) const { return table[key & (FILTER_SIZE - 1)]; }
    uint8_t&                  operator[](Key key) { return table[key & (FILTER_SIZE - 1)]; }

   private:
    uint8_t table[1 << 14];
};

// Keep track of what a move changes on the board (used by NNUE)
struct DirtyPiece {

    // Number of changed pieces
    int dirty_num;

    // Max 2 pieces can change in one move. A capture moves the captured
    // piece to SQ_NONE and the piece to the capture square.
    Piece piece[2];

    // From and to squares, which may be SQ_NONE
    Square from[2];
    Square to[2];

    bool requires_refresh[2];
};

    #define ENABLE_INCR_OPERATORS_ON(T) \
        inline T& operator++(T& d) { return d = T(int(d) + 1); } \
        inline T& operator--(T& d) { return d = T(int(d) - 1); }

ENABLE_INCR_OPERATORS_ON(PieceType)
ENABLE_INCR_OPERATORS_ON(Square)
ENABLE_INCR_OPERATORS_ON(File)
ENABLE_INCR_OPERATORS_ON(Rank)

    #undef ENABLE_INCR_OPERATORS_ON

constexpr Direction operator-(Direction d) { return Direction(-int(d)); }
constexpr Direction operator+(Direction d1, Direction d2) { return Direction(int(d1) + int(d2)); }
constexpr Direction operator*(int i, Direction d) { return Direction(i * int(d)); }

// Additional operators to add a Direction to a Square
constexpr Square operator+(Square s, Direction d) { return Square(int(s) + int(d)); }
constexpr Square operator-(Square s, Direction d) { return Square(int(s) - int(d)); }
inline Square&   operator+=(Square& s, Direction d) { return s = s + d; }
inline Square&   operator-=(Square& s, Direction d) { return s = s - d; }

// 翻转红黑方
constexpr Color operator~(Color c) { return Color(c ^ BLACK); }

// Swap color of piece B_KNIGHT <-> W_KNIGHT
constexpr Piece operator~(Piece pc) { return Piece(pc ^ 8); }

constexpr Value mate_in(int ply) { return VALUE_MATE - ply; }

constexpr Value mated_in(int ply) { return -VALUE_MATE + ply; }

constexpr Square make_square(File f, Rank r) { return Square(r * FILE_NB + f); }

constexpr Piece make_piece(Color c, PieceType pt) { return Piece((c << 3) + pt); }

constexpr PieceType type_of(Piece pc) { return PieceType(pc & 7); }

inline Color color_of(Piece pc) {
    assert(pc != NO_PIECE);
    return Color(pc >> 3);
}

// 位置是否合法
constexpr bool is_ok(Square s) { return s >= SQ_A0 && s <= SQ_I9; }

constexpr File file_of(Square s) { return File(s % FILE_NB); }

constexpr Rank rank_of(Square s) { return Rank(s / FILE_NB); }

// 交换 A0 <-> A9
constexpr Square flip_rank(Square s) { return make_square(file_of(s), Rank(RANK_9 - rank_of(s))); }

// 交换 A0 <-> I0
constexpr Square flip_file(Square s) { return make_square(File(FILE_I - file_of(s)), rank_of(s)); }

// Based on a congruential pseudo-random number generator
constexpr Key make_key(uint64_t seed) {
    return seed * 6364136223846793005ULL + 1442695040888963407ULL;
}

// 一次走子需要16位来存储
//
// 位  0- 6: 目标位置（从0到89）
// 位  7-13: 起始位置（从0到89）
// （注：仅使用14位，高2位未被使用）
//
// 特殊情况是 Move::none() 和 Move::null()。我们可以将这些情况嵌入，因为在任何正常走子中，目标位置和起始位置总是不同的，
// 但 Move::none() 和 Move::null() 的起始位置和目标位置是相同的。

class Move { // 存储着法的类
   public:
    Move() = default;
    constexpr explicit Move(std::uint16_t d) :
        data(d) {}

    constexpr Move(Square from, Square to) :
        data((from << 7) + to) {}

    static constexpr Move make(Square from, Square to) { return Move((from << 7) + to); } // 将from（走子前的位置）和to（走子后的位置）存入一个uint16_t中

    constexpr Square from_sq() const { // 提取出棋子走子前的位置
        assert(is_ok());
        return Square((data >> 7) & 0x7F);
    }

    constexpr Square to_sq() const { // 提取出棋子走子后的位置
        assert(is_ok());
        return Square(data & 0x7F);
    }

    constexpr int from_to() const { return data & 0x3FFF; } // 将高2位设置为0，因为高2位未被使用

    constexpr bool is_ok() const { return none().data != data && null().data != data; }

    static constexpr Move null() { return Move(129); } // 用于空着裁剪（Null Move Pruning）的特殊移动值。起始位置=1，目标位置=1 (1*128 + 1 == 129)，使用UCI表示法表示为“0000”
    static constexpr Move none() { return Move(0); }   // 无效值，表示特殊值。起始位置=0，目标位置=0

    constexpr bool operator==(const Move& m) const { return data == m.data; }
    constexpr bool operator!=(const Move& m) const { return data != m.data; }

    constexpr explicit operator bool() const { return data != 0; }

    constexpr std::uint16_t raw() const { return data; }

    struct MoveHash {
        std::size_t operator()(const Move& m) const { return make_key(m.data); }
    };

   protected:
    std::uint16_t data;
};

}  // namespace Stockfish

#endif  // #ifndef TYPES_H_INCLUDED

#include "tune.h"  // Global visibility to tuning setup
