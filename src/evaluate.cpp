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

#include "evaluate.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdlib>
#include <iomanip>
#include <memory>
#include <sstream>

#include "nnue/network.h"
#include "nnue/nnue_misc.h"
#include "position.h"
#include "types.h"
#include "uci.h"
#include "nnue/nnue_accumulator.h"

namespace Stockfish {

// Evaluate is the evaluator for the outer world. It returns a static evaluation
// of the position from the point of view of the side to move.

// 该函数用于评估当前局面的局势，返回一个针对当前走子方的分数（正数为当前走子方优势，负数为另一方优势）
Value Eval::evaluate(const Eval::NNUE::Network& network,
                     const Position&            pos,
                     NNUE::AccumulatorCaches&   caches,
                     int                        optimism) {

    assert(!pos.checkers());

    auto [psqt, positional] = network.evaluate(pos, &caches.cache);
    Value nnue              = psqt + positional;
    int   nnueComplexity    = std::abs(psqt - positional);

    // Blend optimism and eval with nnue complexity
    optimism += optimism * nnueComplexity / 485;
    nnue -= nnue * nnueComplexity / 11683;

    int mm = pos.major_material() / 40;
    int v  = (nnue * (443 + mm) + optimism * (76 + mm)) / 503;

    // Damp down the evaluation linearly when shuffling
    v -= (v * pos.rule60_count()) / 267;

    // Guarantee evaluation does not hit the mate range
    // 保证得出的估值不为绝杀分值
    // 将v的值限制在VALUE_MATED_IN_MAX_PLY + 1和VALUE_MATE_IN_MAX_PLY - 1之间
    v = std::clamp(v, VALUE_MATED_IN_MAX_PLY + 1, VALUE_MATE_IN_MAX_PLY - 1);

    return v;
}

// Like evaluate(), but instead of returning a value, it returns
// a string (suitable for outputting to stdout) that contains the detailed
// descriptions and values of each evaluation term. Useful for debugging.
// Trace scores are from white's point of view

// trace可以查看当前局面的静态评估，用于调试
// 其中的分数是针对红方而言的
std::string Eval::trace(Position& pos, const Eval::NNUE::Network& network) {

    // 若当前局面存在将军则无静态评估
    if (pos.checkers())
        return "Final evaluation: none (in check)";

    auto caches = std::make_unique<Eval::NNUE::AccumulatorCaches>(network);

    std::stringstream ss;
    // 用于格式化输出的字符串流
    ss << std::showpoint << std::noshowpos << std::fixed << std::setprecision(2);

    // 输出NNUE的trace信息
    ss << '\n' << NNUE::trace(pos, network, *caches) << '\n';

    ss << std::showpoint << std::showpos << std::fixed << std::setprecision(2) << std::setw(15);

    auto [psqt, positional] = network.evaluate(pos, &caches->cache);
    Value v                 = psqt + positional;
    v                       = pos.side_to_move() == WHITE ? v : -v; // 调整评估的视角，使其始终是针对红方的
    // 输出纯NNUE的评估
    ss << "NNUE evaluation        " << 0.01 * UCIEngine::to_cp(v, pos) << " (white side)\n"; // white side（白方）是沿用了国际象棋的说法，中国象棋应为red side（红方）

    v = evaluate(network, pos, *caches, VALUE_ZERO); // 直接调用评估函数
    v = pos.side_to_move() == WHITE ? v : -v; // 调整评估的视角，使其始终是针对红方的
    ss << "Final evaluation       " << 0.01 * UCIEngine::to_cp(v, pos) << " (white side)";
    ss << " [with scaled NNUE, ...]";
    ss << "\n";

    // 返回包含所有评估信息的字符串
    return ss.str();
}

}  // namespace Stockfish
