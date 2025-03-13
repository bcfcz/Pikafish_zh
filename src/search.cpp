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
/*
Stockfish，一个基于 Glaurung 2.1 的 UCI 国际象棋引擎
版权所有 (C) 2004-2025 Stockfish 开发者（详见 AUTHORS 文件）

Stockfish 是自由软件：您可以根据自由软件基金会发布的 GNU 通用公共许可证的条款重新分发或修改它；许可证版本为第 3 版，或（由您选择）任何更高版本。

Stockfish 的分发是希望它有用，但 没有任何担保；甚至没有对适销性或特定用途适用性的暗示担保。详情请参阅 GNU 通用公共许可证。

您应该已经随本程序收到了 GNU 通用公共许可证的副本。如果没有，请访问 http://www.gnu.org/licenses/ 获取。
*/

#include "search.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <initializer_list>
#include <string>
#include <utility>

#include "evaluate.h"
#include "history.h"
#include "misc.h"
#include "movegen.h"
#include "movepick.h"
#include "nnue/network.h"
#include "nnue/nnue_accumulator.h"
#include "nnue/nnue_common.h"
#include "nnue/nnue_misc.h"
#include "position.h"
#include "thread.h"
#include "timeman.h"
#include "tt.h"
#include "uci.h"
#include "ucioption.h"

namespace Stockfish {

using namespace Search;

namespace {

// Futility margin
// 无效剪枝边界
Value futility_margin(Depth d, bool noTtCutNode, bool improving, bool oppWorsening) {
    Value futilityMult       = 140 - 33 * noTtCutNode;
    Value improvingDeduction = improving * futilityMult * 2;
    Value worseningDeduction = oppWorsening * futilityMult / 3;

    return futilityMult * d - improvingDeduction - worseningDeduction;
}

constexpr int futility_move_count(bool improving, Depth depth) {
    return (3 + depth * depth) / (2 - improving);
}

int correction_value(const Worker& w, const Position& pos, Stack* ss) {
    const Color us    = pos.side_to_move();
    const auto  m     = (ss - 1)->currentMove;
    const auto  pcv   = w.pawnCorrectionHistory[us][pawn_structure_index<Correction>(pos)];
    const auto  macv  = w.majorPieceCorrectionHistory[us][major_piece_index(pos)];
    const auto  micv  = w.minorPieceCorrectionHistory[us][minor_piece_index(pos)];
    const auto  wnpcv = w.nonPawnCorrectionHistory[WHITE][us][non_pawn_index<WHITE>(pos)];
    const auto  bnpcv = w.nonPawnCorrectionHistory[BLACK][us][non_pawn_index<BLACK>(pos)];
    const auto  cntcv =
      m.is_ok() ? (*(ss - 2)->continuationCorrectionHistory)[pos.piece_on(m.to_sq())][m.to_sq()]
                 : 0;

    return (4539 * pcv + 3697 * macv + 3347 * micv + 7373 * (wnpcv + bnpcv) + 8482 * cntcv);
}

// Add correctionHistory value to raw staticEval and guarantee evaluation
// does not hit the tablebase range.
// 将 校正历史 值添加到原始的静态评估中，并保证评估
// 不会触及棋谱库的范围。 
Value to_corrected_static_eval(Value v, const int cv) {
    return std::clamp(v + cv / 131072, VALUE_MATED_IN_MAX_PLY + 1, VALUE_MATE_IN_MAX_PLY - 1);
}

// History and stats update bonus, based on depth
// 基于深度的历史和统计更新奖励 
int stat_bonus(Depth d) { return std::min(158 * d - 87, 2168); }

// History and stats update malus, based on depth
// 基于深度的历史和统计更新减益 
int stat_malus(Depth d) { return std::min(977 * d - 282, 1524); }

// Add a small random component to draw evaluations to avoid 3-fold blindness
// 在和棋评估中添加一个小的随机成分，以避免三重重复局面的盲点
Value value_draw(size_t nodes) { return VALUE_DRAW - 1 + Value(nodes & 0x2); }

Value value_to_tt(Value v, int ply);
Value value_from_tt(Value v, int ply, int r60c);
void  update_pv(Move* pv, Move move, const Move* childPv);
void  update_continuation_histories(Stack* ss, Piece pc, Square to, int bonus);
void  update_quiet_histories(
   const Position& pos, Stack* ss, Search::Worker& workerThread, Move move, int bonus);
void update_all_stats(const Position&      pos,
                      Stack*               ss,
                      Search::Worker&      workerThread,
                      Move                 bestMove,
                      Square               prevSq,
                      ValueList<Move, 32>& quietsSearched,
                      ValueList<Move, 32>& capturesSearched,
                      Depth                depth);

}  // namespace

Search::Worker::Worker(SharedState&                    sharedState,
                       std::unique_ptr<ISearchManager> sm,
                       size_t                          threadId,
                       NumaReplicatedAccessToken       token) :
    // Unpack the SharedState struct into member variables
    // 将 共享状态 结构体解包为成员变量
    threadIdx(threadId),
    numaAccessToken(token),
    manager(std::move(sm)),
    options(sharedState.options),
    threads(sharedState.threads),
    tt(sharedState.tt),
    network(sharedState.network),
    refreshTable(network[token]) {
    clear();
}

void Search::Worker::ensure_network_replicated() {
    // Access once to force lazy initialization.
    // We do this because we want to avoid initialization during search.
    // 访问一次以强制进行延迟初始化。
    // 我们这样做是因为我们希望在搜索期间避免初始化。 
    (void) (network[numaAccessToken]);
}

// 开始搜索的入口函数，属于 Search::Worker 类
void Search::Worker::start_searching() {

    // 非主线程直接进入迭代加深搜索
    if (!is_mainthread())
    {
        iterative_deepening();  // 非主线程直接执行迭代加深算法
        return;                 // 非主线程后续无需处理其他逻辑
    }

    /*********************** 主线程专属逻辑 ***********************/
    // 初始化时间管理器（计算剩余时间、分配时间策略等）
    main_manager()->tm.init(limits, rootPos.side_to_move(), rootPos.game_ply(), options,
                            main_manager()->originalTimeAdjust);
    tt.new_search();  // 重置置换表（Transposition Table），标记新搜索开始

    // 处理无有效着法的特殊情况
    if (rootMoves.empty())
    {
        rootMoves.emplace_back(Move::none());  // 插入一个无效着法占位
        // 通知无有效着法的结果（直接输棋）
        main_manager()->updates.onUpdateNoMoves({0, {-VALUE_MATE, rootPos}});
    }
    else
    {
        threads.start_searching();  // 启动所有非主线程开始搜索
        iterative_deepening();      // 主线程自身也开始迭代深化搜索
    }

    /*********************** 搜索结束后的同步处理 ***********************/
    /* 当达到最大深度时，可能未触发 threads.stop。但在ponder或无限搜索模式下，
       UCI协议要求必须在收到"stop"或"ponderhit"后才能输出最佳着法 */
    // 忙等待：直到收到停止信号 或 退出ponder/无限模式
    while (!threads.stop && (main_manager()->ponder || limits.infinite))
    {}  // 空循环等待，消耗CPU但响应及时

    threads.stop = true;  // 强制设置停止标志，确保所有线程终止

    threads.wait_for_search_finished();  // 阻塞等待所有线程完全停止

    // 处理"nodes as time"模式：将实际时间转换为虚拟节点数进行管理
    if (limits.npmsec)
        main_manager()->tm.advance_nodes_time(threads.nodes_searched()
                                              - limits.inc[rootPos.side_to_move()]);

    /*********************** 确定最佳结果并输出 ***********************/
    Worker* bestThread = this;  // 默认当前线程为最佳

    // 当启用单线MultiPV且非固定深度搜索时，从所有线程中选取最佳
    if (int(options["MultiPV"]) == 1 && !limits.depth && rootMoves[0].pv[0] != Move::none())
        bestThread = threads.get_best_thread()->worker.get();

    // 记录最佳分数用于后续搜索参考
    main_manager()->bestPreviousScore        = bestThread->rootMoves[0].score;
    main_manager()->bestPreviousAverageScore = bestThread->rootMoves[0].averageScore;

    // 如果最佳线程非当前线程，需重新发送其PV信息
    if (bestThread != this)
        main_manager()->pv(*bestThread, threads, tt, bestThread->completedDepth);

    // 准备ponder着法（对手的预期回应）
    std::string ponder;
    if (bestThread->rootMoves[0].pv.size() > 1 ||  // PV列表中有后续着法
        // 或从置换表中提取ponder着法（例如哈希表中有历史信息）
        bestThread->rootMoves[0].extract_ponder_from_tt(tt, rootPos))
        ponder = UCIEngine::move(bestThread->rootMoves[0].pv[1]);  // 取PV的第二个着法作为ponder

    // 转换最佳着法为UCI格式并通知上层
    auto bestmove = UCIEngine::move(bestThread->rootMoves[0].pv[0]);
    main_manager()->updates.onBestmove(bestmove, ponder);  // 触发最佳着法回调
}

// Main iterative deepening loop. It calls search()
// repeatedly with increasing depth until the allocated thinking time has been
// consumed, the user stops the search, or the maximum search depth is reached.

// 主要迭代加深循环。不断调用search()增加搜索深度，直到时间耗尽、用户停止或达到最大深度
void Search::Worker::iterative_deepening() {

    // 获取主线程指针（如果是主线程）
    SearchManager* mainThread = (is_mainthread() ? main_manager() : nullptr);

    // 存储最佳走法序列（主要变例，Principal Variation）的数组
    Move pv[MAX_PLY + 1];

    Depth lastBestMoveDepth = 0;
    Value lastBestScore     = -VALUE_INFINITE;
    auto  lastBestPV        = std::vector{Move::none()};

    Value  alpha, beta; // Alpha-Beta剪枝变量
    Value  bestValue     = -VALUE_INFINITE;
    Color  us            = rootPos.side_to_move();
    double timeReduction = 1, totBestMoveChanges = 0;
    int    delta, iterIdx                        = 0;

    // Allocate stack with extra size to allow access from (ss - 7) to (ss + 2):
    // (ss - 7) is needed for update_continuation_histories(ss - 1) which accesses (ss - 6),
    // (ss + 2) is needed for initialization of cutOffCnt.
    // 分配带有额外大小的栈，以便能从 (ss - 7) 访问到 (ss + 2)：
    // 需要 (ss - 7) 是因为 update_continuation_histories (ss - 1) 会访问 (ss - 6)，
    // 需要 (ss + 2) 是因为要初始化 cutOffCnt。
    Stack  stack[MAX_PLY + 10] = {};
    Stack* ss                  = stack + 7;
  
    for (int i = 7; i > 0; --i)
    {
        // 将 (ss - i) 的 continuationHistory 指向一个哨兵值
        (ss - i)->continuationHistory =
          &this->continuationHistory[0][0][NO_PIECE][0]; // Use as a sentinel // 用作哨兵
    
        // 将 (ss - i) 的 continuationCorrectionHistory 指向一个哨兵值
        (ss - i)->continuationCorrectionHistory = &this->continuationCorrectionHistory[NO_PIECE][0];
    
        // 将 (ss - i) 的 staticEval 设置为 VALUE_NONE，表示未评估的状态
        (ss - i)->staticEval = VALUE_NONE;
    }

    for (int i = 0; i <= MAX_PLY + 2; ++i)
        (ss + i)->ply = i;

    ss->pv = pv;

    if (mainThread)
    {
        if (mainThread->bestPreviousScore == VALUE_INFINITE)
            mainThread->iterValue.fill(VALUE_ZERO);
        else
            mainThread->iterValue.fill(mainThread->bestPreviousScore);
    }

    size_t multiPV = size_t(options["MultiPV"]);

    multiPV = std::min(multiPV, rootMoves.size());

    int searchAgainCounter = 0;

    lowPlyHistory.fill(106);

    // Iterative deepening loop until requested to stop or the target depth is reached
    // 迭代加深循环，直到收到停止请求或达到目标深度
    while (++rootDepth < MAX_PLY && !threads.stop
           && !(limits.depth && mainThread && rootDepth > limits.depth))
    {
        // Age out PV variability metric
        if (mainThread)
            totBestMoveChanges /= 2;

        // Save the last iteration's scores before the first PV line is searched and
        // all the move scores except the (new) PV are set to -VALUE_INFINITE.
        for (RootMove& rm : rootMoves)
            rm.previousScore = rm.score;

        size_t pvFirst = 0;
        pvLast         = rootMoves.size();

        if (!threads.increaseDepth)
            searchAgainCounter++;

        // MultiPV loop. We perform a full root search for each PV line
        for (pvIdx = 0; pvIdx < multiPV; ++pvIdx)
        {
            // Reset UCI info selDepth for each depth and each PV line
            selDepth = 0;

            // Reset aspiration window starting size
            delta     = 10 + std::abs(rootMoves[pvIdx].meanSquaredScore) / 44420;
            Value avg = rootMoves[pvIdx].averageScore;
            alpha     = std::max(avg - delta, -VALUE_INFINITE);
            beta      = std::min(avg + delta, VALUE_INFINITE);

            // Adjust optimism based on root move's averageScore (~4 Elo)
            optimism[us]  = 99 * avg / (std::abs(avg) + 92);
            optimism[~us] = -optimism[us];

            // Start with a small aspiration window and, in the case of a fail
            // high/low, re-search with a bigger window until we don't fail
            // high/low anymore.
            int failedHighCnt = 0;
            while (true)
            {
                // Adjust the effective depth searched, but ensure at least one
                // effective increment for every four searchAgain steps (see issue #2717).
                Depth adjustedDepth =
                  std::max(1, rootDepth - failedHighCnt - 3 * (searchAgainCounter + 1) / 4);
                rootDelta = beta - alpha;
                bestValue = search<Root>(rootPos, ss, alpha, beta, adjustedDepth, false);

                // Bring the best move to the front. It is critical that sorting
                // is done with a stable algorithm because all the values but the
                // first and eventually the new best one is set to -VALUE_INFINITE
                // and we want to keep the same order for all the moves except the
                // new PV that goes to the front. Note that in the case of MultiPV
                // search the already searched PV lines are preserved.
                std::stable_sort(rootMoves.begin() + pvIdx, rootMoves.begin() + pvLast);

                // If search has been stopped, we break immediately. Sorting is
                // safe because RootMoves is still valid, although it refers to
                // the previous iteration.
                // 检查停止信号
                if (threads.stop)
                    break;

                // When failing high/low give some update before a re-search. To avoid
                // excessive output that could hang GUIs, only start at nodes > 10M
                // (rather than depth N, which can be reached quickly)
                if (mainThread && multiPV == 1 && (bestValue <= alpha || bestValue >= beta)
                    && nodes > 10000000)
                    main_manager()->pv(*this, threads, tt, rootDepth);

                // In case of failing low/high increase aspiration window and re-search,
                // otherwise exit the loop.
                if (bestValue <= alpha)
                {
                    beta  = (alpha + beta) / 2;
                    alpha = std::max(bestValue - delta, -VALUE_INFINITE);

                    failedHighCnt = 0;
                    if (mainThread)
                        mainThread->stopOnPonderhit = false;
                }
                else if (bestValue >= beta)
                {
                    beta = std::min(bestValue + delta, VALUE_INFINITE);
                    ++failedHighCnt;
                }
                else
                    break;

                delta += delta / 3;

                assert(alpha >= -VALUE_INFINITE && beta <= VALUE_INFINITE);
            }

            // Sort the PV lines searched so far and update the GUI
            std::stable_sort(rootMoves.begin() + pvFirst, rootMoves.begin() + pvIdx + 1);

            if (mainThread
                && (threads.stop || pvIdx + 1 == multiPV || nodes > 10000000)
                // A thread that aborted search can have mated-in PV and
                // score that cannot be trusted, i.e. it can be delayed or refuted
                // if we would have had time to fully search other root-moves. Thus
                // we suppress this output and below pick a proven score/PV for this
                // thread (from the previous iteration).
                && !(threads.abortedSearch && is_loss(rootMoves[0].uciScore)))
                main_manager()->pv(*this, threads, tt, rootDepth);

            if (threads.stop)
                break;
        }

        if (!threads.stop)
            completedDepth = rootDepth;

        // We make sure not to pick an unproven mated-in score,
        // in case this thread prematurely stopped search (aborted-search).
        if (threads.abortedSearch && rootMoves[0].score != -VALUE_INFINITE
            && is_loss(rootMoves[0].score))
        {
            // Bring the last best move to the front for best thread selection.
            Utility::move_to_front(rootMoves, [&lastBestPV = std::as_const(lastBestPV)](
                                                const auto& rm) { return rm == lastBestPV[0]; });
            rootMoves[0].pv    = lastBestPV;
            rootMoves[0].score = rootMoves[0].uciScore = lastBestScore;
        }
        else if (rootMoves[0].pv[0] != lastBestPV[0])
        {
            lastBestPV        = rootMoves[0].pv;
            lastBestScore     = rootMoves[0].score;
            lastBestMoveDepth = rootDepth;
        }

        if (!mainThread)
            continue;

        // Have we found a "mate in x"?
        // 检查是否找到指定步数内的杀棋
        if (limits.mate && rootMoves[0].score == rootMoves[0].uciScore
            && ((rootMoves[0].score >= VALUE_MATE_IN_MAX_PLY
                 && VALUE_MATE - rootMoves[0].score <= 2 * limits.mate)
                || (rootMoves[0].score != -VALUE_INFINITE
                    && rootMoves[0].score <= VALUE_MATED_IN_MAX_PLY
                    && VALUE_MATE + rootMoves[0].score <= 2 * limits.mate)))
            threads.stop = true;

        // Use part of the gained time from a previous stable move for the current move
        for (auto&& th : threads)
        {
            totBestMoveChanges += th->worker->bestMoveChanges;
            th->worker->bestMoveChanges = 0;
        }

        // Do we have time for the next iteration? Can we stop searching now?
        // 时间管理逻辑
        if (limits.use_time_management() && !threads.stop && !mainThread->stopOnPonderhit)
        {
            int nodesEffort = rootMoves[0].effort * 144 / std::max(size_t(1), size_t(nodes));

            double fallingEval = (86 + 14 * (mainThread->bestPreviousAverageScore - bestValue)
                                  + 4 * (mainThread->iterValue[iterIdx] - bestValue))
                               / 566.87;
            fallingEval = std::clamp(fallingEval, 0.62, 1.76);

            // If the bestMove is stable over several iterations, reduce time accordingly
            timeReduction    = lastBestMoveDepth + 12 < completedDepth ? 1.59 : 0.63;
            double reduction = (1.91 + mainThread->previousTimeReduction) / (3.17 * timeReduction);
            double bestMoveInstability = 0.87 + 1.62 * totBestMoveChanges / threads.size();

            double totalTime =
              mainThread->tm.optimum() * fallingEval * reduction * bestMoveInstability;

            auto elapsedTime = elapsed();

            if (completedDepth >= 9 && nodesEffort >= 111 && elapsedTime > totalTime * 0.73
                && !mainThread->ponder)
                threads.stop = true;

            // Stop the search if we have exceeded the totalTime
            if (elapsedTime > totalTime)
            {
                // If we are allowed to ponder do not stop the search now but
                // keep pondering until the GUI sends "ponderhit" or "stop".
                if (mainThread->ponder)
                    mainThread->stopOnPonderhit = true;
                else
                    threads.stop = true;
            }
            else
                threads.increaseDepth = mainThread->ponder || elapsedTime <= totalTime * 0.279;
        }

        mainThread->iterValue[iterIdx] = bestValue;
        iterIdx                        = (iterIdx + 1) & 3;
    }

    if (!mainThread)
        return;

    mainThread->previousTimeReduction = timeReduction;
}

// Reset histories, usually before a new game
void Search::Worker::clear() {
    mainHistory.fill(61);
    lowPlyHistory.fill(106);
    captureHistory.fill(-598);
    pawnHistory.fill(-1181);
    pawnCorrectionHistory.fill(0);
    majorPieceCorrectionHistory.fill(0);
    minorPieceCorrectionHistory.fill(0);
    nonPawnCorrectionHistory[WHITE].fill(0);
    nonPawnCorrectionHistory[BLACK].fill(0);

    for (auto& to : continuationCorrectionHistory)
        for (auto& h : to)
            h->fill(0);

    for (bool inCheck : {false, true})
        for (StatsType c : {NoCaptures, Captures})
            for (auto& to : continuationHistory[inCheck][c])
                for (auto& h : to)
                    h->fill(-427);

    for (size_t i = 1; i < reductions.size(); ++i)
        reductions[i] = int(14.60 * std::log(i));

    refreshTable.clear(network[numaAccessToken]);
}


// Main search function for both PV and non-PV nodes
// PV和非PV节点的主搜索函数
template<NodeType nodeType>
Value Search::Worker::search(
  Position& pos, Stack* ss, Value alpha, Value beta, Depth depth, bool cutNode) {

    constexpr bool PvNode   = nodeType != NonPV;
    constexpr bool rootNode = nodeType == Root;
    const bool     allNode  = !(PvNode || cutNode);

    // Dive into quiescence search when the depth reaches zero
    // 当深度达到零时，开始静态搜索
    if (depth <= 0)
    {
        constexpr auto nt = PvNode ? PV : NonPV; // 判断当前节点是否为PV节点
        return qsearch<nt>(pos, ss, alpha, beta); // 进入静态搜索
    }

    // Limit the depth if extensions made it too large
    // 如果延伸太大，限制深度
    depth = std::min(depth, MAX_PLY - 1);

    assert(-VALUE_INFINITE <= alpha && alpha < beta && beta <= VALUE_INFINITE);
    assert(PvNode || (alpha == beta - 1));
    assert(0 < depth && depth < MAX_PLY);
    assert(!(PvNode && cutNode));

    Move      pv[MAX_PLY + 1];
    StateInfo st;
    ASSERT_ALIGNED(&st, Eval::NNUE::CacheLineSize);

    Key   posKey;
    Move  move, excludedMove, bestMove;
    Depth extension, newDepth;
    Value bestValue, value, eval, maxValue, probCutBeta;
    bool  givesCheck, improving, priorCapture, opponentWorsening;
    bool  capture, ttCapture;
    Piece movedPiece;

    ValueList<Move, 32> capturesSearched;
    ValueList<Move, 32> quietsSearched;

    // Step 1. Initialize node
    // 步骤1.初始化节点
    Worker* thisThread = this;
    ss->inCheck        = bool(pos.checkers()); // 是否为将军状态
    priorCapture       = pos.captured_piece(); // 之前吃的棋子
    Color us           = pos.side_to_move(); // 我方的颜色
    ss->moveCount      = 0; // 重置当前局面中合法着法的数量
    bestValue          = -VALUE_INFINITE; // 将bestValue初始化为负无穷。只要存在一个有效的着法，bestValue的值就会比初始值大
    maxValue           = VALUE_INFINITE;

    // Check for the available remaining time
    // 检查剩余可用时间
    if (is_mainthread())
        main_manager()->check_time(*thisThread);

    // Used to send selDepth info to GUI (selDepth counts from 1, ply from 0)
    // 用于将selDepth（选择性搜索深度）信息发送到GUI（selDepth计数从1开始，层数从0开始）
    if (PvNode && thisThread->selDepth < ss->ply + 1) // 相当于max操作
        thisThread->selDepth = ss->ply + 1;

    // 如果不是根节点
    if (!rootNode)
    {
        // Step 2. Check for aborted search and repetition
        // 步骤2. 检查是否中止搜索和循环
        Value result = VALUE_NONE;
        if (pos.rule_judge(result, ss->ply)) // 棋规裁决
            return result == VALUE_DRAW ? value_draw(thisThread->nodes) : result;
        if (result != VALUE_NONE)
        {
            assert(result != VALUE_DRAW);

            // 2 fold result is mate for us, the only chance for the opponent is to get a draw
            // We can guarantee to get at least a draw score during searching for that line
            // 2次循环的裁决结果是我们可以将死对方（MATE），对手唯一的机会是和棋
            // 在寻找这一线路的过程中，我们可以保证至少获得一个和棋的结果
            // 因此分数下界为和棋
            if (result > VALUE_DRAW)
                alpha = std::max(alpha, VALUE_DRAW - 1);
            // 2 fold result is mated for us, the only chance for us is to get a draw
            // We can guarantee to get no more than a draw score during searching for that line
            // 2次循环的裁决结果对我们来说是必输的（MATED），我们唯一的希望就是争取和棋
            // 在寻找那条路线的过程中，我们能保证得到的最好结果就是和棋
            // 因此分数上界为和棋
            else
                beta = std::min(beta, VALUE_DRAW + 1);
        }

        if (threads.stop.load(std::memory_order_relaxed) || ss->ply >= MAX_PLY)
            return (ss->ply >= MAX_PLY && !ss->inCheck) ? evaluate(pos)
                                                        : value_draw(thisThread->nodes);

        // Step 3. Mate distance pruning. Even if we mate at the next move our score
        // would be at best mate_in(ss->ply + 1), but if alpha is already bigger because
        // a shorter mate was found upward in the tree then there is no need to search
        // because we will never beat the current alpha. Same logic but with reversed
        // signs apply also in the opposite condition of being mated instead of giving
        // mate. In this case, return a fail-high score.
        // 第 3 步：绝杀距离剪枝。即使我们在下一步走棋就能绝杀，我们的得分最多也只是mate_in(ss->ply + 1)，
        // 但如果alpha值已经更大，因为向上在树中找到了更短的绝杀，那么就没有必要继续搜索，因为我们永远无法超越当前的alpha值。
        // 同样的逻辑，但符号相反，也适用于相反的情况，即被绝杀而不是给予绝杀。在这种情况下，返回一个fail-high得分。
        alpha = std::max(mated_in(ss->ply), alpha); // 最差的情况是被对方将死
        beta  = std::min(mate_in(ss->ply + 1), beta); // 最好的情况是在下一步将死对方
        if (alpha >= beta) // Alpha-Beta剪枝
            return alpha;
    }

    assert(0 <= ss->ply && ss->ply < MAX_PLY);

    // 初始化
    bestMove            = Move::none();
    (ss + 2)->cutoffCnt = 0;
    Square prevSq = ((ss - 1)->currentMove).is_ok() ? ((ss - 1)->currentMove).to_sq() : SQ_NONE;
    ss->statScore = 0;

    // Step 4. Transposition table lookup
    // 第4步：查找置换表
    excludedMove                   = ss->excludedMove; // 如果当前局面存在排除的着法，则将其赋值
    posKey                         = pos.key(); // 获取当前局面的key（哈希键）
    auto [ttHit, ttData, ttWriter] = tt.probe(posKey); // 查找置换表，返回值是std::tuple
    // Need further processing of the saved data
    // 需要对保存的数据进行进一步处理
    ss->ttHit    = ttHit;
    // 如果是根节点，则返回PV中的着法；否则返回置换表中的着法。
    // 这是为了防止在根节点置换表被覆盖后，返回错误的着法。
    // 根节点中不进行分离，因此pvIdx是当前探索中的PV线，而pv[0]是根节点的当前最佳着法。
    ttData.move  = rootNode ? thisThread->rootMoves[thisThread->pvIdx].pv[0]
                 : ttHit    ? ttData.move
                            : Move::none();
    // 当前局面在置换表中注册的分值
    ttData.value = ttHit ? value_from_tt(ttData.value, ss->ply, pos.rule60_count()) : VALUE_NONE;
    ss->ttPv     = excludedMove ? ss->ttPv : PvNode || (ttHit && ttData.is_pv);
    ttCapture    = ttData.move && pos.capture(ttData.move);

    // At this point, if excluded, skip straight to step 5, static eval. However,
    // to save indentation, we list the condition in all code between here and there.
    // 此时，如果被排除，则直接跳到第5步，静态评估。但是，为了节省缩进空间，我们将在此处到那里的所有代码中列出该条件

    // At non-PV nodes we check for an early TT cutoff
    // 在非PV节点，我们检查是否可以提前进行置换表截断
    if (!PvNode && !excludedMove && ttData.depth > depth - (ttData.value <= beta)
        && is_valid(ttData.value)  // Can happen when !ttHit or when access race in probe()// 可能发生在 !ttHit 时，或 probe() 中出现多线程冲突时
        && (ttData.bound & (ttData.value >= beta ? BOUND_LOWER : BOUND_UPPER))
        && (cutNode == (ttData.value >= beta) || depth > 9))
    {
        // If ttMove is quiet, update move sorting heuristics on TT hit (~2 Elo)
        // 如果置换表移动(ttMove)是静默移动，在置换表命中时更新移动排序启发式评分（约提升2 Elo）
        if (ttData.move && ttData.value >= beta)
        {
            // Bonus for a quiet ttMove that fails high (~2 Elo)
            // 对导致Beta剪枝的静默移动给予奖励（约+2 Elo）
            if (!ttCapture)
                update_quiet_histories(pos, ss, *this, ttData.move, stat_bonus(depth) * 747 / 1024);

            // Extra penalty for early quiet moves of
            // the previous ply (~1 Elo on STC, ~2 Elo on LTC)
            // 对前一层早期的静默移动施加额外惩罚（短时间控制+1 Elo，长时间控制+2 Elo）
            if (prevSq != SQ_NONE && (ss - 1)->moveCount <= 2 && !priorCapture)
                update_continuation_histories(ss - 1, pos.piece_on(prevSq), prevSq,
                                            -stat_malus(depth + 1) * 1091 / 1024);
        }

        // Partial workaround for the graph history interaction problem
        // For high rule60 counts don't produce transposition table cutoffs.
        // 针对图历史交互问题的部分解决方法
        // 当rule60计数较高时，不进行置换表剪枝
        if (pos.rule60_count() < 110)
            return ttData.value;
    }

    // Step 5. Static evaluation of the position
    // 第5步：局面的静态评估
    Value      unadjustedStaticEval = VALUE_NONE;
    const auto correctionValue      = correction_value(*thisThread, pos, ss);
    if (ss->inCheck)
    {
        // 将军时跳过早期的剪枝
        ss->staticEval = eval = (ss - 2)->staticEval;
        improving             = false;
        goto moves_loop;
    }
    else if (excludedMove)
    {
        // Providing the hint that this node's accumulator will be used often
        // brings significant Elo gain (~13 Elo).
        // 提示该节点的评估累加器将被频繁使用，可显著提升引擎强度（约+13 Elo）
        // 优化原理：通过 NUMA 亲和性预加载神经网络权重，减少缓存失效
        Eval::NNUE::hint_common_parent_position(
            pos,                   // 当前棋盘位置
            network[numaAccessToken], // NUMA 节点对应的神经网络（优化跨节点访问延迟）
            refreshTable           // 神经网络权重刷新表
        );

        // 直接使用静态评估值（不进行动态调整）
        // ss->staticEval 是之前计算的原始静态评估值
        unadjustedStaticEval = eval = ss->staticEval; 
    }
    else if (ss->ttHit)
    {
        // Never assume anything about values stored in TT
        // 永远不要对存储在 TT 中的值做任何假设
        unadjustedStaticEval = ttData.eval;
        if (!is_valid(unadjustedStaticEval))
            unadjustedStaticEval = evaluate(pos);
        else if (PvNode)
            Eval::NNUE::hint_common_parent_position(pos, network[numaAccessToken], refreshTable);

        ss->staticEval = eval = to_corrected_static_eval(unadjustedStaticEval, correctionValue);

        // ttValue can be used as a better position evaluation (~7 Elo)
        // ttValue 可用作更好的局面评估（约 7 个 Elo 分） 
        if (is_valid(ttData.value)
            && (ttData.bound & (ttData.value > eval ? BOUND_LOWER : BOUND_UPPER)))
            eval = ttData.value;
    }
    else
    {
        unadjustedStaticEval = evaluate(pos);
        ss->staticEval = eval = to_corrected_static_eval(unadjustedStaticEval, correctionValue);

        // Static evaluation is saved as it was before adjustment by correction history
        // 静态评估被保存为在通过修正历史进行调整之前的样子 
        ttWriter.write(posKey, VALUE_NONE, ss->ttPv, BOUND_NONE, DEPTH_UNSEARCHED, Move::none(),
                       unadjustedStaticEval, tt.generation());
    }

    // Use static evaluation difference to improve quiet move ordering (~9 Elo)
    // 使用静态评估差异来改进安静着法的排序（约 9 个 Elo 分） 
    if (((ss - 1)->currentMove).is_ok() && !(ss - 1)->inCheck && !priorCapture)
    {
        int bonus = std::clamp(-17 * int((ss - 1)->staticEval + ss->staticEval), -1024, 2058) + 332;
        thisThread->mainHistory[~us][((ss - 1)->currentMove).from_to()] << bonus * 1340 / 1024;
        if (type_of(pos.piece_on(prevSq)) != PAWN)
            thisThread->pawnHistory[pawn_structure_index(pos)][pos.piece_on(prevSq)][prevSq]
              << bonus * 1159 / 1024;
    }

    // Set up the improving flag, which is true if current static evaluation is
    // bigger than the previous static evaluation at our turn (if we were in
    // check at our previous move we go back until we weren't in check) and is
    // false otherwise. The improving flag is used in various pruning heuristics.
    // 设置改进标志，如果在我们回合时当前的静态评估大于之前的静态评估
    //（如果我们在上一步移动时处于将军状态，则回溯直到我们不处于将军状态），
    //则该标志为真，否则为假。改进标志用于各种剪枝启发式算法。 
    improving = ss->staticEval > (ss - 2)->staticEval;

    opponentWorsening = ss->staticEval + (ss - 1)->staticEval > 2;

    // Step 6. Razoring (~1 Elo)
    // If eval is really low, check with qsearch if we can exceed alpha. If the
    // search suggests we cannot exceed alpha, return a speculative fail low.
    // 步骤 6. 剃刀剪枝（约 1 个 Elo 分）
    // 如果评估值非常低，通过 qsearch 检查我们是否能超过 alpha。如果搜索表明我们不能超过 alpha，返回一个推测的失败低值。 
    if (eval < alpha - 1373 - 252 * depth * depth)
    {
        value = qsearch<NonPV>(pos, ss, alpha - 1, alpha);
        if (value < alpha && !is_decisive(value))
            return value;
    }

    // Step 7. Futility pruning: child node (~40 Elo)
    // The depth condition is important for mate finding.
    // 第7步：无效剪枝：子节点（~40 Elo）
    // 注意：深度条件对杀棋检测至关重要
    // 如果静态评估减去无效剪枝边界仍然大于等于beta，说明该节点有很大可能在搜索后超过beta，发生beta截断
    if (!ss->ttPv && depth < 16
        && eval - futility_margin(depth, cutNode && !ss->ttHit, improving, opponentWorsening)
               - (ss - 1)->statScore / 159
               + (ss->staticEval == eval) * (40 - std::abs(correctionValue) / 131072)
             >= beta
        && eval >= beta && (!ttData.move || ttCapture) && !is_loss(beta) && !is_win(eval))
        return beta + (eval - beta) / 3;

    improving |= ss->staticEval >= beta + 113;

    // Step 8. Null move search with verification search (~35 Elo)
    // 第8步：带验证的空着裁剪
    if (cutNode && (ss - 1)->currentMove != Move::null() && eval >= beta
        && ss->staticEval >= beta - 8 * depth + 189 && !excludedMove && pos.major_material(us)
        && ss->ply >= thisThread->nmpMinPly && !is_loss(beta))
    {
        assert(eval - beta >= 0);

        // Null move dynamic reduction based on depth and eval
        // 基于深度和评估的空着法动态减少 
        Depth R = std::min(int(eval - beta) / 254, 5) + depth / 3 + 5;

        ss->currentMove                   = Move::null();
        ss->continuationHistory           = &thisThread->continuationHistory[0][0][NO_PIECE][0];
        ss->continuationCorrectionHistory = &thisThread->continuationCorrectionHistory[NO_PIECE][0];

        pos.do_null_move(st, tt); // 执行空着

        Value nullValue = -search<NonPV>(pos, ss + 1, -beta, -beta + 1, depth - R, false);

        pos.undo_null_move(); // 撤销空着

        // Do not return unproven mate
        // 不要返回未经证实的将杀 
        if (nullValue >= beta && !is_win(nullValue))
        {
            if (thisThread->nmpMinPly || depth < 15)
                return nullValue;

            assert(!thisThread->nmpMinPly);  // Recursive verification is not allowed // 不允许递归验证 

            // Do verification search at high depths, with null move pruning disabled
            // until ply exceeds nmpMinPly.
            // 在深度较高时进行验证搜索，在 ply 超过 nmpMinPly 之前禁用空着法剪枝 
            thisThread->nmpMinPly = ss->ply + 3 * (depth - R) / 4;

            Value v = search<NonPV>(pos, ss, beta - 1, beta, depth - R, false);

            thisThread->nmpMinPly = 0;

            if (v >= beta)
                return nullValue;
        }
    }

    // Step 9. Internal iterative reductions (~9 Elo)
    // For PV nodes without a ttMove, we decrease depth.
    // 步骤 9. 内部迭代减少（约 9 个 Elo 分）
    // 对于没有 TT 走法的主变节点，我们降低深度。 
    if (PvNode && !ttData.move)
        depth -= 2;

    // Use qsearch if depth <= 0
    // 深度<=0时进行静态搜索
    if (depth <= 0)
        return qsearch<PV>(pos, ss, alpha, beta);

    // For cutNodes, if depth is high enough, decrease depth by 2 if there is no ttMove,
    // or by 1 if there is a ttMove with an upper bound.
    // 对于cutNodes，当深度足够深时，如果没有ttMove，则将深度减少2；如果存在带有上界(Upper Bound)的ttMove，则将深度减少1。
    if (cutNode && depth >= 7 && (!ttData.move || ttData.bound == BOUND_UPPER))
        depth -= 1 + !ttData.move;

    // Step 10. ProbCut (~10 Elo)
    // If we have a good enough capture and a reduced search
    // returns a value much above beta, we can (almost) safely prune the previous move.
    // 步骤10. ProbCut（约10 Elo）
// 若存在足够好的吃子着法且简化搜索的返回值大幅高于beta值，
// 则我们可以（几乎）安全地剪枝前一着法。
    probCutBeta = beta + 234 - 66 * improving;
    if (!PvNode && depth > 4
        && !is_decisive(beta)
        // If value from transposition table is lower than probCutBeta, don't attempt
        // probCut there and in further interactions with transposition table cutoff
        // depth is set to depth - 3 because probCut search has depth set to depth - 4
        // but we also do a move before it. So effective depth is equal to depth - 3.
        // 若置换表中的值低于probCutBeta，则在此处及后续置换表交互中不尝试ProbCut；
// 此处将深度设置为depth - 3，因为ProbCut搜索的深度设定为depth - 4，
// 但在其之前还需执行一步着法，因此有效深度等于depth - 3。
        && !(ttData.depth >= depth - 3 && is_valid(ttData.value) && ttData.value < probCutBeta))
    {
        assert(probCutBeta < VALUE_INFINITE && probCutBeta > beta);

        MovePicker mp(pos, ttData.move, probCutBeta - ss->staticEval, &thisThread->captureHistory);
        Piece      captured;

        while ((move = mp.next_move()) != Move::none())
        {
            assert(move.is_ok());

            if (move == excludedMove)
                continue;

            if (!pos.legal(move))
                continue;

            assert(pos.capture(move));

            movedPiece = pos.moved_piece(move);
            captured   = pos.piece_on(move.to_sq());

            // Prefetch the TT entry for the resulting position
            // 预取后续局面对应的置换表项
            prefetch(tt.first_entry(pos.key_after(move)));

            ss->currentMove = move;
            ss->continuationHistory =
              &this->continuationHistory[ss->inCheck][true][pos.moved_piece(move)][move.to_sq()];
            ss->continuationCorrectionHistory =
              &this->continuationCorrectionHistory[pos.moved_piece(move)][move.to_sq()];

            thisThread->nodes.fetch_add(1, std::memory_order_relaxed);
            pos.do_move(move, st);

            // Perform a preliminary qsearch to verify that the move holds
            // 执行预静态搜索以验证着法可行性
            value = -qsearch<NonPV>(pos, ss + 1, -probCutBeta, -probCutBeta + 1);

            // If the qsearch held, perform the regular search
            // 若静态搜索验证通过，则执行常规搜索
            if (value >= probCutBeta)
                value =
                  -search<NonPV>(pos, ss + 1, -probCutBeta, -probCutBeta + 1, depth - 4, !cutNode);

            pos.undo_move(move);

            if (value >= probCutBeta)
            {
                thisThread->captureHistory[movedPiece][move.to_sq()][type_of(captured)] << 1226;

                // Save ProbCut data into transposition table
                // 将ProbCut数据存储至置换表
                ttWriter.write(posKey, value_to_tt(value, ss->ply), ss->ttPv, BOUND_LOWER,
                               depth - 3, move, unadjustedStaticEval, tt.generation());
                return is_decisive(value) ? value : value - (probCutBeta - beta);
            }
        }

        Eval::NNUE::hint_common_parent_position(pos, network[numaAccessToken], refreshTable);
    }

moves_loop:  // 将军时搜索从这里开始

    // Step 11. A small Probcut idea (~4 Elo)
    // 步骤11. 小型ProbCut方案（约4 Elo）
    probCutBeta = beta + 441;
    if ((ttData.bound & BOUND_LOWER) && ttData.depth >= depth - 3 && ttData.value >= probCutBeta
        && !is_decisive(beta) && is_valid(ttData.value) && !is_decisive(ttData.value))
        return probCutBeta;

    const PieceToHistory* contHist[] = {(ss - 1)->continuationHistory,
                                        (ss - 2)->continuationHistory,
                                        (ss - 3)->continuationHistory,
                                        (ss - 4)->continuationHistory,
                                        nullptr,
                                        (ss - 6)->continuationHistory};


    MovePicker mp(pos, ttData.move, depth, &thisThread->mainHistory, &thisThread->lowPlyHistory,
                  &thisThread->captureHistory, contHist, &thisThread->pawnHistory, ss->ply);

    value = bestValue;

    int moveCount = 0;

    // Step 12. Loop through all pseudo-legal moves until no moves remain
    // or a beta cutoff occurs.
    // 步骤12. 循环遍历所有伪合法着法，直至无剩余着法或触发beta截断
    while ((move = mp.next_move()) != Move::none())
    {
        assert(move.is_ok());

        if (move == excludedMove)
            continue;

        // Check for legality
        // 检查合法性
        if (!pos.legal(move))
            continue;

        // At root obey the "searchmoves" option and skip moves not listed in Root Move List.
        // In MultiPV mode we also skip PV moves that have been already searched.
        // 在根节点处遵循"searchmoves"选项，跳过未列在根着法列表中的着法；
// 在MultiPV模式下，同时跳过已搜索过的主要变例着法(PV moves)。
        if (rootNode
            && !std::count(thisThread->rootMoves.begin() + thisThread->pvIdx,
                           thisThread->rootMoves.begin() + thisThread->pvLast, move))
            continue;

        ss->moveCount = ++moveCount;

        if (rootNode && is_mainthread() && nodes > 10000000)
        {
            main_manager()->updates.onIter(
              {depth, UCIEngine::move(move), moveCount + thisThread->pvIdx});
        }

        if (PvNode)
            (ss + 1)->pv = nullptr;

        extension  = 0;
        capture    = pos.capture(move);
        movedPiece = pos.moved_piece(move);
        givesCheck = pos.gives_check(move);

        // Calculate new depth for this move
        // 计算此移动的新深度
        newDepth = depth - 1;

        int delta = beta - alpha;

        Depth r = reduction(improving, depth, moveCount, delta);

        // Step 13. Pruning at shallow depth (~120 Elo).
        // Depth conditions are important for mate finding.
        // 第13步：浅层剪枝
        // 注意：深度条件对杀棋检测至关重要
        if (!rootNode && pos.major_material(us) && !is_loss(bestValue))
        {
            // Skip quiet moves if movecount exceeds our FutilityMoveCount threshold (~8 Elo)
            // 如果着法数量超过我们的“无用着法数量”阈值，跳过非吃子着法（约8个埃洛等级分优势） 
            if (moveCount >= futility_move_count(improving, depth))
                mp.skip_quiet_moves();

            // Reduced depth of the next LMR search
            // 下一次LMR搜索的深度降低 
            int lmrDepth = newDepth - r / 1054;

            if (capture || givesCheck)
            {
                Piece capturedPiece = pos.piece_on(move.to_sq());
                int   captHist =
                  thisThread->captureHistory[movedPiece][move.to_sq()][type_of(capturedPiece)];

                // Futility pruning for captures (~2 Elo)
                // 吃子的无效剪枝（约2个Elo等级分） 
                if (!givesCheck && lmrDepth < 18 && !ss->inCheck)
                {
                    Value futilityValue = ss->staticEval + 332 + 371 * lmrDepth
                                        + PieceValue[capturedPiece] + captHist / 5;
                    if (futilityValue <= alpha)
                        continue;
                }

                // SEE based pruning for captures and checks (~11 Elo)
                // 基于静态交换评估（SEE）对吃子和将军进行剪枝（约11个Elo等级分） 
                int seeHist = std::clamp(captHist / 28, -243 * depth, 179 * depth);
                if (!pos.see_ge(move, -275 * depth - seeHist))
                    continue;
            }
            else
            {
                int history =
                  (*contHist[0])[movedPiece][move.to_sq()]
                  + (*contHist[1])[movedPiece][move.to_sq()]
                  + thisThread->pawnHistory[pawn_structure_index(pos)][movedPiece][move.to_sq()];

                // Continuation history based pruning (~2 Elo)
                // 基于历史分数的剪枝// 基于延续历史的剪枝（约2个Elo等级分） 
                if (history < -3190 * depth)
                    continue;

                history += 2 * thisThread->mainHistory[us][move.from_to()];

                lmrDepth += history / 3718;

                Value futilityValue =
                  ss->staticEval + (bestValue < ss->staticEval - 45 ? 215 : 96) + 120 * lmrDepth;

                // Futility pruning: parent node (~13 Elo)
                // 父节点无效剪枝// 无效剪枝：父节点（约13个Elo等级分） 
                if (!ss->inCheck && lmrDepth < 10 && futilityValue <= alpha)
                {
                    if (bestValue <= futilityValue && !is_decisive(bestValue)
                        && !is_win(futilityValue))
                        bestValue = futilityValue;
                    continue;
                }

                lmrDepth = std::max(lmrDepth, 0);

                // Prune moves with negative SEE (~4 Elo)
                // 负SEE值剪枝// 对静态交换评估（SEE）为负的走法进行剪枝（约4个Elo等级分） 
                if (!pos.see_ge(move, -36 * lmrDepth * lmrDepth))
                    continue;
            }
        }

        // Step 14. Extensions (~100 Elo)
        // We take care to not overdo to avoid search getting stuck.
        // 第14步：搜索扩展（价值约100 Elo）
        // 注意：需谨慎控制扩展次数，避免搜索陷入死循环
        if (ss->ply < thisThread->rootDepth * 2)
        {
            // Singular extension search (~76 Elo, ~170 nElo). If all moves but one
            // fail low on a search of (alpha-s, beta-s), and just one fails high on
            // (alpha, beta), then that move is singular and should be extended. To
            // verify this we do a reduced search on the position excluding the ttMove
            // and if the result is lower than ttValue minus a margin, then we will
            // extend the ttMove. Recursive singular search is avoided.

            // Note: the depth margin and singularBeta margin are known for having
            // non-linear scaling. Their values are optimized to time controls of
            // 180+1.8 and longer so changing them requires tests at these types of
            // time controls. Generally, higher singularBeta (i.e closer to ttValue)
            // and lower extension margins scale well.
            
            // 奇异扩展搜索（约76个Elo等级分，约170个nElo等级分）。
            // 如果在对(alpha - s, beta - s)进行搜索时，除了一步棋之外所有棋步都评估分数较低，
            // 而只有一步在(alpha, beta)评估分数较高，那么这步棋就是奇异的，应该进行扩展。
            // 为了验证这一点，我们对排除了转置表棋步（ttMove）的局面进行一次降低深度的搜索，
            // 如果结果低于转置表值（ttValue）减去一个差值，那么我们将扩展转置表棋步。要避免递归奇异搜索。

            // 注意：深度差值和奇异β差值已知具有非线性缩放特性。它们的值是针对180 + 1.8及更长时间控制进行优化的，
            // 所以更改它们需要在这类时间控制下进行测试。一般来说，较高的奇异β值（即更接近ttValue）和较低的扩展差值能很好地进行缩放。 

            if (!rootNode && move == ttData.move && !excludedMove
                && depth >= 4 - (thisThread->completedDepth > 32) + ss->ttPv
                && is_valid(ttData.value) && !is_decisive(ttData.value)
                && (ttData.bound & BOUND_LOWER) && ttData.depth >= depth - 3)
            {
                Value singularBeta  = ttData.value - (41 + 73 * (ss->ttPv && !PvNode)) * depth / 76;
                Depth singularDepth = newDepth / 2;

                ss->excludedMove = move;
                value =
                  search<NonPV>(pos, ss, singularBeta - 1, singularBeta, singularDepth, cutNode);
                ss->excludedMove = Move::none();

                if (value < singularBeta)
                {
                    int doubleMargin = 246 * PvNode - 108 * !ttCapture;
                    int tripleMargin = 132 + 334 * PvNode - 279 * !ttCapture + 68 * ss->ttPv;

                    extension = 1 + (value < singularBeta - doubleMargin)
                              + (value < singularBeta - tripleMargin);

                    depth += ((!PvNode) && (depth < 20));
                }

                // Multi-cut pruning
                // Our ttMove is assumed to fail high based on the bound of the TT entry,
                // and if after excluding the ttMove with a reduced search we fail high
                // over the original beta, we assume this expected cut-node is not
                // singular (multiple moves fail high), and we can prune the whole
                // subtree by returning a softbound.
                // 多切枝剪枝
                // 基于转置表（TT）项的界限，我们假定转置表走法（ttMove）评估分数较高（fail high）。
                // 如果在排除ttMove并进行降深度搜索后，评估分数仍高于原始的β值，
                // 我们就假定这个预期的剪枝节点不是奇异的（多个走法评估分数较高），
                // 并且我们可以通过返回一个软边界来对整个子树进行剪枝。 
                else if (value >= beta && !is_decisive(value))
                    return value;

                // Negative extensions
                // If other moves failed high over (ttValue - margin) without the
                // ttMove on a reduced search, but we cannot do multi-cut because
                // (ttValue - margin) is lower than the original beta, we do not know
                // if the ttMove is singular or can do a multi-cut, so we reduce the
                // ttMove in favor of other moves based on some conditions:
                // 负扩展
                // 如果在降深度搜索中，排除ttMove后其他走法的评估分数高于（ttValue - 差值），
                // 但由于（ttValue - 差值）低于原始的β值而无法进行多切枝剪枝，
                // 我们就无法确定ttMove是否为奇异走法或能否进行多切枝剪枝。
                // 因此，基于某些条件，我们会减少对ttMove的探索，而更倾向于其他走法： 

                // If the ttMove is assumed to fail high over current beta (~7 Elo)
                // 如果假定ttMove的评估结果高于当前的beta值（约7个Elo等级分） 
                else if (ttData.value >= beta)
                    extension = -3;

                // If we are on a cutNode but the ttMove is not assumed to fail high
                // over current beta (~1 Elo)
                // 如果我们处于一个剪枝节点，但并不假定ttMove的评估结果高于当前的beta值（约1个Elo等级分） 
                else if (cutNode)
                    extension = -2;
            }

            // Extension for capturing the previous moved piece (~1 Elo at LTC)
            // 捕获上一步移动棋子的扩展（在长时控制下约1个Elo等级分） 
            else if (PvNode && move.to_sq() == prevSq
                     && thisThread->captureHistory[movedPiece][move.to_sq()]
                                                  [type_of(pos.piece_on(move.to_sq()))]
                          > 5255)
                extension = 1;
        }

        // Add extension to new depth
        // 将扩展添加到新的深度
        newDepth += extension;

        // Speculative prefetch as early as possible
        // 尽早进行推测性预取 
        prefetch(tt.first_entry(pos.key_after(move)));

        // Update the current move (this must be done after singular extension search)
        // 更新当前走法（此操作必须在奇异扩展搜索之后进行） 
        ss->currentMove = move;
        ss->continuationHistory =
          &thisThread->continuationHistory[ss->inCheck][capture][movedPiece][move.to_sq()];
        ss->continuationCorrectionHistory =
          &thisThread->continuationCorrectionHistory[movedPiece][move.to_sq()];
        uint64_t nodeCount = rootNode ? uint64_t(nodes) : 0;

        // Step 15. Make the move
        // 步骤15. 执行这步棋 
        thisThread->nodes.fetch_add(1, std::memory_order_relaxed);
        pos.do_move(move, st, givesCheck);

        // These reduction adjustments have proven non-linear scaling.
        // They are optimized to time controls of 180 + 1.8 and longer,
        // so changing them or adding conditions that are similar requires
        // tests at these types of time controls.
        // 这些缩减调整已被证明具有非线性缩放特性。
        // 它们针对180 + 1.8及更长时间控制进行了优化，
        // 所以更改这些调整或添加类似条件需要在这类时间控制下进行测试。 

        // Decrease reduction if position is or has been on the PV (~7 Elo)
        // 如果局面处于或曾处于主变（PV）上，则减少缩减量（约7个Elo等级分） 
        if (ss->ttPv)
            r -= 1024 + (ttData.value > alpha) * 1024 + (ttData.depth >= depth) * 1024;

        // Decrease reduction for PvNodes (~0 Elo on STC, ~2 Elo on LTC)
        // 减少主变节点（PvNodes）的缩减量（快棋赛（STC）中约0个Elo等级分，慢棋赛（LTC）中约2个Elo等级分） 
        if (PvNode)
            r -= 1024;

        // These reduction adjustments have no proven non-linear scaling
        // 这些缩减调整尚未证明具有非线性缩放特性 

        r += 330;

        r -= std::abs(correctionValue) / 32768;

        // Increase reduction for cut nodes (~4 Elo)
        // 增加剪枝节点的缩减量（约4个Elo等级分） 
        if (cutNode)
            r += 3179 - (ttData.depth >= depth && ss->ttPv) * 949;

        // Increase reduction if ttMove is a capture but the current move is not a capture (~3 Elo)
        // 如果转置表走法（ttMove）是吃子走法，但当前走法不是吃子走法，则增加缩减量（约3个Elo等级分） 
        if (ttCapture && !capture)
            r += 1401 + (depth < 8) * 1471;

        // Increase reduction if next ply has a lot of fail high (~5 Elo)
        // 如果下一层次有很多评估分数较高（fail high）的情况，则增加缩减量（约5个Elo等级分） 
        if ((ss + 1)->cutoffCnt > 3)
            r += 1332 + allNode * 959;

        // For first picked move (ttMove) reduce reduction (~3 Elo)
        // 对于首个选择的走法（转置表走法ttMove），减少缩减量（约3个Elo等级分） 
        else if (move == ttData.move)
            r -= 2775;

        if (capture)
            ss->statScore =
              7 * int(PieceValue[pos.captured_piece()])
              + thisThread->captureHistory[movedPiece][move.to_sq()][type_of(pos.captured_piece())]
              - 5000;
        else
            ss->statScore = 2 * thisThread->mainHistory[us][move.from_to()]
                          + (*contHist[0])[movedPiece][move.to_sq()]
                          + (*contHist[1])[movedPiece][move.to_sq()] - 4241;

        // Decrease/increase reduction for moves with a good/bad history (~8 Elo)
        // 对于历史评分良好/不佳的走法，减少/增加缩减量（约8个Elo等级分） 
        r -= ss->statScore * 2652 / 18912;

        // Step 16. Late moves reduction / extension (LMR, ~117 Elo)
        // 第16步：延迟走法缩减 / 扩展
        if (depth >= 2 && moveCount > 1)
        {
            // In general we want to cap the LMR depth search at newDepth, but when
            // reduction is negative, we allow this move a limited search extension
            // beyond the first move depth.
            // To prevent problems when the max value is less than the min value,
            // std::clamp has been replaced by a more robust implementation.
            /* LMR核心思想：
            * 对后续走法进行缩减深度的试探性搜索，若发现潜在好棋则重新完整搜索
            * 既限制搜索宽度又避免错过关键变化 */
            Depth d = std::max(
              1, std::min(newDepth - r / 1024, newDepth + !allNode + (PvNode && !bestMove)));

            value = -search<NonPV>(pos, ss + 1, -(alpha + 1), -alpha, d, true); // 零窗口搜索

            // Do a full-depth search when reduced LMR search fails high
            // 如果返回的值是fail high的（value>=alpha+1），需要重新完整搜索
            if (value > alpha && d < newDepth)
            {
                /* 根据LMR搜索结果动态调整搜索策略：
                * 若结果明显优于当前最佳值(bestValue)则加深搜索
                * 若结果不够理想则保持或减少深度 */
                // Adjust full-depth search based on LMR results - if the result was
                // good enough search deeper, if it was bad enough search shallower.
                const bool doDeeperSearch    = value > (bestValue + 58 + 2 * newDepth);  // (~1 Elo)
                const bool doShallowerSearch = value < bestValue + 8;                    // (~2 Elo

                newDepth += doDeeperSearch - doShallowerSearch;

                // 当调整后的深度大于初始LMR深度时，执行完整搜索
                if (newDepth > d)
                    value = -search<NonPV>(pos, ss + 1, -(alpha + 1), -alpha, newDepth, !cutNode);

                // Post LMR continuation history updates (~1 Elo)
                // 发布LMR后续历史更新（约1等级分）
                int bonus = (value >= beta) * 2048;
                update_continuation_histories(ss, movedPiece, move.to_sq(), bonus);
            }
        }

        // Step 17. Full-depth search when LMR is skipped
        // 步骤17：当跳过LMR时进行全深度搜索
        else if (!PvNode || moveCount > 1)
        {
            // Increase reduction if ttMove is not present (~6 Elo)
            // 如果不存在转置表走法（ttMove），则增加缩减量（约6个Elo等级分） 
            if (!ttData.move)
                r += 1744;

            // Note that if expected reduction is high, we reduce search depth by 1 here (~9 Elo)
            // 请注意，如果预期缩减量较高，我们在此将搜索深度减少1（约9个Elo等级分） 
            value =
              -search<NonPV>(pos, ss + 1, -(alpha + 1), -alpha, newDepth - (r > 4047), !cutNode);
        }

        // For PV nodes only, do a full PV search on the first move or after a fail high,
        // otherwise let the parent node fail low with value <= alpha and try another move.
        // 仅针对主变（PV）节点，对第一步棋或在评估分数较高（fail high）后进行完整的主变搜索，
        // 否则让父节点以小于等于α的值评估分数较低（fail low），并尝试另一步棋。 
        if (PvNode && (moveCount == 1 || value > alpha))
        {
            (ss + 1)->pv    = pv;
            (ss + 1)->pv[0] = Move::none();

            // Extend move from transposition table if we are about to dive into qsearch.
            // 如果我们即将进入残局搜索（qsearch），则从置换表中扩展走法。 
            if (move == ttData.move && ss->ply <= thisThread->rootDepth * 2)
                newDepth = std::max(newDepth, 1);

            value = -search<PV>(pos, ss + 1, -beta, -alpha, newDepth, false);
        }

        // Step 18. Undo move
        // 步骤18. 撤销走法
        pos.undo_move(move);

        assert(value > -VALUE_INFINITE && value < VALUE_INFINITE);

        // Step 19. Check for a new best move
        // Finished searching the move. If a stop occurred, the return value of
        // the search cannot be trusted, and we return immediately without updating
        // best move, principal variation nor transposition table.
        // 步骤19. 检查是否有新的最佳走法
        // 已完成该走法的搜索。如果出现了停止情况，搜索的返回值不可信，我们会立即返回，
        // 而不更新最佳走法、主变以及置换表。 
        if (threads.stop.load(std::memory_order_relaxed))
            return VALUE_ZERO;

        if (rootNode)
        {
            RootMove& rm =
              *std::find(thisThread->rootMoves.begin(), thisThread->rootMoves.end(), move);

            rm.effort += nodes - nodeCount;

            rm.averageScore =
              rm.averageScore != -VALUE_INFINITE ? (2 * value + rm.averageScore) / 3 : value;

            rm.meanSquaredScore = rm.meanSquaredScore != -VALUE_INFINITE * VALUE_INFINITE
                                  ? (value * std::abs(value) + rm.meanSquaredScore) / 2
                                  : value * std::abs(value);

            // PV move or new best move?
            // 这是主变走法还是新的最佳走法？
            if (moveCount == 1 || value > alpha)
            {
                rm.score = rm.uciScore = value;
                rm.selDepth            = thisThread->selDepth;
                rm.scoreLowerbound = rm.scoreUpperbound = false;

                if (value >= beta)
                {
                    rm.scoreLowerbound = true;
                    rm.uciScore        = beta;
                }
                else if (value <= alpha)
                {
                    rm.scoreUpperbound = true;
                    rm.uciScore        = alpha;
                }

                rm.pv.resize(1);

                assert((ss + 1)->pv);

                for (Move* m = (ss + 1)->pv; *m != Move::none(); ++m)
                    rm.pv.push_back(*m);

                // We record how often the best move has been changed in each iteration.
                // This information is used for time management. In MultiPV mode,
                // we must take care to only do this for the first PV line.
                // 我们记录每次迭代中最佳走法的变更频率。此信息用于时间管理。
                // 在多主变（MultiPV）模式下，我们必须注意仅对第一条主变线路执行此操作。 
                if (moveCount > 1 && !thisThread->pvIdx)
                    ++thisThread->bestMoveChanges;
            }
            else
                // All other moves but the PV, are set to the lowest value: this
                // is not a problem when sorting because the sort is stable and the
                // move position in the list is preserved - just the PV is pushed up.
                // 除主变走法之外的所有其他走法，都被设置为最低值：这在排序时并非问题，
                // 因为排序是稳定的，列表中走法的位置得以保留——只是主变走法被移至靠前位置。 
                rm.score = -VALUE_INFINITE;
        }

        // In case we have an alternative move equal in eval to the current bestmove,
        // promote it to bestmove by pretending it just exceeds alpha (but not beta).
        // 如果我们有另一个走法，其评估值与当前最佳走法相等，
        // 那么通过假设它刚好超过alpha（但不超过beta），将其提升为最佳走法。 
        int inc = (value == bestValue && ss->ply + 2 >= thisThread->rootDepth
                   && (int(nodes) & 15) == 0 && !is_win(std::abs(value) + 1));

        if (value + inc > bestValue)
        {
            bestValue = value;

            if (value + inc > alpha)
            {
                bestMove = move;

                if (PvNode && !rootNode)  // Update pv even in fail-high case// 即使在评估分数较高（fail high）的情况下，也要更新主变（PV） 
                    update_pv(ss->pv, move, (ss + 1)->pv);

                if (value >= beta) // beta截断
                {
                    ss->cutoffCnt += !ttData.move + (extension < 2);
                    assert(value >= beta);  // Fail high// 评估分数较高（棋局搜索中的一种结果状态） 
                    break;
                }
                else
                {
                    // Reduce other moves if we have found at least one score improvement (~2 Elo)
                    // 如果我们至少发现一个分数提升，就减少其他走法的评估（约2个Elo等级分） 
                    if (depth > 2 && depth < 10 && !is_decisive(value))
                        depth -= 2;

                    assert(depth > 0);
                    alpha = value;  // Update alpha! Always alpha < beta// 更新alpha值！始终保持alpha小于beta 
                }
            }
        }

        // If the move is worse than some previously searched move,
        // remember it, to update its stats later.
        // 如果该走法比之前搜索过的某些走法差，记住它，以便稍后更新其统计数据。 
        if (move != bestMove && moveCount <= 32)
        {
            if (capture)
                capturesSearched.push_back(move);
            else
                quietsSearched.push_back(move);
        }
    }

    // Step 20. Check for mate
    // All legal moves have been searched and if there are no legal moves,
    // it must be a mate. If we are in a singular extension search then
    // return a fail low score.
    // 步骤20. 检查是否将杀
    // 所有合法走法均已搜索完毕，若不存在合法走法，则必定是将杀局面。
    // 若我们处于奇异扩展搜索中，那么返回一个较低失败分数。 

    assert(moveCount || !ss->inCheck || excludedMove || !MoveList<LEGAL>(pos).size());

    // Adjust best value for fail high cases at non-pv nodes
    // 针对非主变（PV）节点评估分数较高（fail high）的情况，调整最佳值。
    if (!PvNode && bestValue >= beta && !is_decisive(bestValue) && !is_decisive(beta)
        && !is_decisive(alpha))
        bestValue = (bestValue * depth + beta) / (depth + 1);

    if (!moveCount)
        bestValue = excludedMove ? alpha : mated_in(ss->ply);

    // If there is a move that produces search value greater than alpha,
    // we update the stats of searched moves.
    // 如果存在某个走法，其搜索值大于alpha，我们就更新已搜索走法的统计数据。 
    else if (bestMove)
        update_all_stats(pos, ss, *this, bestMove, prevSq, quietsSearched, capturesSearched, depth);

    // Bonus for prior countermove that caused the fail low
    // 对之前导致评估分数较低（fail low）的反制走法给予奖励 
    else if (!priorCapture && prevSq != SQ_NONE)
    {
        int bonusScale = (184 * (depth > 6) + 80 * !allNode + 152 * ((ss - 1)->moveCount > 11)
                          + 77 * (!ss->inCheck && bestValue <= ss->staticEval - 157)
                          + 169 * (!(ss - 1)->inCheck && bestValue <= -(ss - 1)->staticEval - 99));

        // Proportional to "how much damage we have to undo"
        // 与 “我们需要挽回多少损失” 成正比 
        bonusScale += std::min(-(ss - 1)->statScore / 79, 234);

        bonusScale = std::max(bonusScale, 0);

        const int scaledBonus = stat_bonus(depth) * bonusScale / 32;

        update_continuation_histories(ss - 1, pos.piece_on(prevSq), prevSq,
                                      scaledBonus * 416 / 1024);

        thisThread->mainHistory[~us][((ss - 1)->currentMove).from_to()] << scaledBonus * 212 / 1024;

        if (type_of(pos.piece_on(prevSq)) != PAWN)
            thisThread->pawnHistory[pawn_structure_index(pos)][pos.piece_on(prevSq)][prevSq]
              << scaledBonus * 1073 / 1024;
    }

    else if (priorCapture && prevSq != SQ_NONE)
    {
        // bonus for prior countermoves that caused the fail low
        // 对之前导致评估分数较低（fail low）的反制走法给予奖励 
        Piece capturedPiece = pos.captured_piece();
        assert(capturedPiece != NO_PIECE);
        thisThread->captureHistory[pos.piece_on(prevSq)][prevSq][type_of(capturedPiece)]
          << stat_bonus(depth) * 2;
    }

    // Bonus when search fails low and there is a TT move
    // 当搜索评估分数较低（fail low）且置换表（TT）中有对应走法时给予奖励 
    else if (ttData.move && !allNode)
        thisThread->mainHistory[us][ttData.move.from_to()] << stat_bonus(depth) * 287 / 1024;

    if (PvNode)
        bestValue = std::min(bestValue, maxValue);

    // If no good move is found and the previous position was ttPv, then the previous
    // opponent move is probably good and the new position is added to the search tree. (~7 Elo)
    // 如果未找到好的走法，且上一局面是置换表主变（ttPv）局面，那么上一步对手的走法很可能是好棋，
    // 新的局面会被添加到搜索树中。（约7个Elo等级分） 
    if (bestValue <= alpha)
        ss->ttPv = ss->ttPv || ((ss - 1)->ttPv && depth > 3);

    // Write gathered information in transposition table. Note that the
    // static evaluation is saved as it was before correction history.
    // 将收集到的信息写入置换表。注意，静态评估值按修正历史之前的状态保存。 
    if (!excludedMove && !(rootNode && thisThread->pvIdx))
        ttWriter.write(posKey, value_to_tt(bestValue, ss->ply), ss->ttPv,
                       bestValue >= beta    ? BOUND_LOWER
                       : PvNode && bestMove ? BOUND_EXACT
                                            : BOUND_UPPER,
                       depth, bestMove, unadjustedStaticEval, tt.generation());

    // Adjust correction history
    // 调整评估修正历史值（用于动态优化静态评估误差）
    if (!ss->inCheck && !(bestMove && pos.capture(bestMove))    // 条件：非将军状态且最佳移动不是吃子
        && ((bestValue < ss->staticEval && bestValue < beta)// negative correction & no fail high// 情况1：负修正（实际值 < 静态评估）且未触发Beta剪枝
            || (bestValue > ss->staticEval && bestMove)))// positive correction & no fail low// 情况2：正修正（实际值 > 静态评估）且存在有效移动
    {
        const auto       m             = (ss - 1)->currentMove;  // 获取前一层移动
        static const int nonPawnWeight = 139;                    // 非兵棋子权重系数（通过遗传算法优化得出）

        // 计算基础修正值（深度加权，限制在±CORRECTION_HISTORY_LIMIT/4范围内）
        auto bonus = std::clamp(int(bestValue - ss->staticEval) * depth / 8,
                                -CORRECTION_HISTORY_LIMIT / 4, CORRECTION_HISTORY_LIMIT / 4);

        // 按棋子类型更新修正历史（系数经海量对局实验优化）：
        // 兵型结构修正（权重148/128 ≈ 1.156）
        thisThread->pawnCorrectionHistory[us][pawn_structure_index<Correction>(pos)] << bonus * 148 / 128;
        
        // 大子（后/车）位置修正（权重185/128 ≈ 1.445）
        thisThread->majorPieceCorrectionHistory[us][major_piece_index(pos)] << bonus * 185 / 128;
        
        // 小子（马/象）位置修正（权重101/128 ≈ 0.789）
        thisThread->minorPieceCorrectionHistory[us][minor_piece_index(pos)] << bonus * 101 / 128;
        
        // 白方非兵棋子全局修正（权重139/128 ≈ 1.086）
        thisThread->nonPawnCorrectionHistory[WHITE][us][non_pawn_index<WHITE>(pos)] << bonus * nonPawnWeight / 128;
        
        // 黑方非兵棋子全局修正（同权重139/128）
        thisThread->nonPawnCorrectionHistory[BLACK][us][non_pawn_index<BLACK>(pos)] << bonus * nonPawnWeight / 128;

        // 更新前前层移动的连续修正历史（用于杀手启发式）
        if (m.is_ok())  // 确保移动合法
            (*(ss - 2)->continuationCorrectionHistory)[pos.piece_on(m.to_sq())][m.to_sq()] << bonus;
    }

    assert(bestValue > -VALUE_INFINITE && bestValue < VALUE_INFINITE);

    return bestValue;
}


// Quiescence search function, which is called by the main search function with
// depth zero, or recursively with further decreasing depth. With depth <= 0, we
// "should" be using static eval only, but tactical moves may confuse the static eval.
// To fight this horizon effect, we implement this qsearch of tactical moves (~155 Elo).
// See https://www.chessprogramming.org/Horizon_Effect
// and https://www.chessprogramming.org/Quiescence_Search
// 残局搜索函数，主搜索函数会以深度为零调用它，或者以不断递减的深度递归调用。
// 当深度小于等于0时，我们 “应该” 只使用静态评估，但战术性走法可能会干扰静态评估。
// 为应对这种视界效应，我们对战术性走法实施此残局搜索（约155个Elo等级分）。
// 参见https://www.chessprogramming.org/Horizon_Effect
// 以及https://www.chessprogramming.org/Quiescence_Search 
template<NodeType nodeType>
Value Search::Worker::qsearch(Position& pos, Stack* ss, Value alpha, Value beta) {

    static_assert(nodeType != Root);
    constexpr bool PvNode = nodeType == PV;

    assert(alpha >= -VALUE_INFINITE && alpha < beta && beta <= VALUE_INFINITE);
    assert(PvNode || (alpha == beta - 1));

    Move      pv[MAX_PLY + 1];
    StateInfo st;
    ASSERT_ALIGNED(&st, Eval::NNUE::CacheLineSize);

    Key   posKey;
    Move  move, bestMove;
    Value bestValue, value, futilityBase;
    bool  pvHit, givesCheck, capture;
    int   moveCount;
    Color us = pos.side_to_move();

    // Step 1. Initialize node
    // 第1步：初始化节点
    if (PvNode)
    {
        (ss + 1)->pv = pv;
        ss->pv[0]    = Move::none();
    }

    Worker* thisThread = this;
    bestMove           = Move::none();
    ss->inCheck        = bool(pos.checkers());
    moveCount          = 0;

    // Used to send selDepth info to GUI (selDepth counts from 1, ply from 0)
    if (PvNode && thisThread->selDepth < ss->ply + 1)
        thisThread->selDepth = ss->ply + 1;

    // Step 2. Check for repetition or maximum ply reached
    Value result = VALUE_NONE;
    if (pos.rule_judge(result, ss->ply))
        return result;
    if (result != VALUE_NONE)
    {
        assert(result != VALUE_DRAW);

        // 2 fold result is mate for us, the only chance for the opponent is to get a draw
        // We can guarantee to get at least a draw score during searching for that line
        // TODO: 为什么删去了+1和-1？https://github.com/official-pikafish/Pikafish/commit/ef66bda24235bb27c76c0d0de28d7f010e89c267
        if (result > VALUE_DRAW)
            alpha = std::max(alpha, VALUE_DRAW);
        // 2 fold result is mated for us, the only chance for us is to get a draw
        // We can guarantee to get no more than a draw score during searching for that line
        else
            beta = std::min(beta, VALUE_DRAW);

        if (alpha >= beta)
            return alpha;
    }

    if (ss->ply >= MAX_PLY)
        return !ss->inCheck ? evaluate(pos) : VALUE_DRAW;

    assert(0 <= ss->ply && ss->ply < MAX_PLY);

    // Step 3. Transposition table lookup
    posKey                         = pos.key();
    auto [ttHit, ttData, ttWriter] = tt.probe(posKey);
    // Need further processing of the saved data
    ss->ttHit    = ttHit;
    ttData.move  = ttHit ? ttData.move : Move::none();
    ttData.value = ttHit ? value_from_tt(ttData.value, ss->ply, pos.rule60_count()) : VALUE_NONE;
    pvHit        = ttHit && ttData.is_pv;

    // At non-PV nodes we check for an early TT cutoff
    if (!PvNode && ttData.depth >= DEPTH_QS
        && is_valid(ttData.value)  // Can happen when !ttHit or when access race in probe()
        && (ttData.bound & (ttData.value >= beta ? BOUND_LOWER : BOUND_UPPER)))
        return ttData.value;

    // Step 4. Static evaluation of the position
    Value      unadjustedStaticEval = VALUE_NONE;
    const auto correctionValue      = correction_value(*thisThread, pos, ss);
    if (ss->inCheck)
        bestValue = futilityBase = -VALUE_INFINITE;
    else
    {
        if (ss->ttHit)
        {
            // Never assume anything about values stored in TT
            unadjustedStaticEval = ttData.eval;
            if (!is_valid(unadjustedStaticEval))
                unadjustedStaticEval = evaluate(pos);
            ss->staticEval = bestValue =
              to_corrected_static_eval(unadjustedStaticEval, correctionValue);

            // ttValue can be used as a better position evaluation (~13 Elo)
            if (is_valid(ttData.value)
                && (ttData.bound & (ttData.value > bestValue ? BOUND_LOWER : BOUND_UPPER)))
                bestValue = ttData.value;
        }
        else
        {
            // In case of null move search, use previous static eval with opposite sign
            unadjustedStaticEval =
              (ss - 1)->currentMove != Move::null() ? evaluate(pos) : -(ss - 1)->staticEval;
            ss->staticEval = bestValue =
              to_corrected_static_eval(unadjustedStaticEval, correctionValue);
        }

        // Stand pat. Return immediately if static value is at least beta
        if (bestValue >= beta)
        {
            if (!is_decisive(bestValue))
                bestValue = (bestValue + beta) / 2;
            if (!ss->ttHit)
                ttWriter.write(posKey, value_to_tt(bestValue, ss->ply), false, BOUND_LOWER,
                               DEPTH_UNSEARCHED, Move::none(), unadjustedStaticEval,
                               tt.generation());
            return bestValue;
        }

        if (bestValue > alpha)
            alpha = bestValue;

        futilityBase = ss->staticEval + 204;
    }

    const PieceToHistory* contHist[] = {(ss - 1)->continuationHistory,
                                        (ss - 2)->continuationHistory};

    Square prevSq = ((ss - 1)->currentMove).is_ok() ? ((ss - 1)->currentMove).to_sq() : SQ_NONE;

    // Initialize a MovePicker object for the current position, and prepare to search
    // the moves. We presently use two stages of move generator in quiescence search:
    // captures, or evasions only when in check.
    MovePicker mp(pos, ttData.move, DEPTH_QS, &thisThread->mainHistory, &thisThread->lowPlyHistory,
                  &thisThread->captureHistory, contHist, &thisThread->pawnHistory, ss->ply);

    // Step 5. Loop through all pseudo-legal moves until no moves remain or a beta
    // cutoff occurs.
    while ((move = mp.next_move()) != Move::none())
    {
        assert(move.is_ok());

        if (!pos.legal(move))
            continue;

        givesCheck = pos.gives_check(move);
        capture    = pos.capture(move);

        moveCount++;

        // Step 6. Pruning
        if (!is_loss(bestValue) && pos.major_material(us))
        {
            // Futility pruning and moveCount pruning (~10 Elo)
            if (!givesCheck && move.to_sq() != prevSq && !is_loss(futilityBase))
            {
                if (moveCount > 2)
                    continue;

                Value futilityValue = futilityBase + PieceValue[pos.piece_on(move.to_sq())];

                // If static eval + value of piece we are going to capture is
                // much lower than alpha, we can prune this move. (~2 Elo)
                if (futilityValue <= alpha)
                {
                    bestValue = std::max(bestValue, futilityValue);
                    continue;
                }

                // If static exchange evaluation is low enough
                // we can prune this move. (~2 Elo)
                if (!pos.see_ge(move, alpha - futilityBase))
                {
                    bestValue = std::min(alpha, futilityBase);
                    continue;
                }
            }

            // Continuation history based pruning (~3 Elo)
            if (!capture
                && (*contHist[0])[pos.moved_piece(move)][move.to_sq()]
                       + (*contHist[1])[pos.moved_piece(move)][move.to_sq()]
                       + thisThread->pawnHistory[pawn_structure_index(pos)][pos.moved_piece(move)]
                                                [move.to_sq()]
                     <= 3047)
                continue;

            // Do not search moves with bad enough SEE values (~5 Elo)
            if (!pos.see_ge(move, -102))
                continue;
        }

        // Speculative prefetch as early as possible
        prefetch(tt.first_entry(pos.key_after(move)));

        // Update the current move
        ss->currentMove = move;
        ss->continuationHistory =
          &thisThread
             ->continuationHistory[ss->inCheck][capture][pos.moved_piece(move)][move.to_sq()];
        ss->continuationCorrectionHistory =
          &thisThread->continuationCorrectionHistory[pos.moved_piece(move)][move.to_sq()];

        // Step 7. Make and search the move
        thisThread->nodes.fetch_add(1, std::memory_order_relaxed);
        pos.do_move(move, st, givesCheck);
        value = -qsearch<nodeType>(pos, ss + 1, -beta, -alpha);
        pos.undo_move(move);

        assert(value > -VALUE_INFINITE && value < VALUE_INFINITE);

        // Step 8. Check for a new best move
        if (value > bestValue)
        {
            bestValue = value;

            if (value > alpha)
            {
                bestMove = move;

                if (PvNode)  // Update pv even in fail-high case
                    update_pv(ss->pv, move, (ss + 1)->pv);

                if (value < beta)  // Update alpha here!
                    alpha = value;
                else
                    break;  // Fail high
            }
        }
    }

    // Step 9. Check for mate
    // All legal moves have been searched. A special case: if no legal
    // moves were found, it is checkmate.
    if (bestValue == -VALUE_INFINITE)
    {
        assert(!MoveList<LEGAL>(pos).size());
        return mated_in(ss->ply);  // Plies to mate from the root
    }

    if (!is_decisive(bestValue) && bestValue >= beta)
        bestValue = (3 * bestValue + beta) / 4;

    // Save gathered info in transposition table. The static evaluation
    // is saved as it was before adjustment by correction history.
    ttWriter.write(posKey, value_to_tt(bestValue, ss->ply), pvHit,
                   bestValue >= beta ? BOUND_LOWER : BOUND_UPPER, DEPTH_QS, bestMove,
                   unadjustedStaticEval, tt.generation());

    assert(bestValue > -VALUE_INFINITE && bestValue < VALUE_INFINITE);

    return bestValue;
}

Depth Search::Worker::reduction(bool i, Depth d, int mn, int delta) const {
    int reductionScale = reductions[d] * reductions[mn];
    return reductionScale - delta * 1181 / rootDelta + !i * reductionScale / 3 + 2199;
}

// elapsed() returns the time elapsed since the search started. If the
// 'nodestime' option is enabled, it will return the count of nodes searched
// instead. This function is called to check whether the search should be
// stopped based on predefined thresholds like time limits or nodes searched.
//
// elapsed_time() returns the actual time elapsed since the start of the search.
// This function is intended for use only when printing PV outputs, and not used
// for making decisions within the search algorithm itself.
TimePoint Search::Worker::elapsed() const {
    return main_manager()->tm.elapsed([this]() { return threads.nodes_searched(); });
}

TimePoint Search::Worker::elapsed_time() const { return main_manager()->tm.elapsed_time(); }

Value Search::Worker::evaluate(const Position& pos) {
    return Eval::evaluate(network[numaAccessToken], pos, refreshTable,
                          optimism[pos.side_to_move()]);
}

namespace {
// Adjusts a mate from "plies to mate from the root" to
// "plies to mate from the current position". Standard scores are unchanged.
// The function is called before storing a value in the transposition table.
Value value_to_tt(Value v, int ply) { return is_win(v) ? v + ply : is_loss(v) ? v - ply : v; }


// Inverse of value_to_tt(): it adjusts a mate score from the transposition
// table (which refers to the plies to mate/be mated from current position) to
// "plies to mate/be mated from the root". However, to avoid potentially false
// mate scores related to the 60 moves rule and the graph history interaction,
// we return the highest non-mate score instead.
Value value_from_tt(Value v, int ply, int r60c) {

    if (!is_valid(v))
        return VALUE_NONE;

    // Handle win
    if (is_win(v))
        // Downgrade a potentially false mate score
        return VALUE_MATE - v > 120 - r60c ? VALUE_MATE_IN_MAX_PLY - 1 : v - ply;

    // Handle loss
    if (is_loss(v))
        // Downgrade a potentially false mate score.
        return VALUE_MATE + v > 120 - r60c ? VALUE_MATED_IN_MAX_PLY + 1 : v + ply;

    return v;
}


// Adds current move and appends child pv[]
void update_pv(Move* pv, Move move, const Move* childPv) {

    for (*pv++ = move; childPv && *childPv != Move::none();)
        *pv++ = *childPv++;
    *pv = Move::none();
}


// Updates stats at the end of search() when a bestMove is found
void update_all_stats(const Position&      pos,
                      Stack*               ss,
                      Search::Worker&      workerThread,
                      Move                 bestMove,
                      Square               prevSq,
                      ValueList<Move, 32>& quietsSearched,
                      ValueList<Move, 32>& capturesSearched,
                      Depth                depth) {

    CapturePieceToHistory& captureHistory = workerThread.captureHistory;
    Piece                  moved_piece    = pos.moved_piece(bestMove);
    PieceType              captured;

    int bonus = stat_bonus(depth);
    int malus = stat_malus(depth);

    if (!pos.capture(bestMove))
    {
        update_quiet_histories(pos, ss, workerThread, bestMove, bonus * 1131 / 1024);

        // Decrease stats for all non-best quiet moves
        for (Move move : quietsSearched)
            update_quiet_histories(pos, ss, workerThread, move, -malus * 1028 / 1024);
    }
    else
    {
        // Increase stats for the best move in case it was a capture move
        captured = type_of(pos.piece_on(bestMove.to_sq()));
        captureHistory[moved_piece][bestMove.to_sq()][captured] << bonus * 1291 / 1024;
    }

    // Extra penalty for a quiet early move that was not a TT move in
    // previous ply when it gets refuted.
    if (prevSq != SQ_NONE && ((ss - 1)->moveCount == 1 + (ss - 1)->ttHit) && !pos.captured_piece())
        update_continuation_histories(ss - 1, pos.piece_on(prevSq), prevSq, -malus * 919 / 1024);

    // Decrease stats for all non-best capture moves
    for (Move move : capturesSearched)
    {
        moved_piece = pos.moved_piece(move);
        captured    = type_of(pos.piece_on(move.to_sq()));
        captureHistory[moved_piece][move.to_sq()][captured] << -malus * 1090 / 1024;
    }
}


// Updates histories of the move pairs formed by moves
// at ply -1, -2, -3, -4, and -6 with current move.
void update_continuation_histories(Stack* ss, Piece pc, Square to, int bonus) {
    static constexpr std::array<ConthistBonus, 5> conthist_bonuses = {
      {{1, 1024}, {2, 571}, {3, 339}, {4, 500}, {6, 592}}};

    for (const auto [i, weight] : conthist_bonuses)
    {
        // Only update the first 2 continuation histories if we are in check
        if (ss->inCheck && i > 2)
            break;
        if (((ss - i)->currentMove).is_ok())
            (*(ss - i)->continuationHistory)[pc][to] << bonus * weight / 1024;
    }
}

// Updates move sorting heuristics

void update_quiet_histories(
  const Position& pos, Stack* ss, Search::Worker& workerThread, Move move, int bonus) {

    Color us = pos.side_to_move();
    workerThread.mainHistory[us][move.from_to()] << bonus;  // Untuned to prevent duplicate effort

    if (ss->ply < LOW_PLY_HISTORY_SIZE)
        workerThread.lowPlyHistory[ss->ply][move.from_to()] << bonus * 874 / 1024;

    update_continuation_histories(ss, pos.moved_piece(move), move.to_sq(), bonus * 853 / 1024);

    int pIndex = pawn_structure_index(pos);
    workerThread.pawnHistory[pIndex][pos.moved_piece(move)][move.to_sq()] << bonus * 628 / 1024;
}

}

// Used to print debug info and, more importantly, to detect
// when we are out of available time and thus stop the search.
void SearchManager::check_time(Search::Worker& worker) {
    if (--callsCnt > 0)
        return;

    // When using nodes, ensure checking rate is not lower than 0.1% of nodes
    callsCnt = worker.limits.nodes ? std::min(512, int(worker.limits.nodes / 1024)) : 512;

    static TimePoint lastInfoTime = now();

    TimePoint elapsed = tm.elapsed([&worker]() { return worker.threads.nodes_searched(); });
    TimePoint tick    = worker.limits.startTime + elapsed;

    if (tick - lastInfoTime >= 1000)
    {
        lastInfoTime = tick;
        dbg_print();
    }

    // We should not stop pondering until told so by the GUI
    if (ponder)
        return;

    if (
      // Later we rely on the fact that we can at least use the mainthread previous
      // root-search score and PV in a multithreaded environment to prove mated-in scores.
      worker.completedDepth >= 1
      && ((worker.limits.use_time_management() && (elapsed > tm.maximum() || stopOnPonderhit))
          || (worker.limits.movetime && elapsed >= worker.limits.movetime)
          || (worker.limits.nodes && worker.threads.nodes_searched() >= worker.limits.nodes)))
        worker.threads.stop = worker.threads.abortedSearch = true;
}

void SearchManager::pv(const Search::Worker&     worker,
                       const ThreadPool&         threads,
                       const TranspositionTable& tt,
                       Depth                     depth) const {

    const auto  nodes     = threads.nodes_searched();
    const auto& rootMoves = worker.rootMoves;
    const auto& pos       = worker.rootPos;
    size_t      pvIdx     = worker.pvIdx;
    TimePoint   time      = tm.elapsed_time() + 1;
    size_t      multiPV   = std::min(size_t(worker.options["MultiPV"]), rootMoves.size());

    for (size_t i = 0; i < multiPV; ++i)
    {
        bool updated = rootMoves[i].score != -VALUE_INFINITE;

        if (depth == 1 && !updated && i > 0)
            continue;

        Depth d = updated ? depth : std::max(1, depth - 1);
        Value v = updated ? rootMoves[i].uciScore : rootMoves[i].previousScore;

        if (v == -VALUE_INFINITE)
            v = VALUE_ZERO;

        std::string pv;
        for (Move m : rootMoves[i].pv)
            pv += UCIEngine::move(m) + " ";

        // Remove last whitespace
        if (!pv.empty())
            pv.pop_back();

        auto wdl   = worker.options["UCI_ShowWDL"] ? UCIEngine::wdl(v, pos) : "";
        auto bound = rootMoves[i].scoreLowerbound
                     ? "lowerbound"
                     : (rootMoves[i].scoreUpperbound ? "upperbound" : "");

        InfoFull info;

        info.depth    = d;
        info.selDepth = rootMoves[i].selDepth;
        info.multiPV  = i + 1;
        info.score    = {v, pos};
        info.wdl      = wdl;

        if (i == pvIdx && updated)  // previous-scores are exact
            info.bound = bound;

        info.timeMs   = time;
        info.nodes    = nodes;
        info.nps      = nodes * 1000 / time;
        info.tbHits   = 0;
        info.pv       = pv;
        info.hashfull = tt.hashfull();

        updates.onUpdateFull(info);
    }
}

// Called in case we have no ponder move before exiting the search,
// for instance, in case we stop the search during a fail high at root.
// We try hard to have a ponder move to return to the GUI,
// otherwise in case of 'ponder on' we have nothing to think about.
bool RootMove::extract_ponder_from_tt(const TranspositionTable& tt, Position& pos) {

    StateInfo st;
    ASSERT_ALIGNED(&st, Eval::NNUE::CacheLineSize);

    assert(pv.size() == 1);
    if (pv[0] == Move::none())
        return false;

    pos.do_move(pv[0], st);

    auto [ttHit, ttData, ttWriter] = tt.probe(pos.key());
    if (ttHit)
    {
        if (MoveList<LEGAL>(pos).contains(ttData.move))
            pv.push_back(ttData.move);
    }

    pos.undo_move(pv[0]);
    return pv.size() > 1;
}

}  // namespace Stockfish
